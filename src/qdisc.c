/*
 * qdisc.c - Queue disciplines for links: active queue management (RED,
 *           CoDel, PIE), scheduling (DRR fair queueing, strict priority),
 *           shaping (token bucket) and an instrumented FIFO.
 *
 * A discipline is selected per link with `link A B qdisc=NAME key=value...`
 * and exists once per direction. The core keeps one FIFO ring per direction
 * and calls enqueue() before a packet enters it and dequeue() when the
 * transmitter is free. AQM disciplines work directly on that ring. The
 * classful ones (fq, prio) need several queues: they move the packets out of
 * the ring into per-class lists kept inside their own state (one malloc'd
 * block: the core releases it with free()), so for them the ring is only a
 * doorstep and the `queue=N` capacity of the link is enforced here.
 *
 * The documentation-only module `qdisc` explains the subject in the study
 * guide and shows the state of the disciplines of a node in its panel.
 */
#include "nl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared helpers                                                     */
/* ------------------------------------------------------------------ */

/* Wake-up timers for disciplines that hold packets back (tbf). They live
 * outside the discipline state, which the core may free at any time. */
static struct kick { nl_link *l; int dir; nl_ev ev; } g_kick[NL_MAX_LINKS][2];

static void kick_fire(void *ctx)
{
    struct kick *k = ctx;

    k->ev = 0;
    nl_link_kick(k->l, k->dir);
}

static void kick_after(nl_link *l, int dir, nl_time delay)
{
    struct kick *k = &g_kick[l->idx][dir];

    if (k->ev)
        return;                         /* the earlier wake-up is still right */
    k->l = l;
    k->dir = dir;
    k->ev = nl_after(NULL, delay < 1 ? 1 : delay, kick_fire, k);
}

static nl_node *tx_node(const nl_link *l, int dir) { return l->n[dir]; }
static const char *tx_if(const nl_link *l, int dir) { return l->n[dir]->ifc[l->ifx[dir]].name; }

/* At most one sample every 50 ms of virtual time */
static int sample_due(nl_time *last)
{
    if (*last && nl_now() - *last < NL_MS(50))
        return 0;
    *last = nl_now();
    return 1;
}

static void sample(nl_link *l, int dir, const char *what, double v)
{
    char name[24];

    snprintf(name, sizeof(name), "%s %s", tx_if(l, dir), what);
    nl_sample(tx_node(l, dir), name, v);
}

/* Time to transmit one packet of n bytes on the link, in microseconds */
static double pkt_time(const nl_link *l, int n)
{
    return l->cfg.bw > 0 ? n * 8.0 * 1e6 / l->cfg.bw : 100.0;
}

/* ---- packet lists for the classful disciplines ---- */

typedef struct { nl_pkt *p; nl_time t_in; int next; } qslot;
typedef struct { int head, tail, n, bytes; } qlist;
typedef struct { int cap, free, total, bytes; qslot *slot; } qpool;

static void pool_init(qpool *pl, qslot *mem, int cap)
{
    int i;

    pl->cap = cap;
    pl->slot = mem;
    pl->total = pl->bytes = 0;
    pl->free = 0;
    for (i = 0; i < cap; i++)
        mem[i].next = i + 1 < cap ? i + 1 : -1;
}

static void list_init(qlist *q) { q->head = q->tail = -1; q->n = q->bytes = 0; }

static int list_push(qpool *pl, qlist *q, nl_pkt *p, nl_time t_in)
{
    int i = pl->free;

    if (i < 0)
        return -1;
    pl->free = pl->slot[i].next;
    pl->slot[i].p = p;
    pl->slot[i].t_in = t_in;
    pl->slot[i].next = -1;
    if (q->tail >= 0)
        pl->slot[q->tail].next = i;
    else
        q->head = i;
    q->tail = i;
    q->n++;
    q->bytes += p->len;
    pl->total++;
    pl->bytes += p->len;
    return 0;
}

static nl_pkt *list_pop(qpool *pl, qlist *q, nl_time *t_in)
{
    int i = q->head;
    nl_pkt *p;

    if (i < 0)
        return NULL;
    p = pl->slot[i].p;
    if (t_in)
        *t_in = pl->slot[i].t_in;
    q->head = pl->slot[i].next;
    if (q->head < 0)
        q->tail = -1;
    q->n--;
    q->bytes -= p->len;
    pl->total--;
    pl->bytes -= p->len;
    pl->slot[i].next = pl->free;
    pl->free = i;
    return p;
}

static int head_len(const qpool *pl, const qlist *q)
{
    return q->head >= 0 ? pl->slot[q->head].p->len : 0;
}

/* ------------------------------------------------------------------ */
/* fifo: drop-tail with measurements                                  */
/* ------------------------------------------------------------------ */

typedef struct { nl_time last; double sojourn_ms; } fifo_st;

static void *fifo_create(nl_link *l, int dir, const nl_args *a)
{
    (void)l; (void)dir; (void)a;
    return calloc(1, sizeof(fifo_st));
}

static nl_pkt *fifo_dequeue(void *st, nl_link *l, int dir)
{
    fifo_st *f = st;
    nl_time soj = 0;
    nl_pkt *p = nl_q_pop(l, dir, &soj);

    if (p && f) {
        f->sojourn_ms = soj / 1000.0;
        if (sample_due(&f->last)) {
            sample(l, dir, "queue", l->d[dir].qlen + 1);
            sample(l, dir, "wait (ms)", f->sojourn_ms);
        }
    }
    return p;
}

