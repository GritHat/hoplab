/*
 * stp.c - Spanning Tree Protocol (IEEE 802.1D) for switch nodes.
 *
 * Bridges exchange configuration BPDUs, elect the bridge with the lowest
 * identifier as root, keep on every bridge the port with the cheapest path
 * to the root (root port) and on every segment the port of the bridge
 * closest to the root (designated port); every other port is blocked, which
 * removes the loops. Ports move blocking -> listening -> learning ->
 * forwarding with the forward-delay timer.
 *
 * Simplifications, stated honestly:
 *  - BPDUs travel in Ethernet II frames with a private EtherType (0x4242)
 *    to 01:80:c2:00:00:00; real STP uses 802.3 + LLC (DSAP/SSAP 0x42). The
 *    BPDU body has the real 35-byte layout.
 *  - Every bridge sends its current information on its designated ports at
 *    each hello (as RSTP does) instead of only relaying the root's BPDUs;
 *    the message age still grows hop by hop and with the time the
 *    information is held, so stale information dies as in 802.1D.
 *  - Topology changes: a TCN BPDU goes up through the root port and the TC
 *    flag comes down in configuration BPDUs; bridges flush their forwarding
 *    table at once instead of shortening its ageing time, and TCNs are
 *    repeated for a while instead of being acknowledged.
 *  - The learning state does not learn addresses (the core bridge neither
 *    learns nor forwards on a port that is not forwarding).
 *  - rapid=1 is only "RSTP flavoured": a new root port forwards at once
 *    after the other ports are put back to blocking (sync); there is no
 *    proposal/agreement handshake. portfast=1 lets ports that face a host
 *    forward immediately.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STP_ETHERTYPE 0x4242
#define STP_COLOR     180
#define BPDU_CONFIG   0x00
#define BPDU_TCN      0x80
#define BPDU_LEN      35

static const uint8_t STP_MAC[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x00 };

enum { ROLE_DISABLED, ROLE_ROOT, ROLE_DESIGNATED, ROLE_BLOCKED };
enum { ST_BLOCKING, ST_LISTENING, ST_LEARNING, ST_FORWARDING };

/* Priority vector: lower is better, compared field by field */
typedef struct {
    uint8_t  root[8];
    uint32_t cost;
    uint8_t  bridge[8];
    uint16_t port;
} stp_vec;

typedef struct stp_state stp_state;

typedef struct {
    stp_state *st;
    int        ifx;
    int        role, state;
    uint32_t   cost;
    int        cost_fixed;
    uint16_t   id;
    int        edge;                    /* faces a host (portfast) */
    int        carrier;
    int        valid;                   /* information heard on this port */
    stp_vec    rx;
    nl_time    rx_age, rx_time;         /* message age in the BPDU, when it arrived */
    nl_time    rx_maxage, rx_hello, rx_fwd;
    int        rx_tc;
    nl_ev      age_timer, state_timer;
    nl_time    tc_until;                /* send TC (designated) / TCN (root port) until then */
    unsigned   bpdu_tx, bpdu_rx;
} stp_port;

struct stp_state {
    nl_node  *n;
    uint8_t   id[8];
    int       priority;
    nl_time   hello, max_age, fwd_delay;        /* configured */
    nl_time   cur_hello, cur_max_age, cur_fwd;  /* in force: the root's */
    int       rapid, portfast;
    uint8_t   root[8];
    uint32_t  root_cost;
    int       root_port;                /* -1 = this bridge is the root */
    nl_ev     hello_timer;
    nl_time   tc_flush_until;
    unsigned  tc_count, root_changes;
    stp_port  port[NL_MAX_IF];
};

static const nl_module stp_mod;
static void stp_recompute(stp_state *s);
static void stp_send_config(stp_state *s, int ifx);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *id_str(const uint8_t id[8])
{
    static char bufs[4][32];
    static int k;
    char *b = bufs[k = (k + 1) % 4];

    snprintf(b, 32, "%u/%s", nl_get16(id), nl_mac_str(id + 2));
    return b;
}

/* Bridge identifier with the name of the node that owns it, when known */
static const char *id_name(const uint8_t id[8])
{
    static char bufs[4][48];
    static int k;
    char *b = bufs[k = (k + 1) % 4];
    int i;

    for (i = 0; i < nl_node_count(); i++) {
        const nl_node *n = nl_node_at(i);
        if (n->nif && memcmp(n->ifc[0].mac, id + 2, 6) == 0) {
            snprintf(b, 48, "%s (%u)", n->name, nl_get16(id));
            return b;
        }
    }
    snprintf(b, 48, "%s", id_str(id));
    return b;
}

