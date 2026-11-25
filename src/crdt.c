/*
 * crdt.c - Conflict-free replicated data types with anti-entropy gossip.
 *
 * Every replica holds three state-based CRDTs and periodically sends its
 * whole state to one random peer, which merges it:
 *   - a counter (G-Counter, or PN-Counter when decrements are allowed):
 *     one increment and one decrement total per replica, merged with max;
 *   - an observed-remove set (OR-Set): every add creates a unique tag
 *     (replica, sequence number), a remove marks the tags it has seen;
 *   - a last-writer-wins register: (timestamp, replica) decides.
 *
 * UDP port 7004. Wire format (big endian): 1, sender id, number of replicas,
 * number of set entries; increments (4 x replicas); decrements (4 x
 * replicas); register timestamp (4), replica (1), length (1), value; then
 * the set entries: replica (1), sequence (2), removed (1), length (1), text.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CR_PORT  7004
#define CR_MAXP  8
#define CR_ELEMS 40

enum { CR_GCOUNTER, CR_PNCOUNTER, CR_ORSET, CR_LWW };
static const char *const CR_TYPES[] = { "gcounter", "pncounter", "orset", "lww" };

typedef struct {
    int      used, removed;
    uint8_t  rep;
    uint16_t seq;
    char     elem[14];
} cr_entry;

typedef struct {
    uint32_t P[CR_MAXP], N[CR_MAXP];
    uint32_t reg_ts;
    uint8_t  reg_rep;
    char     reg[20];
    cr_entry e[CR_ELEMS];
} cr_data;

typedef struct {
    nl_node *n;
    int      np, me, type;
    char     names[CR_MAXP][16];
    nl_ip    ips[CR_MAXP];
    nl_time  sync, skew;
    nl_ev    timer;
    cr_data  d;
    uint16_t next_seq;
    int      own_write;                 /* the register holds a value written here */
    unsigned merges, lost, rounds;
    unsigned types;                     /* bit mask of the data types this replica hosts */
    uint8_t  agree[CR_MAXP];            /* the last state heard from that peer equalled ours */
} crdt;

static const nl_module crdt_mod;

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

static long counter_value(const crdt *c)
{
    long v = 0;
    int i;

    for (i = 0; i < c->np; i++)
        v += (long)c->d.P[i] - (long)c->d.N[i];
    return v;
}

/* The visible set as text; returns its size */
static int set_text(const crdt *c, char *out, size_t n)
{
    int i, k, size = 0;
    size_t len = 0;

    out[0] = '\0';
    for (i = 0; i < CR_ELEMS; i++) {
        const cr_entry *e = &c->d.e[i];
        int dup = 0;

        if (!e->used || e->removed)
            continue;
        for (k = 0; k < i; k++)
            if (c->d.e[k].used && !c->d.e[k].removed && strcmp(c->d.e[k].elem, e->elem) == 0)
                dup = 1;
        if (dup)
            continue;
        len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s%s", size ? ", " : "", e->elem);
        size++;
    }
    return size;
}

