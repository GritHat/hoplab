/*
 * mac.c - Medium access control laboratory: pure and slotted ALOHA, CSMA
 *         (non-persistent, 1-persistent, p-persistent), CSMA/CD with binary
 *         exponential backoff, CSMA/CA with optional RTS/CTS and hidden
 *         terminals.
 *
 * The shared medium is modelled inside the module (netlab links are point
 * to point): time advances in ticks of one propagation delay, so a station
 * that senses the channel sees what the others were doing one tick ago.
 * That single tick is the vulnerable period that makes carrier sensing
 * imperfect.
 */
#include "nl_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAC_MAXS   64                   /* stations */
#define MAC_QCAP   16                   /* frames queued per station */
#define MAC_HIST   256                  /* timeline cells kept per station */
#define MAC_CELLS  4                    /* timeline cells per frame time */
#define SWEEP_PTS  9

enum { P_ALOHA, P_SLOTTED, P_CSMA, P_CSMA1, P_CSMAP, P_CSMACD, P_CSMACA, P_COUNT };
static const char *const PROTO_NAME[P_COUNT] = { "aloha", "slotted", "csma", "csma1", "csmap", "csmacd", "csmaca" };
static const char *const PROTO_TITLE[P_COUNT] = {
    N_("pure ALOHA"), N_("slotted ALOHA"), N_("non-persistent CSMA"), N_("1-persistent CSMA"),
    N_("p-persistent CSMA"), N_("CSMA/CD (Ethernet)"), N_("CSMA/CA (Wi-Fi)"),
};

enum { ST_IDLE, ST_READY, ST_WAIT, ST_TX };
enum { H_NONE, H_WAIT, H_TX, H_OK, H_COLL };

typedef struct {
    int     state, q, qh, k;            /* k = failed attempts of the head frame */
    long    arr[MAC_QCAP];              /* arrival tick of queued frames */
    long    wait, left, tx_start, rts_left;
    int     collided, jam, was_tx, idle_run, bo;
    uint8_t hist[MAC_HIST];
} station;

typedef struct {
    int     proto, n, F, hidden, rts;   /* F = ticks per frame = 1/a */
    double  p, load;
    long    tick, nav_until;
    int     prev[2], prev_total;        /* transmitters heard in the previous tick, per group */
    long    attempts, offered, success, collisions, dropped, qdrops;
    double  delay_sum;
    station st[MAC_MAXS];
} sim;

typedef struct {
    nl_node *n;
    sim      s;
    int      running;
    nl_time  frame, until;
    nl_ev    timer;
    long     win_success, win_frames;
    /* last sweep */
    int      have_sweep, sweep_proto, sweep_pts;     /* points computed so far */
    int      sw_proto, sw_n, sw_F, sw_hidden, sw_rts, sw_k;
    double   sw_p;
    nl_ev    sw_timer;
    struct { double load, g, s, theory; } pt[SWEEP_PTS];
} mac_state;

static const nl_module mac_mod;

/* ------------------------------------------------------------------ */
/* The simulation                                                     */
/* ------------------------------------------------------------------ */

static int proto_by_name(const char *s)
{
    int i;

    for (i = 0; i < P_COUNT; i++)
        if (strcmp(s, PROTO_NAME[i]) == 0)
            return i;
    if (strcmp(s, "csma_cd") == 0 || strcmp(s, "ethernet") == 0) return P_CSMACD;
    if (strcmp(s, "csma_ca") == 0 || strcmp(s, "wifi") == 0)     return P_CSMACA;
    return -1;
}

static void sim_reset(sim *s, int proto, int n, int F, double load, double p, int hidden, int rts)
{
    memset(s, 0, sizeof(*s));
    s->proto = proto;
    s->n = n;
    s->F = F;
    s->load = load;
    s->p = p;
    s->hidden = hidden;
    s->rts = rts;
}

/* Throughput predicted by the classic infinite-population models, or -1 */
static double theory(int proto, double G, double a)
{
    switch (proto) {
    case P_ALOHA:   return G * exp(-2 * G);
    case P_SLOTTED: return G * exp(-G);
    case P_CSMA:    return G * exp(-a * G) / (G * (1 + 2 * a) + exp(-a * G));
    case P_CSMA1:
        return G * (1 + G + a * G * (1 + G + a * G / 2)) * exp(-G * (1 + 2 * a)) /
               (G * (1 + 2 * a) - (1 - exp(-a * G)) + (1 + a * G) * exp(-G * (1 + a)));
    default:        return -1;
    }
}

