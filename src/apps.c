/*
 * apps.c - Basic tools every lab needs: ping, traceroute, a UDP echo server
 *          and client, a constant-bit-rate traffic generator with its sink.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A destination is IPv6 when it is written as an IPv6 address, when the
 * command says v6=1, or when the name has no IPv4 address but has an IPv6
 * one. Returns 4, 6 or 0 (unknown). */
static int resolve_target(const nl_args *a, const char *target, nl_ip *v4, nl_ip6 *v6)
{
    if (!target)
        return 0;
    if (nl_parse_ip6(target, v6))
        return 6;
    if (nl_arg_int(a, "v6", 0))
        return nl_resolve6(target, v6) ? 6 : 0;
    if (nl_resolve(target, v4))
        return 4;
    return nl_ip6_enabled() && nl_resolve6(target, v6) ? 6 : 0;
}

/* ------------------------------------------------------------------ */
/* ping                                                               */
/* ------------------------------------------------------------------ */

#define PING_MAX 6

typedef struct ping_sess {
    nl_node *n;
    int      used;
    nl_ip    dst;
    nl_ip6   dst6;
    int      v6;
    char     name[24];
    uint16_t id;
    int      count, sent, recv, size, ttl, df;
    nl_time  interval, timeout;
    nl_time  t_sent[64];                /* by sequence number modulo 64 */
    int      answered[64];
    double   rtt_min, rtt_max, rtt_sum;
    nl_ev    timer;
    int      done;
} ping_sess;

typedef struct { ping_sess s[PING_MAX]; uint16_t next_id; } ping_state;

static const nl_module ping_mod;

static void ping_finish(ping_sess *s)
{
    if (s->done)
        return;
    s->done = 1;
    nl_log(s->n, NL_L_APP, 1, 0, "%sping %s: %d %s, %d %s, %d%% %s%s",
           s->recv == s->sent && s->sent ? T_OK : s->recv ? T_WARN : T_ERR, s->name, s->sent, _("sent"),
           s->recv, _("received"), s->sent ? 100 * (s->sent - s->recv) / s->sent : 0, _("loss"),
           s->recv ? "" : _(" — destination unreachable?"));
    if (s->recv)
        nl_log(s->n, NL_L_APP, 1, 0, T_DIM "     rtt min/avg/max = %.2f / %.2f / %.2f ms", s->rtt_min,
               s->rtt_sum / s->recv, s->rtt_max);
}

static void ping_tick(void *ctx)
{
    ping_sess *s = ctx;
    int prev = s->sent - 1;

    s->timer = 0;
    if (prev >= 0 && !s->answered[prev % 64] && s->timeout <= s->interval) {
        nl_log(s->n, NL_L_APP, 1, 0, T_ERR "ping %s: %s seq=%d", s->name, _("no reply"), prev + 1);
        nl_count(s->n, "ping.lost", 1);
    }
    if (s->count && s->sent >= s->count) {
        ping_finish(s);
        return;
    }
    s->sent++;
    s->t_sent[(s->sent - 1) % 64] = nl_now();
    s->answered[(s->sent - 1) % 64] = 0;
    nl_count(s->n, "ping.sent", 1);
    if (!(s->v6 ? nl_icmp6_echo(s->n, &s->dst6, s->id, (uint16_t)s->sent, s->ttl, s->size)
                : nl_icmp_echo(s->n, s->dst, s->id, (uint16_t)s->sent, s->ttl, s->size, s->df)))
        nl_log(s->n, NL_L_APP, 1, 0, T_ERR "ping %s: %s", s->name, _("cannot send (no route or packet too big)"));
    s->timer = nl_after(s->n, s->interval, ping_tick, s);
}

static void ping_icmp(nl_node *n, void *ctx, const nl_icmpmeta *m)
{
    ping_state *st = ctx;
    int i;

    for (i = 0; i < PING_MAX; i++) {
        ping_sess *s = &st->s[i];
        int seq;

        if (!s->used || s->v6)
            continue;
        if (m->type == NL_ICMP_ECHO_REPLY && m->id == s->id) {
            double rtt;

            seq = m->seq;
            if (seq < 1 || seq > s->sent || s->answered[(seq - 1) % 64])
                return;
            s->answered[(seq - 1) % 64] = 1;
            rtt = (nl_now() - s->t_sent[(seq - 1) % 64]) / 1000.0;
            if (!s->recv || rtt < s->rtt_min) s->rtt_min = rtt;
            if (rtt > s->rtt_max) s->rtt_max = rtt;
            s->rtt_sum += rtt;
            s->recv++;
            nl_count(n, "ping.reply", 1);
            nl_sample(n, "ping rtt (ms)", rtt);
            nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "ping %s: %s" T_RESET " %s %s seq=%d ttl=%d time=%.2f ms",
                   s->name, _("reply"), _("from"), nl_ip_str(m->from), seq, m->ttl, rtt);
            return;
        }
        if ((m->type == NL_ICMP_UNREACH || m->type == NL_ICMP_TIME_EXCEEDED) &&
            m->orig_proto == NL_PROTO_ICMP && m->orig_id == s->id && m->orig_dst == s->dst) {
            seq = m->orig_sport;
            if (seq >= 1 && seq <= s->sent)
                s->answered[(seq - 1) % 64] = 2;
            nl_count(n, "ping.error", 1);
            nl_log(n, NL_L_APP, 1, m->pkt_id, T_ERR "ping %s: %s %s: %s", s->name, _("from"), nl_ip_str(m->from),
                   m->type == NL_ICMP_TIME_EXCEEDED ? _("time to live exceeded")
                   : m->code == 4 ? _("fragmentation needed (packet too big)")
                   : m->code == 3 ? _("port unreachable") : m->code == 1 ? _("host unreachable")
                   : _("network unreachable"));
            return;
        }
    }
}