static int fifo_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    fifo_st *f = st;
    int row = 0;

    if (h < 2)
        return 0;
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("queue"));
    tui_bar(x + 14, y + row, w - 26 > 4 ? w - 26 : 4, (double)l->d[dir].qlen / l->cfg.qcap, C_WARN);
    tui_print(x + w - 11, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%d/%d", l->d[dir].qlen, l->cfg.qcap);
    row += nl_ui_kv(x, y + row, w, _("last wait"), "%.1f ms", f ? f->sojourn_ms : 0.0);
    return row;
}

/* ------------------------------------------------------------------ */
/* RED - random early detection                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    double  minth, maxth, maxp, wq, avg, p;
    int     gentle, ecn, count;
    nl_time idle_since, last;
    unsigned early, forced, marks;
} red_st;

static void *red_create(nl_link *l, int dir, const nl_args *a)
{
    red_st *r = calloc(1, sizeof(*r));

    (void)dir;
    if (!r)
        return NULL;
    r->minth = nl_arg_dbl(a, "min", l->cfg.qcap / 6.0 < 2 ? 2 : l->cfg.qcap / 6.0);
    r->maxth = nl_arg_dbl(a, "max", r->minth * 3);
    if (r->maxth <= r->minth)
        r->maxth = r->minth + 1;
    r->maxp = nl_arg_dbl(a, "maxp", 0.1);
    r->wq = nl_arg_dbl(a, "wq", 0.02);
    r->gentle = (int)nl_arg_int(a, "gentle", 1);
    r->ecn = (int)nl_arg_int(a, "ecn", 0);
    r->count = -1;
    r->idle_since = nl_now();
    return r;
}

static int red_enqueue(void *st, nl_link *l, int dir, nl_pkt *p)
{
    red_st *r = st;
    nl_linkdir *q = &l->d[dir];
    int qlen = q->qlen + (q->sending ? 1 : 0);
    double pb, pa;

    if (!r)
        return NL_Q_ACCEPT;
    /* average queue: EWMA per arrival; while the queue was idle it decays
     * as if empty slots had been observed */
    if (qlen == 0 && r->idle_since) {
        double m = (double)(nl_now() - r->idle_since) / pkt_time(l, 500);
        r->avg *= pow(1.0 - r->wq, m);
        r->idle_since = 0;
    } else {
        r->avg += r->wq * (qlen - r->avg);
    }
    if (sample_due(&r->last)) {
        sample(l, dir, "queue", qlen);
        sample(l, dir, "avg queue", r->avg);
    }
    if (r->avg < r->minth) {
        r->count = -1;
        r->p = 0;
        return NL_Q_ACCEPT;
    }
    if (r->avg < r->maxth) {
        pb = r->maxp * (r->avg - r->minth) / (r->maxth - r->minth);
    } else if (r->gentle && r->avg < 2 * r->maxth) {
        pb = r->maxp + (1.0 - r->maxp) * (r->avg - r->maxth) / r->maxth;
    } else {
        r->p = 1;
        r->count = 0;
        r->forced++;
        nl_count(tx_node(l, dir), "red.forced", 1);
        nl_count(tx_node(l, dir), "red.drops", 1);
        return NL_Q_DROP;
    }
    r->p = pb;
    r->count++;
    /* spread the drops evenly instead of in clumps */
    pa = r->count * pb < 1.0 ? pb / (1.0 - r->count * pb) : 1.0;
    if (nl_rand01() >= pa)
        return NL_Q_ACCEPT;
    r->count = 0;
    if (r->ecn && r->avg < r->maxth && nl_ecn_mark(p)) {
        r->marks++;
        q->marked++;
        nl_count(tx_node(l, dir), "red.marks", 1);
        nl_log(tx_node(l, dir), NL_L_NET, 3, p->id, T_WARN "RED" T_RESET " %s (avg %.1f, p %.3f)",
               _("ECN mark instead of a drop"), r->avg, pb);
        return NL_Q_ACCEPT;
    }
    r->early++;
    nl_count(tx_node(l, dir), "red.early", 1);
    nl_count(tx_node(l, dir), "red.drops", 1);
    return NL_Q_DROP;
}

static nl_pkt *red_dequeue(void *st, nl_link *l, int dir)
{
    red_st *r = st;
    nl_pkt *p = nl_q_pop(l, dir, NULL);

    if (r && l->d[dir].qlen == 0)
        r->idle_since = nl_now();
    return p;
}

static int red_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    red_st *r = st;
    int row = 0, bw = w - 26 > 4 ? w - 26 : 4;

    if (!r || h < 4)
        return 0;
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("queue"));
    tui_bar(x + 14, y + row, bw, (double)l->d[dir].qlen / l->cfg.qcap, C_WARN);
    tui_print(x + 14 + bw + 1, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%d/%d", l->d[dir].qlen, l->cfg.qcap);
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("average"));
    tui_bar(x + 14, y + row, bw, r->avg / l->cfg.qcap, r->avg < r->minth ? C_OK : r->avg < r->maxth ? C_WARN : C_ERR);
    tui_print(x + 14 + bw + 1, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%.1f", r->avg);
    row += nl_ui_kv(x, y + row, w, _("thresholds"), "min %.0f  max %.0f  maxp %.2f  wq %.3f%s", r->minth, r->maxth,
                    r->maxp, r->wq, r->gentle ? "  gentle" : "");
    row += nl_ui_kv(x, y + row, w, _("drop probability"), "%.3f", r->p);
    if (row < h)
        row += nl_ui_kv(x, y + row, w, _("early / forced / marked"), "%u / %u / %u", r->early, r->forced, r->marks);
    return row;
}