/* Fingerprint of the whole state, independent of the order of the entries */
static uint32_t state_digest(const crdt *c)
{
    uint32_t h = 2166136261u, sum = 0;
    const char *s;
    int i;

    for (i = 0; i < c->np; i++)
        h = (h ^ c->d.P[i]) * 16777619u, h = (h ^ c->d.N[i]) * 16777619u;
    h = (h ^ c->d.reg_ts) * 16777619u;
    h = (h ^ c->d.reg_rep) * 16777619u;
    for (s = c->d.reg; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    for (i = 0; i < CR_ELEMS; i++) {
        const cr_entry *e = &c->d.e[i];
        uint32_t x = 2166136261u;

        if (!e->used)
            continue;
        x = (x ^ e->rep) * 16777619u;
        x = (x ^ e->seq) * 16777619u;
        x = (x ^ (uint32_t)e->removed) * 16777619u;
        for (s = e->elem; *s; s++)
            x = (x ^ (unsigned char)*s) * 16777619u;
        sum += x;
    }
    return h ^ sum;
}

/* A replica cannot see the others, but gossip tells it: it has converged
 * when the last state received from every peer was identical to its own. */
static int converged(const crdt *c)
{
    int i;

    for (i = 0; i < c->np; i++)
        if (i != c->me && !c->agree[i])
            return 0;
    return 1;
}

static void publish(crdt *c)
{
    char buf[200];

    nl_count_set(c->n, "crdt.converged", converged(c));
    nl_count_set(c->n, "crdt.value", (double)counter_value(c));
    nl_count_set(c->n, "crdt.size", set_text(c, buf, sizeof(buf)));
    nl_count_set(c->n, "crdt.register", atof(c->d.reg));
    nl_count_set(c->n, "crdt.merges", c->merges);
    nl_count_set(c->n, "crdt.lost", c->lost);
    nl_sample(c->n, "counter value", (double)counter_value(c));
}

static cr_entry *entry_find(crdt *c, int rep, int seq, int create)
{
    cr_entry *freeslot = NULL;
    int i;

    for (i = 0; i < CR_ELEMS; i++) {
        if (c->d.e[i].used && c->d.e[i].rep == rep && c->d.e[i].seq == seq)
            return &c->d.e[i];
        if (!c->d.e[i].used && !freeslot)
            freeslot = &c->d.e[i];
    }
    if (!create || !freeslot)
        return NULL;
    memset(freeslot, 0, sizeof(*freeslot));
    freeslot->used = 1;
    freeslot->rep = (uint8_t)rep;
    freeslot->seq = (uint16_t)seq;
    return freeslot;
}

/* Send the whole state to one peer. `reply` marks an answer (the peer
 * asked, or sent a state that differs from ours): answers are not answered,
 * unless they still differ. */
static void cr_send(crdt *c, int to, int reply)
{
    uint8_t b[1400];
    int len = 4, i, cnt = 0;

    b[0] = 1;
    /* bit 7: "I do not know yet whether we agree, please answer" */
    b[1] = (uint8_t)(c->me | (!reply && !c->agree[to] ? 0x80 : 0));
    b[2] = (uint8_t)c->np;
    for (i = 0; i < c->np; i++, len += 4)
        nl_put32(b + len, c->d.P[i]);
    for (i = 0; i < c->np; i++, len += 4)
        nl_put32(b + len, c->d.N[i]);
    nl_put32(b + len, c->d.reg_ts);
    b[len + 4] = c->d.reg_rep;
    b[len + 5] = (uint8_t)strlen(c->d.reg);
    memcpy(b + len + 6, c->d.reg, b[len + 5]);
    len += 6 + b[len + 5];
    for (i = 0; i < CR_ELEMS && len < (int)sizeof(b) - 24; i++) {
        const cr_entry *e = &c->d.e[i];
        if (!e->used)
            continue;
        b[len] = e->rep;
        nl_put16(b + len + 1, e->seq);
        b[len + 3] = (uint8_t)e->removed;
        b[len + 4] = (uint8_t)strlen(e->elem);
        memcpy(b + len + 5, e->elem, b[len + 4]);
        len += 5 + b[len + 4];
        cnt++;
    }
    b[3] = (uint8_t)cnt;
    nl_put32(b + len, state_digest(c));     /* so that the receiver can tell whether we now agree */
    len += 4;
    nl_log(c->n, NL_L_DIST, 3, 0, T_INFO "crdt" T_RESET " %s → %s (%d B)", _("state sent to"), c->names[to], len);
    nl_udp_send(c->n, CR_PORT, c->ips[to], CR_PORT, b, len);
}

static void cr_gossip(void *ctx)
{
    crdt *c = ctx;
    int to;

    c->timer = nl_after(c->n, c->sync, cr_gossip, c);
    if (c->np < 2)
        return;
    do
        to = (int)nl_rand((unsigned)c->np);
    while (to == c->me);
    c->rounds++;
    cr_send(c, to, 0);
}

/* Merge: the least upper bound of the two states */
static void cr_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    crdt *c = ctx;
    const uint8_t *d = m->data;
    int from, np, cnt, off = 4, i, changed = 0, rlen, wants;
    uint32_t ts;
    long before = counter_value(c);
    char sbefore[200], safter[200], rbefore[20];

    if (m->len < 4 || d[0] != 1 || d[2] != c->np || (d[1] & 0x7f) >= c->np || m->len < 4 + 8 * c->np + 6)
        return;
    from = d[1] & 0x7f;
    wants = d[1] & 0x80;
    np = d[2];
    cnt = d[3];
    set_text(c, sbefore, sizeof(sbefore));
    snprintf(rbefore, sizeof(rbefore), "%s", c->d.reg);

    /* counters: element-wise maximum */
    for (i = 0; i < np; i++, off += 4)
        if (nl_get32(d + off) > c->d.P[i]) {
            c->d.P[i] = nl_get32(d + off);
            changed = 1;
        }
    for (i = 0; i < np; i++, off += 4)
        if (nl_get32(d + off) > c->d.N[i]) {
            c->d.N[i] = nl_get32(d + off);
            changed = 1;
        }

    /* register: the larger (timestamp, replica) wins */
    ts = nl_get32(d + off);
    rlen = d[off + 5];
    if (off + 6 + rlen > m->len)
        return;
    if (ts > c->d.reg_ts || (ts == c->d.reg_ts && d[off + 4] > c->d.reg_rep)) {
        char val[20];

        snprintf(val, sizeof(val), "%.*s", rlen, (const char *)d + off + 6);
        if (c->own_write && strcmp(val, c->d.reg) != 0) {
            c->lost++;
            nl_log(n, NL_L_DIST, 1, m->pkt_id, T_WARN "● crdt LWW" T_RESET ": %s «%s» %s «%s» (%s %s): %s",
                   _("my write"), c->d.reg, _("is replaced by"), val, _("written on"), c->names[d[off + 4]],
                   _("last writer wins, the other update is silently lost"));
        }
        c->own_write = 0;
        c->d.reg_ts = ts;
        c->d.reg_rep = d[off + 4];
        snprintf(c->d.reg, sizeof(c->d.reg), "%s", val);
        changed = 1;
    }
    off += 6 + rlen;

    /* set: union of the tags, union of the removals */
    for (i = 0; i < cnt && off + 5 <= m->len; i++) {
        int elen = d[off + 4], known;
        cr_entry *e;

        if (off + 5 + elen > m->len)
            break;
        known = entry_find(c, d[off], nl_get16(d + off + 1), 0) != NULL;
        e = entry_find(c, d[off], nl_get16(d + off + 1), 1);
        if (e) {
            if (!known) {
                snprintf(e->elem, sizeof(e->elem), "%.*s", elen, (const char *)d + off + 5);
                changed = 1;
            }
            if (d[off + 3] && !e->removed) {
                e->removed = 1;
                changed = 1;
            }
        }
        off += 5 + elen;
    }
    if (changed)
        memset(c->agree, 0, sizeof(c->agree));
    c->agree[from] = off + 4 <= m->len && nl_get32(d + off) == state_digest(c);
    /* anti-entropy in both directions: answer whoever asked, or whoever
     * showed a state that lacks something we have */
    if (wants || !c->agree[from])
        cr_send(c, from, c->agree[from]);
    if (!changed) {
        nl_log(n, NL_L_DIST, 3, m->pkt_id, T_INFO "crdt" T_RESET " %s %s: %s", _("state from"), c->names[from],
               _("nothing new"));
        publish(c);
        return;
    }
    c->merges++;
    set_text(c, safter, sizeof(safter));
    if (before != counter_value(c))
        nl_log(n, NL_L_DIST, 1, m->pkt_id, T_OK "⇄ crdt" T_RESET " %s %s: %s %ld → %ld", _("merge with"),
               c->names[from], _("counter"), before, counter_value(c));
    if (strcmp(sbefore, safter) != 0)
        nl_log(n, NL_L_DIST, 1, m->pkt_id, T_OK "⇄ crdt" T_RESET " %s %s: %s {%s} → {%s}", _("merge with"),
               c->names[from], _("set"), sbefore, safter);
    if (strcmp(rbefore, c->d.reg) != 0)
        nl_log(n, NL_L_DIST, 1, m->pkt_id, T_OK "⇄ crdt" T_RESET " %s %s: %s «%s» → «%s»", _("merge with"),
               c->names[from], _("register"), rbefore, c->d.reg);
    publish(c);
}