static void ping_icmp6(nl_node *n, void *ctx, const nl_icmp6meta *m)
{
    ping_state *st = ctx;
    int i;

    for (i = 0; i < PING_MAX; i++) {
        ping_sess *s = &st->s[i];
        int seq;

        if (!s->used || !s->v6)
            continue;
        if (m->type == NL_ICMP6_ECHO_REPLY && m->id == s->id) {
            double rtt;

            seq = m->seq;
            if (seq < 1 || seq > s->sent || s->answered[(seq - 1) % 64])
                return;
            s->answered[(seq - 1) % 64] = 1;
            rtt = (nl_now() - s->t_sent[(seq - 1) % 64]) / 1000.0;
            if (!s->recv || rtt < s->rtt_min) s->rtt_min = rtt;
            if (rtt > s->rtt_max) s->rtt_max = rtt;
            s->rtt_sum += rtt;
            s->recv++;
            nl_count(n, "ping.reply", 1);
            nl_sample(n, "ping rtt (ms)", rtt);
            nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "ping %s: %s" T_RESET " %s %s seq=%d hlim=%d time=%.2f ms",
                   s->name, _("reply"), _("from"), nl_ip6_str(&m->from), seq, m->hlim, rtt);
            return;
        }
        if (m->type < 128 && m->orig_proto == NL_PROTO_ICMP6 && m->orig_id == s->id &&
            nl_ip6_eq(&m->orig_dst, &s->dst6)) {
            char why[96];

            seq = m->orig_seq;
            if (seq >= 1 && seq <= s->sent)
                s->answered[(seq - 1) % 64] = 2;
            nl_count(n, "ping.error", 1);
            if (m->type == NL_ICMP6_TOO_BIG)
                snprintf(why, sizeof(why), "%s (MTU %d)", _("packet too big"), m->mtu);
            else
                snprintf(why, sizeof(why), "%s",
                         m->type == NL_ICMP6_TIME_EXCEEDED ? _("hop limit exceeded")
                         : m->code == 4 ? _("port unreachable") : m->code == 3 ? _("address unreachable")
                         : _("no route to destination"));
            nl_log(n, NL_L_APP, 1, m->pkt_id, T_ERR "ping %s: %s %s: %s", s->name, _("from"), nl_ip6_str(&m->from),
                   why);
            return;
        }
    }
}

static int ping_run(nl_node *n, const nl_args *a)
{
    ping_state *st = nl_mod_get(n, &ping_mod);
    const char *target = nl_arg_pos(a, 0);
    ping_sess *s = NULL;
    nl_ip dst = 0;
    nl_ip6 dst6;
    int i, family = resolve_target(a, target, &dst, &dst6);

    if (!family) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "ping: %s '%s'", _("unknown destination"), target ? target : "");
        return -1;
    }
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->next_id = (uint16_t)(n->idx * 256 + 1);
        nl_mod_set(n, &ping_mod, st);
        nl_icmp_watch(n, ping_icmp, st);
        nl_icmp6_watch(n, ping_icmp6, st);
    }
    for (i = 0; i < PING_MAX && !s; i++)
        if (!st->s[i].used || st->s[i].done)
            s = &st->s[i];
    if (!s) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "ping: %s", _("too many sessions"));
        return -1;
    }
    nl_cancel(&s->timer);
    memset(s, 0, sizeof(*s));
    s->n = n;
    s->used = 1;
    s->dst = dst;
    s->dst6 = dst6;
    s->v6 = family == 6;
    snprintf(s->name, sizeof(s->name), "%s", target);
    s->id = st->next_id++;
    s->count = (int)nl_arg_int(a, "count", 4);
    s->size = (int)nl_arg_int(a, "size", 56);
    s->ttl = (int)nl_arg_int(a, "ttl", 64);
    s->df = (int)nl_arg_int(a, "df", 0);
    s->interval = nl_arg_time(a, "interval", NL_SEC(1));
    s->timeout = s->interval;
    nl_log(n, NL_L_APP, 1, 0, T_INFO "ping %s" T_RESET " (%s): %d %s", target,
           s->v6 ? nl_ip6_str(&dst6) : nl_ip_str(dst), s->size, _("data bytes"));
    ping_tick(s);
    return 0;
}