/* ------------------------------------------------------------------ */
/* CoDel - controlled delay (RFC 8289)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_time target, interval, first_above, drop_next, last;
    int     dropping, ecn;
    unsigned count, lastcount, drops, marks;
    double  sojourn_ms;
} codel_st;

static void *codel_create(nl_link *l, int dir, const nl_args *a)
{
    codel_st *c = calloc(1, sizeof(*c));

    (void)l; (void)dir;
    if (!c)
        return NULL;
    c->target = nl_arg_time(a, "target", NL_MS(5));
    c->interval = nl_arg_time(a, "interval", NL_MS(100));
    c->ecn = (int)nl_arg_int(a, "ecn", 0);
    return c;
}

static nl_time codel_law(const codel_st *c, nl_time t)
{
    return t + (nl_time)((double)c->interval / sqrt((double)(c->count ? c->count : 1)));
}

/* Take the head packet and tell whether the queue has been "bad" (sojourn
 * above target) for at least one interval */
static nl_pkt *codel_take(codel_st *c, nl_link *l, int dir, int *ok_to_drop)
{
    nl_time soj = 0, now = nl_now();
    nl_pkt *p = nl_q_pop(l, dir, &soj);

    *ok_to_drop = 0;
    if (!p) {
        c->first_above = 0;
        return NULL;
    }
    c->sojourn_ms = soj / 1000.0;
    if (soj < c->target || l->d[dir].qbytes <= l->cfg.mtu) {
        c->first_above = 0;
    } else if (!c->first_above) {
        c->first_above = now + c->interval;
    } else if (now >= c->first_above) {
        *ok_to_drop = 1;
    }
    return p;
}

/* Signal congestion on p: mark it (returns 1, deliver it) or drop it */
static int codel_signal(codel_st *c, nl_link *l, int dir, nl_pkt *p)
{
    if (c->ecn && nl_ecn_mark(p)) {
        c->marks++;
        l->d[dir].marked++;
        nl_count(tx_node(l, dir), "codel.marks", 1);
        return 1;
    }
    c->drops++;
    nl_count(tx_node(l, dir), "codel.drops", 1);
    nl_q_drop(l, dir, p, "codel");
    return 0;
}

static nl_pkt *codel_dequeue(void *st, nl_link *l, int dir)
{
    codel_st *c = st;
    nl_time now = nl_now();
    int ok;
    nl_pkt *p;

    if (!c)
        return nl_q_pop(l, dir, NULL);
    p = codel_take(c, l, dir, &ok);
    if (c->dropping) {
        if (!ok) {
            c->dropping = 0;
        } else {
            while (p && c->dropping && now >= c->drop_next) {
                c->count++;
                if (codel_signal(c, l, dir, p)) {
                    c->drop_next = codel_law(c, c->drop_next);
                    break;
                }
                p = codel_take(c, l, dir, &ok);
                if (!ok)
                    c->dropping = 0;
                else
                    c->drop_next = codel_law(c, c->drop_next);
            }
        }
    } else if (ok && p) {
        unsigned delta = c->count - c->lastcount;
        int kept = codel_signal(c, l, dir, p);

        if (!kept)
            p = codel_take(c, l, dir, &ok);
        c->dropping = 1;
        /* resume near the old rate when the last dropping phase was recent */
        c->count = delta > 1 && now - c->drop_next < 16 * c->interval ? delta : 1;
        c->drop_next = codel_law(c, now);
        c->lastcount = c->count;
        nl_log(tx_node(l, dir), NL_L_NET, 2, 0, T_WARN "CoDel" T_RESET " %s → %s: %s (%.1f ms > %s)",
               tx_node(l, dir)->name, l->n[!dir]->name, _("standing queue: start dropping"), c->sojourn_ms,
               nl_time_str(c->target));
    }
    if (sample_due(&c->last)) {
        sample(l, dir, "queue", l->d[dir].qlen + (p ? 1 : 0));
        sample(l, dir, "wait (ms)", c->sojourn_ms);
    }
    return p;
}

static int codel_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    codel_st *c = st;
    int row = 0, bw = w - 26 > 4 ? w - 26 : 4;

    if (!c || h < 4)
        return 0;
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("queue"));
    tui_bar(x + 14, y + row, bw, (double)l->d[dir].qlen / l->cfg.qcap, C_WARN);
    tui_print(x + 14 + bw + 1, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%d/%d", l->d[dir].qlen, l->cfg.qcap);
    row += nl_ui_kv(x, y + row, w, _("sojourn time"), "%s%.1f ms" T_RESET "  (target %s, interval %s)",
                    c->sojourn_ms * 1000 > (double)c->target ? T_WARN : T_OK, c->sojourn_ms,
                    nl_time_str(c->target), nl_time_str(c->interval));
    row += nl_ui_kv(x, y + row, w, _("state"), "%s  count %u", c->dropping ? _("dropping") : _("idle"), c->count);
    row += nl_ui_kv(x, y + row, w, _("dropped / marked"), "%u / %u", c->drops, c->marks);
    return row;
}

/* ------------------------------------------------------------------ */
/* PIE - proportional integral controller enhanced (RFC 8033, basic)  */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_time target, tupdate, last_update, last;
    double  p, qdelay, qdelay_old;      /* delays in seconds */
    int     ecn;
    unsigned drops, marks;
} pie_st;

static void *pie_create(nl_link *l, int dir, const nl_args *a)
{
    pie_st *s = calloc(1, sizeof(*s));

    (void)l; (void)dir;
    if (!s)
        return NULL;
    s->target = nl_arg_time(a, "target", NL_MS(15));
    s->tupdate = nl_arg_time(a, "tupdate", NL_MS(15));
    s->ecn = (int)nl_arg_int(a, "ecn", 0);
    return s;
}

