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
 *  - A port sees its link go down or come back within 100 ms (the carrier
 *    signal), not at the next hello.
 *
 * rapid=1 is Rapid Spanning Tree (IEEE 802.1w, 802.1D-2004):
 *  - BPDUs are RST BPDUs (type 2) whose flags carry the role and state of
 *    the sending port, Proposal and Agreement;
 *  - a designated port that is not forwarding proposes; the bridge below,
 *    for which that is the root port, first puts its own non-edge
 *    designated ports back to discarding (sync), then forwards on the root
 *    port and answers with an agreement, on which the proposer forwards at
 *    once; the ports just discarded propose in turn, and the handshake
 *    walks down the tree, one link at a time, with no timer involved;
 *  - an alternate port also answers a proposal with an agreement (it is
 *    discarding anyway), and becomes root port and forwards at once when
 *    the root port fails; the retired root port, if it is now designated,
 *    goes back to discarding and proposes;
 *  - a designated port that hears no BPDU for 3 hello times is an edge port
 *    (auto edge: it faces hosts), forwards at once and is left alone by
 *    sync; it stops being one as soon as a BPDU arrives;
 *  - information is refreshed by every hello and dropped after 3 missing
 *    hellos, instead of living for max age.
 * Not implemented: the timers rrWhile/rbWhile of the port role machines as
 * such, backup ports, the per-port migration to plain STP (a rapid bridge
 * and a plain one do interoperate here: without an agreement the designated
 * port just waits for its forward delays), and 802.1w's own rules for
 * topology changes (the 802.1D ones above are used).
 * portfast=1 makes ports that face a host edge ports from the start.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STP_ETHERTYPE 0x4242
#define STP_COLOR     180
#define BPDU_CONFIG   0x00
#define BPDU_RST      0x02
#define BPDU_TCN      0x80
#define BPDU_LEN      35

/* flags of an RST BPDU */
#define F_TC          0x01
#define F_PROPOSAL    0x02
#define F_ROLE_MASK   0x0c
#define F_ROLE_ALT    0x04
#define F_ROLE_ROOT   0x08
#define F_ROLE_DESG   0x0c
#define F_LEARNING    0x10
#define F_FORWARDING  0x20
#define F_AGREEMENT   0x40

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
    int        edge;                    /* edge port: faces hosts only (portfast, or auto edge) */
    int        bpdu_seen;               /* a BPDU arrived since the port came up */
    nl_ev      edge_timer;              /* rapid: no BPDU for 3 hellos -> edge port */
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
    nl_ev     hello_timer, carrier_timer;
    int       synced;                   /* rapid: sync already done for sync_vec */
    stp_vec   sync_vec;
    unsigned  proposals, agreements;
    nl_time   tc_flush_until;
    unsigned  tc_count, root_changes;
    stp_port  port[NL_MAX_IF];
};