static void ping_stop(nl_node *n)
{
    ping_state *st = nl_mod_get(n, &ping_mod);
    int i;

    if (!st)
        return;
    for (i = 0; i < PING_MAX; i++)
        nl_cancel(&st->s[i].timer);
    nl_icmp_unwatch(n, ping_icmp, st);
    nl_icmp6_unwatch(n, ping_icmp6, st);
    nl_mod_set(n, &ping_mod, NULL);
    free(st);
}

static int ping_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    ping_state *st = nl_mod_get(n, &ping_mod);
    int i, row = 0;

    (void)detail;
    for (i = 0; i < PING_MAX && row < h; i++) {
        ping_sess *s = &st->s[i];
        if (!s->used)
            continue;
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0),
                  "%s%s" T_RESET " %-14s " T_DIM "%s" T_RESET " %d/%d  " T_DIM "rtt" T_RESET " %.1f ms",
                  s->done ? T_DIM : T_OK, s->done ? "■" : "●", s->name, _("replies"), s->recv, s->sent,
                  s->recv ? s->rtt_sum / s->recv : 0.0);
    }
    if (row + 6 <= h)
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "ping rtt (ms)"), C_OK, "rtt (ms)");
    return h;
}

static const nl_module ping_mod = {
    .name = "ping", .title = N_("ping — is a host reachable, and how far is it?"), .layer = NL_L_NET,
    .usage = N_("run NODE ping DEST [count=4] [interval=1s] [size=56] [ttl=64] [df=1] [v6=1]\n"
                "  DEST is an address or a node name; count=0 pings forever.\n"
                "  An IPv6 address, or v6=1 with a node name, pings over IPv6 (ICMPv6)."),
    .theory = N_("ping sends ICMP Echo Request messages and waits for the matching Echo Reply. "
                 "Each request carries an identifier and a sequence number, so the sender can match "
                 "replies, measure the round-trip time and notice losses. Routers on the path only "
                 "forward the packets; if one cannot, it answers with an ICMP error (destination "
                 "unreachable, time exceeded) that ping reports."),
    .run = ping_run, .stop = ping_stop, .panel = ping_panel,
};
NL_MODULE(ping_mod)

/* ------------------------------------------------------------------ */
/* traceroute                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_node *n;
    nl_ip    dst;
    nl_ip6   dst6;
    int      v6;
    char     name[24];
    uint16_t id;
    int      ttl, maxhops, active, reached;
    nl_time  t_sent;
    nl_ev    timer;
    struct { nl_ip from; nl_ip6 from6; double ms; int state; } hop[32];   /* state: 0 none, 1 answer, 2 timeout */
} tr_state;

static const nl_module tr_mod;
static void tr_probe(tr_state *t);

static void tr_timeout(void *ctx)
{
    tr_state *t = ctx;

    t->timer = 0;
    t->hop[t->ttl].state = 2;
    nl_log(t->n, NL_L_APP, 1, 0, "traceroute %s: %2d  " T_DIM "* (%s)", t->name, t->ttl, _("no answer"));
    tr_probe(t);
}

static void tr_probe(tr_state *t)
{
    if (t->reached || t->ttl >= t->maxhops) {
        t->active = 0;
        nl_log(t->n, NL_L_APP, 1, 0, t->reached ? T_OK "traceroute %s: %s %d %s" : T_ERR "traceroute %s: %s",
               t->name, t->reached ? _("destination reached in") : _("destination not reached"), t->ttl,
               _("hops"));
        nl_count_set(t->n, "traceroute.hops", t->reached ? t->ttl : 0);
        return;
    }
    t->ttl++;
    t->t_sent = nl_now();
    if (t->v6)
        nl_icmp6_echo(t->n, &t->dst6, t->id, (uint16_t)t->ttl, t->ttl, 32);
    else
        nl_icmp_echo(t->n, t->dst, t->id, (uint16_t)t->ttl, t->ttl, 32, 0);
    t->timer = nl_after(t->n, NL_SEC(2), tr_timeout, t);
}