static void pie_update(pie_st *s, nl_link *l, int dir)
{
    double target = s->target / 1e6, alpha = 0.125, beta = 1.25, dp;

    if (nl_now() - s->last_update < s->tupdate)
        return;
    s->last_update = nl_now();
    /* queueing delay estimated from the backlog and the link rate */
    s->qdelay = pkt_time(l, l->d[dir].qbytes) / 1e6;
    dp = alpha * (s->qdelay - target) + beta * (s->qdelay - s->qdelay_old);
    if (s->p < 0.000001)     dp /= 2048;
    else if (s->p < 0.00001) dp /= 512;
    else if (s->p < 0.0001)  dp /= 128;
    else if (s->p < 0.001)   dp /= 32;
    else if (s->p < 0.01)    dp /= 8;
    else if (s->p < 0.1)     dp /= 2;
    s->p += dp;
    if (s->qdelay == 0 && s->qdelay_old == 0)
        s->p *= 0.98;
    if (s->p < 0) s->p = 0;
    if (s->p > 1) s->p = 1;
    s->qdelay_old = s->qdelay;
    if (sample_due(&s->last)) {
        sample(l, dir, "queue", l->d[dir].qlen);
        sample(l, dir, "drop prob", s->p);
    }
}

static int pie_enqueue(void *st, nl_link *l, int dir, nl_pkt *p)
{
    pie_st *s = st;

    if (!s)
        return NL_Q_ACCEPT;
    pie_update(s, l, dir);
    if (s->p <= 0 || l->d[dir].qlen <= 2 || (s->qdelay_old < s->target / 2e6 && s->p < 0.2))
        return NL_Q_ACCEPT;
    if (nl_rand01() >= s->p)
        return NL_Q_ACCEPT;
    if (s->ecn && s->p < 0.1 && nl_ecn_mark(p)) {
        s->marks++;
        l->d[dir].marked++;
        nl_count(tx_node(l, dir), "pie.marks", 1);
        return NL_Q_ACCEPT;
    }
    s->drops++;
    nl_count(tx_node(l, dir), "pie.drops", 1);
    return NL_Q_DROP;
}

static nl_pkt *pie_dequeue(void *st, nl_link *l, int dir)
{
    if (st)
        pie_update(st, l, dir);
    return nl_q_pop(l, dir, NULL);
}

static int pie_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    pie_st *s = st;
    int row = 0;

    if (!s || h < 3)
        return 0;
    row += nl_ui_kv(x, y + row, w, _("queue"), "%d/%d", l->d[dir].qlen, l->cfg.qcap);
    row += nl_ui_kv(x, y + row, w, _("estimated delay"), "%.1f ms  (target %s)", s->qdelay * 1000,
                    nl_time_str(s->target));
    row += nl_ui_kv(x, y + row, w, _("drop probability"), "%.4f", s->p);
    if (row < h)
        row += nl_ui_kv(x, y + row, w, _("dropped / marked"), "%u / %u", s->drops, s->marks);
    return row;
}

/* ------------------------------------------------------------------ */
/* fq - fair queueing with deficit round robin                        */
/* ------------------------------------------------------------------ */

#define FQ_FLOWS 32

typedef struct {
    qlist    q;
    int      deficit;
    nl_ip    src, dst;
    uint16_t sport, dport;
    uint8_t  proto, used;
    unsigned sent, drops;
} fq_flow;

typedef struct {
    qpool    pool;
    int      limit, quantum, cur, topped;
    nl_time  last;
    unsigned drops;
    fq_flow  flow[FQ_FLOWS];
    qslot    mem[];
} fq_st;

static void *fq_create(nl_link *l, int dir, const nl_args *a)
{
    int cap = l->cfg.qcap + 4, i;
    fq_st *f = calloc(1, sizeof(*f) + (size_t)cap * sizeof(qslot));

    (void)dir;
    if (!f)
        return NULL;
    pool_init(&f->pool, f->mem, cap);
    f->limit = l->cfg.qcap;
    f->quantum = (int)nl_arg_int(a, "quantum", 1514);
    if (f->quantum < 64)
        f->quantum = 64;
    for (i = 0; i < FQ_FLOWS; i++)
        list_init(&f->flow[i].q);
    return f;
}

static fq_flow *fq_classify(fq_st *f, const nl_pkt *p)
{
    nl_ipview v;
    uint32_t h;
    fq_flow *fl;

    if (nl_ipv_parse(p, &v)) {
        h = v.src * 2654435761u ^ v.dst * 2246822519u ^ ((uint32_t)v.sport << 16 | v.dport) * 3266489917u ^ v.proto;
        h ^= h >> 16;                   /* murmur3 finaliser: every input bit reaches the low bits */
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
    } else {
        memset(&v, 0, sizeof(v));
        h = 0;                          /* ARP and other non-IP frames share flow 0 */
    }
    fl = &f->flow[h % FQ_FLOWS];
    if (!fl->q.n) {
        fl->src = v.src;
        fl->dst = v.dst;
        fl->sport = v.sport;
        fl->dport = v.dport;
        fl->proto = v.proto;
        fl->used = 1;
    }
    return fl;
}

/* Move what the core queued into the per-flow lists */
static void fq_absorb(fq_st *f, nl_link *l, int dir)
{
    nl_time soj;
    nl_pkt *p;

    while ((p = nl_q_pop(l, dir, &soj)) != NULL)
        if (list_push(&f->pool, &fq_classify(f, p)->q, p, nl_now() - soj) != 0)
            nl_q_drop(l, dir, p, "fq");
}