static const nl_module stp_mod;
static void stp_recompute(stp_state *s);
static void stp_send_bpdu(stp_state *s, int ifx, int agreement);
#define stp_send_config(s, ifx) stp_send_bpdu(s, ifx, 0)

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
    nl_count_set(s->n, "stp.agreements", s->agreements);
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

    /* flush every time: an entry learned a moment ago may already point the
     * wrong way (802.1D keeps the table short-lived for the whole period) */
    nl_fdb_flush(s->n);
    if (nl_now() >= s->tc_flush_until || from < 0) {
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

    /* 3. states. In plain STP every root or designated port walks through
     * listening and learning. In RSTP a root port forwards at once: the
     * loop it could close is cut either upstream (the designated port there
     * waits for our agreement) or here (the retired root port, if it became
     * designated, goes back to discarding and has to propose). */
    if (changed)
        s->synced = 0;
    if (s->rapid && s->root_port >= 0 && s->root_port != old_root_port && old_root_port >= 0) {
        stp_port *old = &s->port[old_root_port];
        if (old->role == ROLE_DESIGNATED && !old->edge && old->state != ST_BLOCKING) {
            port_block(old);
            announce[old_root_port] = 1;
        }
    }
    for (i = 0; i < n->nif; i++) {
        stp_port *p = &s->port[i];

        if (p->role == ROLE_ROOT || p->role == ROLE_DESIGNATED)
            port_unblock(p, p->edge || (s->rapid && p->role == ROLE_ROOT));
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

/* A configuration BPDU from a designated port; with rapid=1 an RST BPDU,
 * which root and alternate ports also send to agree to a proposal */
static void stp_send_bpdu(stp_state *s, int ifx, int agreement)
{
    stp_port *p = &s->port[ifx];
    uint8_t b[BPDU_LEN + 1];
    nl_time age = 0;
    int len = BPDU_LEN;

    if (!p->carrier)
        return;
    if (s->root_port >= 0) {
        const stp_port *rp = &s->port[s->root_port];
        /* one hop older than what we heard, plus the time we have held it */
        age = rp->rx_age + (nl_now() - rp->rx_time) + NL_SEC(1) / 4;
    }
    memset(b, 0, sizeof(b));
    b[3] = BPDU_CONFIG;
    b[4] = nl_now() < p->tc_until ? F_TC : 0x00;
    if (s->rapid) {
        b[2] = 2;                       /* protocol version: RSTP */
        b[3] = BPDU_RST;
        b[4] |= p->role == ROLE_ROOT ? F_ROLE_ROOT : p->role == ROLE_DESIGNATED ? F_ROLE_DESG : F_ROLE_ALT;
        if (p->state >= ST_LEARNING)
            b[4] |= F_LEARNING;
        if (p->state == ST_FORWARDING)
            b[4] |= F_FORWARDING;
        if (agreement) {
            b[4] |= F_AGREEMENT;
            s->agreements++;
        } else if (p->role == ROLE_DESIGNATED && p->state != ST_FORWARDING && !p->edge) {
            b[4] |= F_PROPOSAL;
            s->proposals++;
        }
        len = BPDU_LEN + 1;             /* version 1 length, always 0 */
    }
    memcpy(b + 5, s->root, 8);
    nl_put32(b + 13, s->root_cost);
    memcpy(b + 17, s->id, 8);
    nl_put16(b + 25, p->id);
    nl_put16(b + 27, to_bpdu_time(age));
    nl_put16(b + 29, to_bpdu_time(s->cur_max_age));
    nl_put16(b + 31, to_bpdu_time(s->cur_hello));
    nl_put16(b + 33, to_bpdu_time(s->cur_fwd));
    p->bpdu_tx++;
    if (b[4] & (F_PROPOSAL | F_AGREEMENT))
        nl_log(s->n, NL_L_LINK, 2, 0, T_INFO "STP" T_RESET " → %s: %s", s->n->ifc[ifx].name,
               b[4] & F_AGREEMENT ? _("agreement: my other ports cannot close a loop, you may forward")
                                  : _("proposal: I am designated here and want to forward"));
    else
        nl_log(s->n, NL_L_LINK, 3, 0, T_DIM "STP → %s: config root %s cost %u%s", s->n->ifc[ifx].name,
               id_name(s->root), s->root_cost, b[4] & F_TC ? " TC" : "");
    nl_eth_send(s->n, ifx, STP_MAC, STP_ETHERTYPE, b, len, 0);
}

/* RSTP sync: before agreeing to a proposal received on the root port, no
 * other port may be able to close a loop. Designated ports that forward
 * (and are not edge ports) go back to discarding and propose in turn. */
static void stp_sync(stp_state *s)
{
    const stp_port *rp = &s->port[s->root_port];
    int i;

    if (s->synced && vec_cmp(&s->sync_vec, &rp->rx) == 0)
        return;
    for (i = 0; i < s->n->nif; i++) {
        stp_port *p = &s->port[i];

        if (i == s->root_port || p->edge || p->role != ROLE_DESIGNATED || p->state < ST_LEARNING)
            continue;
        port_block(p);
        port_unblock(p, 0);
        stp_send_bpdu(s, i, 0);
    }
    s->synced = 1;
    s->sync_vec = rp->rx;
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
    int same_sender, flags;

    if (m->in_if < 0 || m->in_if >= n->nif || m->len < 4)
        return;
    p = &s->port[m->in_if];
    p->bpdu_rx++;
    if (b[3] == BPDU_TCN) {
        nl_log(n, NL_L_LINK, 3, 0, T_DIM "STP ← %s: TCN", n->ifc[m->in_if].name);
        topology_change(s, m->in_if);
        return;
    }
    if ((b[3] != BPDU_CONFIG && b[3] != BPDU_RST) || m->len < BPDU_LEN)
        return;
    flags = b[3] == BPDU_RST ? b[4] : (b[4] & F_TC);
    p->bpdu_seen = 1;
    nl_cancel(&p->edge_timer);
    if (p->edge) {                      /* a bridge is out there after all */
        p->edge = 0;
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "STP" T_RESET " %s %s: %s", _("port"), n->ifc[m->in_if].name,
               _("BPDU received: no longer an edge port"));
    }
    memcpy(v.root, b + 5, 8);
    v.cost = nl_get32(b + 13);
    memcpy(v.bridge, b + 17, 8);
    v.port = nl_get16(b + 25);
    age = from_bpdu_time(nl_get16(b + 27));
    max_age = from_bpdu_time(nl_get16(b + 29));
    nl_log(n, NL_L_LINK, 3, 0, T_DIM "STP ← %s: config root %s cost %u from %s age %s%s%s%s", n->ifc[m->in_if].name,
           id_name(v.root), v.cost, id_name(v.bridge), nl_time_str(age), flags & F_TC ? " TC" : "",
           flags & F_PROPOSAL ? " proposal" : "", flags & F_AGREEMENT ? " agreement" : "");
    if (age >= max_age) {
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "STP" T_RESET " %s %s: %s", _("port"), n->ifc[m->in_if].name,
               _("BPDU older than max-age ignored (stale information)"));
        return;
    }
    if (memcmp(v.bridge, s->id, 8) == 0)
        return;                         /* our own BPDU came back */
    /* RSTP: a BPDU from a root or alternate port carries no claim, only
     * (possibly) the agreement that lets our designated port forward */
    if ((flags & F_ROLE_MASK) == F_ROLE_ROOT || (flags & F_ROLE_MASK) == F_ROLE_ALT) {
        if (s->rapid && (flags & F_AGREEMENT) && p->role == ROLE_DESIGNATED && memcmp(v.root, s->root, 8) == 0 &&
            p->state != ST_FORWARDING) {
            nl_log(n, NL_L_LINK, 2, 0, T_OK "STP" T_RESET " ← %s: %s", n->ifc[m->in_if].name,
                   _("agreement received: forwarding without waiting for the timers"));
            port_unblock(p, 1);
            update_counters(s);
        }
        return;
    }
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
    /* RSTP refreshes at every hello and gives up after three missing ones */
    p->age_timer = nl_after(n, s->rapid && 3 * p->rx_hello < max_age - age ? 3 * p->rx_hello : max_age - age,
                            age_expired, p);
    stp_recompute(s);
    /* the TC flag is meaningful when it comes from the direction of the root */
    if ((flags & F_TC) && m->in_if == s->root_port)
        topology_change(s, m->in_if);
    /* RSTP: answer a proposal from the designated port of this segment */
    if (s->rapid && (flags & F_PROPOSAL) && (flags & F_ROLE_MASK) == F_ROLE_DESG && p->valid) {
        if (p->role == ROLE_ROOT) {
            stp_sync(s);
            port_unblock(p, 1);
            stp_send_bpdu(s, m->in_if, 1);
            update_counters(s);
        } else if (p->role == ROLE_BLOCKED) {
            stp_send_bpdu(s, m->in_if, 1);
        }
    }
}