static int vec_cmp(const stp_vec *a, const stp_vec *b)
{
    int c = memcmp(a->root, b->root, 8);

    if (c)
        return c;
    if (a->cost != b->cost)
        return a->cost < b->cost ? -1 : 1;
    c = memcmp(a->bridge, b->bridge, 8);
    if (c)
        return c;
    return a->port < b->port ? -1 : a->port > b->port;
}

static const char *role_name(int role)
{
    switch (role) {
    case ROLE_ROOT:       return _("root");
    case ROLE_DESIGNATED: return _("designated");
    case ROLE_BLOCKED:    return _("alternate");
    default:              return _("disabled");
    }
}

static const char *state_name(int state)
{
    switch (state) {
    case ST_LISTENING:  return _("listening");
    case ST_LEARNING:   return _("learning");
    case ST_FORWARDING: return _("forwarding");
    default:            return _("blocking");
    }
}

static uint32_t default_cost(const nl_link *l)
{
    double c = l && l->cfg.bw > 0 ? 1e9 / l->cfg.bw : 1;

    return c < 1 ? 1 : c > 65535 ? 65535 : (uint32_t)(c + 0.5);
}

static void make_id(stp_state *s)
{
    nl_put16(s->id, (uint16_t)s->priority);
    memcpy(s->id + 2, s->n->ifc[0].mac, 6);
}

static void update_counters(stp_state *s)
{
    int i, blocked = 0, fwd = 0;

    for (i = 0; i < s->n->nif; i++) {
        if (s->port[i].role == ROLE_BLOCKED)
            blocked++;
        if (s->port[i].state == ST_FORWARDING)
            fwd++;
    }
    nl_count_set(s->n, "stp.is_root", s->root_port < 0);
    nl_count_set(s->n, "stp.blocked_ports", blocked);
    nl_count_set(s->n, "stp.forwarding_ports", fwd);
    nl_count_set(s->n, "stp.topology_changes", s->tc_count);
    nl_count_set(s->n, "stp.root_changes", s->root_changes);
}

/* ------------------------------------------------------------------ */
/* Topology change                                                    */
/* ------------------------------------------------------------------ */

/* Flush the forwarding table and announce the change on every port except
 * `from` (-1 = detected here) */
static void topology_change(stp_state *s, int from)
{
    nl_time until = nl_now() + 2 * s->cur_hello + s->cur_hello / 2;
    int i;

    if (nl_now() >= s->tc_flush_until) {
        nl_fdb_flush(s->n);
        nl_log(s->n, NL_L_LINK, from < 0 ? 1 : 2, 0, T_WARN "STP" T_RESET " %s",
               from < 0 ? _("topology change detected: forwarding table flushed, notifying the tree")
                        : _("topology change notified: forwarding table flushed"));
    }
    s->tc_flush_until = until;
    if (from < 0) {
        s->tc_count++;
        nl_count_set(s->n, "stp.last_change_ms", nl_now() / 1000.0);
    }
    for (i = 0; i < s->n->nif; i++)
        if (i != from && !s->port[i].edge)
            s->port[i].tc_until = until;
}

/* ------------------------------------------------------------------ */
/* Port states                                                        */
/* ------------------------------------------------------------------ */

static void set_state(stp_port *p, int state)
{
    stp_state *s = p->st;
    nl_iface *f = &s->n->ifc[p->ifx];
    int old = p->state;

    p->state = state;
    f->blocked = state != ST_FORWARDING;
    if (old == state)
        return;
    nl_count_set(s->n, "stp.last_change_ms", nl_now() / 1000.0);
    nl_log(s->n, NL_L_LINK, state == ST_FORWARDING || (state == ST_BLOCKING && old == ST_FORWARDING) ? 1 : 2, 0,
           "%sSTP" T_RESET " %s %s: %s → " T_BOLD "%s" T_RESET " (%s)",
           state == ST_FORWARDING ? T_OK : state == ST_BLOCKING ? T_ERR : T_WARN, _("port"), f->name,
           state_name(old), state_name(state), role_name(p->role));
    if (!p->edge && (state == ST_FORWARDING || old == ST_FORWARDING))
        topology_change(s, -1);
}

static void state_step(void *ctx)
{
    stp_port *p = ctx;

    p->state_timer = 0;
    if (p->state == ST_LISTENING) {
        set_state(p, ST_LEARNING);
        p->state_timer = nl_after(p->st->n, p->st->cur_fwd, state_step, p);
    } else if (p->state == ST_LEARNING) {
        set_state(p, ST_FORWARDING);
        update_counters(p->st);
    }
}

