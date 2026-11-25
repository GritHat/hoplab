/*
 * rip.c - RIP version 2 (RFC 2453): distance-vector routing over UDP 520.
 *
 * Real message format (command, version, 20-byte entries with address
 * family, route tag, address, mask, next hop and metric), periodic and
 * triggered updates, route timeout and garbage collection, split horizon
 * and poisoned reverse. Each protection can be switched off to reproduce
 * count-to-infinity.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIP_PORT   520
#define RIP_GROUP  NL_IP(224, 0, 0, 9)
#define RIP_INF    16
#define RIP_MAX    128
#define RIP_COLOR  150

enum { RS_FREE, RS_VALID, RS_GARBAGE };

typedef struct {
    int     state;
    nl_ip   dst;
    int     plen;
    nl_ip   nh;                 /* 0 for local routes */
    int     ifx;
    int     metric;             /* hops as stored: 0 = attached here, 16 = unreachable */
    int     local;              /* connected or redistributed */
    int     changed;            /* to be sent in the next triggered update */
    nl_time t;                  /* last refresh, or start of garbage collection */
} rip_route;

typedef struct {
    nl_node  *n;
    nl_time   update, timeout, gc;
    int       split, poison, triggered, redist, jitter;
    nl_time   phase;              /* fixed offset of the first periodic update, or -1 */
    rip_route r[RIP_MAX];
    int       if_up[NL_MAX_IF];
    nl_ev     upd_ev, tick_ev, trig_ev;
    unsigned  sent, rcvd, ntrig, counting;
} rip_state;

static const nl_module rip_mod;