/* RSTP auto edge: nothing that speaks spanning tree is on this port */
static void edge_detect(void *ctx)
{
    stp_port *p = ctx;
    stp_state *s = p->st;

    p->edge_timer = 0;
    if (p->bpdu_seen || !p->carrier || p->role != ROLE_DESIGNATED)
        return;
    p->edge = 1;
    nl_log(s->n, NL_L_LINK, 2, 0, T_INFO "STP" T_RESET " %s %s: %s", _("port"), s->n->ifc[p->ifx].name,
           _("no BPDU heard for 3 hello times: edge port, forwarding at once"));
    port_unblock(p, 1);
    update_counters(s);
}

static void port_reset(stp_state *s, int i)
{
    stp_port *p = &s->port[i];
    nl_node *peer = nl_if_peer(s->n, i, NULL);

    nl_cancel(&p->age_timer);
    nl_cancel(&p->state_timer);
    nl_cancel(&p->edge_timer);
    p->st = s;
    p->ifx = i;
    p->bpdu_seen = 0;
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
    if (s->rapid && p->carrier && !p->edge)
        p->edge_timer = nl_after(s->n, 3 * s->hello, edge_detect, p);
}

/* A port whose link went down or came back (carrier) */
static void check_carrier(stp_state *s)
{
    nl_node *n = s->n;
    int i, carrier_change = 0;

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
            nl_cancel(&p->age_timer);
            nl_cancel(&p->edge_timer);
            port_block(p);
        } else {
            port_reset(s, i);
        }
    }
    if (carrier_change)
        stp_recompute(s);
}

static void carrier_tick(void *ctx)
{
    stp_state *s = ctx;

    s->carrier_timer = 0;
    check_carrier(s);
    s->carrier_timer = nl_after(s->n, NL_MS(100), carrier_tick, s);
}