static void port_block(stp_port *p)
{
    nl_cancel(&p->state_timer);
    set_state(p, ST_BLOCKING);
}

/* A root or designated port starts (or continues) its way to forwarding */
static void port_unblock(stp_port *p, int at_once)
{
    if (p->state == ST_FORWARDING)
        return;
    if (at_once) {
        nl_cancel(&p->state_timer);
        set_state(p, ST_FORWARDING);
    } else if (p->state == ST_BLOCKING) {
        set_state(p, ST_LISTENING);
        p->state_timer = nl_after(p->st->n, p->st->cur_fwd, state_step, p);
    }
}

/* ------------------------------------------------------------------ */
/* The spanning tree computation                                      */
/* ------------------------------------------------------------------ */

static void stp_recompute(stp_state *s)
{
    nl_node *n = s->n;
    stp_vec best, mine;
    int i, best_port = -1, old_root_port = s->root_port, changed;
    int announce[NL_MAX_IF] = { 0 };
    uint8_t old_root[8];
    uint32_t old_cost = s->root_cost;

    memcpy(old_root, s->root, 8);

    /* 1. the root: the best identifier heard, or this bridge */
    memset(&best, 0, sizeof(best));
    memcpy(best.root, s->id, 8);
    memcpy(best.bridge, s->id, 8);
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];
        stp_vec v;

        if (!p->valid || !p->carrier)
            continue;
        v = p->rx;
        v.cost += p->cost;
        if (memcmp(v.root, s->id, 8) == 0)
            continue;                   /* somebody still believes we are the root */
        /* ties between our own ports go to the better designated port on
         * the other side, then to our lower port number (scan order) */
        if (vec_cmp(&v, &best) < 0) {
            best = v;
            best_port = i;
        }
    }
    s->root_port = best_port;
    memcpy(s->root, best.root, 8);
    s->root_cost = best_port >= 0 ? best.cost : 0;
    if (best_port >= 0) {
        s->cur_hello = s->port[best_port].rx_hello;
        s->cur_max_age = s->port[best_port].rx_maxage;
        s->cur_fwd = s->port[best_port].rx_fwd;
    } else {
        s->cur_hello = s->hello;
        s->cur_max_age = s->max_age;
        s->cur_fwd = s->fwd_delay;
    }
    changed = memcmp(old_root, s->root, 8) != 0 || old_cost != s->root_cost || old_root_port != s->root_port;

    if (memcmp(old_root, s->root, 8) != 0) {
        s->root_changes++;
        if (best_port < 0)
            nl_log(n, NL_L_LINK, 1, 0, T_OK "STP" T_RESET " %s " T_BOLD "%s", _("this bridge is the root:"),
                   id_name(s->id));
        else
            nl_log(n, NL_L_LINK, 1, 0, T_INFO "STP" T_RESET " %s " T_BOLD "%s" T_RESET ", %s %s, %s %u",
                   _("root bridge is"), id_name(s->root), _("root port"), n->ifc[best_port].name, _("cost"),
                   s->root_cost);
    } else if (old_root_port != s->root_port && best_port >= 0) {
        nl_log(n, NL_L_LINK, 1, 0, T_INFO "STP" T_RESET " %s %s (%s %u)", _("new root port"),
               n->ifc[best_port].name, _("cost"), s->root_cost);
    }

    /* 2. the role of every port */
    memcpy(mine.root, s->root, 8);
    mine.cost = s->root_cost;
    memcpy(mine.bridge, s->id, 8);
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];
        int role;

        mine.port = p->id;
        if (!p->carrier)
            role = ROLE_DISABLED;
        else if (i == s->root_port)
            role = ROLE_ROOT;
        else if (!p->valid || vec_cmp(&mine, &p->rx) < 0)
            role = ROLE_DESIGNATED;
        else
            role = ROLE_BLOCKED;
        if (role != p->role) {
            nl_log(n, NL_L_LINK, 2, 0, T_WARN "STP" T_RESET " %s %s: %s → %s", _("port"), n->ifc[i].name,
                   role_name(p->role), role_name(role));
            p->role = role;
            announce[i] = 1;
        }
        /* what a designated port heard is worse than what we say there: forget it */
        if (role == ROLE_DESIGNATED && p->valid) {
            p->valid = 0;
            nl_cancel(&p->age_timer);
        }
    }

    /* 3. states. With rapid=1 a new root port forwards at once, after the
     * other ports gave up forwarding (so that no loop can form) */
    if (s->rapid && s->root_port >= 0 && s->root_port != old_root_port &&
        s->port[s->root_port].state != ST_FORWARDING) {
        for (i = 0; i < n->nif; i++)
            if (i != s->root_port && !s->port[i].edge && s->port[i].state != ST_BLOCKING)
                port_block(&s->port[i]);
        port_unblock(&s->port[s->root_port], 1);
    }
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];

        if (p->role == ROLE_ROOT || p->role == ROLE_DESIGNATED)
            port_unblock(p, p->edge);
        else
            port_block(p);
    }
    update_counters(s);

    /* tell the neighbours at once when what we announce has changed */
    for (i = 0; i < n->nif; i++)
        if (s->port[i].role == ROLE_DESIGNATED && (changed || announce[i]))
            stp_send_config(s, i);
}