static int fq_enqueue(void *st, nl_link *l, int dir, nl_pkt *p)
{
    fq_st *f = st;
    fq_flow *fat = NULL;
    int i;

    (void)p;
    if (!f)
        return NL_Q_ACCEPT;
    fq_absorb(f, l, dir);
    if (f->pool.total + 1 <= f->limit)
        return NL_Q_ACCEPT;
    /* full: the flow with the largest backlog pays, from its head */
    for (i = 0; i < FQ_FLOWS; i++)
        if (f->flow[i].q.n && (!fat || f->flow[i].q.bytes > fat->q.bytes))
            fat = &f->flow[i];
    f->drops++;
    nl_count(tx_node(l, dir), "fq.drops", 1);
    if (!fat)
        return NL_Q_DROP;
    fat->drops++;
    nl_q_drop(l, dir, list_pop(&f->pool, &fat->q, NULL), "fq");
    return NL_Q_ACCEPT;
}

static nl_pkt *fq_dequeue(void *st, nl_link *l, int dir)
{
    fq_st *f = st;
    int guard;

    if (!f)
        return nl_q_pop(l, dir, NULL);
    fq_absorb(f, l, dir);
    if (sample_due(&f->last))
        sample(l, dir, "queue", f->pool.total);
    if (!f->pool.total)
        return NULL;
    /* deficit round robin: a flow gets one quantum per visit and sends
     * while its deficit covers the packet at its head */
    for (guard = 0; guard < 64 * FQ_FLOWS; guard++) {
        fq_flow *fl = &f->flow[f->cur];

        if (fl->q.n) {
            if (!f->topped) {
                fl->deficit += f->quantum;
                f->topped = 1;
            }
            if (fl->deficit >= head_len(&f->pool, &fl->q)) {
                nl_pkt *p = list_pop(&f->pool, &fl->q, NULL);
                fl->deficit -= p->len;
                fl->sent++;
                return p;
            }
        } else {
            fl->deficit = 0;            /* an idle flow does not save credit */
        }
        f->cur = (f->cur + 1) % FQ_FLOWS;
        f->topped = 0;
    }
    return NULL;
}

static int fq_backlog(void *st, nl_link *l, int dir)
{
    fq_st *f = st;

    (void)l; (void)dir;
    return f ? f->pool.total : 0;
}

static void fq_flush(void *st, nl_link *l, int dir)
{
    fq_st *f = st;
    nl_pkt *p;
    int i;

    (void)l; (void)dir;
    for (i = 0; f && i < FQ_FLOWS; i++) {
        while ((p = list_pop(&f->pool, &f->flow[i].q, NULL)) != NULL)
            nl_pkt_free(p);
        f->flow[i].deficit = 0;
    }
}

static int fq_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    fq_st *f = st;
    int row = 0, i;

    (void)l; (void)dir;
    if (!f || h < 2)
        return 0;
    row += nl_ui_kv(x, y + row, w, _("backlog"), "%d/%d %s  quantum %d B  %s %u", f->pool.total, f->limit,
                    _("packets"), f->quantum, _("drops"), f->drops);
    for (i = 0; i < FQ_FLOWS && row < h; i++) {
        const fq_flow *fl = &f->flow[i];
        if (!fl->used || (!fl->q.n && !fl->sent))
            continue;
        if (fl->proto)
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0),
                      T_INFO "%s:%u" T_RESET " → " T_INFO "%s:%u" T_RESET " p%d  " T_DIM "%s" T_RESET " %d  "
                      T_DIM "%s" T_RESET " %u  " T_DIM "%s" T_RESET " %u",
                      nl_ip_str(fl->src), fl->sport, nl_ip_str(fl->dst), fl->dport, fl->proto, _("queued"),
                      fl->q.n, _("sent"), fl->sent, _("dropped"), fl->drops);
        else
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s  %s %d  %s %u", _("non-IP frames"),
                      _("queued"), fl->q.n, _("sent"), fl->sent);
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* prio - strict priority bands                                       */
/* ------------------------------------------------------------------ */

#define PRIO_BANDS 3

typedef struct {
    qpool    pool;
    int      limit;
    nl_time  last;
    qlist    band[PRIO_BANDS];
    unsigned sent[PRIO_BANDS], drops[PRIO_BANDS];
    qslot    mem[];
} prio_st;

static void *prio_create(nl_link *l, int dir, const nl_args *a)
{
    int cap = l->cfg.qcap + 4, i;
    prio_st *s = calloc(1, sizeof(*s) + (size_t)cap * sizeof(qslot));

    (void)dir; (void)a;
    if (!s)
        return NULL;
    pool_init(&s->pool, s->mem, cap);
    s->limit = l->cfg.qcap;
    for (i = 0; i < PRIO_BANDS; i++)
        list_init(&s->band[i]);
    return s;
}

/* Band from the DSCP: 0 = expedited (CS5, EF and above; also non-IP control
 * frames such as ARP), 1 = assured (any other non-zero DSCP), 2 = best effort */
static int prio_band(const nl_pkt *p)
{
    nl_ipview v;
    int dscp;

    if (!nl_ipv_parse(p, &v))
        return 0;
    dscp = v.tos >> 2;
    return dscp >= 40 ? 0 : dscp >= 8 ? 1 : 2;
}

static void prio_absorb(prio_st *s, nl_link *l, int dir)
{
    nl_time soj;
    nl_pkt *p;

    while ((p = nl_q_pop(l, dir, &soj)) != NULL)
        if (list_push(&s->pool, &s->band[prio_band(p)], p, nl_now() - soj) != 0)
            nl_q_drop(l, dir, p, "prio");
}