static int cr_run(nl_node *n, const nl_args *a)
{
    crdt *c = nl_mod_get(n, &crdt_mod);
    const char *cmd = nl_arg_pos(a, 0), *arg = nl_arg_pos(a, 1);
    char buf[200];
    int i;

    if (!c) {
        char list[200];
        const char *type = nl_arg_str(a, "type", "pncounter");
        char *tok, *save = NULL;

        c = calloc(1, sizeof(*c));
        if (!c)
            return -1;
        c->n = n;
        c->me = -1;
        {   /* one type, or several separated by commas: the replica hosts one object of each */
            char tl[64];
            char *tt, *ts = NULL;

            snprintf(tl, sizeof(tl), "%s", type);
            c->type = -1;
            for (tt = strtok_r(tl, ",+", &ts); tt; tt = strtok_r(NULL, ",+", &ts)) {
                for (i = 0; i < 4; i++)
                    if (strcmp(tt, CR_TYPES[i]) == 0) {
                        c->types |= 1u << i;
                        if (c->type < 0)
                            c->type = i;
                    }
            }
            if (!c->types) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s '%s' (gcounter, pncounter, orset, lww)", _("unknown type"),
                       type);
                free(c);
                return -1;
            }
        }
        snprintf(list, sizeof(list), "%s", nl_arg_str(a, "peers", n->name));
        for (tok = strtok_r(list, ",", &save); tok && c->np < CR_MAXP; tok = strtok_r(NULL, ",", &save)) {
            snprintf(c->names[c->np], sizeof(c->names[0]), "%s", tok);
            if (strcmp(tok, n->name) == 0)
                c->me = c->np;
            else if (!(c->ips[c->np] = peer_addr(n, tok))) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s '%s'", _("unknown peer"), tok);
                free(c);
                return -1;
            }
            c->np++;
        }
        if (c->me < 0) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s", _("this node must be in its own peers list"));
            free(c);
            return -1;
        }
        c->sync = nl_arg_time(a, "sync", NL_MS(500));
        if (c->sync < NL_MS(20))
            c->sync = NL_MS(20);
        c->skew = nl_arg_time(a, "skew", 0);
        nl_mod_set(n, &crdt_mod, c);
        nl_udp_bind(n, CR_PORT, cr_rx, c);
        c->timer = nl_after(n, (nl_time)(nl_rand01() * (double)c->sync), cr_gossip, c);
        publish(c);
        if (!cmd)
            return 0;
    }
    if (!cmd)
        return 0;
    {   /* an operation must belong to a type this replica hosts */
        unsigned need = !strcmp(cmd, "inc") ? (1u << CR_GCOUNTER | 1u << CR_PNCOUNTER)
                      : !strcmp(cmd, "dec") ? (1u << CR_GCOUNTER | 1u << CR_PNCOUNTER)
                      : !strcmp(cmd, "add") || !strcmp(cmd, "remove") ? 1u << CR_ORSET
                      : !strcmp(cmd, "set") ? 1u << CR_LWW : 0;

        if (need && !(c->types & need)) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s '%s' %s (%s)", _("operation"), cmd,
                   _("does not exist for the type of this replica"),
                   need & (1u << CR_ORSET) ? "type=orset" : need & (1u << CR_LWW) ? "type=lww" : "type=pncounter");
            nl_count(n, "crdt.refused", 1);
            return -1;
        }
    }
    memset(c->agree, 0, sizeof(c->agree));
    if (strcmp(cmd, "inc") == 0 || strcmp(cmd, "dec") == 0) {
        uint32_t by = (uint32_t)nl_arg_int(a, "by", 1);

        if (cmd[0] == 'd' && !(c->types & 1u << CR_PNCOUNTER)) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s", _("a G-Counter can only grow (use type=pncounter)"));
            nl_count(n, "crdt.refused", 1);
            return -1;
        }
        if (cmd[0] == 'i')
            c->d.P[c->me] += by;
        else
            c->d.N[c->me] += by;
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "● crdt" T_RESET " %s %u: %s %ld", cmd[0] == 'i' ? "+" : "−", by,
               _("local counter value"), counter_value(c));
    } else if (strcmp(cmd, "add") == 0 && arg) {
        cr_entry *e = entry_find(c, c->me, ++c->next_seq, 1);

        if (!e) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s", _("the set is full"));
            return -1;
        }
        snprintf(e->elem, sizeof(e->elem), "%s", arg);
        set_text(c, buf, sizeof(buf));
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "● crdt" T_RESET " add %s " T_DIM "(%s %s:%u)" T_RESET " → {%s}", arg,
               _("tag"), n->name, e->seq, buf);
    } else if (strcmp(cmd, "remove") == 0 && arg) {
        int k = 0;

        for (i = 0; i < CR_ELEMS; i++)
            if (c->d.e[i].used && !c->d.e[i].removed && strcmp(c->d.e[i].elem, arg) == 0) {
                c->d.e[i].removed = 1;
                k++;
            }
        set_text(c, buf, sizeof(buf));
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "● crdt" T_RESET " remove %s " T_DIM "(%d %s)" T_RESET " → {%s}", arg, k,
               _("observed tags removed"), buf);
    } else if (strcmp(cmd, "set") == 0 && arg) {
        nl_time wall = (nl_now() + c->skew) / 1000;
        uint32_t ts = wall < 1 ? 1 : (uint32_t)wall;

        if (ts <= c->d.reg_ts && c->skew == 0)
            ts = c->d.reg_ts + 1;       /* never go back on what we have seen */
        c->d.reg_ts = ts;
        c->d.reg_rep = (uint8_t)c->me;
        snprintf(c->d.reg, sizeof(c->d.reg), "%s", arg);
        c->own_write = 1;
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "● crdt" T_RESET " set «%s» " T_DIM "(%s %u ms)", arg, _("timestamp"),
               ts);
    } else {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "crdt: %s", _("usage: inc [by=N] | dec [by=N] | add X | remove X | set V"));
        return -1;
    }
    publish(c);
    return 0;
}