static int rts_ticks(const sim *s) { return s->F / 20 > 2 ? s->F / 20 : 2; }
static int ack_ticks(const sim *s) { return s->F / 20 > 2 ? s->F / 20 : 2; }

/* What station i hears now: the others as they were one tick ago, or a
 * reservation announced by the access point (CTS / ACK) */
static int busy(const sim *s, int i)
{
    int heard = s->hidden ? s->prev[i & 1] : s->prev_total;

    /* a station that has just stopped still hears its own tail: everybody
     * agrees on the instant the channel becomes free */
    return heard > 0 || s->tick < s->nav_until;
}

static void new_frame(sim *s, station *t)
{
    t->state = ST_READY;
    t->k = 0;
    t->idle_run = 0;
    t->bo = s->proto == P_CSMACA ? (int)nl_rand(32) : 0;
}

static void start_tx(sim *s, station *t)
{
    t->state = ST_TX;
    t->left = s->F;
    t->rts_left = 0;
    if (s->proto == P_CSMACA && s->rts) {
        t->rts_left = rts_ticks(s);
        t->left += 2 * t->rts_left;     /* RTS, then CTS, then the data */
    }
    t->collided = 0;
    t->jam = 0;
    t->tx_start = s->tick;
    s->attempts++;
    if (s->proto != P_CSMA)
        s->offered++;
}

static void finish_tx(sim *s, station *t, int ok)
{
    long c, cw = s->F / MAC_CELLS > 0 ? s->F / MAC_CELLS : 1;
    int win;

    for (c = t->tx_start / cw; c <= s->tick / cw; c++)
        if (s->tick / cw - c < MAC_HIST && t->hist[c % MAC_HIST] == H_TX)
            t->hist[c % MAC_HIST] = ok ? H_OK : H_COLL;
    if (ok) {
        s->success++;
        s->delay_sum += (double)(s->tick + 1 - t->arr[t->qh]);
        t->qh = (t->qh + 1) % MAC_QCAP;
        t->q--;
        if (s->proto == P_CSMACA && s->tick + 1 + ack_ticks(s) > s->nav_until)
            s->nav_until = s->tick + 1 + ack_ticks(s);          /* everyone hears the ACK */
        if (t->q > 0) {
            new_frame(s, t);
            /* the random-access protocols space out their own frames too,
             * or one station with a backlog would keep the channel */
            if (s->proto == P_ALOHA || s->proto == P_CSMA) {
                t->state = ST_WAIT;
                t->wait = 1 + (long)nl_rand((unsigned)(s->n * s->F));
            } else if (s->proto == P_SLOTTED) {
                t->state = ST_WAIT;
                t->wait = (long)nl_rand((unsigned)s->n) * s->F;
            }
        } else {
            t->state = ST_IDLE;
        }
        return;
    }
    s->collisions++;
    t->k++;
    t->state = ST_WAIT;
    switch (s->proto) {
    case P_ALOHA:
        t->wait = 1 + (long)nl_rand((unsigned)(2 * s->n * s->F));    /* random retransmission delay */
        break;
    case P_SLOTTED:
        t->wait = (long)nl_rand((unsigned)(2 * s->n)) * s->F;
        break;
    case P_CSMACD:
        if (t->k >= 16) {               /* excessive collisions: the frame is dropped */
            s->dropped++;
            t->qh = (t->qh + 1) % MAC_QCAP;
            t->q--;
            if (t->q > 0)
                new_frame(s, t);
            else
                t->state = ST_IDLE;
            return;
        }
        t->wait = (long)nl_rand(1u << (t->k < 10 ? t->k : 10)) * 2;   /* slot = 2 x propagation */
        t->idle_run = 0;
        break;
    case P_CSMACA:
        win = (32 << (t->k < 5 ? t->k : 5)) - 1;
        t->bo = (int)nl_rand((unsigned)win + 1);
        t->idle_run = 0;
        t->wait = ack_ticks(s) + 1;     /* ACK timeout */
        break;
    default:
        t->wait = 1 + (long)nl_rand((unsigned)(s->F << (t->k < 5 ? t->k : 5)));
        break;
    }
}

