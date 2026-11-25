/*
 * twopc.c - Atomic commitment: two-phase commit with a write-ahead log,
 *           timeouts, crash recovery and the blocking problem; optional
 *           three-phase commit per transaction.
 *
 * UDP port 7002. Wire format: type (1 byte), flags (1 byte: bit 0 = three
 * phase), transaction name (16 bytes, zero padded).
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TPC_PORT 7002
#define TPC_MAXP 8
#define TPC_MAXTX 8
#define TPC_LOG 12

enum { MT_PREPARE = 1, MT_YES, MT_NO, MT_PRECOMMIT, MT_PREACK, MT_COMMIT, MT_ABORT, MT_ACK, MT_QUERY };
enum { S_NONE, S_WAIT, S_READY, S_PRE, S_COMMIT, S_ABORT };

static const char *const TYPE_NAMES[] = { "?", "PREPARE", "VOTE-YES", "VOTE-NO", "PRE-COMMIT", "PRE-ACK",
                                          "COMMIT", "ABORT", "ACK", "QUERY" };

struct tpc;
typedef struct {
    struct tpc *owner;
    char     name[17];
    int      state, three;              /* state and protocol are on stable storage */
    unsigned votes, preacks, acks;      /* volatile */
    int      tries, blocked;
    nl_time  t_ready;
    nl_ev    timer;
} tpc_tx;

typedef struct tpc {
    nl_node *n;
    int      coord;                     /* role */
    int      np;
    char     names[TPC_MAXP][16];
    nl_ip    ips[TPC_MAXP];             /* participants (coordinator role) */
    nl_ip    coord_ip;
    char     coord_name[16];
    int      vote_yes;
    nl_time  timeout;
    tpc_tx   tx[TPC_MAXTX];
    int      ntx;
    char     log[TPC_LOG][64];          /* the write-ahead log, as text */
    int      nlog;
} tpc;

static const nl_module twopc_mod;

static nl_ip peer_addr(const nl_node *n, const char *name)
{
    const nl_node *p = nl_node_find(name);
    nl_ip ip = 0;
    int i, k;

    if (!p) {
        nl_resolve(name, &ip);
        return ip;
    }
    for (i = 0; i < n->nif; i++)
        for (k = 0; k < p->nif; k++)
            if (n->ifc[i].ip && p->ifc[k].ip && n->ifc[i].plen == p->ifc[k].plen &&
                ((n->ifc[i].ip ^ p->ifc[k].ip) & nl_mask(n->ifc[i].plen)) == 0)
                return p->ifc[k].ip;
    return nl_node_ip(p);
}

static const char *state_name(int s)
{
    switch (s) {
    case S_WAIT:   return "WAIT";
    case S_READY:  return "READY";
    case S_PRE:    return "PRE-COMMIT";
    case S_COMMIT: return "COMMITTED";
    case S_ABORT:  return "ABORTED";
    default:       return "INIT";
    }
}

/* Force a record to stable storage (it survives a crash) */
static void wal(tpc *t, const char *rec, const char *tx)
{
    if (t->nlog == TPC_LOG) {
        memmove(t->log[0], t->log[1], sizeof(t->log[0]) * (TPC_LOG - 1));
        t->nlog--;
    }
    snprintf(t->log[t->nlog++], sizeof(t->log[0]), "%s %s", rec, tx);
    nl_log(t->n, NL_L_DIST, 3, 0, T_INFO "2pc" T_RESET " %s: %s %s", _("log record forced to disk"), rec, tx);
}

static tpc_tx *tx_find(tpc *t, const char *name, int create)
{
    int i;

    for (i = 0; i < t->ntx; i++)
        if (strcmp(t->tx[i].name, name) == 0)
            return &t->tx[i];
    if (!create || t->ntx >= TPC_MAXTX)
        return NULL;
    memset(&t->tx[t->ntx], 0, sizeof(tpc_tx));
    t->tx[t->ntx].owner = t;
    snprintf(t->tx[t->ntx].name, sizeof(t->tx[0].name), "%s", name);
    return &t->tx[t->ntx++];
}

static void msg_send(tpc *t, nl_ip to, int type, const tpc_tx *x)
{
    uint8_t b[18];

    memset(b, 0, sizeof(b));
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)(x->three ? 1 : 0);
    memcpy(b + 2, x->name, strlen(x->name) > 16 ? 16 : strlen(x->name));
    nl_udp_send(t->n, TPC_PORT, to, TPC_PORT, b, sizeof(b));
}