static void cr_stop(nl_node *n)
{
    crdt *c = nl_mod_get(n, &crdt_mod);

    if (!c)
        return;
    nl_cancel(&c->timer);
    nl_udp_unbind(n, CR_PORT);
    nl_mod_set(n, &crdt_mod, NULL);
    free(c);
}

/* The replica state is on disk; only the gossip timer must be restarted */
static void cr_crash(nl_node *n)
{
    crdt *c = nl_mod_get(n, &crdt_mod);

    if (c)
        c->timer = 0;
}

static void cr_recover(nl_node *n)
{
    crdt *c = nl_mod_get(n, &crdt_mod);

    if (c)
        c->timer = nl_after(n, c->sync, cr_gossip, c);
}

static int cr_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    crdt *c = nl_mod_get(n, &crdt_mod);
    char buf[200], vec[120] = "";
    int row = 0, i, size;
    size_t len = 0;

    {
        char tn[64] = "";
        size_t tl = 0;

        for (i = 0; i < 4; i++)
            if (c->types & 1u << i)
                tl += (size_t)snprintf(tn + tl, sizeof(tn) - tl, "%s%s", tl ? " + " : "", CR_TYPES[i]);
        row += nl_ui_kv(x, y + row, w, _("replica"), "%s (%d/%d), %s%s", n->name, c->me + 1, c->np, tn,
                        converged(c) ? T_OK "  ✓ in sync" : T_WARN "  …");
    }
    row += nl_ui_kv(x, y + row, w, _("gossip"), "%s, %u %s, %u %s", nl_time_str(c->sync), c->rounds, _("rounds"),
                    c->merges, _("merges"));
    if (c->types & (1u << CR_GCOUNTER | 1u << CR_PNCOUNTER)) {
        row += nl_ui_kv(x, y + row, w, _("counter"), "%ld", counter_value(c));
        for (i = 0; i < c->np; i++)
            len += (size_t)snprintf(vec + len, len < sizeof(vec) ? sizeof(vec) - len : 0, "%s%s +%u −%u",
                                    i ? "  " : "", c->names[i], c->d.P[i], c->d.N[i]);
        if (row < h)
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "  %s", vec);
    }
    size = set_text(c, buf, sizeof(buf));
    if (c->types & 1u << CR_ORSET)
        row += nl_ui_kv(x, y + row, w, _("set"), "{%s} (%d)", buf, size);
    if (detail >= 2 && (c->types & 1u << CR_ORSET))
        for (i = 0; i < CR_ELEMS && row < h; i++)
            if (c->d.e[i].used)
                tui_print(x, y + row++, w, TS(c->d.e[i].removed ? C_FAINT : C_TEXT, C_DEFAULT, 0),
                          "  %-12s " T_DIM "%s %s:%u%s", c->d.e[i].elem, _("tag"), c->names[c->d.e[i].rep],
                          c->d.e[i].seq, c->d.e[i].removed ? "  ✗" : "");
    if (c->types & 1u << CR_LWW)
        row += nl_ui_kv(x, y + row, w, _("register"), "«%s»  (%u ms, %s)%s", c->d.reg, c->d.reg_ts,
                        c->d.reg_ts ? c->names[c->d.reg_rep] : "—", c->lost ? "  LWW!" : "");
    if (h - row >= 5 && (c->types & (1u << CR_GCOUNTER | 1u << CR_PNCOUNTER)))
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "counter value"), C_OK, _("counter"));
    return h;
}

