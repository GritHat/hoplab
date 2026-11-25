/*
 * clock.c - Logical time and ordered broadcast: Lamport clocks, vector
 *           clocks, and FIFO / causal / total-order broadcast built on them.
 *
 * Every process knows the same ordered list of peers; its position in the
 * list is its process id. Messages travel over UDP port 7001.
 *
 * Wire format (big endian):
 *   0 type (1 unicast, 2 broadcast, 3 ack)   1 sender id   2 number of peers
 *   3 ack: id of the broadcast's sender      4 Lamport timestamp (4)
 *   8 ack: timestamp of the broadcast (4)    12 vector clock (4 x peers)
 *   then the broadcast dependency vector (4 x peers), then the text.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLK_PORT 7001
#define CLK_MAXP 8
#define CLK_HOLD 16
#define CLK_HIST 10

enum { M_UNI = 1, M_BCAST = 2, M_ACK = 3 };
enum { ORD_NONE, ORD_FIFO, ORD_CAUSAL, ORD_TOTAL };

typedef struct {
    int      used;                      /* 1 = complete, 2 = only acks seen so far */
    uint8_t  type, from, ref_from;
    uint32_t ts, ref_ts;
    uint32_t vc[CLK_MAXP], bv[CLK_MAXP];
    unsigned acks;                      /* total order: who acknowledged */
    char     text[48];
} cmsg;

typedef struct {
    nl_node *n;
    int      np, me, vector, order;
    char     names[CLK_MAXP][16];
    nl_ip    ips[CLK_MAXP];
    uint32_t L, vc[CLK_MAXP];
    uint32_t D[CLK_MAXP];               /* broadcasts delivered, per sender */
    uint32_t sentb;                     /* broadcasts sent */
    cmsg     hold[CLK_HOLD];            /* hold-back queue (fifo / causal) */
    cmsg     tq[CLK_HOLD];              /* total order queue, sorted by (ts, sender) */
    int      ntq;
    char     hist[CLK_HIST][110];
    int      nhist;
    unsigned delivered, heldback, violations;
} clk;

static const nl_module clock_mod;
static const char *const ORD_NAMES[] = { "none", "fifo", "causal", "total" };

/* Address of a peer: the one on a subnet we share, if any */
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

static const char *stamp(const clk *c, uint32_t ts, const uint32_t *vc)
{
    static char bufs[4][80];
    static int k;
    char *b = bufs[k = (k + 1) % 4];
    int i, len;

    if (!c->vector) {
        snprintf(b, 80, "L=%u", ts);
        return b;
    }
    len = snprintf(b, 80, "[");
    for (i = 0; i < c->np && len < 70; i++)
        len += snprintf(b + len, (size_t)(80 - len), "%s%u", i ? "," : "", vc[i]);
    snprintf(b + len, (size_t)(80 - len), "]");
    return b;
}

/* Vector clocks answer a question Lamport clocks cannot: are two events
 * causally related? After each event of this process, compare its vector
 * with the latest event of every other process and say which are
 * concurrent (neither vector dominates). The comparison looks at the other
 * processes' state directly: it is the view of an outside observer, which
 * no process could have, and it only covers processes simulated here. */
static void relate(clk *c)
{
    int i, k;

    if (!c->vector)
        return;
    for (i = 0; i < c->np; i++) {
        const nl_node *pn = i == c->me ? NULL : nl_node_find(c->names[i]);
        const clk *o = pn && pn->local ? nl_mod_get(pn, &clock_mod) : NULL;
        int less = 0, more = 0, any = 0;

        if (!o || !o->vector || o->np != c->np)
            continue;
        for (k = 0; k < c->np; k++) {
            if (o->vc[k] < c->vc[k]) more = 1;
            if (o->vc[k] > c->vc[k]) less = 1;
            any |= o->vc[k] != 0;
        }
        if (!any)
            continue;                   /* that process has done nothing yet */
        if (less && more) {
            nl_count(c->n, "clock.concurrent", 1);
            nl_log(c->n, NL_L_DIST, 1, 0, T_WARN "  ∥ %s %s %s" T_RESET T_DIM "  %s", _("concurrent with the latest event of"),
                   c->names[i], stamp(o, o->L, o->vc), _("neither vector is ≤ the other: no message links the two"));
        } else {
            nl_log(c->n, NL_L_DIST, 2, 0, T_DIM "  → %s %s %s", less ? _("causally before the latest event of")
                   : _("causally after the latest event of"), c->names[i], stamp(o, o->L, o->vc));
        }
    }
}