/* ------------------------------------------------------------------ */
/* BPDUs                                                              */
/* ------------------------------------------------------------------ */

static uint16_t to_bpdu_time(nl_time t) { return (uint16_t)(t * 256 / 1000000); }
static nl_time from_bpdu_time(uint16_t v) { return (nl_time)v * 1000000 / 256; }

static void stp_send_config(stp_state *s, int ifx)
{
    stp_port *p = &s->port[ifx];
    uint8_t b[BPDU_LEN];
    nl_time age = 0;

    if (!p->carrier)
        return;
    if (s->root_port >= 0) {
        const stp_port *rp = &s->port[s->root_port];
        /* one hop older than what we heard, plus the time we have held it */
        age = rp->rx_age + (nl_now() - rp->rx_time) + NL_SEC(1) / 4;
    }
    memset(b, 0, sizeof(b));
    b[3] = BPDU_CONFIG;
    b[4] = nl_now() < p->tc_until ? 0x01 : 0x00;
    memcpy(b + 5, s->root, 8);
    nl_put32(b + 13, s->root_cost);
    memcpy(b + 17, s->id, 8);
    nl_put16(b + 25, p->id);
    nl_put16(b + 27, to_bpdu_time(age));
    nl_put16(b + 29, to_bpdu_time(s->cur_max_age));
    nl_put16(b + 31, to_bpdu_time(s->cur_hello));
    nl_put16(b + 33, to_bpdu_time(s->cur_fwd));
    p->bpdu_tx++;
    nl_log(s->n, NL_L_LINK, 3, 0, T_DIM "STP → %s: config root %s cost %u%s", s->n->ifc[ifx].name,
           id_name(s->root), s->root_cost, b[4] ? " TC" : "");
    nl_eth_send(s->n, ifx, STP_MAC, STP_ETHERTYPE, b, BPDU_LEN, 0);
}

static void stp_send_tcn(stp_state *s, int ifx)
{
    uint8_t b[4] = { 0, 0, 0, BPDU_TCN };

    s->port[ifx].bpdu_tx++;
    nl_log(s->n, NL_L_LINK, 3, 0, T_DIM "STP → %s: TCN", s->n->ifc[ifx].name);
    nl_eth_send(s->n, ifx, STP_MAC, STP_ETHERTYPE, b, 4, 0);
}

static void age_expired(void *ctx)
{
    stp_port *p = ctx;
    stp_state *s = p->st;

    p->age_timer = 0;
    if (!p->valid)
        return;
    p->valid = 0;
    nl_log(s->n, NL_L_LINK, 1, 0, T_WARN "STP" T_RESET " %s %s: %s (%s %s)", _("port"), s->n->ifc[p->ifx].name,
           _("no BPDU for max-age, information discarded"), _("was"), id_name(p->rx.bridge));
    stp_recompute(s);
}