static int mask_len(nl_ip mask)
{
    int n = 0;

    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

static rip_route *rip_find(rip_state *s, nl_ip dst, int plen, int create)
{
    rip_route *slot = NULL;
    int i;

    for (i = 0; i < RIP_MAX; i++) {
        if (s->r[i].state != RS_FREE && s->r[i].dst == dst && s->r[i].plen == plen)
            return &s->r[i];
        if (s->r[i].state == RS_FREE && !slot)
            slot = &s->r[i];
    }
    if (!create || !slot)
        return NULL;
    memset(slot, 0, sizeof(*slot));
    slot->dst = dst;
    slot->plen = plen;
    return slot;
}

/* ---- sending ---- */

static void rip_send_table(rip_state *s, int ifx, int only_changed)
{
    uint8_t b[4 + 25 * 20];
    nl_iptx tx;
    int i, ne = 0;

    memset(&tx, 0, sizeof(tx));
    tx.dst = RIP_GROUP;
    tx.ttl = 1;
    tx.ifx = ifx;
    tx.use_if = 1;
    for (i = 0; i <= RIP_MAX; i++) {
        rip_route *r = i < RIP_MAX ? &s->r[i] : NULL;
        int metric;
        uint8_t *e;

        if (!r || ne == 25) {
            if (ne) {
                b[0] = 2;               /* response */
                b[1] = 2;               /* version */
                b[2] = b[3] = 0;
                nl_udp_send_ex(s->n, &tx, RIP_PORT, RIP_PORT, b, 4 + ne * 20);
                s->sent++;
                nl_count(s->n, "rip.updates_sent", 1);
            }
            ne = 0;
            if (!r)
                break;
        }
        if (r->state == RS_FREE || (only_changed && !r->changed))
            continue;
        metric = r->metric + 1 > RIP_INF ? RIP_INF : r->metric + 1;
        if (r->ifx == ifx) {
            /* never tell a network what we learned from it... */
            if (r->local)
                continue;
            if (s->split) {
                if (!s->poison)
                    continue;
                metric = RIP_INF;       /* ...or tell it loudly that we are no way there */
            }
        }
        e = b + 4 + ne++ * 20;
        nl_put16(e, 2);                 /* AF_INET */
        nl_put16(e + 2, 0);
        nl_put32(e + 4, r->dst);
        nl_put32(e + 8, nl_mask(r->plen));
        nl_put32(e + 12, 0);
        nl_put32(e + 16, (uint32_t)metric);
    }
}

static void rip_send_all(rip_state *s, int only_changed)
{
    int i;

    for (i = 0; i < s->n->nif; i++)
        if (s->n->ifc[i].ip && nl_if_usable(s->n, i))
            rip_send_table(s, i, only_changed);
    for (i = 0; i < RIP_MAX; i++)
        s->r[i].changed = 0;
}

static void rip_request(rip_state *s, int ifx)
{
    uint8_t b[24];
    nl_iptx tx;

    memset(b, 0, sizeof(b));
    b[0] = 1;                           /* request for the whole table */
    b[1] = 2;
    nl_put32(b + 20, RIP_INF);
    memset(&tx, 0, sizeof(tx));
    tx.dst = RIP_GROUP;
    tx.ttl = 1;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_udp_send_ex(s->n, &tx, RIP_PORT, RIP_PORT, b, sizeof(b));
}

static void rip_trig_fire(void *ctx)
{
    rip_state *s = ctx;

    s->trig_ev = 0;
    s->ntrig++;
    nl_count(s->n, "rip.triggered", 1);
    nl_log(s->n, NL_L_NET, 2, 0, T_OK "RIP" T_RESET " %s", _("triggered update: only the routes that changed"));
    rip_send_all(s, 1);
}

static void rip_changed(rip_state *s)
{
    if (s->triggered && !s->trig_ev)
        s->trig_ev = nl_after(s->n, NL_MS(100) + (nl_time)nl_rand(400000), rip_trig_fire, s);
}

static void rip_periodic(void *ctx)
{
    rip_state *s = ctx;

    nl_log(s->n, NL_L_NET, 3, 0, T_OK "RIP" T_RESET " %s", _("periodic update: the whole table to every neighbor"));
    rip_send_all(s, 0);
    /* a little jitter keeps routers from synchronizing */
    s->upd_ev = nl_after(s->n, s->jitter ? s->update - s->update / 10 +
                         (nl_time)(nl_rand01() * (double)s->update / 5) : s->update, rip_periodic, s);
}

/* ---- table maintenance ---- */

static void rip_unreachable(rip_state *s, rip_route *r, const char *why)
{
    if (r->state != RS_VALID)
        return;
    nl_log(s->n, NL_L_NET, 1, 0, T_WARN "RIP" T_RESET " %s/%d %s (%s)", nl_ip_str(r->dst), r->plen,
           _("is now unreachable: metric 16"), why);
    if (!r->local)
        nl_route_del(s->n, r->dst, r->plen, NL_RT_RIP);
    r->state = RS_GARBAGE;
    r->metric = RIP_INF;
    r->t = nl_now();
    r->changed = 1;
    rip_changed(s);
}

static void rip_local(rip_state *s, nl_ip net, int plen, int ifx)
{
    rip_route *r = rip_find(s, net, plen, 1);

    if (!r || (r->state == RS_VALID && r->local && r->ifx == ifx))
        return;
    if (r->state == RS_VALID && !r->local)
        nl_route_del(s->n, net, plen, NL_RT_RIP);
    r->state = RS_VALID;
    r->local = 1;
    r->nh = 0;
    r->ifx = ifx;
    r->metric = 0;
    r->t = nl_now();
    r->changed = 1;
    rip_changed(s);
}

static void rip_tick(void *ctx)
{
    rip_state *s = ctx;
    nl_node *n = s->n;
    nl_time now = nl_now();
    int i, k, valid = 0;

    for (i = 0; i < n->nif; i++) {
        int up = n->ifc[i].ip && nl_if_usable(n, i);

        if (up) {
            rip_local(s, n->ifc[i].ip & nl_mask(n->ifc[i].plen), n->ifc[i].plen, i);
            if (!s->if_up[i])
                rip_request(s, i);
        } else if (s->if_up[i]) {
            for (k = 0; k < RIP_MAX; k++)
                if (s->r[k].state == RS_VALID && s->r[k].ifx == i)
                    rip_unreachable(s, &s->r[k], _("interface down"));
        }
        s->if_up[i] = up;
    }
    if (s->redist)
        for (i = 0; i < n->nrt; i++)
            if (n->rt[i].proto == NL_RT_STATIC && nl_if_usable(n, n->rt[i].ifx))
                rip_local(s, n->rt[i].dst, n->rt[i].plen, n->rt[i].ifx);
    for (i = 0; i < RIP_MAX; i++) {
        rip_route *r = &s->r[i];

        if (r->state == RS_VALID && !r->local && now - r->t > s->timeout)
            rip_unreachable(s, r, _("no update heard: timeout"));
        else if (r->state == RS_GARBAGE && now - r->t > s->gc)
            r->state = RS_FREE;
        if (r->state == RS_VALID)
            valid++;
    }
    nl_count_set(n, "rip.routes", valid);
    s->tick_ev = nl_after(n, NL_SEC(1), rip_tick, s);
}

/* ---- receiving ---- */

static void rip_entry(rip_state *s, const nl_udpmsg *m, nl_ip dst, int plen, int metric)
{
    rip_route *r = rip_find(s, dst, plen, 0);

    if (!r) {
        if (metric >= RIP_INF || !(r = rip_find(s, dst, plen, 1)))
            return;
    } else if (r->local && r->state == RS_VALID) {
        return;
    } else if (r->nh == m->src && r->ifx == m->in_if && !r->local) {
        /* news from the router we already use: believe it, better or worse */
        if (metric >= RIP_INF) {
            rip_unreachable(s, r, _("the next hop says so"));
            return;
        }
        if (r->state == RS_VALID && metric == r->metric) {
            r->t = nl_now();
            return;
        }
        if (r->state == RS_VALID && metric > r->metric) {
            s->counting++;
            nl_count(s->n, "rip.counting", 1);
            nl_log(s->n, NL_L_NET, 1, m->pkt_id, T_ERR "RIP" T_RESET " %s/%d: metric %d → %d %s %s", nl_ip_str(dst),
                   plen, r->metric, metric, _("via"), nl_ip_str(m->src));
        }
    } else if (metric >= r->metric) {
        return;                         /* another router, not better */
    }
    if (r->state != RS_VALID || r->nh != m->src)
        nl_log(s->n, NL_L_NET, 1, m->pkt_id, T_OK "RIP" T_RESET " %s/%d %s %s, %d %s", nl_ip_str(dst), plen,
               _("reachable via"), nl_ip_str(m->src), metric, _("hops"));
    r->state = RS_VALID;
    r->local = 0;
    r->nh = m->src;
    r->ifx = m->in_if;
    r->metric = metric;
    r->t = nl_now();
    r->changed = 1;
    nl_route_set(s->n, dst, plen, m->src, m->in_if, metric, NL_RT_RIP);
    rip_changed(s);
}

static void rip_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    rip_state *s = ctx;
    const nl_iface *f;
    int i, ne;

    if (m->in_if < 0 || m->len < 4 || m->data[1] != 2 || nl_ip_is_local(n, m->src))
        return;
    f = &n->ifc[m->in_if];
    if (!f->ip || ((f->ip ^ m->src) & nl_mask(f->plen)) != 0)
        return;                         /* not a neighbor on this network */
    ne = (m->len - 4) / 20;
    if (m->data[0] == 1) {
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_OK "RIP" T_RESET " %s %s", _("table requested by"), nl_ip_str(m->src));
        rip_send_table(s, m->in_if, 0);
        return;
    }
    if (m->data[0] != 2)
        return;
    s->rcvd++;
    nl_count(n, "rip.updates_rcvd", 1);
    nl_log(n, NL_L_NET, 3, m->pkt_id, T_OK "RIP" T_RESET " response %s %s: %d %s", _("from"), nl_ip_str(m->src), ne,
           _("routes"));
    for (i = 0; i < ne; i++) {
        const uint8_t *e = m->data + 4 + i * 20;
        uint32_t metric = nl_get32(e + 16);

        if (nl_get16(e) != 2 || metric < 1 || metric > RIP_INF)
            continue;
        rip_entry(s, m, nl_get32(e + 4) & nl_get32(e + 8), mask_len(nl_get32(e + 8)), (int)metric);
    }
}