static void send_parts(tpc *t, int type, const tpc_tx *x, unsigned skip)
{
    int i;

    for (i = 0; i < t->np; i++)
        if (!(skip & 1u << i))
            msg_send(t, t->ips[i], type, x);
}

/* ---- coordinator ---- */

static void coord_timer(void *ctx);

static void coord_decide(tpc *t, tpc_tx *x, int commit, const char *why)
{
    nl_cancel(&x->timer);
    x->state = commit ? S_COMMIT : S_ABORT;
    x->acks = 0;
    x->tries = 0;
    wal(t, commit ? "COMMIT" : "ABORT", x->name);
    nl_count(t->n, commit ? "twopc.committed" : "twopc.aborted", 1);
    nl_log(t->n, NL_L_DIST, 1, 0, "%s%s %s: %s" T_RESET " (%s)", commit ? T_OK "✓ " : T_ERR "✗ ",
           _("transaction"), x->name, commit ? _("decision COMMIT") : _("decision ABORT"), why);
    send_parts(t, commit ? MT_COMMIT : MT_ABORT, x, 0);
    x->timer = nl_after(t->n, t->timeout, coord_timer, x);
}

static void coord_timer(void *ctx)
{
    tpc_tx *x = ctx;
    tpc *t = x->owner;
    unsigned all = (1u << t->np) - 1;

    x->timer = 0;
    switch (x->state) {
    case S_WAIT:
        coord_decide(t, x, 0, _("a vote did not arrive in time"));
        break;
    case S_PRE:
        coord_decide(t, x, 1, _("pre-commit phase over"));
        break;
    case S_COMMIT:
    case S_ABORT:
        if ((x->acks & all) == all || ++x->tries > 8)
            break;
        nl_log(t->n, NL_L_DIST, 2, 0, T_INFO "2pc" T_RESET " %s %s: %s", _("transaction"), x->name,
               _("decision not acknowledged by everybody: sending it again"));
        send_parts(t, x->state == S_COMMIT ? MT_COMMIT : MT_ABORT, x, x->acks);
        x->timer = nl_after(t->n, t->timeout, coord_timer, x);
        break;
    default:
        break;
    }
}

static void coord_begin(tpc *t, const char *name, int three)
{
    tpc_tx *x = tx_find(t, name, 0);

    if (x || !(x = tx_find(t, name, 1))) {
        nl_log(t->n, NL_L_DIST, 1, 0, T_ERR "2pc: %s '%s'", _("transaction exists already, or too many:"), name);
        return;
    }
    x->state = S_WAIT;
    x->three = three;
    wal(t, three ? "BEGIN-3PC" : "BEGIN", x->name);
    nl_log(t->n, NL_L_DIST, 1, 0, T_INFO "● %s %s" T_RESET ": %s (%s)", _("transaction"), x->name,
           _("phase 1, PREPARE sent to every participant"),
           three ? _("three-phase commit") : _("two-phase commit"));
    send_parts(t, MT_PREPARE, x, 0);
    x->timer = nl_after(t->n, t->timeout, coord_timer, x);
}

static void coord_rx(tpc *t, int type, tpc_tx *x, const char *name, const nl_udpmsg *m)
{
    unsigned all = (1u << t->np) - 1, bit = 0;
    int i;

    for (i = 0; i < t->np; i++)
        if (t->ips[i] == m->src)
            bit = 1u << i;
    if (!bit)
        return;
    if (type == MT_QUERY) {
        tpc_tx tmp;

        if (x && (x->state == S_WAIT || x->state == S_PRE))
            return;                     /* still deciding */
        memset(&tmp, 0, sizeof(tmp));
        snprintf(tmp.name, sizeof(tmp.name), "%s", name);
        nl_log(t->n, NL_L_DIST, 2, m->pkt_id, T_INFO "2pc" T_RESET " %s %s %s %s: %s", _("query about"), name,
               _("from"), nl_ip_str(m->src),
               x ? state_name(x->state) : _("unknown transaction, presumed aborted"));
        msg_send(t, m->src, x && x->state == S_COMMIT ? MT_COMMIT : MT_ABORT, x ? x : &tmp);
        return;
    }
    if (!x)
        return;
    switch (type) {
    case MT_YES:
        if (x->state != S_WAIT)
            break;
        x->votes |= bit;
        if ((x->votes & all) != all)
            break;
        if (x->three) {
            nl_cancel(&x->timer);
            x->state = S_PRE;
            wal(t, "PRE-COMMIT", x->name);
            nl_log(t->n, NL_L_DIST, 1, 0, T_INFO "● %s %s" T_RESET ": %s", _("transaction"), x->name,
                   _("all voted yes; phase 2, PRE-COMMIT sent"));
            send_parts(t, MT_PRECOMMIT, x, 0);
            x->timer = nl_after(t->n, t->timeout, coord_timer, x);
        } else {
            coord_decide(t, x, 1, _("all participants voted yes"));
        }
        break;
    case MT_NO:
        if (x->state == S_WAIT)
            coord_decide(t, x, 0, _("a participant voted no"));
        break;
    case MT_PREACK:
        if (x->state != S_PRE)
            break;
        x->preacks |= bit;
        if ((x->preacks & all) == all)
            coord_decide(t, x, 1, _("pre-commit acknowledged by everybody"));
        break;
    case MT_ACK:
        x->acks |= bit;
        if ((x->acks & all) == all && x->timer) {
            nl_cancel(&x->timer);
            wal(t, "END", x->name);
            nl_log(t->n, NL_L_DIST, 2, 0, T_INFO "2pc" T_RESET " %s %s: %s", _("transaction"), x->name,
                   _("acknowledged by everybody, finished"));
        }
        break;
    default:
        break;
    }
}