static void tr_icmp(nl_node *n, void *ctx, const nl_icmpmeta *m)
{
    tr_state *t = ctx;
    int ttl, final = 0;
    nl_node *who;

    if (!t->active || t->v6)
        return;
    if (m->type == NL_ICMP_ECHO_REPLY && m->id == t->id) {
        ttl = m->seq;
        final = 1;
    } else if ((m->type == NL_ICMP_TIME_EXCEEDED || m->type == NL_ICMP_UNREACH) &&
               m->orig_proto == NL_PROTO_ICMP && m->orig_id == t->id) {
        ttl = m->orig_sport;
        final = m->type == NL_ICMP_UNREACH;
    } else {
        return;
    }
    if (ttl != t->ttl)
        return;
    nl_cancel(&t->timer);
    t->hop[ttl].from = m->from;
    t->hop[ttl].ms = (nl_now() - t->t_sent) / 1000.0;
    t->hop[ttl].state = 1;
    who = nl_node_by_ip(m->from);
    nl_log(n, NL_L_APP, 1, m->pkt_id, "traceroute %s: %2d  " T_INFO "%-15s" T_RESET " %-8s %.2f ms%s", t->name,
           ttl, nl_ip_str(m->from), who ? who->name : "", t->hop[ttl].ms,
           m->type == NL_ICMP_UNREACH ? T_ERR "  !unreachable" : "");
    if (final)
        t->reached = m->type == NL_ICMP_ECHO_REPLY;
    if (m->type == NL_ICMP_UNREACH)
        t->ttl = t->maxhops;
    tr_probe(t);
}

static void tr_icmp6(nl_node *n, void *ctx, const nl_icmp6meta *m)
{
    tr_state *t = ctx;
    int ttl, final = 0;
    nl_node *who;

    if (!t->active || !t->v6)
        return;
    if (m->type == NL_ICMP6_ECHO_REPLY && m->id == t->id) {
        ttl = m->seq;
        final = 1;
    } else if ((m->type == NL_ICMP6_TIME_EXCEEDED || m->type == NL_ICMP6_UNREACH) &&
               m->orig_proto == NL_PROTO_ICMP6 && m->orig_id == t->id) {
        ttl = m->orig_seq;
        final = m->type == NL_ICMP6_UNREACH;
    } else {
        return;
    }
    if (ttl != t->ttl)
        return;
    nl_cancel(&t->timer);
    t->hop[ttl].from6 = m->from;
    t->hop[ttl].ms = (nl_now() - t->t_sent) / 1000.0;
    t->hop[ttl].state = 1;
    who = nl_node_by_ip6(&m->from);
    nl_log(n, NL_L_APP, 1, m->pkt_id, "traceroute %s: %2d  " T_INFO "%-22s" T_RESET " %-8s %.2f ms%s", t->name, ttl,
           nl_ip6_str(&m->from), who ? who->name : "", t->hop[ttl].ms,
           m->type == NL_ICMP6_UNREACH ? T_ERR "  !unreachable" : "");
    if (final)
        t->reached = m->type == NL_ICMP6_ECHO_REPLY;
    if (m->type == NL_ICMP6_UNREACH)
        t->ttl = t->maxhops;
    tr_probe(t);
}

static int tr_run(nl_node *n, const nl_args *a)
{
    tr_state *t = nl_mod_get(n, &tr_mod);
    const char *target = nl_arg_pos(a, 0);
    nl_ip dst = 0;
    nl_ip6 dst6;
    int family = resolve_target(a, target, &dst, &dst6);

    if (!family) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "traceroute: %s '%s'", _("unknown destination"), target ? target : "");
        return -1;
    }
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t)
            return -1;
        t->n = n;
        nl_mod_set(n, &tr_mod, t);
        nl_icmp_watch(n, tr_icmp, t);
        nl_icmp6_watch(n, tr_icmp6, t);
    }
    nl_cancel(&t->timer);
    memset(t->hop, 0, sizeof(t->hop));
    t->dst = dst;
    t->dst6 = dst6;
    t->v6 = family == 6;
    snprintf(t->name, sizeof(t->name), "%s", target);
    t->id = (uint16_t)(0x7000 + n->idx * 64 + (nl_rand(64)));
    t->ttl = 0;
    t->reached = 0;
    t->active = 1;
    t->maxhops = (int)nl_arg_int(a, "maxhops", 16);
    if (t->maxhops > 31)
        t->maxhops = 31;
    nl_log(n, NL_L_APP, 1, 0, T_INFO "traceroute %s" T_RESET " (%s), %s %d", target,
           t->v6 ? nl_ip6_str(&dst6) : nl_ip_str(dst), _("max hops"), t->maxhops);
    tr_probe(t);
    return 0;
}

static void tr_stop(nl_node *n)
{
    tr_state *t = nl_mod_get(n, &tr_mod);

    if (!t)
        return;
    nl_cancel(&t->timer);
    nl_icmp_unwatch(n, tr_icmp, t);
    nl_icmp6_unwatch(n, tr_icmp6, t);
    nl_mod_set(n, &tr_mod, NULL);
    free(t);
}