static void sim_tick(sim *s)
{
    double lam = s->load / ((double)s->n * s->F);
    long cw = s->F / MAC_CELLS > 0 ? s->F / MAC_CELLS : 1;
    int cur[2] = { 0, 0 }, total = 0, i;

    for (i = 0; i < s->n; i++) {
        station *t = &s->st[i];
        int b;

        if (s->tick % cw == 0)
            t->hist[(s->tick / cw) % MAC_HIST] = H_NONE;
        if (nl_rand01() < lam) {
            if (t->q < MAC_QCAP)
                t->arr[(t->qh + t->q++) % MAC_QCAP] = s->tick;
            else
                s->qdrops++;
        }
        if (t->state == ST_IDLE && t->q > 0)
            new_frame(s, t);
        if (t->state == ST_WAIT && --t->wait <= 0)
            t->state = ST_READY;
        else if (t->state == ST_READY) {
            b = busy(s, i);
            switch (s->proto) {
            case P_ALOHA:
                start_tx(s, t);
                break;
            case P_SLOTTED:
                if (s->tick % s->F == 0)
                    start_tx(s, t);
                break;
            case P_CSMA:                /* busy: go away and come back later */
                s->offered++;
                if (!b) {
                    start_tx(s, t);
                } else {
                    t->state = ST_WAIT;
                    t->wait = 1 + (long)nl_rand((unsigned)(2 * s->F));
                }
                break;
            case P_CSMA1:               /* busy: keep listening, go as soon as it is free */
                if (!b)
                    start_tx(s, t);
                break;
            case P_CSMAP:
                if (!b && nl_rand01() < s->p)
                    start_tx(s, t);
                break;
            case P_CSMACD:              /* 1-persistent after a short inter-frame gap */
                if (b)
                    t->idle_run = 0;
                else if (++t->idle_run >= 2)
                    start_tx(s, t);
                break;
            case P_CSMACA:              /* DIFS, then count the backoff down while idle */
                if (b)
                    t->idle_run = 0;
                else if (t->idle_run < 4)
                    t->idle_run++;
                else if (t->bo == 0)
                    start_tx(s, t);
                else
                    t->bo--;
                break;
            }
        }
        if (t->state == ST_TX) {
            cur[i & 1]++;
            total++;
        }
        if (t->state != ST_IDLE && t->hist[(s->tick / cw) % MAC_HIST] < (t->state == ST_TX ? H_TX : H_WAIT))
            t->hist[(s->tick / cw) % MAC_HIST] = t->state == ST_TX ? H_TX : H_WAIT;
    }

    for (i = 0; i < s->n; i++) {
        station *t = &s->st[i];

        if (t->state != ST_TX) {
            t->was_tx = 0;
            continue;
        }
        if (total >= 2)
            t->collided = 1;
        /* collision detection: someone else's signal has reached us */
        if (s->proto == P_CSMACD && !t->jam && s->prev_total - t->was_tx > 0) {
            t->collided = 1;
            t->jam = 1;
            t->left = 3;                /* jam signal, then stop */
        }
        t->was_tx = 1;
        t->left--;
        if (t->rts_left > 0 && --t->rts_left == 0) {
            if (t->collided) {          /* no CTS comes back: only the short RTS was wasted */
                finish_tx(s, t, 0);
                continue;
            }
            if (s->tick + 1 + t->left > s->nav_until)
                s->nav_until = s->tick + 1 + t->left;           /* CTS: everybody else stays silent */
        }
        if (t->left <= 0)
            finish_tx(s, t, !t->collided);
    }
    s->prev[0] = cur[0];
    s->prev[1] = cur[1];
    s->prev_total = total;
    s->tick++;
}

static double sim_frames(const sim *s) { return s->tick ? (double)s->tick / s->F : 1.0; }
static double sim_S(const sim *s)      { return s->success / sim_frames(s); }
static double sim_G(const sim *s)      { return s->offered / sim_frames(s); }

/* ------------------------------------------------------------------ */
/* Live run                                                           */
/* ------------------------------------------------------------------ */

static void mac_counters(mac_state *m)
{
    nl_count_set(m->n, "mac.sent", (double)m->s.attempts);
    nl_count_set(m->n, "mac.success", (double)m->s.success);
    nl_count_set(m->n, "mac.collisions", (double)m->s.collisions);
    nl_count_set(m->n, "mac.dropped", (double)(m->s.dropped + m->s.qdrops));
    nl_count_set(m->n, "mac.throughput_pct", 100.0 * sim_S(&m->s));
}