/* ---- participant ---- */

static void part_timer(void *ctx);

static void part_finish(tpc *t, tpc_tx *x, int commit, const char *why)
{
    nl_cancel(&x->timer);
    if (x->state != S_COMMIT && x->state != S_ABORT) {
        int was_blocked = x->blocked;

        x->state = commit ? S_COMMIT : S_ABORT;
        x->blocked = 0;
        wal(t, commit ? "COMMIT" : "ABORT", x->name);
        nl_count(t->n, commit ? "twopc.committed" : "twopc.aborted", 1);
        nl_log(t->n, NL_L_DIST, 1, 0, "%s%s %s: %s" T_RESET " (%s)", commit ? T_OK "✓ " : T_ERR "✗ ",
               _("transaction"), x->name, commit ? _("committed") : _("aborted"), why);
        if (was_blocked)
            nl_log(t->n, NL_L_DIST, 1, 0, T_OK "● %s %s" T_RESET ": %s %.1f s", _("transaction"), x->name,
                   _("unblocked; locks were held for"), (nl_now() - x->t_ready) / 1e6);
    }
}

static void part_timer(void *ctx)
{
    tpc_tx *x = ctx;
    tpc *t = x->owner;

    x->timer = 0;
    if (x->state == S_PRE) {
        part_finish(t, x, 1, _("3PC: pre-committed and the coordinator is silent, so everybody is prepared to commit"));
        return;
    }
    if (x->state != S_READY)
        return;
    if (x->three) {
        part_finish(t, x, 0, _("3PC: no PRE-COMMIT arrived, so nobody can have committed"));
        return;
    }
    if (!x->blocked) {
        x->blocked = 1;
        nl_count(t->n, "twopc.blocked", 1);
        nl_log(t->n, NL_L_DIST, 1, 0, T_WARN "‖ %s %s: %s" T_RESET " — %s", _("transaction"), x->name,
               _("BLOCKED"),
               _("voted yes and heard nothing: it can neither commit nor abort on its own, and keeps its locks"));
    }
    nl_log(t->n, NL_L_DIST, 2, 0, T_INFO "2pc" T_RESET " %s %s: %s", _("transaction"), x->name,
           _("asking the coordinator for the decision"));
    msg_send(t, t->coord_ip, MT_QUERY, x);
    x->timer = nl_after(t->n, t->timeout, part_timer, x);
}