static int prio_enqueue(void *st, nl_link *l, int dir, nl_pkt *p)
{
    prio_st *s = st;
    int b = prio_band(p), i;

    if (!s)
        return NL_Q_ACCEPT;
    prio_absorb(s, l, dir);
    if (s->pool.total + 1 <= s->limit)
        return NL_Q_ACCEPT;
    /* full: make room at the expense of a lower band, if there is one */
    for (i = PRIO_BANDS - 1; i > b; i--)
        if (s->band[i].n) {
            s->drops[i]++;
            nl_count(tx_node(l, dir), "prio.drops", 1);
            nl_q_drop(l, dir, list_pop(&s->pool, &s->band[i], NULL), "prio");
            return NL_Q_ACCEPT;
        }
    s->drops[b]++;
    nl_count(tx_node(l, dir), "prio.drops", 1);
    return NL_Q_DROP;
}

static nl_pkt *prio_dequeue(void *st, nl_link *l, int dir)
{
    prio_st *s = st;
    int i;

    if (!s)
        return nl_q_pop(l, dir, NULL);
    prio_absorb(s, l, dir);
    if (sample_due(&s->last))
        sample(l, dir, "queue", s->pool.total);
    for (i = 0; i < PRIO_BANDS; i++)
        if (s->band[i].n) {
            s->sent[i]++;
            return list_pop(&s->pool, &s->band[i], NULL);
        }
    return NULL;
}

static int prio_backlog(void *st, nl_link *l, int dir)
{
    prio_st *s = st;

    (void)l; (void)dir;
    return s ? s->pool.total : 0;
}

static void prio_flush(void *st, nl_link *l, int dir)
{
    prio_st *s = st;
    nl_pkt *p;
    int i;

    (void)l; (void)dir;
    for (i = 0; s && i < PRIO_BANDS; i++)
        while ((p = list_pop(&s->pool, &s->band[i], NULL)) != NULL)
            nl_pkt_free(p);
}

static int prio_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    static const char *const names[PRIO_BANDS] = { N_("expedited"), N_("assured"), N_("best effort") };
    prio_st *s = st;
    int row = 0, i, bw = w - 44 > 4 ? w - 44 : 4;

    (void)l; (void)dir;
    if (!s)
        return 0;
    for (i = 0; i < PRIO_BANDS && row < h; i++) {
        tui_print(x, y + row, 16, TS(C_DIM, C_DEFAULT, 0), "%d %s", i, _(names[i]));
        tui_bar(x + 16, y + row, bw, (double)s->band[i].n / s->limit, i == 0 ? C_OK : i == 1 ? C_WARN : C_DATA);
        tui_print(x + 16 + bw + 1, y + row++, 27, TS(C_TEXT, C_DEFAULT, 0),
                  "%3d  " T_DIM "%s" T_RESET " %u  " T_DIM "%s" T_RESET " %u", s->band[i].n, _("sent"), s->sent[i],
                  _("dropped"), s->drops[i]);
    }
    return row;
}

/* ------------------------------------------------------------------ */
/* tbf - token bucket shaper                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    double  rate, bucket, tokens;       /* bit/s, bytes, bytes */
    nl_time t_tokens, last;
    unsigned delayed, sent;
} tbf_st;

static void *tbf_create(nl_link *l, int dir, const nl_args *a)
{
    tbf_st *t = calloc(1, sizeof(*t));

    if (!t)
        return NULL;
    t->rate = nl_arg_rate(a, "shape", 1e6);
    if (t->rate < 1000)
        t->rate = 1000;
    t->bucket = nl_arg_dbl(a, "bucket", 3000);
    if (t->bucket < NL_FRAME_MAX)
        t->bucket = NL_FRAME_MAX;       /* must hold at least one full frame */
    t->tokens = t->bucket;
    t->t_tokens = nl_now();
    nl_cancel(&g_kick[l->idx][dir].ev);
    return t;
}

static void tbf_refill(tbf_st *t)
{
    t->tokens += (double)(nl_now() - t->t_tokens) * t->rate / 8e6;
    if (t->tokens > t->bucket)
        t->tokens = t->bucket;
    t->t_tokens = nl_now();
}

static nl_pkt *tbf_dequeue(void *st, nl_link *l, int dir)
{
    tbf_st *t = st;
    const nl_qitem *head = nl_q_peek(l, dir, 0);

    if (!t || !head)
        return nl_q_pop(l, dir, NULL);
    tbf_refill(t);
    if (sample_due(&t->last)) {
        sample(l, dir, "queue", l->d[dir].qlen);
        sample(l, dir, "tokens (B)", t->tokens);
    }
    if (t->tokens >= head->p->len) {
        t->tokens -= head->p->len;
        t->sent++;
        return nl_q_pop(l, dir, NULL);
    }
    /* not enough tokens: come back when the bucket has filled enough */
    t->delayed++;
    kick_after(l, dir, (nl_time)((head->p->len - t->tokens) * 8e6 / t->rate) + 1);
    return NULL;
}