static void mac_summary(mac_state *m)
{
    const sim *s = &m->s;
    double th = theory(s->proto, sim_G(s), 1.0 / s->F);
    char tbuf[48] = "";

    if (th >= 0)
        snprintf(tbuf, sizeof(tbuf), " (%s %.3f)", _("theory"), th);
    nl_log(m->n, NL_L_LINK, 1, 0, T_INFO "mac %s" T_RESET ": G=%.2f  S=%.3f%s  %ld %s, %ld %s, %s %.1f %s",
           PROTO_NAME[s->proto], sim_G(s), sim_S(s), tbuf, s->success, _("frames delivered"), s->collisions,
           _("collided transmissions"), _("mean delay"), s->success ? s->delay_sum / s->success / s->F : 0.0,
           _("frame times"));
}

static void mac_tick(void *ctx)
{
    mac_state *m = ctx;
    long before = m->s.success, coll = m->s.collisions;
    int i;

    m->timer = 0;
    for (i = 0; i < m->s.F; i++)
        sim_tick(&m->s);
    m->win_success += m->s.success - before;
    if (m->s.collisions > coll)
        nl_log(m->n, NL_L_LINK, 3, 0, T_ERR "mac" T_RESET " %s (%ld)", _("collision"), m->s.collisions - coll);
    if (++m->win_frames >= 20) {
        nl_sample(m->n, "throughput S", (double)m->win_success / m->win_frames);
        nl_log(m->n, NL_L_LINK, 2, 0, T_INFO "mac" T_RESET " %s %d %s: S=%.2f  " T_DIM "(%s S=%.3f, G=%.2f)",
               _("last"), (int)m->win_frames, _("frame times"), (double)m->win_success / m->win_frames,
               _("so far"), sim_S(&m->s), sim_G(&m->s));
        m->win_success = m->win_frames = 0;
    }
    mac_counters(m);
    if (m->until && nl_now() >= m->until) {
        m->running = 0;
        mac_summary(m);
        return;
    }
    m->timer = nl_after(m->n, m->frame, mac_tick, m);
}

/* A sweep simulates nine offered loads. Each one is a separate short
 * simulation; they are computed one per timer event, 150 ms of virtual time
 * apart, so that the interface keeps drawing while the table fills. */
static const double SWEEP_LOADS[SWEEP_PTS] = { 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0 };

static void sweep_step(void *ctx)
{
    mac_state *m = ctx;
    sim *s = malloc(sizeof(*s));
    char tbuf[40] = "", name[28];
    int k = m->sw_k, j;
    long i, steps = 2000L * m->sw_F;
    double peak = 0;

    m->sw_timer = 0;
    if (!s)
        return;
    sim_reset(s, m->sw_proto, m->sw_n, m->sw_F, SWEEP_LOADS[k], m->sw_p, m->sw_hidden, m->sw_rts);
    for (i = 0; i < steps; i++)
        sim_tick(s);
    m->pt[k].load = SWEEP_LOADS[k];
    m->pt[k].g = sim_G(s);
    m->pt[k].s = sim_S(s);
    m->pt[k].theory = theory(m->sw_proto, m->pt[k].g, 1.0 / m->sw_F);
    free(s);
    if (m->pt[k].theory >= 0)
        snprintf(tbuf, sizeof(tbuf), "  %s %.3f", _("theory"), m->pt[k].theory);
    nl_log(m->n, NL_L_LINK, 1, 0, "  %s %.2f  G=%5.2f  " T_BOLD "S=%.3f" T_RESET T_DIM "%s", _("load"),
           SWEEP_LOADS[k], m->pt[k].g, m->pt[k].s, tbuf);
    m->sweep_pts = k + 1;
    if (++m->sw_k < SWEEP_PTS) {
        m->sw_timer = nl_after(m->n, NL_MS(150), sweep_step, m);
        return;
    }
    for (j = 0; j < SWEEP_PTS; j++)
        if (m->pt[j].s > peak)
            peak = m->pt[j].s;
    nl_log(m->n, NL_L_LINK, 1, 0, T_OK "  %s S=%.3f", _("best throughput"), peak);
    snprintf(name, sizeof(name), "mac.%s.peak", PROTO_NAME[m->sw_proto]);
    nl_count_set(m->n, name, 100.0 * peak);
    nl_count_set(m->n, "mac.sweep_peak_pct", 100.0 * peak);
    nl_count(m->n, "mac.sweeps", 1);
}