static int tr_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    tr_state *t = nl_mod_get(n, &tr_mod);
    int i, row = 0;

    (void)detail;
    tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_DIM "%s" T_RESET " %s (%s)", _("to"), t->name,
              t->v6 ? nl_ip6_str(&t->dst6) : nl_ip_str(t->dst));
    for (i = 1; i <= t->ttl && row < h; i++) {
        nl_node *who = t->hop[i].state != 1 ? NULL
                     : t->v6 ? nl_node_by_ip6(&t->hop[i].from6) : nl_node_by_ip(t->hop[i].from);
        if (t->hop[i].state == 1)
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%2d  " T_INFO "%-15s" T_RESET " %-8s %.2f ms", i,
                      t->v6 ? nl_ip6_str(&t->hop[i].from6) : nl_ip_str(t->hop[i].from), who ? who->name : "",
                      t->hop[i].ms);
        else
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%2d  %s", i, t->hop[i].state == 2 ? "*" : "…");
    }
    return row;
}

static const nl_module tr_mod = {
    .name = "traceroute", .title = N_("traceroute — which routers does a packet cross?"), .layer = NL_L_NET,
    .usage = N_("run NODE traceroute DEST [maxhops=16] [v6=1]"),
    .theory = N_("traceroute sends probes with TTL 1, 2, 3, ... Every router decrements the TTL; the "
                 "one that brings it to zero discards the packet and reports back with an ICMP Time "
                 "Exceeded message, revealing its address. The probe that finally reaches the "
                 "destination is answered directly, and the list of hops is complete."),
    .run = tr_run, .stop = tr_stop, .panel = tr_panel,
};
NL_MODULE(tr_mod)

/* ------------------------------------------------------------------ */
/* UDP echo server and client                                         */
/* ------------------------------------------------------------------ */

typedef struct { int port; unsigned served; } echo_state;
static const nl_module echo_mod;

static void echo_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    echo_state *e = ctx;
    char text[40];

    e->served++;
    tui_payload(m->data, (size_t)m->len, text, sizeof(text), 24);
    nl_log(n, NL_L_APP, 2, m->pkt_id, T_FIN "echo" T_RESET " %s %s:%u «%s» → %s", _("from"), nl_ip_str(m->src),
           m->sport, text, _("sent back"));
    nl_count(n, "echo.served", 1);
    nl_udp_send(n, (uint16_t)e->port, m->src, m->sport, m->data, m->len);
}

static void echo_rx6(nl_node *n, void *ctx, const nl_udp6msg *m)
{
    echo_state *e = ctx;
    char text[40];

    e->served++;
    tui_payload(m->data, (size_t)m->len, text, sizeof(text), 24);
    nl_log(n, NL_L_APP, 2, m->pkt_id, T_FIN "echo" T_RESET " %s [%s]:%u «%s» → %s", _("from"), nl_ip6_str(&m->src),
           m->sport, text, _("sent back"));
    nl_count(n, "echo.served", 1);
    nl_udp6_send(n, (uint16_t)e->port, &m->src, m->sport, m->data, m->len);
}

static int echo_run(nl_node *n, const nl_args *a)
{
    echo_state *e = nl_mod_get(n, &echo_mod);

    if (e)
        return 0;
    e = calloc(1, sizeof(*e));
    if (!e)
        return -1;
    e->port = (int)nl_arg_int(a, "port", 7);
    nl_mod_set(n, &echo_mod, e);
    nl_udp_bind(n, (uint16_t)e->port, echo_rx, e);
    nl_udp6_bind(n, (uint16_t)e->port, echo_rx6, e);
    nl_log(n, NL_L_APP, 2, 0, T_FIN "echo" T_RESET " %s %d", _("server listening on UDP port"), e->port);
    return 0;
}

static void echo_stop(nl_node *n)
{
    echo_state *e = nl_mod_get(n, &echo_mod);

    if (!e)
        return;
    nl_udp_unbind(n, (uint16_t)e->port);
    nl_udp6_unbind(n, (uint16_t)e->port);
    nl_mod_set(n, &echo_mod, NULL);
    free(e);
}

static int echo_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    echo_state *e = nl_mod_get(n, &echo_mod);

    (void)h;
    (void)detail;
    nl_ui_kv(x, y, w, _("UDP port"), "%d", e->port);
    nl_ui_kv(x, y + 1, w, _("datagrams echoed"), "%u", e->served);
    return 2;
}