static void hello_tick(void *ctx)
{
    stp_state *s = ctx;
    nl_node *n = s->n;
    int i;

    s->hello_timer = 0;
    check_carrier(s);
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
    s->synced = 0;
    nl_cancel(&s->hello_timer);
    nl_cancel(&s->carrier_timer);
    hello_tick(s);
    s->carrier_timer = nl_after(s->n, NL_MS(100), carrier_tick, s);
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
    nl_cancel(&s->carrier_timer);
    for (i = 0; i < NL_MAX_IF; i++) {
        nl_cancel(&s->port[i].age_timer);
        nl_cancel(&s->port[i].state_timer);
        nl_cancel(&s->port[i].edge_timer);
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
    nl_eth_unbind(n, STP_ETHERTYPE);
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
    if (s->rapid)
        row += nl_ui_kv(x, y + row, w, _("proposals / agreements sent"), "%u / %u", s->proposals, s->agreements);
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
                snprintf(who, sizeof(who), "%s%s", _("this bridge"), p->edge ? _(" (edge)") : "");
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
    nl_dis_field(o, "STP", "BPDU type", off + 3, 1, "0x%02x (%s)", d[3],
                 d[3] == BPDU_RST ? "rapid spanning tree" : "configuration");
    if (d[3] == BPDU_RST)
        nl_dis_field(o, "STP", "Flags", off + 4, 1, "0x%02x role=%s%s%s%s%s%s", d[4],
                     (d[4] & F_ROLE_MASK) == F_ROLE_DESG ? "designated" : (d[4] & F_ROLE_MASK) == F_ROLE_ROOT ? "root"
                     : (d[4] & F_ROLE_MASK) == F_ROLE_ALT ? "alternate" : "unknown",
                     d[4] & F_PROPOSAL ? " proposal" : "", d[4] & F_AGREEMENT ? " agreement" : "",
                     d[4] & F_LEARNING ? " learning" : "", d[4] & F_FORWARDING ? " forwarding" : "",
                     d[4] & F_TC ? " TC" : "");
    else
        nl_dis_field(o, "STP", "Flags", off + 4, 1, "0x%02x%s", d[4], d[4] & 1 ? " (topology change)" : "");
    nl_dis_field(o, "STP", "Root identifier", off + 5, 8, "%s", id_str(d + 5));
    nl_dis_field(o, "STP", "Root path cost", off + 13, 4, "%u", nl_get32(d + 13));
    nl_dis_field(o, "STP", "Bridge identifier", off + 17, 8, "%s", id_str(d + 17));
    nl_dis_field(o, "STP", "Port identifier", off + 25, 2, "0x%04x", nl_get16(d + 25));
    nl_dis_field(o, "STP", "Message age", off + 27, 2, "%.2f s", nl_get16(d + 27) / 256.0);
    nl_dis_field(o, "STP", "Max age", off + 29, 2, "%.2f s", nl_get16(d + 29) / 256.0);
    nl_dis_field(o, "STP", "Hello time", off + 31, 2, "%.2f s", nl_get16(d + 31) / 256.0);
    nl_dis_field(o, "STP", "Forward delay", off + 33, 2, "%.2f s", nl_get16(d + 33) / 256.0);
    if (d[3] == BPDU_RST)
        nl_dis_summary(o, "RSTP", STP_COLOR, "RST.%s%s%s Root = %s  Cost = %u  Bridge = %s  Port = 0x%04x",
                       d[4] & F_PROPOSAL ? " Proposal" : "", d[4] & F_AGREEMENT ? " Agreement" : "",
                       d[4] & F_TC ? " TC" : "", id_str(d + 5), nl_get32(d + 13), id_str(d + 17),
                       nl_get16(d + 25));
    else
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
                "  portfast=1: ports that face a host forward at once. rapid=1: Rapid Spanning Tree\n"
                "  (802.1w): proposal/agreement between neighbours instead of the forward-delay timers,\n"
                "  alternate ports that take over at once, automatic edge ports, 3 hellos to forget.\n"
                "  Counters: stp.is_root stp.blocked_ports stp.forwarding_ports stp.topology_changes\n"
                "  stp.root_changes stp.last_change_ms stp.agreements"),
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
                 "reached through a different port.\n"
                 "\n"
                 "Rapid Spanning Tree (802.1w, rapid=1 here) keeps the same tree and replaces the timers "
                 "with a conversation. A designated port that is not yet forwarding says so in its BPDU: "
                 "a proposal. The bridge on the other end, for which that port leads to the root, makes "
                 "sure that none of its own ports could close a loop, by putting its designated ports "
                 "back to discarding (sync), then forwards on its root port and answers with an "
                 "agreement; the proposer forwards the moment it hears it. The ports just discarded "
                 "propose in turn, so the handshake travels down the tree one link at a time and the "
                 "whole network converges in the time the BPDUs take to cross it. A blocked port is "
                 "kept as an alternate root port and takes over at once when the root port fails. "
                 "Ports where no BPDU is ever heard are edge ports: they face hosts, forward at once "
                 "and take no part in the handshake. And since every bridge now sends a BPDU at each "
                 "hello, three missing hellos are enough to declare a neighbour gone, instead of max "
                 "age. Today data centres often avoid spanning tree altogether with routed fabrics and "
                 "ECMP."),
    .run = stp_run, .stop = stp_stop, .crash = stp_crash, .recover = stp_recover, .panel = stp_panel,
};
NL_MODULE(stp_mod)

NL_INIT(stp_dissector)
{
    nl_dissector_eth(STP_ETHERTYPE, stp_dissect);
}