static void part_rx(tpc *t, int type, tpc_tx *x, const char *name, int three, const nl_udpmsg *m)
{
    tpc_tx tmp;

    if (m->src != t->coord_ip)
        return;
    switch (type) {
    case MT_PREPARE:
        if (!x) {
            x = tx_find(t, name, 1);
            if (!x)
                return;
            x->three = three;
            if (t->vote_yes) {
                x->state = S_READY;
                x->t_ready = nl_now();
                wal(t, "READY", x->name);
                nl_log(t->n, NL_L_DIST, 1, m->pkt_id, T_INFO "● %s %s" T_RESET ": %s", _("transaction"),
                       x->name, _("prepared (changes and locks on disk), vote YES"));
                x->timer = nl_after(t->n, 2 * t->timeout, part_timer, x);
            } else {
                x->state = S_ABORT;
                wal(t, "ABORT", x->name);
                nl_count(t->n, "twopc.aborted", 1);
                nl_log(t->n, NL_L_DIST, 1, m->pkt_id, T_ERR "✗ %s %s" T_RESET ": %s", _("transaction"),
                       x->name, _("cannot do it, vote NO and abort locally"));
            }
        }
        if (x->state == S_READY || x->state == S_ABORT)
            msg_send(t, t->coord_ip, x->state == S_READY ? MT_YES : MT_NO, x);
        break;
    case MT_PRECOMMIT:
        if (!x || (x->state != S_READY && x->state != S_PRE))
            break;
        if (x->state == S_READY) {
            x->state = S_PRE;
            wal(t, "PRE-COMMIT", x->name);
            nl_log(t->n, NL_L_DIST, 2, m->pkt_id, T_INFO "2pc" T_RESET " %s %s: %s", _("transaction"), x->name,
                   _("pre-commit: now I know everybody voted yes"));
        }
        msg_send(t, t->coord_ip, MT_PREACK, x);
        nl_cancel(&x->timer);
        x->timer = nl_after(t->n, 2 * t->timeout, part_timer, x);
        break;
    case MT_COMMIT:
    case MT_ABORT:
        if (!x) {                       /* decision for something we never saw: just acknowledge */
            memset(&tmp, 0, sizeof(tmp));
            snprintf(tmp.name, sizeof(tmp.name), "%s", name);
            msg_send(t, t->coord_ip, MT_ACK, &tmp);
            break;
        }
        part_finish(t, x, type == MT_COMMIT, _("decision received from the coordinator"));
        msg_send(t, t->coord_ip, MT_ACK, x);
        break;
    default:
        break;
    }
}

static void tpc_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    tpc *t = ctx;
    char name[17];
    tpc_tx *x;

    if (m->len < 18 || m->data[0] < 1 || m->data[0] > MT_QUERY)
        return;
    memcpy(name, m->data + 2, 16);
    name[16] = '\0';
    x = tx_find(t, name, 0);
    nl_log(n, NL_L_DIST, 2, m->pkt_id, T_INFO "2pc" T_RESET " ← %s %s %s %s", TYPE_NAMES[m->data[0]], name,
           _("from"), nl_ip_str(m->src));
    if (t->coord)
        coord_rx(t, m->data[0], x, name, m);
    else
        part_rx(t, m->data[0], x, name, m->data[1] & 1, m);
}

/* ---- module ---- */

static int tpc_run(nl_node *n, const nl_args *a)
{
    tpc *t = nl_mod_get(n, &twopc_mod);
    const char *cmd = nl_arg_pos(a, 0), *vote = nl_arg_str(a, "vote", NULL);

    if (!t) {
        char list[200];
        char *tok, *save = NULL;

        if (!cmd || (strcmp(cmd, "coordinator") && strcmp(cmd, "participant"))) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "2pc: %s", _("start with `coordinator` or `participant`"));
            return -1;
        }
        t = calloc(1, sizeof(*t));
        if (!t)
            return -1;
        t->n = n;
        t->coord = cmd[0] == 'c';
        t->timeout = nl_arg_time(a, "timeout", NL_SEC(1));
        t->vote_yes = !vote || strcmp(vote, "no") != 0;
        if (t->coord) {
            snprintf(list, sizeof(list), "%s", nl_arg_str(a, "participants", ""));
            for (tok = strtok_r(list, ",", &save); tok && t->np < TPC_MAXP; tok = strtok_r(NULL, ",", &save)) {
                snprintf(t->names[t->np], sizeof(t->names[0]), "%s", tok);
                t->ips[t->np] = peer_addr(n, tok);
                if (!t->ips[t->np++]) {
                    nl_log(n, NL_L_DIST, 1, 0, T_ERR "2pc: %s '%s'", _("unknown participant"), tok);
                    free(t);
                    return -1;
                }
            }
            if (!t->np) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "2pc: %s", _("the coordinator needs participants=a,b,c"));
                free(t);
                return -1;
            }
        } else {
            snprintf(t->coord_name, sizeof(t->coord_name), "%s", nl_arg_str(a, "coordinator", ""));
            t->coord_ip = peer_addr(n, t->coord_name);
            if (!t->coord_ip) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "2pc: %s '%s'", _("unknown coordinator"), t->coord_name);
                free(t);
                return -1;
            }
        }
        nl_mod_set(n, &twopc_mod, t);
        nl_udp_bind(n, TPC_PORT, tpc_rx, t);
        return 0;
    }
    if (vote) {
        t->vote_yes = strcmp(vote, "no") != 0;
        nl_log(n, NL_L_DIST, 2, 0, T_INFO "2pc" T_RESET " %s %s", _("from now on this participant votes"),
               t->vote_yes ? "YES" : "NO");
    }
    if (!cmd)
        return 0;
    if (strcmp(cmd, "begin") == 0 && t->coord) {
        static int serial;
        char name[17];

        snprintf(name, sizeof(name), "tx%d", ++serial);
        coord_begin(t, nl_arg_str(a, "tx", name), (int)nl_arg_int(a, "three", 0));
        return 0;
    }
    if (strcmp(cmd, "coordinator") == 0 || strcmp(cmd, "participant") == 0)
        return 0;
    nl_log(n, NL_L_DIST, 1, 0, T_ERR "2pc: %s '%s'", _("unknown command"), cmd);
    return -1;
}