static const nl_module echo_mod = {
    .name = "echo", .title = N_("UDP echo server"), .layer = NL_L_APP,
    .usage = N_("run NODE echo [port=7]"),
    .theory = N_("The simplest possible service: every datagram received is sent back to its sender. "
                 "UDP adds only ports and a checksum to IP: no connection, no retransmission, no order."),
    .run = echo_run, .stop = echo_stop, .panel = echo_panel,
};
NL_MODULE(echo_mod)

typedef struct {
    nl_node *n;
    nl_ip    dst;
    nl_ip6   dst6;
    int      v6;
    int      dport, sport, count, sent, recv, size;
    nl_time  interval, t_last;
    char     msg[200];
    nl_ev    timer;
} udpc_state;
static const nl_module udpc_mod;

static void udpc_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    udpc_state *u = ctx;
    char text[40];

    u->recv++;
    tui_payload(m->data, (size_t)m->len, text, sizeof(text), 24);
    nl_count(n, "udp.reply", 1);
    nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "udp: %s" T_RESET " %s %s:%u «%s» (%d B, %.2f ms)", _("answer"),
           _("from"), nl_ip_str(m->src), m->sport, text, m->len, (nl_now() - u->t_last) / 1000.0);
}

static void udpc_rx6(nl_node *n, void *ctx, const nl_udp6msg *m)
{
    udpc_state *u = ctx;
    char text[40];

    u->recv++;
    tui_payload(m->data, (size_t)m->len, text, sizeof(text), 24);
    nl_count(n, "udp.reply", 1);
    nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "udp: %s" T_RESET " %s [%s]:%u «%s» (%d B, %.2f ms)", _("answer"),
           _("from"), nl_ip6_str(&m->src), m->sport, text, m->len, (nl_now() - u->t_last) / 1000.0);
}

static void udpc_tick(void *ctx)
{
    udpc_state *u = ctx;
    static uint8_t buf[8192];
    int len = (int)strlen(u->msg), i;

    u->timer = 0;
    if (u->sent >= u->count)
        return;
    memcpy(buf, u->msg, (size_t)len);
    for (i = len; i < u->size; i++)
        buf[i] = (uint8_t)('a' + i % 26);
    if (u->size > len)
        len = u->size;
    u->sent++;
    u->t_last = nl_now();
    nl_count(u->n, "udp.sent", 1);
    if (u->v6) {
        nl_log(u->n, NL_L_APP, 1, 0, T_INFO "udp" T_RESET " → [%s]:%d «%s» (%d B)", nl_ip6_str(&u->dst6), u->dport,
               u->msg, len);
        nl_udp6_send(u->n, (uint16_t)u->sport, &u->dst6, (uint16_t)u->dport, buf, len);
    } else {
        nl_log(u->n, NL_L_APP, 1, 0, T_INFO "udp" T_RESET " → %s:%d «%s» (%d B)", nl_ip_str(u->dst), u->dport,
               u->msg, len);
        nl_udp_send(u->n, (uint16_t)u->sport, u->dst, (uint16_t)u->dport, buf, len);
    }
    u->timer = nl_after(u->n, u->interval, udpc_tick, u);
}

static int udpc_run(nl_node *n, const nl_args *a)
{
    udpc_state *u = nl_mod_get(n, &udpc_mod);
    const char *target = nl_arg_pos(a, 0);
    nl_ip dst = 0;
    nl_ip6 dst6;
    int family = resolve_target(a, target, &dst, &dst6);

    if (!family) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "udp: %s '%s'", _("unknown destination"), target ? target : "");
        return -1;
    }
    if (!u) {
        u = calloc(1, sizeof(*u));
        if (!u)
            return -1;
        u->n = n;
        u->sport = nl_udp_bind(n, 0, udpc_rx, u);
        nl_udp6_bind(n, (uint16_t)u->sport, udpc_rx6, u);
        nl_mod_set(n, &udpc_mod, u);
    }
    nl_cancel(&u->timer);
    u->dst = dst;
    u->dst6 = dst6;
    u->v6 = family == 6;
    u->dport = (int)nl_arg_int(a, "port", 7);
    u->count = (int)nl_arg_int(a, "count", 1);
    u->size = (int)nl_arg_int(a, "size", 0);
    if (u->size > 8000)
        u->size = 8000;
    u->interval = nl_arg_time(a, "interval", NL_SEC(1));
    u->sent = 0;
    snprintf(u->msg, sizeof(u->msg), "%s", nl_arg_str(a, "msg", "hello"));
    udpc_tick(u);
    return 0;
}

static void udpc_stop(nl_node *n)
{
    udpc_state *u = nl_mod_get(n, &udpc_mod);

    if (!u)
        return;
    nl_cancel(&u->timer);
    nl_udp_unbind(n, (uint16_t)u->sport);
    nl_udp6_unbind(n, (uint16_t)u->sport);
    nl_mod_set(n, &udpc_mod, NULL);
    free(u);
}