/* ---- module ---- */

static void rip_arm(rip_state *s)
{
    s->tick_ev = nl_after(s->n, 0, rip_tick, s);
    s->upd_ev = nl_after(s->n, s->phase >= 0 ? s->phase : NL_MS(50) + (nl_time)nl_rand(200000),
                         rip_periodic, s);
}

static int rip_run(nl_node *n, const nl_args *a)
{
    rip_state *s = nl_mod_get(n, &rip_mod);
    int fresh = !s;

    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        s->n = n;
        s->update = NL_SEC(30);
        s->split = s->triggered = s->jitter = 1;
        s->phase = -1;
        nl_mod_set(n, &rip_mod, s);
        nl_udp_bind(n, RIP_PORT, rip_rx, s);
        n->forward = 1;
    }
    s->update = nl_arg_time(a, "update", s->update);
    if (s->update < NL_MS(100))
        s->update = NL_MS(100);
    s->timeout = nl_arg_time(a, "timeout", s->update * 6);
    s->gc = nl_arg_time(a, "gc", s->update * 4);
    s->split = (int)nl_arg_int(a, "split", s->split);
    s->poison = (int)nl_arg_int(a, "poison", s->poison);
    s->triggered = (int)nl_arg_int(a, "triggered", s->triggered);
    s->redist = (int)nl_arg_int(a, "redistribute", s->redist);
    s->jitter = (int)nl_arg_int(a, "jitter", s->jitter);
    s->phase = nl_arg_time(a, "phase", s->phase);
    if (fresh) {
        nl_log(n, NL_L_NET, 2, 0, T_OK "RIP" T_RESET " %s %s, split horizon %s, poisoned reverse %s, triggered %s",
               _("started: update every"), nl_time_str(s->update), s->split ? "on" : "off",
               s->poison ? "on" : "off", s->triggered ? "on" : "off");
        rip_arm(s);
    }
    return 0;
}