static void stp_rx(nl_node *n, void *ctx, const nl_ethmeta *m)
{
    stp_state *s = ctx;
    const uint8_t *b = m->payload;
    stp_port *p;
    stp_vec v;
    nl_time age, max_age;
    int same_sender;

    if (m->in_if < 0 || m->in_if >= n->nif || m->len < 4)
        return;
    p = &s->port[m->in_if];
    p->bpdu_rx++;
    if (b[3] == BPDU_TCN) {
        nl_log(n, NL_L_LINK, 3, 0, T_DIM "STP ← %s: TCN", n->ifc[m->in_if].name);
        topology_change(s, m->in_if);
        return;
    }
    if (b[3] != BPDU_CONFIG || m->len < BPDU_LEN)
        return;
    memcpy(v.root, b + 5, 8);
    v.cost = nl_get32(b + 13);
    memcpy(v.bridge, b + 17, 8);
    v.port = nl_get16(b + 25);
    age = from_bpdu_time(nl_get16(b + 27));
    max_age = from_bpdu_time(nl_get16(b + 29));
    nl_log(n, NL_L_LINK, 3, 0, T_DIM "STP ← %s: config root %s cost %u from %s age %s%s", n->ifc[m->in_if].name,
           id_name(v.root), v.cost, id_name(v.bridge), nl_time_str(age), b[4] & 1 ? " TC" : "");
    if (age >= max_age) {
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "STP" T_RESET " %s %s: %s", _("port"), n->ifc[m->in_if].name,
               _("BPDU older than max-age ignored (stale information)"));
        return;
    }
    if (memcmp(v.bridge, s->id, 8) == 0)
        return;                         /* our own BPDU came back */
    same_sender = p->valid && memcmp(p->rx.bridge, v.bridge, 8) == 0 && p->rx.port == v.port;
    /* keep the best information per port; the bridge we already listen to
     * may also make it worse (it lost its path to the root) */
    if (p->valid && !same_sender && vec_cmp(&v, &p->rx) >= 0)
        return;
    if (!p->valid) {
        /* worse than what we announce on this port ourselves: the sender
         * will give way when it hears our next BPDU */
        stp_vec mine;

        memcpy(mine.root, s->root, 8);
        mine.cost = s->root_cost;
        memcpy(mine.bridge, s->id, 8);
        mine.port = p->id;
        if (vec_cmp(&mine, &v) < 0) {
            if (p->role == ROLE_DESIGNATED)
                stp_send_config(s, m->in_if);
            return;
        }
    }
    p->valid = 1;
    p->rx = v;
    p->rx_age = age;
    p->rx_time = nl_now();
    p->rx_maxage = max_age;
    p->rx_hello = from_bpdu_time(nl_get16(b + 31));
    p->rx_fwd = from_bpdu_time(nl_get16(b + 33));
    if (p->rx_hello < NL_MS(50)) p->rx_hello = NL_MS(50);
    if (p->rx_fwd < NL_MS(50)) p->rx_fwd = NL_MS(50);
    nl_cancel(&p->age_timer);
    p->age_timer = nl_after(n, max_age - age, age_expired, p);
    stp_recompute(s);
    /* the TC flag is meaningful when it comes from the direction of the root */
    if ((b[4] & 1) && m->in_if == s->root_port)
        topology_change(s, m->in_if);
}

static void stp_rx_ignore(nl_node *n, void *ctx, const nl_ethmeta *m)
{
    (void)n; (void)ctx; (void)m;
}

static void port_reset(stp_state *s, int i)
{
    stp_port *p = &s->port[i];
    nl_node *peer = nl_if_peer(s->n, i, NULL);

    nl_cancel(&p->age_timer);
    nl_cancel(&p->state_timer);
    p->st = s;
    p->ifx = i;
    p->id = (uint16_t)(0x8000 | (i + 1));
    if (!p->cost_fixed)
        p->cost = default_cost(s->n->ifc[i].link);
    p->edge = s->portfast && peer && peer->kind != NL_SWITCH;
    p->carrier = nl_if_usable(s->n, i);
    p->valid = 0;
    p->role = p->carrier ? ROLE_DESIGNATED : ROLE_DISABLED;
    p->state = ST_BLOCKING;
    p->tc_until = 0;
    s->n->ifc[i].blocked = 1;
}

static void hello_tick(void *ctx)
{
    stp_state *s = ctx;
    nl_node *n = s->n;
    int i, carrier_change = 0;

    s->hello_timer = 0;
    /* a port whose link went down or came back (carrier) */
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];
        int c = nl_if_usable(n, i);

        if (c == p->carrier)
            continue;
        carrier_change = 1;
        nl_log(n, NL_L_LINK, 1, 0, "%sSTP" T_RESET " %s %s: %s", c ? T_OK : T_ERR, _("port"), n->ifc[i].name,
               c ? _("link is up") : _("link is down"));
        if (!c && p->state == ST_FORWARDING && !p->edge) {
            p->carrier = 0;
            p->valid = 0;
            p->role = ROLE_DISABLED;
            port_block(p);
        } else {
            port_reset(s, i);
        }
    }
    if (carrier_change)
        stp_recompute(s);
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];

        if (p->role == ROLE_DESIGNATED)
            stp_send_config(s, i);
        else if (p->role == ROLE_ROOT && nl_now() < p->tc_until)
            stp_send_tcn(s, i);
    }
    s->hello_timer = nl_after(n, s->cur_hello, hello_tick, s);
}