static void tpc_stop(nl_node *n)
{
    tpc *t = nl_mod_get(n, &twopc_mod);
    int i;

    if (!t)
        return;
    for (i = 0; i < t->ntx; i++)
        nl_cancel(&t->tx[i].timer);
    nl_udp_unbind(n, TPC_PORT);
    nl_mod_set(n, &twopc_mod, NULL);
    free(t);
}

/* The log (state of every transaction) survives; votes, acknowledgements
 * and timers are in memory and are lost. */
static void tpc_crash(nl_node *n)
{
    tpc *t = nl_mod_get(n, &twopc_mod);
    int i;

    if (!t)
        return;
    for (i = 0; i < t->ntx; i++) {
        nl_cancel(&t->tx[i].timer);
        t->tx[i].votes = t->tx[i].preacks = t->tx[i].acks = 0;
        t->tx[i].tries = 0;
    }
}

static void tpc_recover(nl_node *n)
{
    tpc *t = nl_mod_get(n, &twopc_mod);
    int i;

    if (!t)
        return;
    nl_log(n, NL_L_DIST, 1, 0, T_INFO "● 2pc" T_RESET " %s", _("recovery: reading the log"));
    for (i = 0; i < t->ntx; i++) {
        tpc_tx *x = &t->tx[i];

        if (t->coord) {
            if (x->state == S_WAIT) {
                coord_decide(t, x, 0, _("recovery: begun but no decision in the log"));
            } else if (x->state == S_PRE) {
                coord_decide(t, x, 1, _("recovery: pre-commit in the log"));
            } else if (x->state == S_COMMIT || x->state == S_ABORT) {
                send_parts(t, x->state == S_COMMIT ? MT_COMMIT : MT_ABORT, x, 0);
                x->timer = nl_after(n, t->timeout, coord_timer, x);
            }
        } else if (x->state == S_READY || x->state == S_PRE) {
            nl_log(n, NL_L_DIST, 1, 0, T_WARN "● %s %s" T_RESET ": %s", _("transaction"), x->name,
                   _("in doubt after the crash, asking the coordinator"));
            msg_send(t, t->coord_ip, MT_QUERY, x);
            x->timer = nl_after(n, t->timeout, part_timer, x);
        }
    }
}

static int tpc_panel(nl_node *n, int x0, int y, int w, int h, int detail)
{
    tpc *t = nl_mod_get(n, &twopc_mod);
    int row = 0, i;

    (void)detail;
    if (t->coord)
        row += nl_ui_kv(x0, y + row, w, _("role"), "%s, %d %s", _("coordinator"), t->np, _("participants"));
    else
        row += nl_ui_kv(x0, y + row, w, _("role"), "%s (%s %s), %s %s", _("participant"), _("coordinator"),
                        t->coord_name, _("votes"), t->vote_yes ? "YES" : "NO");
    row += nl_ui_kv(x0, y + row, w, _("timeout"), "%s", nl_time_str(t->timeout));
    for (i = 0; i < t->ntx && row < h; i++) {
        const tpc_tx *x = &t->tx[i];
        const char *col = x->state == S_COMMIT ? T_OK : x->state == S_ABORT ? T_ERR : T_WARN;

        if (t->coord)
            tui_print(x0, y + row++, w, TS(C_TEXT, C_DEFAULT, 0),
                      T_BOLD "%-8s" T_RESET " %s%-10s" T_RESET T_DIM " %s" T_RESET " %x " T_DIM "acks" T_RESET " %x%s",
                      x->name, col, state_name(x->state), _("votes"), x->votes, x->acks, x->three ? "  3PC" : "");
        else
            tui_print(x0, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_BOLD "%-8s" T_RESET " %s%-10s" T_RESET "%s%s",
                      x->name, col, state_name(x->state), x->blocked ? T_CHIP_WARN " BLOCKED " : "",
                      x->three ? "  3PC" : "");
    }
    if (row < h)
        tui_print(x0, y + row++, w, TS(C_DIM, C_DEFAULT, TA_BOLD), "%s", _("write-ahead log"));
    for (i = 0; i < t->nlog && row < h; i++)
        tui_print(x0, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " %2d  %s", i + 1, t->log[i]);
    return row;
}