static void rip_cancel(rip_state *s)
{
    nl_cancel(&s->tick_ev);
    nl_cancel(&s->upd_ev);
    nl_cancel(&s->trig_ev);
}

static void rip_stop(nl_node *n)
{
    rip_state *s = nl_mod_get(n, &rip_mod);

    if (!s)
        return;
    rip_cancel(s);
    nl_udp_unbind(n, RIP_PORT);
    nl_route_flush(n, NL_RT_RIP);
    nl_mod_set(n, &rip_mod, NULL);
    free(s);
}

static void rip_crash(nl_node *n)
{
    rip_state *s = nl_mod_get(n, &rip_mod);

    rip_cancel(s);
    memset(s->r, 0, sizeof(s->r));
    memset(s->if_up, 0, sizeof(s->if_up));
    nl_route_flush(n, NL_RT_RIP);
}

static void rip_recover(nl_node *n)
{
    rip_arm(nl_mod_get(n, &rip_mod));
}

static int rip_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    rip_state *s = nl_mod_get(n, &rip_mod);
    int i, row = 0;

    row += nl_ui_kv(x, y + row, w, _("update / timeout / gc"), "%s / %s / %s", nl_time_str(s->update),
                    nl_time_str(s->timeout), nl_time_str(s->gc));
    row += nl_ui_kv(x, y + row, w, _("protections"), "split horizon %s · poisoned reverse %s · triggered %s",
                    s->split ? "on" : "off", s->poison ? "on" : "off", s->triggered ? "on" : "off");
    if (detail >= 2)
        row += nl_ui_kv(x, y + row, w, _("updates sent / heard"), "%u / %u  (%u triggered, %u metric climbs)",
                        s->sent, s->rcvd, s->ntrig, s->counting);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-18s %-6s %-15s %-5s %s", _("network"), _("metric"),
                  _("next hop"), _("dev"), _("age"));
    for (i = 0; i < RIP_MAX && row < h; i++) {
        rip_route *r = &s->r[i];
        char net[24];

        if (r->state == RS_FREE)
            continue;
        snprintf(net, sizeof(net), "%s/%d", nl_ip_str(r->dst), r->plen);
        tui_print(x, y + row++, w, TS(r->state == RS_VALID ? C_TEXT : C_ERR, C_DEFAULT, 0),
                  "%-18s %-6d %-15s %-5s %s", net, r->metric, r->local ? _("attached") : nl_ip_str(r->nh),
                  n->ifc[r->ifx].name,
                  r->local ? "" : r->state == RS_GARBAGE ? _("unreachable") : nl_time_str(nl_now() - r->t));
    }
    return row;
}