static void history(clk *c, const char *fmt, ...)
{
    va_list ap;

    if (c->nhist == CLK_HIST) {
        memmove(c->hist[0], c->hist[1], sizeof(c->hist[0]) * (CLK_HIST - 1));
        c->nhist--;
    }
    va_start(ap, fmt);
    vsnprintf(c->hist[c->nhist++], sizeof(c->hist[0]), fmt, ap);
    va_end(ap);
}

static void publish(clk *c)
{
    nl_count_set(c->n, "clock.lamport", c->L);
    nl_count_set(c->n, "clock.delivered", c->delivered);
    nl_count_set(c->n, "clock.heldback", c->heldback);
    nl_count_set(c->n, "clock.violations", c->violations);
}

static void tick(clk *c)
{
    c->L++;
    c->vc[c->me]++;
    nl_count(c->n, "clock.events", 1);
}

static void wire_send(clk *c, int to, const cmsg *m)
{
    uint8_t b[12 + 8 * CLK_MAXP + sizeof(m->text)];
    int i, len = 12, tl = (int)strlen(m->text);

    b[0] = m->type;
    b[1] = m->from;
    b[2] = (uint8_t)c->np;
    b[3] = m->ref_from;
    nl_put32(b + 4, m->ts);
    nl_put32(b + 8, m->ref_ts);
    for (i = 0; i < c->np; i++, len += 4)
        nl_put32(b + len, m->vc[i]);
    for (i = 0; i < c->np; i++, len += 4)
        nl_put32(b + len, m->bv[i]);
    memcpy(b + len, m->text, (size_t)tl);
    nl_udp_send(c->n, CLK_PORT, c->ips[to], CLK_PORT, b, len + tl);
}

static void send_all(clk *c, const cmsg *m)
{
    int i;

    for (i = 0; i < c->np; i++)
        if (i != c->me)
            wire_send(c, i, m);
}

/* Hand a broadcast to the application */
static void deliver(clk *c, const cmsg *m)
{
    int k, missing = -1;

    for (k = 0; k < c->np; k++)
        if (k != m->from && k != c->me && m->bv[k] > c->D[k])
            missing = k;
    if (m->from != c->me && m->bv[m->from] > c->D[m->from] + 1)
        missing = m->from;
    if (m->bv[m->from] > c->D[m->from])
        c->D[m->from] = m->bv[m->from];
    c->delivered++;
    nl_log(c->n, NL_L_DIST, 1, 0, T_OK "✓ %s" T_RESET " «%s» %s %s  " T_DIM "%s", _("delivered"), m->text,
           _("from"), c->names[m->from], stamp(c, m->ts, m->vc));
    history(c, "✓ «%s» ← %s %s", m->text, c->names[m->from], stamp(c, m->ts, m->vc));
    if (missing >= 0) {
        c->violations++;
        nl_log(c->n, NL_L_DIST, 1, 0, T_ERR "✗ %s" T_RESET " «%s»: %s %s",
               _("causality violated"), m->text, _("it depends on a message not delivered yet, from"),
               c->names[missing]);
        history(c, "✗ %s", _("delivered before its cause"));
    }
    publish(c);
}