static int mac_sweep(mac_state *m, int proto, int n, int F, double p, int hidden, int rts)
{
    nl_cancel(&m->sw_timer);
    nl_log(m->n, NL_L_LINK, 1, 0, T_INFO "mac sweep" T_RESET " %s (%s), %d %s, a=%.3f", PROTO_NAME[proto],
           _(PROTO_TITLE[proto]), n, _("stations"), 1.0 / F);
    m->sw_proto = proto;
    m->sw_n = n;
    m->sw_F = F;
    m->sw_p = p;
    m->sw_hidden = hidden;
    m->sw_rts = rts;
    m->sw_k = 0;
    m->have_sweep = 1;
    m->sweep_proto = proto;
    m->sweep_pts = 0;
    m->sw_timer = nl_after(m->n, 0, sweep_step, m);
    return 0;
}

static int mac_run(nl_node *n, const nl_args *a)
{
    mac_state *m = nl_mod_get(n, &mac_mod);
    const char *what = nl_arg_pos(a, 0), *pname = nl_arg_str(a, "proto", "aloha");
    int proto = proto_by_name(pname), ns = (int)nl_arg_int(a, "stations", 10), F;
    nl_time frame = nl_arg_time(a, "frame", NL_MS(1)), prop, dur;
    double load = nl_arg_dbl(a, "load", 0.5), p = nl_arg_dbl(a, "p", 0.1);
    int hidden = (int)nl_arg_int(a, "hidden", 0), rts = (int)nl_arg_int(a, "rts", 0);

    if (!m) {
        m = calloc(1, sizeof(*m));
        if (!m)
            return -1;
        m->n = n;
        nl_mod_set(n, &mac_mod, m);
    }
    if (what && strcmp(what, "stop") == 0) {
        nl_cancel(&m->timer);
        if (m->running)
            mac_summary(m);
        m->running = 0;
        return 0;
    }
    if (proto < 0) {
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "mac: %s '%s' (aloha slotted csma csma1 csmap csmacd csmaca)",
               _("unknown protocol"), pname);
        return -1;
    }
    if (ns < 2) ns = 2;
    if (ns > MAC_MAXS) {
        nl_log(n, NL_L_LINK, 1, 0, T_WARN "mac: %s %d", _("too many stations: using the maximum,"), MAC_MAXS);
        ns = MAC_MAXS;
    }
    if (frame < 100) frame = 100;
    prop = nl_arg_time(a, "prop", frame / 100);
    F = prop > 0 ? (int)((frame + prop / 2) / prop) : 400;
    if (F < 4) F = 4;
    if (F > 400) F = 400;
    if (p <= 0 || p > 1) p = 0.1;
    if (load < 0) load = 0;
    if (what && strcmp(what, "sweep") == 0)
        return mac_sweep(m, proto, ns, F, p, hidden, rts);

    nl_cancel(&m->timer);
    sim_reset(&m->s, proto, ns, F, load, p, hidden, rts);
    m->frame = frame;
    dur = nl_arg_time(a, "duration", 0);
    m->until = dur ? nl_now() + dur : 0;
    m->running = 1;
    m->win_success = m->win_frames = 0;
    nl_log(n, NL_L_LINK, 1, 0, T_INFO "mac %s" T_RESET " (%s): %d %s, %s %.2f, %s %s, a=%.3f%s%s", PROTO_NAME[proto],
           _(PROTO_TITLE[proto]), ns, _("stations"), _("offered load"), load, _("frame time"), nl_time_str(frame),
           1.0 / F, hidden ? _(", two groups that cannot hear each other") : "", rts ? ", RTS/CTS" : "");
    m->timer = nl_after(n, frame, mac_tick, m);
    return 0;
}

static void mac_stop(nl_node *n)
{
    mac_state *m = nl_mod_get(n, &mac_mod);

    if (!m)
        return;
    nl_cancel(&m->timer);
    nl_cancel(&m->sw_timer);
    nl_mod_set(n, &mac_mod, NULL);
    free(m);
}