static const nl_module rip_mod = {
    .name = "rip", .title = N_("RIP — distance-vector routing (RFC 2453)"), .layer = NL_L_NET,
    .usage = N_("run ROUTER rip [update=30s] [timeout=6×update] [gc=4×update]\n"
                "               [split=1] [poison=0] [triggered=1] [redistribute=0]\n"
                "               [phase=TIME] [jitter=1]\n"
                "  split=0      disable split horizon (to reproduce count-to-infinity)\n"
                "  poison=1     split horizon with poisoned reverse\n"
                "  triggered=0  no triggered updates: news travel only with the periodic ones\n"
                "  redistribute=1  also advertise static routes\n"
                "  phase=T jitter=0  first periodic update at T and no random jitter: makes the\n"
                "               order of the updates of different routers repeatable"),
    .theory = N_("RIP is the classic distance-vector protocol. Every router keeps, for each network, the "
                 "distance in hops and the neighbor to go through, and every 30 seconds tells all its "
                 "neighbors the whole table. A router that hears \"I reach X in 3 hops\" concludes that it "
                 "reaches X in 4 through that neighbor, and keeps the shortest offer (Bellman-Ford, run "
                 "by the network as a whole). Nobody knows the topology: only distances and directions.\n\n"
                 "The weakness is bad news. When a network disappears, a router may hear from a neighbor a "
                 "route that actually passed through itself, believe it, and advertise it back one hop "
                 "longer: the two bounce the route between them while the metric climbs one step per "
                 "update until it reaches 16, which means infinity. This is count-to-infinity, and it is "
                 "why RIP networks cannot be more than 15 hops wide. Split horizon (do not advertise a "
                 "route on the interface it was learned from), poisoned reverse (advertise it there with "
                 "metric 16) and triggered updates (send changes immediately instead of waiting for the "
                 "timer) remove the two-router loop and speed convergence up; loops among three or more "
                 "routers remain possible. A route not refreshed for 180 seconds times out, is advertised "
                 "as unreachable for a while (garbage collection) and then forgotten.\n\n"
                 "Here: RIPv2 messages with the real format on UDP port 520, multicast to 224.0.0.9. "
                 "Authentication and route tags are not implemented."),
    .run = rip_run, .stop = rip_stop, .crash = rip_crash, .recover = rip_recover, .panel = rip_panel,
};
NL_MODULE(rip_mod)

static void dis_rip(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int ne = (len - 4) / 20, i, inf = 0;

    if (len < 4)
        return;
    nl_dis_field(o, "RIP", "Command", off, 1, "%d (%s)", d[0], d[0] == 1 ? "request" : "response");
    nl_dis_field(o, "RIP", "Version", off + 1, 1, "%d", d[1]);
    for (i = 0; i < ne; i++) {
        const uint8_t *e = d + 4 + i * 20;
        uint32_t metric = nl_get32(e + 16);

        if (metric >= RIP_INF)
            inf++;
        nl_dis_field(o, "RIP", "Route", off + 4 + i * 20, 20, "%s/%d metric %u%s", nl_ip_str(nl_get32(e + 4)),
                     mask_len(nl_get32(e + 8)), metric, metric >= RIP_INF ? " (unreachable)" : "");
    }
    if (d[0] == 1)
        nl_dis_summary(o, "RIP", RIP_COLOR, "Request: send me your table");
    else
        nl_dis_summary(o, "RIP", RIP_COLOR, "Response: %d routes%s", ne, inf ? ", some unreachable (16)" : "");
}

NL_INIT(rip)
{
    nl_dissector_udp(RIP_PORT, dis_rip);
}