static int deliverable(const clk *c, const cmsg *m)
{
    int k;

    if (m->bv[m->from] != c->D[m->from] + 1)
        return 0;
    if (c->order == ORD_CAUSAL)
        for (k = 0; k < c->np; k++)
            if (k != m->from && k != c->me && m->bv[k] > c->D[k])
                return 0;
    return 1;
}

static void drain_hold(clk *c)
{
    int i, again = 1;

    while (again) {
        again = 0;
        for (i = 0; i < CLK_HOLD; i++)
            if (c->hold[i].used && deliverable(c, &c->hold[i])) {
                cmsg m = c->hold[i];
                c->hold[i].used = 0;
                nl_log(c->n, NL_L_DIST, 2, 0, T_INFO "clock" T_RESET " «%s» %s", m.text,
                       _("leaves the hold-back queue"));
                deliver(c, &m);
                again = 1;
            }
    }
}

/* Total order: find or create the queue entry of broadcast (ts, from) */
static cmsg *tq_entry(clk *c, uint32_t ts, int from)
{
    int i, pos;

    for (i = 0; i < c->ntq; i++)
        if (c->tq[i].ts == ts && c->tq[i].from == from)
            return &c->tq[i];
    if (c->ntq >= CLK_HOLD)
        return NULL;
    for (pos = 0; pos < c->ntq; pos++)
        if (c->tq[pos].ts > ts || (c->tq[pos].ts == ts && c->tq[pos].from > from))
            break;
    memmove(&c->tq[pos + 1], &c->tq[pos], (size_t)(c->ntq - pos) * sizeof(cmsg));
    c->ntq++;
    memset(&c->tq[pos], 0, sizeof(cmsg));
    c->tq[pos].used = 2;
    c->tq[pos].ts = ts;
    c->tq[pos].from = (uint8_t)from;
    return &c->tq[pos];
}

static void tq_drain(clk *c)
{
    unsigned all = (1u << c->np) - 1;

    while (c->ntq > 0 && c->tq[0].used == 1 && (c->tq[0].acks & all) == all) {
        cmsg m = c->tq[0];
        memmove(&c->tq[0], &c->tq[1], (size_t)(c->ntq - 1) * sizeof(cmsg));
        c->ntq--;
        deliver(c, &m);
    }
}