static void mac_crash(nl_node *n)
{
    mac_state *m = nl_mod_get(n, &mac_mod);

    if (m)
        m->timer = m->sw_timer = 0;
}

static void mac_recover(nl_node *n)
{
    mac_state *m = nl_mod_get(n, &mac_mod);

    if (m && m->running && !m->timer)
        m->timer = nl_after(n, m->frame, mac_tick, m);
    if (m && m->have_sweep && m->sw_k < SWEEP_PTS && !m->sw_timer)
        m->sw_timer = nl_after(n, NL_MS(150), sweep_step, m);
}

static int mac_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    mac_state *m = nl_mod_get(n, &mac_mod);
    const sim *s = &m->s;
    tstyle txt = TS(C_TEXT, C_DEFAULT, 0), dim = TS(C_DIM, C_DEFAULT, 0);
    int row = 0, i, k;

    (void)detail;
    if (s->n) {
        long cw = s->F / MAC_CELLS > 0 ? s->F / MAC_CELLS : 1, now_cell = s->tick / cw;
        int cols = w - 5, rows = s->n, sweep_rows = m->have_sweep ? SWEEP_PTS + 1 : 0;
        double th = theory(s->proto, sim_G(s), 1.0 / s->F);

        if (cols > MAC_HIST - 1)
            cols = MAC_HIST - 1;
        tui_print(x, y + row++, w, txt, T_BOLD "%s" T_RESET "  %d %s  a=%.3f  %s%s  " T_OK "█" T_RESET " %s  "
                  T_ERR "█" T_RESET " %s  " T_WARN "·" T_RESET " %s", _(PROTO_TITLE[s->proto]), s->n,
                  _("stations"), 1.0 / s->F, m->running ? T_OK "●" : T_DIM "■", T_RESET, _("delivered"),
                  _("collision"), _("waiting"));
        if (rows > h - 4 - sweep_rows)
            rows = h - 4 - sweep_rows;
        for (i = 0; i < rows; i++) {
            tui_print(x, y + row, 4, dim, "s%-2d", i);
            for (k = 0; k < cols; k++) {
                long c = now_cell - (cols - 1 - k);
                int v = c >= 0 ? s->st[i].hist[c % MAC_HIST] : H_NONE;

                if (v == H_OK)        tui_put(x + 5 + k, y + row, TS(C_OK, C_DEFAULT, 0), "█");
                else if (v == H_COLL) tui_put(x + 5 + k, y + row, TS(C_ERR, C_DEFAULT, 0), "█");
                else if (v == H_TX)   tui_put(x + 5 + k, y + row, TS(C_DATA, C_DEFAULT, 0), "█");
                else if (v == H_WAIT) tui_put(x + 5 + k, y + row, TS(C_WARN, C_DEFAULT, 0), "·");
            }
            row++;
        }
        if (row < h) {
            if (th >= 0)
                tui_print(x, y + row++, w, txt, T_DIM "G" T_RESET " %.2f   " T_DIM "S" T_RESET " " T_BOLD "%.3f"
                          T_RESET "   " T_DIM "%s" T_RESET " %.3f", sim_G(s), sim_S(s), _("theory"), th);
            else
                tui_print(x, y + row++, w, txt, T_DIM "G" T_RESET " %.2f   " T_DIM "S" T_RESET " " T_BOLD "%.3f",
                          sim_G(s), sim_S(s));
        }
        if (row < h)
            tui_print(x, y + row++, w, txt, T_DIM "%s" T_RESET " %ld   " T_DIM "%s" T_RESET " %ld   " T_DIM "%s"
                      T_RESET " %ld   " T_DIM "%s" T_RESET " %.1f", _("delivered"), s->success,
                      _("collisions"), s->collisions, _("dropped"), s->dropped + s->qdrops, _("delay"),
                      s->success ? s->delay_sum / s->success / s->F : 0.0);
    }
    if (m->have_sweep && row + 2 <= h) {
        int bw = w - 34;

        tui_print(x, y + row++, w, txt, T_BOLD "S(G)" T_RESET " %s", _(PROTO_TITLE[m->sweep_proto]));
        for (k = 0; k < m->sweep_pts && row < h; k++) {
            tui_print(x, y + row, 8, dim, "G=%-5.2f", m->pt[k].g);
            if (bw > 4) {
                tui_bar(x + 8, y + row, bw, m->pt[k].s, C_OK);
                if (m->pt[k].theory >= 0)
                    tui_put(x + 8 + (int)(m->pt[k].theory * (bw - 1)), y + row, TS(C_WARN, C_PANEL, TA_BOLD), "│");
            }
            if (m->pt[k].theory >= 0)
                tui_print(x + 9 + (bw > 4 ? bw : 0), y + row, 24, txt, "S=%.3f " T_DIM "(%.3f)", m->pt[k].s,
                          m->pt[k].theory);
            else
                tui_print(x + 9 + (bw > 4 ? bw : 0), y + row, 24, txt, "S=%.3f", m->pt[k].s);
            row++;
        }
    }
    if (!row)
        tui_print(x, y + row++, w, dim, "%s", _("run NODE mac proto=aloha stations=10 load=0.5"));
    return row;
}