static void dis_crdt(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int np, i;
    long v = 0;

    if (len < 4 || d[0] != 1)
        return;
    np = d[2];
    if (np > CR_MAXP || len < 4 + 8 * np + 6)
        return;
    for (i = 0; i < np; i++)
        v += (long)nl_get32(d + 4 + 4 * i) - (long)nl_get32(d + 4 + 4 * np + 4 * i);
    nl_dis_field(o, "CRDT", "Sender replica", off + 1, 1, "%d%s", (d[1] & 0x7f) + 1, d[1] & 0x80 ? " (please answer)" : "");
    nl_dis_field(o, "CRDT", "Increments", off + 4, 4 * np, "%d replicas", np);
    nl_dis_field(o, "CRDT", "Decrements", off + 4 + 4 * np, 4 * np, "%d replicas (value %ld)", np, v);
    nl_dis_field(o, "CRDT", "Register", off + 4 + 8 * np, 6 + d[4 + 8 * np + 5], "«%.*s» ts %u replica %d",
                 d[4 + 8 * np + 5], (const char *)d + 4 + 8 * np + 6, nl_get32(d + 4 + 8 * np),
                 d[4 + 8 * np + 4] + 1);
    nl_dis_field(o, "CRDT", "Set entries", off + 3, 1, "%d (tags and tombstones)", d[3]);
    nl_dis_summary(o, "CRDT", 209, "state of replica %d: counter %ld, %d set tags", (d[1] & 0x7f) + 1, v, d[3]);
}