static void clk_rx(nl_node *n, void *ctx, const nl_udpmsg *um)
{
    clk *c = ctx;
    cmsg m;
    int i, len = 12, before = 0, after = 0;

    if (um->len < 12 || um->data[2] != c->np || um->len < 12 + 8 * c->np || um->data[1] >= c->np)
        return;
    memset(&m, 0, sizeof(m));
    m.used = 1;
    m.type = um->data[0];
    m.from = um->data[1];
    m.ref_from = um->data[3];
    m.ts = nl_get32(um->data + 4);
    m.ref_ts = nl_get32(um->data + 8);
    for (i = 0; i < c->np; i++, len += 4)
        m.vc[i] = nl_get32(um->data + len);
    for (i = 0; i < c->np; i++, len += 4)
        m.bv[i] = nl_get32(um->data + len);
    snprintf(m.text, sizeof(m.text), "%.*s", um->len - len, (const char *)um->data + len);

    /* receive rule: jump past the sender's clock */
    for (i = 0; i < c->np; i++) {
        if (m.vc[i] > c->vc[i]) after = 1;       /* the message knows something we do not */
        if (m.vc[i] < c->vc[i]) before = 1;      /* we know something its sender did not */
        if (m.vc[i] > c->vc[i])
            c->vc[i] = m.vc[i];
    }
    c->L = (m.ts > c->L ? m.ts : c->L) + 1;
    c->vc[c->me]++;
    nl_count(n, "clock.events", 1);

    if (m.type == M_ACK) {
        cmsg *e = tq_entry(c, m.ref_ts, m.ref_from);
        nl_log(n, NL_L_DIST, 3, um->pkt_id, T_INFO "clock" T_RESET " ack %s %s (%s %s L=%u)", _("from"),
               c->names[m.from], _("for"), c->names[m.ref_from], m.ref_ts);
        if (e)
            e->acks |= 1u << m.from;
        tq_drain(c);
        publish(c);
        return;
    }
    nl_log(n, NL_L_DIST, 2, um->pkt_id, T_INFO "clock" T_RESET " %s «%s» %s %s %s → %s %s%s%s",
           m.type == M_UNI ? _("received") : _("broadcast arrived"), m.text, _("from"), c->names[m.from],
           stamp(c, m.ts, m.vc), _("my clock"), stamp(c, c->L, c->vc),
           c->vector ? "  " : "",
           !c->vector ? "" : before && after ? _("(concurrent with what I had seen)")
           : _("(causally after everything I had seen)"));
    if (m.type == M_UNI) {
        history(c, "← «%s» %s %s → %s", m.text, c->names[m.from], stamp(c, m.ts, m.vc), stamp(c, c->L, c->vc));
        nl_log(n, NL_L_DIST, 1, um->pkt_id, T_OK "● %s" T_RESET " «%s» %s %s: %s %s", _("receive event"),
               m.text, _("from"), c->names[m.from], _("clock"), stamp(c, c->L, c->vc));
        nl_count(n, "clock.received", 1);
        relate(c);
        publish(c);
        return;
    }
    switch (c->order) {
    case ORD_NONE:
        deliver(c, &m);
        break;
    case ORD_FIFO:
    case ORD_CAUSAL:
        if (m.bv[m.from] <= c->D[m.from])
            break;                              /* duplicate */
        if (deliverable(c, &m)) {
            deliver(c, &m);
            drain_hold(c);
            break;
        }
        for (i = 0; i < CLK_HOLD; i++)
            if (!c->hold[i].used) {
                c->hold[i] = m;
                c->heldback++;
                nl_log(n, NL_L_DIST, 1, um->pkt_id, T_WARN "‖ %s" T_RESET " «%s» %s %s: %s", _("held back"),
                       m.text, _("from"), c->names[m.from],
                       c->order == ORD_FIFO ? _("an earlier message of the same sender is missing")
                                            : _("a message it depends on has not arrived yet"));
                history(c, "‖ «%s» ← %s", m.text, c->names[m.from]);
                break;
            }
        break;
    default: {
        cmsg *e = tq_entry(c, m.ts, m.from), ack;
        unsigned acks;

        if (!e)
            break;
        acks = e->acks | 1u << m.from | 1u << c->me;
        *e = m;
        e->acks = acks;
        memset(&ack, 0, sizeof(ack));
        ack.type = M_ACK;
        ack.from = (uint8_t)c->me;
        ack.ref_from = m.from;
        ack.ref_ts = m.ts;
        c->L++;
        ack.ts = c->L;
        memcpy(ack.vc, c->vc, sizeof(ack.vc));
        send_all(c, &ack);
        nl_log(n, NL_L_DIST, 2, um->pkt_id, T_WARN "‖ %s" T_RESET " «%s» L=%u.%d (%s)", _("queued"), m.text,
               m.ts, m.from + 1, _("waiting for every process to acknowledge"));
        tq_drain(c);
        break;
    }
    }
    publish(c);
}

static int clk_configure(clk *c, const nl_args *a)
{
    const char *mode = nl_arg_str(a, "mode", NULL), *order = nl_arg_str(a, "order", NULL);
    int i;

    if (mode)
        c->vector = strcmp(mode, "vector") == 0;
    if (order) {
        for (i = 0; i < 4; i++)
            if (strcmp(order, ORD_NAMES[i]) == 0)
                break;
        if (i == 4) {
            nl_log(c->n, NL_L_DIST, 1, 0, T_ERR "clock: %s '%s'", _("unknown order"), order);
            return -1;
        }
        if (c->order != i)
            nl_log(c->n, NL_L_DIST, 2, 0, T_INFO "clock" T_RESET " %s: %s", _("broadcast order"), order);
        c->order = i;
    }
    return 0;
}