/* ------------------------------------------------------------------ */
/* Module                                                             */
/* ------------------------------------------------------------------ */

static void stp_boot(stp_state *s)
{
    int i;

    make_id(s);
    memcpy(s->root, s->id, 8);
    s->root_cost = 0;
    s->root_port = -1;
    s->cur_hello = s->hello;
    s->cur_max_age = s->max_age;
    s->cur_fwd = s->fwd_delay;
    s->tc_flush_until = 0;
    for (i = 0; i < s->n->nif; i++)
        port_reset(s, i);
    nl_fdb_flush(s->n);
    nl_log(s->n, NL_L_LINK, 1, 0, T_INFO "STP" T_RESET " %s " T_BOLD "%s" T_RESET " — %s", _("bridge"),
           id_str(s->id), _("every port starts blocked; claiming to be the root"));
    stp_recompute(s);
    nl_cancel(&s->hello_timer);
    hello_tick(s);
}

static void stp_config(stp_state *s, const nl_args *a)
{
    s->priority = (int)nl_arg_int(a, "priority", s->priority) & 0xffff;
    s->hello = nl_arg_time(a, "hello", s->hello);
    s->max_age = nl_arg_time(a, "maxage", s->max_age);
    s->fwd_delay = nl_arg_time(a, "fwd", s->fwd_delay);
    s->rapid = (int)nl_arg_int(a, "rapid", s->rapid);
    s->portfast = (int)nl_arg_int(a, "portfast", s->portfast);
    if (s->hello < NL_MS(50)) s->hello = NL_MS(50);
    if (s->fwd_delay < NL_MS(50)) s->fwd_delay = NL_MS(50);
    if (s->max_age < 2 * s->hello) s->max_age = 2 * s->hello;
    if (s->max_age > NL_SEC(250)) s->max_age = NL_SEC(250);
}

static int stp_run(nl_node *n, const nl_args *a)
{
    stp_state *s = nl_mod_get(n, &stp_mod);
    const char *cmd = nl_arg_pos(a, 0);

    if (n->kind != NL_SWITCH) {
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "stp: %s", _("only switches run the spanning tree"));
        return -1;
    }
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        s->n = n;
        s->priority = 32768;
        s->hello = NL_SEC(1);
        s->max_age = NL_SEC(6);
        s->fwd_delay = NL_SEC(3);
        stp_config(s, a);
        nl_mod_set(n, &stp_mod, s);
        nl_eth_bind(n, STP_ETHERTYPE, stp_rx, s);
        stp_boot(s);
        return 0;
    }
    if (cmd && strcmp(cmd, "cost") == 0) {
        const char *port = nl_arg_pos(a, 1), *val = nl_arg_pos(a, 2);
        int ifx = port ? nl_if_find(n, port) : -1;

        if (ifx < 0 || !val || atoi(val) < 1) {
            nl_log(n, NL_L_LINK, 1, 0, T_ERR "stp: %s", _("usage: stp cost PORT N"));
            return -1;
        }
        s->port[ifx].cost = (uint32_t)atoi(val);
        s->port[ifx].cost_fixed = 1;
        nl_log(n, NL_L_LINK, 1, 0, T_INFO "STP" T_RESET " %s %s: %s %u", _("port"), port, _("cost"),
               s->port[ifx].cost);
        stp_recompute(s);
        return 0;
    }
    if (cmd && strcmp(cmd, "priority") == 0 && nl_arg_pos(a, 1)) {
        s->priority = atoi(nl_arg_pos(a, 1)) & 0xffff;
    } else if (cmd) {
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "stp: %s '%s'", _("unknown command"), cmd);
        return -1;
    } else {
        stp_config(s, a);
    }
    /* a new identifier or new timers: start again, as after a reboot */
    stp_boot(s);
    return 0;
}

static void stp_halt(stp_state *s)
{
    int i;

    nl_cancel(&s->hello_timer);
    for (i = 0; i < NL_MAX_IF; i++) {
        nl_cancel(&s->port[i].age_timer);
        nl_cancel(&s->port[i].state_timer);
    }
}