static const nl_module crdt_mod = {
    .name = "crdt", .title = N_("CRDTs: replicas that converge without coordination"), .layer = NL_L_DIST,
    .usage = N_("run NODE crdt peers=n1,n2,n3 [type=gcounter|pncounter|orset|lww] [sync=500ms] [skew=TIME]\n"
                "run NODE crdt inc [by=N] | dec [by=N]     counter\n"
                "run NODE crdt add X | remove X            observed-remove set\n"
                "run NODE crdt set V                       last-writer-wins register\n"
                "  Every node needs the same peers list. type selects what the replica hosts and which\n"
                "  operations it accepts: gcounter (inc), pncounter (inc, dec), orset (add, remove), lww\n"
                "  (set); several types separated by commas host one object of each (type=orset,lww).\n"
                "  skew shifts the clock used by the register. crdt.converged is 1 when the last state\n"
                "  heard from every peer was identical to the local one."),
    .theory = N_("Replicating data on several machines forces a choice when the network splits: refuse "
                 "updates until the replicas can agree (consistency), or accept them everywhere and "
                 "reconcile later (availability). Conflict-free replicated data types make the second "
                 "choice safe by construction: replicas accept updates locally, exchange state in any "
                 "order, any number of times, and are guaranteed to converge to the same value once "
                 "they have seen the same updates. This property is strong eventual consistency.\n\n"
                 "A state-based CRDT is a data structure whose states form a join semilattice: there "
                 "is a merge operation that is commutative, associative and idempotent, and every "
                 "update moves the state upwards. Because merging is insensitive to order, duplication "
                 "and delay, the transport can be as unreliable as gossip: here each replica sends its "
                 "whole state to one random peer at every round (anti-entropy).\n\n"
                 "The grow-only counter keeps one total per replica; a replica only increments its own "
                 "entry, the merge is the element-wise maximum and the value is the sum. A plain shared "
                 "integer would lose concurrent increments; this one cannot. Two such vectors, one for "
                 "increments and one for decrements, give the PN-Counter.\n\n"
                 "Sets are harder because add and remove do not commute. The observed-remove set tags "
                 "every add with a unique identifier; a remove deletes only the tags its replica has "
                 "observed. An add concurrent with a remove carries a tag the remover never saw, so the "
                 "element survives: add wins, and no update is lost. The cost is metadata: removed tags "
                 "must be remembered as tombstones.\n\n"
                 "The last-writer-wins register simply keeps the value with the highest (timestamp, "
                 "replica) pair. It converges too, but by discarding one of two concurrent writes, "
                 "and which one depends on clocks: a replica with a slow clock loses every race. It is "
                 "the default of many databases and a classic source of silently lost updates. CRDTs "
                 "do not replace consensus: they cannot enforce invariants that span replicas, such as "
                 "a balance that never goes below zero."),
    .run = cr_run, .stop = cr_stop, .crash = cr_crash, .recover = cr_recover, .panel = cr_panel,
};
NL_MODULE(crdt_mod)

NL_INIT(crdt_dissector)
{
    nl_dissector_udp(CR_PORT, dis_crdt);
}