static int clk_run(nl_node *n, const nl_args *a)
{
    clk *c = nl_mod_get(n, &clock_mod);
    const char *cmd = nl_arg_pos(a, 0), *text = nl_arg_str(a, "text", "m");
    cmsg m;

    if (!c) {
        char list[200];
        char *tok, *save = NULL;

        c = calloc(1, sizeof(*c));
        if (!c)
            return -1;
        c->n = n;
        c->me = -1;
        snprintf(list, sizeof(list), "%s", nl_arg_str(a, "peers", n->name));
        for (tok = strtok_r(list, ",", &save); tok && c->np < CLK_MAXP; tok = strtok_r(NULL, ",", &save)) {
            snprintf(c->names[c->np], sizeof(c->names[0]), "%s", tok);
            if (strcmp(tok, n->name) == 0)
                c->me = c->np;
            else
                c->ips[c->np] = peer_addr(n, tok);
            if (c->me != c->np && !c->ips[c->np]) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "clock: %s '%s'", _("unknown peer"), tok);
                free(c);
                return -1;
            }
            c->np++;
        }
        if (c->me < 0) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "clock: %s", _("this node must be in its own peers list"));
            free(c);
            return -1;
        }
        if (clk_configure(c, a) != 0) {
            free(c);
            return -1;
        }
        nl_mod_set(n, &clock_mod, c);
        nl_udp_bind(n, CLK_PORT, clk_rx, c);
        nl_log(n, NL_L_DIST, 2, 0, T_INFO "clock" T_RESET " %s %d/%d, %s, %s %s", _("process"), c->me + 1, c->np,
               c->vector ? _("vector clock") : _("Lamport clock"), _("broadcast order"), ORD_NAMES[c->order]);
        publish(c);
        if (!cmd)
            return 0;
    }
    if (!cmd)
        return clk_configure(c, a);
    memset(&m, 0, sizeof(m));
    m.used = 1;
    m.from = (uint8_t)c->me;
    snprintf(m.text, sizeof(m.text), "%s", text);
    if (strcmp(cmd, "event") == 0) {
        tick(c);
        nl_log(n, NL_L_DIST, 1, 0, T_OK "● %s" T_RESET ": %s %s", _("local event"), _("clock"),
               stamp(c, c->L, c->vc));
        history(c, "● %s %s", _("local event"), stamp(c, c->L, c->vc));
        relate(c);
    } else if (strcmp(cmd, "send") == 0) {
        const char *to = nl_arg_pos(a, 1);
        int i, dst = -1;

        for (i = 0; to && i < c->np; i++)
            if (strcmp(c->names[i], to) == 0 && i != c->me)
                dst = i;
        if (dst < 0) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "clock send: %s '%s'", _("unknown peer"), to ? to : "");
            return -1;
        }
        tick(c);
        m.type = M_UNI;
        m.ts = c->L;
        memcpy(m.vc, c->vc, sizeof(m.vc));
        nl_log(n, NL_L_DIST, 1, 0, T_OK "● %s" T_RESET " «%s» → %s: %s %s", _("send event"), m.text, to,
               _("clock"), stamp(c, c->L, c->vc));
        history(c, "→ «%s» %s %s", m.text, to, stamp(c, c->L, c->vc));
        relate(c);
        wire_send(c, dst, &m);
    } else if (strcmp(cmd, "bcast") == 0) {
        tick(c);
        m.type = M_BCAST;
        m.ts = c->L;
        memcpy(m.vc, c->vc, sizeof(m.vc));
        memcpy(m.bv, c->D, sizeof(m.bv));
        m.bv[c->me] = ++c->sentb;
        nl_log(n, NL_L_DIST, 1, 0, T_OK "● %s" T_RESET " «%s»: %s %s", _("broadcast"), m.text, _("clock"),
               stamp(c, c->L, c->vc));
        history(c, "⇉ «%s» %s", m.text, stamp(c, c->L, c->vc));
        if (c->order == ORD_TOTAL) {
            cmsg *e = tq_entry(c, m.ts, c->me);
            if (e) {
                unsigned acks = e->acks | 1u << c->me;
                *e = m;
                e->acks = acks;
            }
            send_all(c, &m);
            tq_drain(c);
        } else {
            send_all(c, &m);
            deliver(c, &m);
        }
    } else {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "clock: %s '%s'", _("unknown command"), cmd);
        return -1;
    }
    publish(c);
    return 0;
}