static void stp_stop(nl_node *n)
{
    stp_state *s = nl_mod_get(n, &stp_mod);
    int i;

    if (!s)
        return;
    stp_halt(s);
    for (i = 0; i < n->nif; i++)
        n->ifc[i].blocked = 0;          /* back to a plain bridge: loops are possible again */
    /* an EtherType handler cannot be unbound: leave one that ignores BPDUs */
    nl_eth_bind(n, STP_ETHERTYPE, stp_rx_ignore, NULL);
    nl_mod_set(n, &stp_mod, NULL);
    free(s);
}

static void stp_crash(nl_node *n)
{
    stp_state *s = nl_mod_get(n, &stp_mod);

    if (s)
        stp_halt(s);
}

static void stp_recover(nl_node *n)
{
    stp_state *s = nl_mod_get(n, &stp_mod);

    if (s)
        stp_boot(s);
}

static int stp_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    stp_state *s = nl_mod_get(n, &stp_mod);
    int row = 0, i;

    row += nl_ui_kv(x, y + row, w, _("bridge id"), "%s", id_str(s->id));
    if (s->root_port < 0)
        row += nl_ui_kv(x, y + row, w, _("root bridge"), T_OK T_BOLD "%s", _("this bridge"));
    else
        row += nl_ui_kv(x, y + row, w, _("root bridge"), "%s  " T_DIM "%s" T_RESET " %s  " T_DIM "%s" T_RESET " %u",
                        id_name(s->root), _("via"), n->ifc[s->root_port].name, _("cost"), s->root_cost);
    row += nl_ui_kv(x, y + row, w, _("timers"), "hello %s  max-age %s  fwd %s%s%s", nl_time_str(s->cur_hello),
                    nl_time_str(s->cur_max_age), nl_time_str(s->cur_fwd), s->rapid ? "  rapid" : "",
                    s->portfast ? "  portfast" : "");
    row += nl_ui_kv(x, y + row, w, _("topology changes"), "%u", s->tc_count);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-6s %-11s %-11s %6s  %s", _("port"), _("role"),
                  _("state"), _("cost"), detail >= 2 ? _("designated bridge on the segment") : "");
    for (i = 0; i < n->nif && row < h; i++) {
        const stp_port *p = &s->port[i];
        const char *col = p->state == ST_FORWARDING ? T_OK : p->role == ROLE_DISABLED ? T_DIM
                        : p->state == ST_BLOCKING ? T_ERR : T_WARN;
        char who[64] = "";

        if (detail >= 2) {
            if (p->role == ROLE_DESIGNATED)
                snprintf(who, sizeof(who), "%s", _("this bridge"));
            else if (p->valid)
                snprintf(who, sizeof(who), "%s", id_name(p->rx.bridge));
            if (detail >= 3)
                snprintf(who + strlen(who), sizeof(who) - strlen(who), "  tx %u rx %u", p->bpdu_tx, p->bpdu_rx);
        }
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%-6s %-11s %s%-11s" T_RESET " %6u  " T_DIM "%s",
                  n->ifc[i].name, role_name(p->role), col, state_name(p->state), p->cost, who);
    }
    return row;
}

static void stp_dissect(nl_dissect *o, const uint8_t *d, int len, int off)
{
    if (len >= 4 && d[3] == BPDU_TCN) {
        nl_dis_field(o, "STP", "BPDU type", off + 3, 1, "0x80 (topology change notification)");
        nl_dis_summary(o, "STP", STP_COLOR, "Topology Change Notification");
        return;
    }
    if (len < BPDU_LEN) {
        nl_dis_summary(o, "STP", STP_COLOR, "truncated BPDU");
        return;
    }
    nl_dis_field(o, "STP", "Protocol / Version", off, 3, "%u / %u", nl_get16(d), d[2]);
    nl_dis_field(o, "STP", "BPDU type", off + 3, 1, "0x%02x (configuration)", d[3]);
    nl_dis_field(o, "STP", "Flags", off + 4, 1, "0x%02x%s", d[4], d[4] & 1 ? " (topology change)" : "");
    nl_dis_field(o, "STP", "Root identifier", off + 5, 8, "%s", id_str(d + 5));
    nl_dis_field(o, "STP", "Root path cost", off + 13, 4, "%u", nl_get32(d + 13));
    nl_dis_field(o, "STP", "Bridge identifier", off + 17, 8, "%s", id_str(d + 17));
    nl_dis_field(o, "STP", "Port identifier", off + 25, 2, "0x%04x", nl_get16(d + 25));
    nl_dis_field(o, "STP", "Message age", off + 27, 2, "%.2f s", nl_get16(d + 27) / 256.0);
    nl_dis_field(o, "STP", "Max age", off + 29, 2, "%.2f s", nl_get16(d + 29) / 256.0);
    nl_dis_field(o, "STP", "Hello time", off + 31, 2, "%.2f s", nl_get16(d + 31) / 256.0);
    nl_dis_field(o, "STP", "Forward delay", off + 33, 2, "%.2f s", nl_get16(d + 33) / 256.0);
    nl_dis_summary(o, "STP", STP_COLOR, "Conf.%s Root = %s  Cost = %u  Bridge = %s  Port = 0x%04x",
                   d[4] & 1 ? " TC" : "", id_str(d + 5), nl_get32(d + 13), id_str(d + 17), nl_get16(d + 25));
}