static void dis_tpc(nl_dissect *o, const uint8_t *d, int len, int off)
{
    char name[17];

    if (len < 18 || d[0] < 1 || d[0] > MT_QUERY)
        return;
    memcpy(name, d + 2, 16);
    name[16] = '\0';
    nl_dis_field(o, "2PC", "Message", off, 1, "%d (%s)", d[0], TYPE_NAMES[d[0]]);
    nl_dis_field(o, "2PC", "Flags", off + 1, 1, "%s", d[1] & 1 ? "three-phase" : "two-phase");
    nl_dis_field(o, "2PC", "Transaction", off + 2, 16, "%s", name);
    nl_dis_summary(o, "2PC", 209, "%s %s", TYPE_NAMES[d[0]], name);
}

static const nl_module twopc_mod = {
    .name = "twopc", .title = N_("Atomic commitment: two-phase and three-phase commit"), .layer = NL_L_DIST,
    .usage = N_("run NODE twopc coordinator participants=p1,p2,p3 [timeout=1s]\n"
                "run NODE twopc participant coordinator=NODE [vote=yes|no] [timeout=1s]\n"
                "run COORD twopc begin [tx=NAME] [three=1]   start a transaction (three=1: 3PC)\n"
                "run PART twopc vote=no                      change how a participant votes\n"
                "  Crash and restart nodes with `node N down` / `node N up` to see recovery."),
    .theory = N_("A distributed transaction touches data on several machines and must be atomic: "
                 "either every machine commits it or none does. No machine may decide alone, because "
                 "another one might be unable to commit; and once a machine has promised, it cannot "
                 "change its mind.\n\n"
                 "Two-phase commit (2PC) gives the decision to a coordinator. Phase 1: it sends PREPARE; "
                 "each participant makes the changes durable without applying them, writes a READY "
                 "record to its log and votes YES, or votes NO and aborts. Phase 2: if all voted YES "
                 "the coordinator writes COMMIT to its log - this single log write is the moment the "
                 "transaction commits - and tells everybody; otherwise it sends ABORT. Participants "
                 "acknowledge, and the coordinator repeats the decision until all have.\n\n"
                 "Logs make the protocol survive crashes. A coordinator that restarts finds either a "
                 "decision, which it sends again, or none, and then aborts (presumed abort). A "
                 "participant that restarts in the READY state is in doubt and asks the coordinator.\n\n"
                 "The weakness is the window between a YES vote and the decision. A participant that has "
                 "voted YES and hears nothing can neither commit (somebody may have voted NO) nor abort "
                 "(the coordinator may have committed): it must wait, holding its locks, until the "
                 "coordinator comes back. 2PC is a blocking protocol.\n\n"
                 "Three-phase commit (3PC) inserts a PRE-COMMIT round between the votes and the commit, "
                 "so that no process commits while another is still uncertain. A participant that times "
                 "out before PRE-COMMIT knows nobody committed and aborts; one that times out after it "
                 "knows everybody voted YES and commits. This removes blocking only if timeouts are "
                 "reliable failure detectors, that is in a synchronous network without partitions; the "
                 "version here also omits the termination protocol that elects a new coordinator. With "
                 "real networks the general answer is consensus (Paxos, Raft): replicate the "
                 "coordinator's decision so that it survives the crash of any single machine."),
    .run = tpc_run, .stop = tpc_stop, .crash = tpc_crash, .recover = tpc_recover, .panel = tpc_panel,
};
NL_MODULE(twopc_mod)

NL_INIT(twopc_dissector)
{
    nl_dissector_udp(TPC_PORT, dis_tpc);
}