static void clk_stop(nl_node *n)
{
    clk *c = nl_mod_get(n, &clock_mod);

    if (!c)
        return;
    nl_udp_unbind(n, CLK_PORT);
    nl_mod_set(n, &clock_mod, NULL);
    free(c);
}

/* A crash loses the queues; the clocks restart from zero like a new process
 * would (a real system would persist them or use incarnation numbers). */
static void clk_crash(nl_node *n)
{
    clk *c = nl_mod_get(n, &clock_mod);

    if (!c)
        return;
    memset(c->hold, 0, sizeof(c->hold));
    c->ntq = 0;
}

static int clk_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    clk *c = nl_mod_get(n, &clock_mod);
    int row = 0, i, held = 0;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("process"), "%s (%d/%d)", n->name, c->me + 1, c->np);
    row += nl_ui_kv(x, y + row, w, _("Lamport clock"), "%u", c->L);
    if (c->vector)
        row += nl_ui_kv(x, y + row, w, _("vector clock"), "%s", stamp(c, c->L, c->vc));
    row += nl_ui_kv(x, y + row, w, _("broadcast order"), "%s", ORD_NAMES[c->order]);
    row += nl_ui_kv(x, y + row, w, _("delivered / held / violations"), "%u / %u / %u", c->delivered,
                    c->heldback, c->violations);
    for (i = 0; i < CLK_HOLD; i++)
        if (c->hold[i].used) {
            if (!held++ && row < h)
                tui_print(x, y + row++, w, TS(C_WARN, C_DEFAULT, TA_BOLD), "%s", _("hold-back queue"));
            if (row < h)
                tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " ‖ «%s» ← %s", c->hold[i].text,
                          c->names[c->hold[i].from]);
        }
    for (i = 0; i < c->ntq && row < h; i++) {
        if (i == 0)
            tui_print(x, y + row++, w, TS(C_WARN, C_DEFAULT, TA_BOLD), "%s", _("total order queue"));
        if (row < h)
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " L=%u.%d «%s» acks %x", c->tq[i].ts,
                      c->tq[i].from + 1, c->tq[i].text, c->tq[i].acks);
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, TA_BOLD), "%s", _("last events"));
    for (i = 0; i < c->nhist && row < h; i++)
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " %s", c->hist[i]);
    return row;
}