static const nl_module stp_mod = {
    .name = "stp", .title = N_("Spanning Tree Protocol — loop-free switching with redundant links"),
    .layer = NL_L_LINK,
    .usage = N_("run SWITCH stp [priority=32768] [hello=1s] [maxage=6s] [fwd=3s] [portfast=0] [rapid=0]\n"
                "run SWITCH stp priority N        change the bridge priority (lowest identifier becomes root)\n"
                "run SWITCH stp cost PORT N       change the path cost of a port (default 10^9 / bandwidth)\n"
                "  The timers are shorter than the standard ones (2 s, 20 s, 15 s) so that labs are\n"
                "  watchable; the timers of the root bridge apply to the whole tree.\n"
                "  portfast=1: ports that face a host forward at once. rapid=1: a new root port\n"
                "  forwards at once (simplified RSTP).\n"
                "  Counters: stp.is_root stp.blocked_ports stp.forwarding_ports stp.topology_changes\n"
                "  stp.root_changes stp.last_change_ms"),
    .theory = N_("Ethernet switches forward a frame for an unknown or broadcast destination out of "
                 "every other port, and Ethernet frames carry no hop limit. If the switches are "
                 "connected in a loop, as one wants for redundancy, a single broadcast therefore "
                 "circulates for ever, is copied to every host at each lap, and the forwarding tables "
                 "keep changing their mind about where each address is: a broadcast storm. The Spanning "
                 "Tree Protocol (IEEE 802.1D, Radia Perlman, 1985) lets the switches agree on a subset "
                 "of the links that reaches everything without loops, a spanning tree, and block the "
                 "rest; a blocked link is kept as a spare and used again when the tree breaks.\n"
                 "\n"
                 "Every bridge has an identifier made of a configurable priority followed by its MAC "
                 "address. Bridges send configuration BPDUs that say: who I think the root is, how far "
                 "I am from it (the root path cost, the sum of the costs of the ports crossed, lower for "
                 "faster links), who I am, and from which port I am speaking. At first every bridge "
                 "claims to be the root; as soon as it hears a better (lower) root identifier it adopts "
                 "it. Three choices follow, all made by comparing these four fields in order. The bridge "
                 "with the lowest identifier is the root. Each other bridge picks as root port the port "
                 "with the cheapest path to the root. On each link, the bridge offering the cheapest "
                 "path to the root owns the designated port. Ports that are neither root nor designated "
                 "are blocked: they still listen to BPDUs but carry no data.\n"
                 "\n"
                 "A port must not start forwarding while the rest of the network may still hold an old "
                 "view, or a temporary loop would form. So a port that becomes root or designated waits "
                 "in the listening state and then in the learning state, one forward delay each, before "
                 "forwarding: with the standard timers that is 30 seconds, the reason a PC plugged into "
                 "a switch port once had to wait for the network (and the reason for portfast on ports "
                 "that face hosts). Information is refreshed by the hello BPDUs and carries an age; when "
                 "nothing has been heard on a port for max age, the information is discarded and the "
                 "election is repeated. A failed link is noticed at once by the bridges attached to it, "
                 "a failed bridge only through this timeout, so recovery takes between two forward "
                 "delays and max age plus two forward delays. After a change, bridges are told to flush "
                 "their forwarding tables (topology change notification), because stations may now be "
                 "reached through a different port. Rapid STP (802.1w) replaces the timers with an "
                 "explicit handshake between neighbours and converges in well under a second; today "
                 "data centres often avoid spanning tree altogether with routed fabrics and ECMP."),
    .run = stp_run, .stop = stp_stop, .crash = stp_crash, .recover = stp_recover, .panel = stp_panel,
};
NL_MODULE(stp_mod)

NL_INIT(stp_dissector)
{
    nl_dissector_eth(STP_ETHERTYPE, stp_dissect);
}