static int tbf_rows(void *st, nl_link *l, int dir, int x, int y, int w, int h)
{
    tbf_st *t = st;
    int row = 0, bw = w - 26 > 4 ? w - 26 : 4;
    double tokens;

    if (!t || h < 3)
        return 0;
    tokens = t->tokens + (double)(nl_now() - t->t_tokens) * t->rate / 8e6;
    if (tokens > t->bucket)
        tokens = t->bucket;
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("tokens"));
    tui_bar(x + 14, y + row, bw, tokens / t->bucket, C_OK);
    tui_print(x + 14 + bw + 1, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%.0f B", tokens);
    tui_print(x, y + row, 14, TS(C_DIM, C_DEFAULT, 0), "%s", _("queue"));
    tui_bar(x + 14, y + row, bw, (double)l->d[dir].qlen / l->cfg.qcap, C_WARN);
    tui_print(x + 14 + bw + 1, y + row++, 11, TS(C_TEXT, C_DEFAULT, 0), "%d/%d", l->d[dir].qlen, l->cfg.qcap);
    row += nl_ui_kv(x, y + row, w, _("rate / bucket"), "%s / %.0f B", nl_rate_str(t->rate), t->bucket);
    return row;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

typedef int (*rows_fn)(void *st, nl_link *l, int dir, int x, int y, int w, int h);

#define PANEL(name) \
    static void name##_panel(void *st, nl_link *l, int dir, int x, int y, int w, int h) \
    { name##_rows(st, l, dir, x, y, w, h); }
PANEL(fifo) PANEL(red) PANEL(codel) PANEL(pie) PANEL(fq) PANEL(prio) PANEL(tbf)

static const nl_qdisc q_fifo = {
    .name = "fifo", .title = N_("drop-tail FIFO that also records queue length and waiting time"),
    .create = fifo_create, .dequeue = fifo_dequeue, .panel = fifo_panel,
};
static const nl_qdisc q_red = {
    .name = "red", .title = N_("RED: random early drops (or ECN marks) as the average queue grows"),
    .create = red_create, .enqueue = red_enqueue, .dequeue = red_dequeue, .panel = red_panel,
};
static const nl_qdisc q_codel = {
    .name = "codel", .title = N_("CoDel: keeps the time packets wait in the queue near a target"),
    .create = codel_create, .dequeue = codel_dequeue, .panel = codel_panel,
};
static const nl_qdisc q_pie = {
    .name = "pie", .title = N_("PIE: drop probability steered by a controller on the queueing delay"),
    .create = pie_create, .enqueue = pie_enqueue, .dequeue = pie_dequeue, .panel = pie_panel,
};
static const nl_qdisc q_fq = {
    .name = "fq", .title = N_("fair queueing: one queue per flow, served by deficit round robin"),
    .create = fq_create, .enqueue = fq_enqueue, .dequeue = fq_dequeue, .panel = fq_panel,
    .backlog = fq_backlog, .flush = fq_flush,
};
static const nl_qdisc q_prio = {
    .name = "prio", .title = N_("strict priority: three bands chosen by the DSCP of the packet"),
    .create = prio_create, .enqueue = prio_enqueue, .dequeue = prio_dequeue, .panel = prio_panel,
    .backlog = prio_backlog, .flush = prio_flush,
};
static const nl_qdisc q_tbf = {
    .name = "tbf", .title = N_("token bucket shaper: a long-term rate with a bounded burst"),
    .create = tbf_create, .dequeue = tbf_dequeue, .panel = tbf_panel,
};

static const struct { const nl_qdisc *q; rows_fn rows; } g_all[] = {
    { &q_fifo, fifo_rows }, { &q_red, red_rows }, { &q_codel, codel_rows }, { &q_pie, pie_rows },
    { &q_fq, fq_rows }, { &q_prio, prio_rows }, { &q_tbf, tbf_rows },
};
#define N_ALL ((int)(sizeof(g_all) / sizeof(g_all[0])))

/* ------------------------------------------------------------------ */
/* The `qdisc` module: documentation and a per-node panel             */
/* ------------------------------------------------------------------ */

static const nl_module qdisc_mod;

static int qdisc_run(nl_node *n, const nl_args *a)
{
    const char *cmd = nl_arg_pos(a, 0);
    int i, k;

    if (!nl_mod_get(n, &qdisc_mod)) {
        int *mark = calloc(1, sizeof(int));
        if (!mark)
            return -1;
        nl_mod_set(n, &qdisc_mod, mark);
    }
    if (cmd && strcmp(cmd, "list") == 0) {
        nl_log(n, NL_L_NET, 1, 0, "  " T_BOLD "%-9s" T_RESET "%s", "droptail",
               _("drop arrivals when the queue is full (default)"));
        for (k = 0; k < N_ALL; k++)
            nl_log(n, NL_L_NET, 1, 0, "  " T_BOLD "%-9s" T_RESET "%s", g_all[k].q->name, _(g_all[k].q->title));
        return 0;
    }
    if (cmd && strcmp(cmd, "show") != 0) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "qdisc: %s '%s'", _("unknown command"), cmd);
        return -1;
    }
    if (!cmd)
        return 0;
    for (i = 0; i < n->nif; i++) {
        const nl_link *l = n->ifc[i].link;
        const nl_linkdir *q;

        if (!l)
            continue;
        q = &l->d[n->ifc[i].end];
        nl_log(n, NL_L_NET, 1, 0,
               T_INFO "%s" T_RESET " → %s: " T_BOLD "%s" T_RESET " %s, %s %d/%d, %s %llu, %s %llu, %s %llu",
               n->ifc[i].name, l->n[!n->ifc[i].end]->name, l->cfg.qdisc, nl_rate_str(l->cfg.bw), _("queue"),
               nl_link_backlog(l, n->ifc[i].end), l->cfg.qcap, _("sent"), (unsigned long long)q->tx, _("dropped"),
               (unsigned long long)q->qdrops, _("ECN marked"), (unsigned long long)q->marked);
    }
    return 0;
}