static void dis_clock(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int np, i, p = 12;
    char vc[80] = "";
    size_t vl = 0;

    if (len < 12)
        return;
    np = d[2];
    nl_dis_field(o, "Clock", "Type", off, 1, "%d (%s)", d[0],
                 d[0] == M_UNI ? "unicast" : d[0] == M_BCAST ? "broadcast" : "ack");
    nl_dis_field(o, "Clock", "Sender id", off + 1, 1, "%d", d[1] + 1);
    nl_dis_field(o, "Clock", "Lamport timestamp", off + 4, 4, "%u", nl_get32(d + 4));
    if (d[0] == M_ACK)
        nl_dis_field(o, "Clock", "Acknowledges", off + 8, 4, "L=%u of process %d", nl_get32(d + 8), d[3] + 1);
    if (len < 12 + 8 * np || np > CLK_MAXP)
        return;
    for (i = 0; i < np; i++)
        vl += (size_t)snprintf(vc + vl, sizeof(vc) - vl, "%s%u", i ? "," : "", nl_get32(d + p + 4 * i));
    nl_dis_field(o, "Clock", "Vector clock", off + p, 4 * np, "[%s]", vc);
    p += 4 * np;
    vl = 0;
    for (i = 0; i < np; i++)
        vl += (size_t)snprintf(vc + vl, sizeof(vc) - vl, "%s%u", i ? "," : "", nl_get32(d + p + 4 * i));
    nl_dis_field(o, "Clock", "Broadcast dependencies", off + p, 4 * np, "[%s]", vc);
    p += 4 * np;
    if (len > p)
        nl_dis_field(o, "Clock", "Text", off + p, len - p, "%.*s", len - p, (const char *)d + p);
    nl_dis_summary(o, "CLOCK", 209, "%s #%d L=%u «%.*s»",
                   d[0] == M_UNI ? "msg" : d[0] == M_BCAST ? "bcast" : "ack", d[1] + 1, nl_get32(d + 4),
                   len - p, (const char *)d + p);
}

static const nl_module clock_mod = {
    .name = "clock", .title = N_("Logical clocks and ordered broadcast (Lamport, vector, causal, total)"),
    .layer = NL_L_DIST,
    .usage = N_("run NODE clock peers=n1,n2,n3 [mode=lamport|vector] [order=none|fifo|causal|total]\n"
                "run NODE clock event                 a local event\n"
                "run NODE clock send PEER [text=T]    send a message to one peer\n"
                "run NODE clock bcast [text=T]        broadcast to every peer\n"
                "run NODE clock order=causal          change the broadcast order at run time\n"
                "  Every node must be started with the same peers list (it defines the process ids)."),
    .theory = N_("A distributed system has no shared clock: each machine only sees its own events and "
                 "the messages it receives. What can still be defined is causality: event a happened "
                 "before event b if they are on the same process in that order, if a is the sending "
                 "of a message and b its reception, or by transitivity. Events not related this way "
                 "are concurrent: no observer can tell which came first.\n\n"
                 "A Lamport clock is one counter per process: increment it at every event, stamp every "
                 "message with it, and on reception jump to max(local, received) + 1. If a happened "
                 "before b then L(a) < L(b). The converse does not hold: a smaller timestamp does not "
                 "prove causality. Breaking ties with the process id gives a total order consistent "
                 "with causality, which is enough to make every process agree on one sequence.\n\n"
                 "A vector clock keeps one counter per process: V[i] counts the events of process i "
                 "known so far. Messages carry the whole vector and the receiver takes the element-wise "
                 "maximum. Now a happened before b exactly when V(a) < V(b) component-wise, and two "
                 "events are concurrent exactly when neither vector dominates the other. The price is "
                 "a timestamp that grows with the number of processes.\n\n"
                 "Ordered broadcast builds on these tools. FIFO broadcast delivers the messages of each "
                 "sender in sending order. Causal broadcast never delivers a message before the ones "
                 "its sender had delivered when it sent it: each broadcast carries a dependency vector, "
                 "and a receiver that lacks a dependency parks the message in a hold-back queue. Total "
                 "order broadcast makes all processes deliver all messages in the same order; here it "
                 "is Lamport's construction: every broadcast is queued by (timestamp, process id), "
                 "every receiver acknowledges it to everybody, and the head of the queue is delivered "
                 "once all processes have acknowledged it. It relies on FIFO channels and on every "
                 "process being alive: one crash blocks delivery, which is why practical systems reach "
                 "total order through consensus instead."),
    .run = clk_run, .stop = clk_stop, .crash = clk_crash, .panel = clk_panel,
};
NL_MODULE(clock_mod)

NL_INIT(clock_dissector)
{
    nl_dissector_udp(CLK_PORT, dis_clock);
}