static const nl_module udpc_mod = {
    .name = "udp", .title = N_("UDP client: send datagrams and show the answers"), .layer = NL_L_TRANSPORT,
    .usage = N_("run NODE udp DEST [port=7] [msg=hello] [count=1] [interval=1s] [size=N] [v6=1]\n"
                "  size pads the datagram to N bytes (try more than the MTU to see fragmentation)."),
    .run = udpc_run, .stop = udpc_stop,
};
NL_MODULE(udpc_mod)

static void dis_echo(nl_dissect *o, const uint8_t *d, int len, int off)
{
    char text[64];

    tui_payload(d, (size_t)len, text, sizeof(text), 40);
    nl_dis_field(o, "Echo", "Text", off, len, "%s", text);
    nl_dis_summary(o, "ECHO", 177, "«%s» (%d bytes)", text, len);
}

/* ------------------------------------------------------------------ */
/* Constant bit rate generator and sink                               */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_node *n;
    nl_ip    dst;
    nl_ip6   dst6;
    int      v6;
    int      port, sport, size;
    double   rate;
    nl_time  until;
    uint32_t seq;
    uint8_t  tos;
    nl_ev    timer;
} cbr_state;
static const nl_module cbr_mod;

static void cbr_tick(void *ctx)
{
    cbr_state *c = ctx;
    static uint8_t buf[2048];
    nl_iptx tx;

    c->timer = 0;
    if (c->until && nl_now() >= c->until) {
        nl_log(c->n, NL_L_APP, 1, 0, T_INFO "cbr" T_RESET " %s: %u %s", _("finished"), c->seq, _("datagrams sent"));
        return;
    }
    memset(buf, 0, (size_t)c->size);
    nl_put32(buf, ++c->seq);
    nl_put32(buf + 4, (uint32_t)(nl_now() >> 32));
    nl_put32(buf + 8, (uint32_t)nl_now());
    if (c->v6) {
        nl_ip6tx tx6;
        memset(&tx6, 0, sizeof(tx6));
        tx6.dst = c->dst6;
        tx6.tclass = c->tos;
        nl_udp6_send_ex(c->n, &tx6, (uint16_t)c->sport, (uint16_t)c->port, buf, c->size);
    } else {
        memset(&tx, 0, sizeof(tx));
        tx.dst = c->dst;
        tx.tos = c->tos;
        nl_udp_send_ex(c->n, &tx, (uint16_t)c->sport, (uint16_t)c->port, buf, c->size);
    }
    nl_count(c->n, "cbr.sent", 1);
    c->timer = nl_after(c->n, (nl_time)(c->size * 8.0 * 1e6 / c->rate), cbr_tick, c);
}

static int cbr_run(nl_node *n, const nl_args *a)
{
    cbr_state *c = nl_mod_get(n, &cbr_mod);
    const char *target = nl_arg_pos(a, 0);
    nl_ip dst = 0;
    nl_ip6 dst6;
    nl_time dur;
    int family;

    if (target && strcmp(target, "stop") == 0) {
        if (c)
            nl_cancel(&c->timer);
        return 0;
    }
    family = resolve_target(a, target, &dst, &dst6);
    if (!family) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "cbr: %s '%s'", _("unknown destination"), target ? target : "");
        return -1;
    }
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c)
            return -1;
        c->n = n;
        c->sport = 40000 + n->idx;
        nl_mod_set(n, &cbr_mod, c);
    }
    nl_cancel(&c->timer);
    c->dst = dst;
    c->dst6 = dst6;
    c->v6 = family == 6;
    c->port = (int)nl_arg_int(a, "port", 9000);
    c->size = (int)nl_arg_int(a, "size", 500);
    if (c->size < 12) c->size = 12;
    if (c->size > 1472) c->size = 1472;
    c->rate = nl_arg_rate(a, "rate", 1e6);
    if (c->rate < 100)
        c->rate = 100;
    c->tos = (uint8_t)nl_arg_int(a, "tos", 0);
    dur = nl_arg_time(a, "duration", 0);
    c->until = dur ? nl_now() + dur : 0;
    nl_log(n, NL_L_APP, 1, 0, T_INFO "cbr" T_RESET " → %s:%d  %s, %d B/datagram",
           c->v6 ? nl_ip6_str(&dst6) : nl_ip_str(dst), c->port,
           nl_rate_str(c->rate), c->size);
    cbr_tick(c);
    return 0;
}

static void cbr_stop(nl_node *n)
{
    cbr_state *c = nl_mod_get(n, &cbr_mod);

    if (!c)
        return;
    nl_cancel(&c->timer);
    nl_mod_set(n, &cbr_mod, NULL);
    free(c);
}