static void qdisc_stop(nl_node *n)
{
    free(nl_mod_get(n, &qdisc_mod));
    nl_mod_set(n, &qdisc_mod, NULL);
}

static int qdisc_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    int row = 0, i, k;

    (void)detail;
    for (i = 0; i < n->nif && row < h; i++) {
        nl_link *l = n->ifc[i].link;
        int dir = n->ifc[i].end;
        nl_linkdir *q;

        if (!l)
            continue;
        q = &l->d[dir];
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0),
                  T_INFO T_BOLD "%s" T_RESET " → %s  " T_BOLD "%s" T_RESET T_DIM "  %s  %s %llu  %s %llu",
                  n->ifc[i].name, l->n[!dir]->name, l->cfg.qdisc, nl_rate_str(l->cfg.bw), _("sent"),
                  (unsigned long long)q->tx, _("dropped"), (unsigned long long)q->qdrops);
        for (k = 0; k < N_ALL; k++)
            if (strcmp(l->cfg.qdisc, g_all[k].q->name) == 0)
                break;
        if (k < N_ALL && q->qstate)
            row += g_all[k].rows(q->qstate, l, dir, x + 2, y + row, w - 2, h - row);
        else if (row < h)
            row += nl_ui_kv(x + 2, y + row, w - 2, _("queue"), "%d/%d", nl_link_backlog(l, dir), l->cfg.qcap);
    }
    return row;
}

static const nl_module qdisc_mod = {
    .name = "qdisc", .title = N_("Queue disciplines: what a router does when packets arrive faster than they leave"),
    .layer = NL_L_NET,
    .usage = N_("link A B qdisc=NAME [queue=N] [parameters]   choose the discipline of a link (both directions)\n"
                "run NODE qdisc            show the queues of NODE in its panel\n"
                "run NODE qdisc show       log the state of every queue of NODE\n"
                "run NODE qdisc list       log the available disciplines\n"
                "  droptail  default: arrivals are dropped when the queue holds `queue` packets\n"
                "  fifo      droptail that records queue length and waiting time for the graphs\n"
                "  red       min=N max=N (packets) maxp=0.1 wq=0.02 gentle=1 ecn=0\n"
                "  codel     target=5ms interval=100ms ecn=0\n"
                "  pie       target=15ms tupdate=15ms ecn=0\n"
                "  fq        quantum=1514 (bytes per round); flows are the IP 5-tuples\n"
                "  prio      band 0: DSCP >= 40 (EF: tos=184) and ARP; band 1: other DSCP; band 2: best effort\n"
                "  tbf       shape=RATE bucket=BYTES (the link bw still applies underneath)\n"
                "  Counters: red.drops red.early red.forced red.marks codel.drops codel.marks pie.drops\n"
                "  pie.marks fq.drops prio.drops, on the node that transmits."),
    .theory = N_("Every output interface of a router has a queue: when packets arrive faster than the "
                 "link can carry them they wait, and when the buffer is full some must be dropped. The "
                 "queue discipline decides which packet leaves next (scheduling) and which packets are "
                 "dropped or marked, and when (queue management). The default, a drop-tail FIFO, is the "
                 "simplest answer and has two well-known problems. First, with a large buffer a "
                 "persistent overload keeps the queue full: every packet then waits behind hundreds of "
                 "others and latency explodes while throughput gains nothing (bufferbloat). Second, it "
                 "signals congestion only when the buffer overflows, so many flows lose packets at the "
                 "same moment and back off together (global synchronisation).\n"
                 "\n"
                 "Active queue management signals congestion early. RED keeps a moving average of the "
                 "queue length and drops arrivals with a probability that rises linearly between two "
                 "thresholds, so flows are told to slow down one at a time before the buffer fills; its "
                 "weakness is that the right thresholds depend on the link and the traffic. CoDel looks "
                 "at time instead of length: it measures how long each packet sat in the queue and, "
                 "when that sojourn time stays above a small target for a whole interval, starts "
                 "dropping at the head with an increasing frequency (interval divided by the square "
                 "root of the number of drops) until the delay comes back under the target. It needs no "
                 "tuning across link speeds. PIE reaches a similar goal with a controller that adjusts a "
                 "drop probability from the estimated queueing delay and its trend. With ECN the same "
                 "algorithms can mark packets (CE in the IP header) instead of dropping them: a "
                 "transport that understands the mark slows down without losing data. None of this "
                 "helps against a sender that does not react, which is why the labs here that use "
                 "constant-rate UDP show AQM bounding the delay but not removing the loss.\n"
                 "\n"
                 "Scheduling divides the link among flows. Strict priority always serves the highest "
                 "non-empty band, chosen here by the DSCP field: ideal for a little delay-sensitive "
                 "traffic such as voice, at the risk of starving the lower bands. Fair queueing gives "
                 "every flow its own queue and serves them in turn; deficit round robin does it in "
                 "constant time by granting each flow a quantum of bytes per round and carrying the "
                 "unused part over, so a flow that sends too much only hurts itself. A token bucket "
                 "shaper limits a flow to a long-term rate while allowing bursts up to the bucket size: "
                 "tokens accumulate at the configured rate, a packet leaves only when enough tokens are "
                 "available, and the excess waits (shaping) instead of being dropped (policing). Modern "
                 "systems combine the ideas: fq_codel and CAKE run CoDel inside each fair queue."),
    .run = qdisc_run, .stop = qdisc_stop, .panel = qdisc_panel,
};
NL_MODULE(qdisc_mod)

NL_INIT(qdiscs)
{
    int k;

    for (k = 0; k < N_ALL; k++)
        nl_qdisc_register(g_all[k].q);
}