static const nl_module mac_mod = {
    .name = "mac", .title = N_("medium access lab — ALOHA, CSMA, CSMA/CD, CSMA/CA on a shared channel"),
    .layer = NL_L_LINK,
    .usage = N_("run NODE mac proto=aloha|slotted|csma|csma1|csmap|csmacd|csmaca [stations=10] [load=0.5]\n"
                "             [frame=1ms] [prop=frame/100] [p=0.1] [hidden=1] [rts=1] [duration=TIME]\n"
                "  load is the new traffic of all stations together, in frames per frame time;\n"
                "  prop is the propagation delay (a = prop/frame); hidden and rts apply to csmaca.\n"
                "run NODE mac sweep proto=NAME [stations=10] [prop=..]   throughput S against load\n"
                "run NODE mac stop"),
    .theory = N_("When many stations share one channel, two transmissions that overlap destroy each other: "
                 "a collision. A medium access protocol decides who may talk and when, without a central "
                 "arbiter.\n\n"
                 "ALOHA does not even try: transmit when you have a frame, and if no acknowledgement "
                 "comes, wait a random time and repeat. A frame survives only if nobody else starts "
                 "during the frame time before it or during the frame itself, a vulnerable period of two "
                 "frame times, which gives a throughput S = G·e^(-2G) and a peak of only 18% at G = 0.5 "
                 "(G is the attempt rate in frames per frame time). Slotted ALOHA lets stations start only "
                 "at slot boundaries, halving the vulnerable period: S = G·e^(-G), peak 37% at G = 1.\n\n"
                 "Carrier sensing (CSMA) listens before talking. It cannot be perfect, because a signal "
                 "takes time to arrive: two stations that start within one propagation delay of each "
                 "other both hear silence. The smaller a = propagation delay / frame time, the better "
                 "CSMA works. Variants differ in what they do when the channel is busy: 1-persistent "
                 "waits and transmits the instant it is free (and collides with everyone else who "
                 "waited), non-persistent goes away for a random time, p-persistent transmits with "
                 "probability p in each small slot.\n\n"
                 "CSMA/CD (classic Ethernet) also listens while transmitting: on a collision it stops "
                 "at once, sends a short jam, and retries after a random number of slots drawn from a "
                 "window that doubles at every collision (binary exponential backoff), so the cost of a "
                 "collision is a few propagation delays instead of a whole frame. Radios cannot listen "
                 "while they transmit, so Wi-Fi uses CSMA/CA: wait for the channel to stay idle (DIFS), "
                 "then count down a random backoff that freezes whenever someone else talks, and rely on "
                 "an acknowledgement to learn about collisions. Two stations that cannot hear each other "
                 "(hidden terminals) defeat carrier sensing altogether; a short RTS answered by a CTS "
                 "from the access point reserves the channel so that only the cheap RTS can collide.\n\n"
                 "Model and simplifications: time advances in ticks of one propagation delay and every "
                 "station is one tick away from every other; frames have a fixed length; arrivals are "
                 "Poisson and each station queues up to 16 frames. The theoretical curves assume "
                 "infinitely many stations and are shown against the measured attempt rate, so expect "
                 "agreement in shape, not in the last digit. CSMA/CA slots, DIFS, RTS/CTS and ACK "
                 "durations are in ticks, not 802.11 timings."),
    .run = mac_run, .stop = mac_stop, .crash = mac_crash, .recover = mac_recover, .panel = mac_panel,
};
NL_MODULE(mac_mod)