static const nl_module cbr_mod = {
    .name = "cbr", .title = N_("Constant-bit-rate UDP traffic generator"), .layer = NL_L_APP,
    .usage = N_("run NODE cbr DEST [rate=1M] [size=500] [port=9000] [duration=10s] [tos=N] [v6=1]\n"
                "run NODE cbr stop\n"
                "  Pair it with `run DEST sink` to measure throughput, loss and delay."),
    .run = cbr_run, .stop = cbr_stop,
};
NL_MODULE(cbr_mod)

typedef struct {
    nl_node *n;
    int      port;
    uint32_t max_seq, recv, reordered;
    uint64_t bytes, win_bytes;
    double   delay_sum, jitter, last_delay;
    nl_ev    timer;
} sink_state;
static const nl_module sink_mod;

static void sink_tick(void *ctx)
{
    sink_state *s = ctx;

    nl_sample(s->n, "rx (kbit/s)", s->win_bytes * 8.0 / 1000.0 * 2.0);
    s->win_bytes = 0;
    s->timer = nl_after(s->n, NL_MS(500), sink_tick, s);
}

static void sink_data(nl_node *n, sink_state *s, const uint8_t *data, int len);

static void sink_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    sink_data(n, ctx, m->data, m->len);
}

static void sink_rx6(nl_node *n, void *ctx, const nl_udp6msg *m)
{
    sink_data(n, ctx, m->data, m->len);
}

static void sink_data(nl_node *n, sink_state *s, const uint8_t *data, int len)
{
    uint32_t seq;
    double delay;

    if (len < 12)
        return;
    seq = nl_get32(data);
    delay = (double)(nl_now() - (((nl_time)nl_get32(data + 4) << 32) | nl_get32(data + 8))) / 1000.0;
    if (seq < s->max_seq)
        s->reordered++;
    else
        s->max_seq = seq;
    if (s->recv)
        s->jitter += ((delay > s->last_delay ? delay - s->last_delay : s->last_delay - delay) - s->jitter) / 16.0;
    s->last_delay = delay;
    s->delay_sum += delay;
    s->recv++;
    s->bytes += (uint64_t)len;
    s->win_bytes += (uint64_t)len;
    nl_count_set(n, "sink.received", s->recv);
    nl_count_set(n, "sink.lost", s->max_seq - s->recv);
    nl_count_set(n, "sink.delay_ms", s->delay_sum / s->recv);
}

static int sink_run(nl_node *n, const nl_args *a)
{
    sink_state *s = nl_mod_get(n, &sink_mod);

    if (s)
        return 0;
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->n = n;
    s->port = (int)nl_arg_int(a, "port", 9000);
    nl_mod_set(n, &sink_mod, s);
    nl_udp_bind(n, (uint16_t)s->port, sink_rx, s);
    nl_udp6_bind(n, (uint16_t)s->port, sink_rx6, s);
    s->timer = nl_after(n, NL_MS(500), sink_tick, s);
    return 0;
}

static void sink_stop(nl_node *n)
{
    sink_state *s = nl_mod_get(n, &sink_mod);

    if (!s)
        return;
    nl_cancel(&s->timer);
    nl_udp_unbind(n, (uint16_t)s->port);
    nl_udp6_unbind(n, (uint16_t)s->port);
    nl_mod_set(n, &sink_mod, NULL);
    free(s);
}

static void sink_recover(nl_node *n)
{
    sink_state *s = nl_mod_get(n, &sink_mod);

    if (s)
        s->timer = nl_after(n, NL_MS(500), sink_tick, s);
}

static int sink_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    sink_state *s = nl_mod_get(n, &sink_mod);
    int row = 0;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("received"), "%u %s (%llu B)", s->recv, _("datagrams"),
                    (unsigned long long)s->bytes);
    row += nl_ui_kv(x, y + row, w, _("lost"), "%u (%.1f%%)", s->max_seq - s->recv,
                    s->max_seq ? 100.0 * (s->max_seq - s->recv) / s->max_seq : 0.0);
    row += nl_ui_kv(x, y + row, w, _("out of order"), "%u", s->reordered);
    row += nl_ui_kv(x, y + row, w, _("mean delay / jitter"), "%.2f / %.2f ms",
                    s->recv ? s->delay_sum / s->recv : 0.0, s->jitter);
    if (h - row >= 5)
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "rx (kbit/s)"), C_ACK, "rx (kbit/s)");
    return h;
}

static const nl_module sink_mod = {
    .name = "sink", .title = N_("Traffic sink: throughput, loss, delay and jitter of a cbr flow"),
    .layer = NL_L_APP,
    .usage = N_("run NODE sink [port=9000]"),
    .run = sink_run, .stop = sink_stop, .recover = sink_recover, .panel = sink_panel,
};
NL_MODULE(sink_mod)

NL_INIT(apps)
{
    nl_dissector_udp(7, dis_echo);
}
