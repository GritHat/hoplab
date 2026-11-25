/*
 * stack.c - The protocol stack of a node: Ethernet (hosts and learning
 *           bridges with 802.1Q VLANs), ARP, IPv4 (routing table, forwarding,
 *           fragmentation and reassembly, packet hooks), ICMP, UDP, and the
 *           dissectors of these protocols.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARP_TTL      NL_SEC(120)
#define ARP_RETRY    NL_SEC(1)
#define FDB_TTL      NL_SEC(300)
#define FRAG_MAX     16384
#define FRAG_TTL     NL_SEC(30)

typedef struct {
    int      used;
    nl_ip    src, dst;
    uint16_t id;
    uint8_t  proto;
    nl_time  t;
    int      total;                     /* known when the last fragment arrives, else -1 */
    uint8_t  buf[FRAG_MAX];
    uint8_t  have[FRAG_MAX / 64];       /* one bit per 8-byte block */
} reasm;

typedef struct {
    struct { uint16_t type; nl_eth_fn fn; void *ctx; } eth[8];
    struct { uint8_t proto; nl_ip_fn fn; void *ctx; } ipp[8];
    struct { uint16_t port; nl_udp_fn fn; void *ctx; } udp[256];
    struct { int point; nl_hook_fn fn; void *ctx; } hook[12];
    struct { nl_icmp_fn fn; void *ctx; } icmpw[8];
    int      neth, nipp, nudp, nhook, nicmpw;
    uint16_t ip_id, eph;
    reasm   *fr[4];
} stack;

static stack *S(const nl_node *n) { return n->stack; }

void nl_stack_init(nl_node *n)
{
    stack *s = calloc(1, sizeof(*s));

    if (!s)
        exit(EXIT_FAILURE);
    s->eph = 49152;
    s->ip_id = (uint16_t)(n->idx * 1000 + 1);
    n->stack = s;
    nl_ip6_init(n);
}

void nl_stack_node_down(nl_node *n)
{
    int i, k;

    for (i = 0; i < (int)(sizeof(n->arp) / sizeof(n->arp[0])); i++) {
        for (k = 0; k < n->arp[i].nwait; k++)
            nl_pkt_free(n->arp[i].wait[k]);
        memset(&n->arp[i], 0, sizeof(n->arp[i]));
    }
    nl_fdb_flush(n);
    nl_ip6_node_down(n);
}

/* ------------------------------------------------------------------ */
/* Routing table                                                      */
/* ------------------------------------------------------------------ */

const char *nl_route_proto_name(int proto)
{
    switch (proto) {
    case NL_RT_CONNECTED: return "connected";
    case NL_RT_STATIC:    return "static";
    case NL_RT_DHCP:      return "dhcp";
    case NL_RT_RA:        return "ra";
    case NL_RT_BGP:       return "bgp";
    case NL_RT_OSPF:      return "ospf";
    case NL_RT_RIP:       return "rip";
    default:              return "other";
    }
}

void nl_route_set(nl_node *n, nl_ip dst, int plen, nl_ip gw, int ifx, int metric, int proto)
{
    nl_route *r = NULL;
    int i;

    dst &= nl_mask(plen);
    for (i = 0; i < n->nrt; i++)
        if (n->rt[i].dst == dst && n->rt[i].plen == plen && n->rt[i].proto == proto)
            r = &n->rt[i];
    if (r && r->gw == gw && r->ifx == ifx && r->metric == metric)
        return;
    if (!r) {
        if (n->nrt >= NL_MAX_ROUTES)
            return;
        r = &n->rt[n->nrt++];
    }
    r->dst = dst;
    r->plen = plen;
    r->gw = gw;
    r->ifx = ifx;
    r->metric = metric;
    r->proto = proto;
    r->since = W.now;
    if (proto != NL_RT_CONNECTED)
        nl_log(n, NL_L_NET, proto == NL_RT_STATIC ? 3 : 2, 0,
               T_INFO "route" T_RESET " %s/%d via %s dev %s metric %d " T_DIM "[%s]",
               nl_ip_str(dst), plen, gw ? nl_ip_str(gw) : "direct",
               ifx >= 0 && ifx < n->nif ? n->ifc[ifx].name : "?", metric, nl_route_proto_name(proto));
}

void nl_route_del(nl_node *n, nl_ip dst, int plen, int proto)
{
    int i;

    dst &= nl_mask(plen);
    for (i = 0; i < n->nrt; i++)
        if (n->rt[i].dst == dst && n->rt[i].plen == plen && n->rt[i].proto == proto) {
            if (proto != NL_RT_CONNECTED)
                nl_log(n, NL_L_NET, 2, 0, T_WARN "route removed" T_RESET " %s/%d " T_DIM "[%s]",
                       nl_ip_str(dst), plen, nl_route_proto_name(proto));
            n->rt[i] = n->rt[--n->nrt];
            return;
        }
}

void nl_route_flush(nl_node *n, int proto)
{
    int i;

    for (i = 0; i < n->nrt; )
        if (n->rt[i].proto == proto)
            n->rt[i] = n->rt[--n->nrt];
        else
            i++;
}

const nl_route *nl_route_lookup(const nl_node *n, nl_ip dst)
{
    const nl_route *best = NULL;
    int i;

    for (i = 0; i < n->nrt; i++) {
        const nl_route *r = &n->rt[i];
        if ((dst & nl_mask(r->plen)) != r->dst || !nl_if_usable(n, r->ifx))
            continue;
        if (!best || r->plen > best->plen ||
            (r->plen == best->plen && (r->proto < best->proto ||
                                       (r->proto == best->proto && r->metric < best->metric))))
            best = r;
    }
    return best;
}

void nl_if_set_addr(nl_node *n, int ifx, nl_ip ip, int plen)
{
    nl_iface *f = &n->ifc[ifx];
    int i;

    for (i = 0; i < n->nrt; )
        if (n->rt[i].proto == NL_RT_CONNECTED && n->rt[i].ifx == ifx)
            n->rt[i] = n->rt[--n->nrt];
        else
            i++;
    f->ip = ip;
    f->plen = plen;
    if (ip)
        nl_route_set(n, ip, plen, 0, ifx, 0, NL_RT_CONNECTED);
}

int nl_ip_is_local(const nl_node *n, nl_ip ip)
{
    int i;

    for (i = 0; i < n->nif; i++)
        if (n->ifc[i].ip && n->ifc[i].ip == ip)
            return 1;
    return (ip >> 24) == 127;
}

int nl_ip_mtu(const nl_node *n, nl_ip dst)
{
    const nl_route *r = nl_route_lookup(n, dst);
    return r && n->ifc[r->ifx].link ? n->ifc[r->ifx].link->cfg.mtu : NL_DEFAULT_MTU;
}

static int is_broadcast(const nl_node *n, nl_ip ip)
{
    int i;

    if (ip == NL_IP_BROADCAST)
        return 1;
    for (i = 0; i < n->nif; i++) {
        const nl_iface *f = &n->ifc[i];
        if (f->ip && f->plen < 31 && ip == (f->ip | ~nl_mask(f->plen)))
            return 1;
    }
    return 0;
}

static int is_multicast(nl_ip ip) { return (ip >> 28) == 14; }

/* ------------------------------------------------------------------ */
/* Hooks, handlers                                                    */
/* ------------------------------------------------------------------ */

void nl_hook_add(nl_node *n, int point, nl_hook_fn fn, void *ctx)
{
    stack *s = S(n);

    if (s->nhook < (int)(sizeof(s->hook) / sizeof(s->hook[0]))) {
        s->hook[s->nhook].point = point;
        s->hook[s->nhook].fn = fn;
        s->hook[s->nhook].ctx = ctx;
        s->nhook++;
    }
}

void nl_hook_del(nl_node *n, int point, nl_hook_fn fn, void *ctx)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->nhook; i++)
        if (s->hook[i].point == point && s->hook[i].fn == fn && s->hook[i].ctx == ctx) {
            memmove(&s->hook[i], &s->hook[i + 1], (size_t)(s->nhook - i - 1) * sizeof(s->hook[0]));
            s->nhook--;
            return;
        }
}

static int run_hooks(nl_node *n, int point, nl_pkt *p, int in_if, int out_if)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->nhook; i++)
        if (s->hook[i].point == point && s->hook[i].fn(n, s->hook[i].ctx, p, in_if, out_if) == NL_HOOK_DROP)
            return NL_HOOK_DROP;
    return NL_HOOK_OK;
}

void nl_eth_bind(nl_node *n, uint16_t type, nl_eth_fn fn, void *ctx)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->neth; i++)
        if (s->eth[i].type == type)
            break;
    if (i == s->neth) {
        if (s->neth >= (int)(sizeof(s->eth) / sizeof(s->eth[0])))
            return;
        s->neth++;
    }
    s->eth[i].type = type;
    s->eth[i].fn = fn;
    s->eth[i].ctx = ctx;
}

void nl_eth_unbind(nl_node *n, uint16_t type)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->neth; i++)
        if (s->eth[i].type == type) {
            s->eth[i] = s->eth[--s->neth];
            return;
        }
}

void nl_ip_bind(nl_node *n, uint8_t proto, nl_ip_fn fn, void *ctx)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->nipp; i++)
        if (s->ipp[i].proto == proto)
            break;
    if (i == s->nipp) {
        if (s->nipp >= (int)(sizeof(s->ipp) / sizeof(s->ipp[0])))
            return;
        s->nipp++;
    }
    s->ipp[i].proto = proto;
    s->ipp[i].fn = fn;
    s->ipp[i].ctx = ctx;
}

void nl_icmp_watch(nl_node *n, nl_icmp_fn fn, void *ctx)
{
    stack *s = S(n);

    if (s->nicmpw < (int)(sizeof(s->icmpw) / sizeof(s->icmpw[0]))) {
        s->icmpw[s->nicmpw].fn = fn;
        s->icmpw[s->nicmpw].ctx = ctx;
        s->nicmpw++;
    }
}

void nl_icmp_unwatch(nl_node *n, nl_icmp_fn fn, void *ctx)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->nicmpw; i++)
        if (s->icmpw[i].fn == fn && s->icmpw[i].ctx == ctx) {
            memmove(&s->icmpw[i], &s->icmpw[i + 1], (size_t)(s->nicmpw - i - 1) * sizeof(s->icmpw[0]));
            s->nicmpw--;
            return;
        }
}

/* ------------------------------------------------------------------ */
/* Ethernet output                                                    */
/* ------------------------------------------------------------------ */

static void eth_out(nl_node *n, int ifx, const uint8_t dst[6], uint16_t type, nl_pkt *p)
{
    memcpy(p->data, dst, 6);
    memcpy(p->data + 6, n->ifc[ifx].mac, 6);
    nl_put16(p->data + 12, type);
    nl_link_tx(n, ifx, p);
}

uint32_t nl_eth_send(nl_node *n, int ifx, const uint8_t dst[6], uint16_t type,
                     const void *payload, int len, uint32_t pkt_id)
{
    nl_pkt *p;
    uint32_t id;

    if (ifx < 0 || ifx >= n->nif || len < 0 || len > NL_FRAME_MAX - NL_ETH_HLEN)
        return 0;
    p = nl_pkt_new(pkt_id);
    id = p->id;
    memcpy(p->data + NL_ETH_HLEN, payload, (size_t)len);
    p->len = (uint16_t)(NL_ETH_HLEN + len);
    eth_out(n, ifx, dst, type, p);
    return id;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                */
/* ------------------------------------------------------------------ */

static nl_arp *arp_find(nl_node *n, nl_ip ip, int create)
{
    nl_arp *freeslot = NULL, *oldest = NULL;
    int i;

    for (i = 0; i < (int)(sizeof(n->arp) / sizeof(n->arp[0])); i++) {
        nl_arp *e = &n->arp[i];
        if (e->state && e->ip == ip)
            return e;
        if (!e->state && !freeslot)
            freeslot = e;
        if (e->state == 2 && (!oldest || e->expire < oldest->expire))
            oldest = e;
    }
    if (!create)
        return NULL;
    if (!freeslot)
        freeslot = oldest;
    if (freeslot) {
        memset(freeslot, 0, sizeof(*freeslot));
        freeslot->ip = ip;
    }
    return freeslot;
}

static void arp_send(nl_node *n, int ifx, int op, const uint8_t *tmac, nl_ip tip)
{
    uint8_t b[28];
    const nl_iface *f = &n->ifc[ifx];

    nl_put16(b, 1);                     /* Ethernet */
    nl_put16(b + 2, NL_ETH_IP);
    b[4] = 6;
    b[5] = 4;
    nl_put16(b + 6, (uint16_t)op);
    memcpy(b + 8, f->mac, 6);
    nl_put32(b + 14, f->ip);
    if (op == 1)
        memset(b + 18, 0, 6);
    else
        memcpy(b + 18, tmac, 6);
    nl_put32(b + 24, tip);
    if (op == 1)
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "ARP" T_RESET " who has %s? tell %s", nl_ip_str(tip), nl_ip_str(f->ip));
    nl_eth_send(n, ifx, op == 1 ? NL_MAC_BROADCAST : tmac, NL_ETH_ARP, b, 28, 0);
}

static void icmp_error(nl_node *n, int type, int code, const uint8_t *orig_ip, int orig_len, int mtu);

typedef struct { nl_node *n; nl_ip ip; } arp_ctx;

static void arp_timeout(void *ctx)
{
    arp_ctx *c = ctx;
    nl_arp *e = arp_find(c->n, c->ip, 0);
    int k;

    if (e && e->state == 1 && e->timer) {
        e->timer = 0;
        if (++e->tries < 3) {
            arp_send(c->n, e->ifx, 1, NULL, e->ip);
            e->timer = nl_after(c->n, ARP_RETRY, arp_timeout, c);
            return;
        }
        nl_log(c->n, NL_L_LINK, 2, 0, T_ERR "ARP" T_RESET " %s %s", _("no answer for"), nl_ip_str(e->ip));
        nl_count(c->n, "arp.failed", 1);
        for (k = 0; k < e->nwait; k++) {
            if (c->n->forward)
                icmp_error(c->n, NL_ICMP_UNREACH, 1, e->wait[k]->data + NL_ETH_HLEN,
                           e->wait[k]->len - NL_ETH_HLEN, 0);
            nl_pkt_free(e->wait[k]);
        }
        memset(e, 0, sizeof(*e));
    }
    free(c);
}

/* Send an IP frame to next hop nh out of ifx, resolving its MAC address */
static void arp_resolve_tx(nl_node *n, int ifx, nl_ip nh, nl_pkt *p)
{
    uint8_t mac[6];
    nl_arp *e;
    arp_ctx *c;

    if (nh == NL_IP_BROADCAST || is_broadcast(n, nh)) {
        eth_out(n, ifx, NL_MAC_BROADCAST, NL_ETH_IP, p);
        return;
    }
    if (is_multicast(nh)) {
        mac[0] = 0x01; mac[1] = 0x00; mac[2] = 0x5e;
        mac[3] = (nh >> 16) & 0x7f; mac[4] = (nh >> 8) & 0xff; mac[5] = nh & 0xff;
        eth_out(n, ifx, mac, NL_ETH_IP, p);
        return;
    }
    e = arp_find(n, nh, 1);
    if (!e) {
        nl_pkt_free(p);
        return;
    }
    if (e->state == 2 && e->expire > W.now) {
        eth_out(n, ifx, e->mac, NL_ETH_IP, p);
        return;
    }
    if (e->nwait < 4)
        e->wait[e->nwait++] = p;
    else
        nl_pkt_free(p);
    if (e->state == 1)
        return;
    e->state = 1;
    e->tries = 0;
    e->ifx = ifx;
    arp_send(n, ifx, 1, NULL, nh);
    c = malloc(sizeof(*c));
    if (c) {
        c->n = n;
        c->ip = nh;
        e->timer = nl_after(n, ARP_RETRY, arp_timeout, c);
    }
}

static void arp_learn(nl_node *n, int ifx, nl_ip ip, const uint8_t *mac, int create)
{
    nl_arp *e = arp_find(n, ip, create);
    int k, nw;
    nl_pkt *w[4];

    if (!e || !ip)
        return;
    if (e->state != 2 || memcmp(e->mac, mac, 6) != 0)
        nl_log(n, NL_L_LINK, 3, 0, T_WARN "ARP" T_RESET " %s: %s → %s", _("cache"), nl_ip_str(ip), nl_mac_str(mac));
    e->timer = 0;                       /* the retry timer finds the entry valid and stops */
    memcpy(e->mac, mac, 6);
    e->state = 2;
    e->ifx = ifx;
    e->expire = W.now + ARP_TTL;
    nw = e->nwait;
    memcpy(w, e->wait, sizeof(w));
    e->nwait = 0;
    for (k = 0; k < nw; k++)
        eth_out(n, ifx, mac, NL_ETH_IP, w[k]);
}

static void arp_input(nl_node *n, int ifx, const uint8_t *b, int len)
{
    int op;
    nl_ip sip, tip;
    const nl_iface *f = &n->ifc[ifx];

    if (len < 28 || nl_get16(b) != 1 || nl_get16(b + 2) != NL_ETH_IP)
        return;
    op = nl_get16(b + 6);
    sip = nl_get32(b + 14);
    tip = nl_get32(b + 24);
    /* learn the sender if it talks to us, refresh it if we already know it */
    arp_learn(n, ifx, sip, b + 8, f->ip && tip == f->ip);
    if (op == 1 && f->ip && tip == f->ip) {
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "ARP" T_RESET " %s is at %s", nl_ip_str(f->ip), nl_mac_str(f->mac));
        arp_send(n, ifx, 2, b + 8, sip);
    }
}

/* ------------------------------------------------------------------ */
/* Learning bridge with VLANs                                         */
/* ------------------------------------------------------------------ */

void nl_fdb_flush(nl_node *n)
{
    memset(n->fdb, 0, sizeof(n->fdb));
}

static nl_fdb *fdb_find(nl_node *n, const uint8_t *mac, int vlan, int create)
{
    nl_fdb *slot = NULL;
    int i;

    for (i = 0; i < (int)(sizeof(n->fdb) / sizeof(n->fdb[0])); i++) {
        nl_fdb *e = &n->fdb[i];
        if (e->used && W.now - e->seen > FDB_TTL)
            e->used = 0;
        if (e->used && e->vlan == vlan && memcmp(e->mac, mac, 6) == 0)
            return e;
        if (!slot || (!e->used && slot->used) || (e->used && slot->used && e->seen < slot->seen))
            slot = e;
    }
    if (!create)
        return NULL;
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, 6);
    slot->vlan = vlan;
    slot->port = -1;
    slot->used = 1;
    return slot;
}

static void bridge_out(nl_node *n, int port, int vid, const nl_pkt *p)
{
    const nl_iface *f = &n->ifc[port];
    nl_pkt *c;

    if (f->vlan == -1 && vid > 0) {                 /* trunk: add the 802.1Q tag */
        c = nl_pkt_new(p->id);
        c->hops = p->hops;
        memcpy(c->data, p->data, 12);
        nl_put16(c->data + 12, NL_ETH_VLAN);
        nl_put16(c->data + 14, (uint16_t)vid);
        memcpy(c->data + 16, p->data + 12, (size_t)(p->len - 12));
        c->len = (uint16_t)(p->len + 4);
    } else {
        c = nl_pkt_clone(p);
    }
    nl_link_tx(n, port, c);
}

static int port_in_vlan(const nl_iface *f, int vid)
{
    return f->vlan == -1 || f->vlan == vid;
}

static void bridge_input(nl_node *n, int in, nl_pkt *p)
{
    nl_iface *f = &n->ifc[in];
    nl_fdb *e;
    int vid = f->vlan > 0 ? f->vlan : 0, i, sent = 0;

    if (nl_get16(p->data + 12) == NL_ETH_VLAN && p->len >= 18) {
        if (f->vlan != -1) {                        /* tagged frame on an access port */
            nl_pkt_free(p);
            return;
        }
        vid = nl_get16(p->data + 14) & 0xfff;
        memmove(p->data + 12, p->data + 16, (size_t)(p->len - 16));
        p->len -= 4;
    } else if (f->vlan == -1) {
        vid = 1;                                    /* native VLAN */
    }

    e = fdb_find(n, p->data + 6, vid, 1);
    if (e->port != in) {
        nl_log(n, NL_L_LINK, 3, p->id, T_WARN "bridge" T_RESET " %s %s → %s%s", _("learned"),
               nl_mac_str(p->data + 6), f->name, e->port >= 0 ? _(" (moved)") : "");
        e->port = in;
    }
    e->seen = W.now;

    e = (p->data[0] & 1) ? NULL : fdb_find(n, p->data, vid, 0);
    if (e && e->port >= 0) {
        if (e->port != in && nl_if_usable(n, e->port) && !n->ifc[e->port].blocked &&
            port_in_vlan(&n->ifc[e->port], vid))
            bridge_out(n, e->port, vid, p);
    } else {
        for (i = 0; i < n->nif; i++)
            if (i != in && nl_if_usable(n, i) && !n->ifc[i].blocked && port_in_vlan(&n->ifc[i], vid)) {
                bridge_out(n, i, vid, p);
                sent++;
            }
        if (!(p->data[0] & 1))
            nl_log(n, NL_L_LINK, 3, p->id, T_WARN "bridge" T_RESET " %s %s: %s (%d)", _("unknown destination"),
                   nl_mac_str(p->data), _("flooding"), sent);
        nl_count(n, "bridge.flood", 1);
    }
    nl_pkt_free(p);
}

/* ------------------------------------------------------------------ */
/* IPv4                                                               */
/* ------------------------------------------------------------------ */

static void ip_hdr_write(uint8_t *h, int total, uint16_t id, int flags_off, uint8_t ttl, uint8_t proto,
                         uint8_t tos, nl_ip src, nl_ip dst)
{
    h[0] = 0x45;
    h[1] = tos;
    nl_put16(h + 2, (uint16_t)total);
    nl_put16(h + 4, id);
    nl_put16(h + 6, (uint16_t)flags_off);
    h[8] = ttl;
    h[9] = proto;
    nl_put16(h + 10, 0);
    nl_put32(h + 12, src);
    nl_put32(h + 16, dst);
    nl_put16(h + 10, nl_cksum(h, 20, 0));
}

/* Emit an IP packet (header fields in h20, payload separately) out of ifx
 * towards next hop nh, fragmenting when it exceeds the MTU. foff/mf are the
 * fragment offset (bytes) and more-fragments flag the packet already has. */
static int ip_emit(nl_node *n, int ifx, nl_ip nh, const uint8_t *h20, const uint8_t *pl, int len,
                   uint32_t pkt_id, int hops, int in_if)
{
    int mtu = n->ifc[ifx].link ? n->ifc[ifx].link->cfg.mtu : NL_DEFAULT_MTU;
    int fo = nl_get16(h20 + 6), df = fo & 0x4000, mf = fo & 0x2000, base = (fo & 0x1fff) * 8;
    int chunk = (mtu - NL_IP_HLEN) & ~7, off = 0, nfrag = 0;

    if (NL_IP_HLEN + len <= mtu)
        chunk = len;
    else if (df)
        return -1;
    else
        nl_log(n, NL_L_NET, 2, pkt_id, T_INFO "IP" T_RESET " %s: %d B > MTU %d → %d %s", _("fragmentation"),
               NL_IP_HLEN + len, mtu, (len + chunk - 1) / chunk, _("fragments"));
    do {
        int part = len - off < chunk ? len - off : chunk;
        int last = off + part >= len;
        nl_pkt *p = nl_pkt_new(pkt_id);
        uint8_t *h = p->data + NL_ETH_HLEN;

        p->hops = (uint8_t)hops;
        ip_hdr_write(h, NL_IP_HLEN + part, nl_get16(h20 + 4),
                     (df ? 0x4000 : 0) | ((!last || mf) ? 0x2000 : 0) | ((base + off) / 8),
                     h20[8], h20[9], h20[1], nl_get32(h20 + 12), nl_get32(h20 + 16));
        memcpy(h + NL_IP_HLEN, pl + off, (size_t)part);
        p->len = (uint16_t)(NL_ETH_HLEN + NL_IP_HLEN + part);
        if (run_hooks(n, NL_HOOK_POST, p, in_if, ifx) == NL_HOOK_DROP)
            nl_pkt_free(p);
        else
            arp_resolve_tx(n, ifx, nh, p);
        off += part;
        nfrag++;
    } while (off < len);
    return nfrag;
}

static void ip_local_input(nl_node *n, int in_if, nl_pkt *p);

typedef struct { nl_node *n; nl_pkt *p; } loop_ctx;

static void loop_deliver(void *ctx)
{
    loop_ctx *c = ctx;

    ip_local_input(c->n, -1, c->p);
    nl_pkt_free(c->p);
    free(c);
}

uint32_t nl_ip_send(nl_node *n, const nl_iptx *tx, const void *l4, int len)
{
    stack *s = S(n);
    const nl_route *r = NULL;
    uint8_t h[NL_IP_HLEN];
    nl_ip src = tx->src, nh = tx->dst;
    uint32_t id = tx->pkt_id ? tx->pkt_id : nl_pkt_newid();
    int ifx = tx->ifx;

    if (!n->up || len < 0 || len > 65000)
        return 0;
    if (!tx->use_if && nl_ip_is_local(n, tx->dst)) {
        loop_ctx *c;
        nl_pkt *p;

        if (len > NL_FRAME_MAX - NL_ETH_HLEN - NL_IP_HLEN)
            return 0;
        p = nl_pkt_new(id);
        c = malloc(sizeof(*c));
        if (!c) {
            nl_pkt_free(p);
            return 0;
        }
        ip_hdr_write(p->data + NL_ETH_HLEN, NL_IP_HLEN + len, s->ip_id++, 0, tx->ttl ? tx->ttl : 64,
                     tx->proto, tx->tos, src ? src : tx->dst, tx->dst);
        memcpy(p->data + NL_ETH_HLEN + NL_IP_HLEN, l4, (size_t)len);
        p->len = (uint16_t)(NL_ETH_HLEN + NL_IP_HLEN + len);
        c->n = n;
        c->p = p;
        nl_after(n, 0, loop_deliver, c);
        return id;
    }
    if (!tx->use_if) {
        r = nl_route_lookup(n, tx->dst);
        if (!r) {
            nl_log(n, NL_L_NET, 2, 0, T_ERR "IP" T_RESET " %s %s", _("no route to"), nl_ip_str(tx->dst));
            nl_count(n, "ip.noroute", 1);
            return 0;
        }
        ifx = r->ifx;
        if (r->gw)
            nh = r->gw;
    } else if (ifx < 0 || ifx >= n->nif) {
        return 0;
    }
    if (!src)
        src = n->ifc[ifx].ip;
    ip_hdr_write(h, NL_IP_HLEN + len, s->ip_id++, tx->df ? 0x4000 : 0, tx->ttl ? tx->ttl : 64,
                 tx->proto, tx->tos, src, tx->dst);
    if (ip_emit(n, ifx, nh, h, l4, len, id, 0, -1) < 0) {
        nl_log(n, NL_L_NET, 2, id, T_ERR "IP" T_RESET " %s (%d B, MTU %d)",
               _("packet too big and DF set: not sent"), NL_IP_HLEN + len,
               n->ifc[ifx].link ? n->ifc[ifx].link->cfg.mtu : 0);
        nl_count(n, "ip.df_drop", 1);
        return 0;
    }
    return id;
}

/* ---- ICMP ---- */

static void icmp_error(nl_node *n, int type, int code, const uint8_t *oip, int olen, int mtu)
{
    uint8_t b[8 + 60 + 8];
    nl_iptx tx;
    int ihl, quote;
    nl_ip osrc;

    if (olen < NL_IP_HLEN)
        return;
    ihl = (oip[0] & 15) * 4;
    osrc = nl_get32(oip + 12);
    /* never about ICMP errors, non-first fragments, broadcasts */
    if ((nl_get16(oip + 6) & 0x1fff) || !osrc || is_broadcast(n, nl_get32(oip + 16)) ||
        is_multicast(nl_get32(oip + 16)))
        return;
    if (oip[9] == NL_PROTO_ICMP && olen >= ihl + 1 && oip[ihl] != NL_ICMP_ECHO && oip[ihl] != NL_ICMP_ECHO_REPLY)
        return;
    quote = olen < ihl + 8 ? olen : ihl + 8;
    memset(b, 0, 8);
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)code;
    if (mtu)
        nl_put16(b + 6, (uint16_t)mtu);
    memcpy(b + 8, oip, (size_t)quote);
    nl_put16(b + 2, nl_cksum(b, 8 + quote, 0));
    memset(&tx, 0, sizeof(tx));
    tx.dst = osrc;
    tx.proto = NL_PROTO_ICMP;
    nl_log(n, NL_L_NET, 2, 0, T_TIME "ICMP" T_RESET " → %s: %s", nl_ip_str(osrc),
           type == NL_ICMP_TIME_EXCEEDED ? "time exceeded (TTL)"
           : code == 0 ? "network unreachable" : code == 1 ? "host unreachable"
           : code == 2 ? "protocol unreachable" : code == 3 ? "port unreachable"
           : code == 4 ? "fragmentation needed" : "unreachable");
    nl_count(n, "icmp.errors_sent", 1);
    nl_ip_send(n, &tx, b, 8 + quote);
}

uint32_t nl_icmp_echo(nl_node *n, nl_ip dst, uint16_t id, uint16_t seq, int ttl, int size, int df)
{
    static uint8_t b[8 + 8192];
    nl_iptx tx;
    int i;

    if (size < 0) size = 0;
    if (size > 8192) size = 8192;
    b[0] = NL_ICMP_ECHO;
    b[1] = 0;
    nl_put16(b + 2, 0);
    nl_put16(b + 4, id);
    nl_put16(b + 6, seq);
    for (i = 0; i < size; i++)
        b[8 + i] = (uint8_t)('a' + i % 26);
    nl_put16(b + 2, nl_cksum(b, 8 + size, 0));
    memset(&tx, 0, sizeof(tx));
    tx.dst = dst;
    tx.proto = NL_PROTO_ICMP;
    tx.ttl = (uint8_t)ttl;
    tx.df = df;
    return nl_ip_send(n, &tx, b, 8 + size);
}

static void icmp_input(nl_node *n, const nl_ipmeta *m)
{
    stack *s = S(n);
    const uint8_t *b = m->l4;
    nl_icmpmeta im;
    int i;

    if (m->l4len < 8)
        return;
    if (nl_cksum(b, m->l4len, 0) != 0) {
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_ERR "ICMP" T_RESET " %s", _("wrong checksum: discarded"));
        nl_count(n, "icmp.bad_checksum", 1);
        return;
    }
    memset(&im, 0, sizeof(im));
    im.from = m->src;
    im.type = b[0];
    im.code = b[1];
    im.ttl = m->ttl;
    im.pkt_id = m->pkt_id;
    im.data = b + 8;
    im.len = m->l4len - 8;
    if (b[0] == NL_ICMP_ECHO || b[0] == NL_ICMP_ECHO_REPLY) {
        im.id = nl_get16(b + 4);
        im.seq = nl_get16(b + 6);
    } else if (im.len >= NL_IP_HLEN) {
        const uint8_t *o = im.data;
        int ihl = (o[0] & 15) * 4;

        im.orig_src = nl_get32(o + 12);
        im.orig_dst = nl_get32(o + 16);
        im.orig_proto = o[9];
        im.mtu = nl_get16(b + 6);
        if (im.len >= ihl + 8) {
            if (o[9] == NL_PROTO_ICMP) {
                im.orig_id = nl_get16(o + ihl + 4);
                im.orig_sport = nl_get16(o + ihl + 6);          /* echo sequence number */
            } else {
                im.orig_sport = nl_get16(o + ihl);
                im.orig_dport = nl_get16(o + ihl + 2);
            }
        }
    }
    if (b[0] == NL_ICMP_ECHO) {
        static uint8_t r[8 + 8192];
        nl_iptx tx;

        if (is_broadcast(n, m->dst) || m->l4len > (int)sizeof(r))
            return;
        memcpy(r, b, (size_t)m->l4len);
        r[0] = NL_ICMP_ECHO_REPLY;
        nl_put16(r + 2, 0);
        nl_put16(r + 2, nl_cksum(r, m->l4len, 0));
        memset(&tx, 0, sizeof(tx));
        tx.src = m->dst;
        tx.dst = m->src;
        tx.proto = NL_PROTO_ICMP;
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_TIME "ICMP" T_RESET " echo request %s %s seq=%u → reply",
               _("from"), nl_ip_str(m->src), im.seq);
        nl_count(n, "icmp.echo_served", 1);
        nl_ip_send(n, &tx, r, m->l4len);
    }
    for (i = 0; i < s->nicmpw; i++)
        s->icmpw[i].fn(n, s->icmpw[i].ctx, &im);
}

/* ---- UDP ---- */

int nl_udp_bind(nl_node *n, uint16_t port, nl_udp_fn fn, void *ctx)
{
    stack *s = S(n);
    int i;

    if (!port) {
        for (;;) {
            port = s->eph++;
            if (s->eph < 49152)
                s->eph = 49152;
            for (i = 0; i < s->nudp; i++)
                if (s->udp[i].port == port)
                    break;
            if (i == s->nudp)
                break;
        }
    }
    for (i = 0; i < s->nudp; i++)
        if (s->udp[i].port == port)
            break;
    if (i == s->nudp) {
        if (s->nudp >= (int)(sizeof(s->udp) / sizeof(s->udp[0])))
            return 0;
        s->nudp++;
    }
    s->udp[i].port = port;
    s->udp[i].fn = fn;
    s->udp[i].ctx = ctx;
    return port;
}

void nl_udp_unbind(nl_node *n, uint16_t port)
{
    stack *s = S(n);
    int i;

    for (i = 0; i < s->nudp; i++)
        if (s->udp[i].port == port) {
            s->udp[i] = s->udp[--s->nudp];
            return;
        }
}

uint32_t nl_udp_send_ex(nl_node *n, const nl_iptx *tx0, uint16_t sport, uint16_t dport,
                        const void *data, int len)
{
    static uint8_t b[NL_UDP_HLEN + 16384];
    nl_iptx tx = *tx0;
    nl_ip src = tx.src;

    if (len < 0 || len > 16384)
        return 0;
    tx.proto = NL_PROTO_UDP;
    if (!src) {                         /* the checksum needs the source address now */
        if (tx.use_if && tx.ifx >= 0 && tx.ifx < n->nif) {
            src = n->ifc[tx.ifx].ip;
        } else if (nl_ip_is_local(n, tx.dst)) {
            src = tx.dst;
        } else {
            const nl_route *r = nl_route_lookup(n, tx.dst);
            if (r)
                src = n->ifc[r->ifx].ip;
        }
        tx.src = src;
    }
    nl_put16(b, sport);
    nl_put16(b + 2, dport);
    nl_put16(b + 4, (uint16_t)(NL_UDP_HLEN + len));
    nl_put16(b + 6, 0);
    memcpy(b + NL_UDP_HLEN, data, (size_t)len);
    nl_put16(b + 6, nl_l4_cksum(src, tx.dst, NL_PROTO_UDP, b, NL_UDP_HLEN + len));
    if (nl_get16(b + 6) == 0)
        nl_put16(b + 6, 0xffff);
    return nl_ip_send(n, &tx, b, NL_UDP_HLEN + len);
}

uint32_t nl_udp_send(nl_node *n, uint16_t sport, nl_ip dst, uint16_t dport, const void *data, int len)
{
    nl_iptx tx;

    memset(&tx, 0, sizeof(tx));
    tx.dst = dst;
    return nl_udp_send_ex(n, &tx, sport, dport, data, len);
}

static void udp_input(nl_node *n, const nl_ipmeta *m)
{
    stack *s = S(n);
    const uint8_t *b = m->l4;
    nl_udpmsg um;
    int i, ulen;

    if (m->l4len < NL_UDP_HLEN)
        return;
    ulen = nl_get16(b + 4);
    if (ulen < NL_UDP_HLEN || ulen > m->l4len)
        ulen = m->l4len;
    if (nl_get16(b + 6) != 0 && nl_l4_cksum(m->src, m->dst, NL_PROTO_UDP, b, ulen) != 0) {
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_ERR "UDP" T_RESET " %s",
               _("wrong checksum: datagram discarded"));
        nl_count(n, "udp.bad_checksum", 1);
        return;
    }
    um.src = m->src;
    um.dst = m->dst;
    um.sport = nl_get16(b);
    um.dport = nl_get16(b + 2);
    um.data = b + NL_UDP_HLEN;
    um.len = ulen - NL_UDP_HLEN;
    um.in_if = m->in_if;
    um.ttl = m->ttl;
    um.pkt_id = m->pkt_id;
    for (i = 0; i < s->nudp; i++)
        if (s->udp[i].port == um.dport) {
            s->udp[i].fn(n, s->udp[i].ctx, &um);
            return;
        }
    nl_count(n, "udp.no_port", 1);
    if (!is_broadcast(n, m->dst) && !is_multicast(m->dst) && m->pkt)
        icmp_error(n, NL_ICMP_UNREACH, 3, m->pkt->data + NL_ETH_HLEN, m->pkt->len - NL_ETH_HLEN, 0);
}

/* ---- reassembly ---- */

static int reasm_add(nl_node *n, const uint8_t *ip, int ihl, int total, const uint8_t **out, int *outlen)
{
    stack *s = S(n);
    nl_ip src = nl_get32(ip + 12), dst = nl_get32(ip + 16);
    uint16_t id = nl_get16(ip + 4);
    int fo = nl_get16(ip + 6), off = (fo & 0x1fff) * 8, mf = fo & 0x2000, len = total - ihl, i, blocks;
    reasm *r = NULL, **slot = NULL;

    for (i = 0; i < 4; i++) {
        reasm *c = s->fr[i];
        if (c && c->used && W.now - c->t > FRAG_TTL)
            c->used = 0;
        if (c && c->used && c->src == src && c->dst == dst && c->id == id && c->proto == ip[9])
            r = c;
        if (!slot && (!c || !c->used))
            slot = &s->fr[i];
    }
    if (!r) {
        if (!slot)
            slot = &s->fr[nl_rand(4)];
        if (!*slot)
            *slot = malloc(sizeof(reasm));
        r = *slot;
        if (!r)
            return 0;
        memset(r, 0, offsetof(reasm, buf));
        memset(r->have, 0, sizeof(r->have));
        r->used = 1;
        r->src = src;
        r->dst = dst;
        r->id = id;
        r->proto = ip[9];
        r->t = W.now;
        r->total = -1;
    }
    if (off + len > FRAG_MAX)
        return 0;
    memcpy(r->buf + off, ip + ihl, (size_t)len);
    for (i = off / 8; i < (off + len + 7) / 8; i++)
        r->have[i / 8] |= (uint8_t)(1 << (i % 8));
    if (!mf)
        r->total = off + len;
    if (r->total < 0)
        return 0;
    blocks = (r->total + 7) / 8;
    for (i = 0; i < blocks; i++)
        if (!(r->have[i / 8] & (1 << (i % 8))))
            return 0;
    r->used = 0;
    *out = r->buf;
    *outlen = r->total;
    return 1;
}

/* Deliver an IP packet addressed to this node */
static void ip_local_input(nl_node *n, int in_if, nl_pkt *p)
{
    stack *s = S(n);
    const uint8_t *ip = p->data + NL_ETH_HLEN;
    int ihl = (ip[0] & 15) * 4, total = nl_get16(ip + 2), i;
    nl_ipmeta m;

    m.src = nl_get32(ip + 12);
    m.dst = nl_get32(ip + 16);
    m.proto = ip[9];
    m.ttl = ip[8];
    m.tos = ip[1];
    m.id = nl_get16(ip + 4);
    m.in_if = in_if;
    m.pkt_id = p->id;
    m.pkt = p;
    m.l4 = ip + ihl;
    m.l4len = total - ihl;
    if (nl_get16(ip + 6) & 0x3fff) {
        if (!reasm_add(n, ip, ihl, total, &m.l4, &m.l4len))
            return;
        nl_log(n, NL_L_NET, 2, p->id, T_INFO "IP" T_RESET " %s (%d B)", _("datagram reassembled"), m.l4len);
        nl_count(n, "ip.reassembled", 1);
    }
    if (m.proto == NL_PROTO_ICMP) {
        icmp_input(n, &m);
        return;
    }
    if (m.proto == NL_PROTO_UDP) {
        udp_input(n, &m);
        return;
    }
    for (i = 0; i < s->nipp; i++)
        if (s->ipp[i].proto == m.proto) {
            s->ipp[i].fn(n, s->ipp[i].ctx, &m);
            return;
        }
    if (!is_broadcast(n, m.dst) && !is_multicast(m.dst))
        icmp_error(n, NL_ICMP_UNREACH, 2, ip, total, 0);
}

static void ip_input(nl_node *n, int in_if, nl_pkt *p)
{
    uint8_t *ip = p->data + NL_ETH_HLEN;
    int ihl, total;
    nl_ip dst;
    const nl_route *r;

    if (p->len < NL_ETH_HLEN + NL_IP_HLEN || (ip[0] >> 4) != 4)
        goto drop;
    ihl = (ip[0] & 15) * 4;
    total = nl_get16(ip + 2);
    if (ihl < NL_IP_HLEN || total < ihl || NL_ETH_HLEN + total > p->len)
        goto drop;
    if (nl_cksum(ip, ihl, 0) != 0) {
        nl_log(n, NL_L_NET, 2, p->id, T_ERR "IP" T_RESET " %s", _("wrong header checksum: packet discarded"));
        nl_count(n, "ip.bad_checksum", 1);
        goto drop;
    }
    p->len = (uint16_t)(NL_ETH_HLEN + total);
    if (run_hooks(n, NL_HOOK_PRE, p, in_if, -1) == NL_HOOK_DROP)
        goto drop;
    dst = nl_get32(ip + 16);

    if (nl_ip_is_local(n, dst) || is_broadcast(n, dst) || is_multicast(dst)) {
        ip_local_input(n, in_if, p);
        goto drop;
    }
    if (!n->forward) {
        nl_count(n, "ip.not_for_me", 1);
        goto drop;
    }
    if (ip[8] <= 1) {
        nl_log(n, NL_L_NET, 2, p->id, T_WARN "IP" T_RESET " TTL %s (%s → %s)", _("expired"),
               nl_ip_str(nl_get32(ip + 12)), nl_ip_str(dst));
        nl_count(n, "ip.ttl_expired", 1);
        icmp_error(n, NL_ICMP_TIME_EXCEEDED, 0, ip, total, 0);
        goto drop;
    }
    r = nl_route_lookup(n, dst);
    if (!r) {
        nl_log(n, NL_L_NET, 2, p->id, T_ERR "IP" T_RESET " %s %s", _("no route to"), nl_ip_str(dst));
        nl_count(n, "ip.noroute", 1);
        icmp_error(n, NL_ICMP_UNREACH, 0, ip, total, 0);
        goto drop;
    }
    if (run_hooks(n, NL_HOOK_FWD, p, in_if, r->ifx) == NL_HOOK_DROP)
        goto drop;
    ip[8]--;
    nl_put16(ip + 10, 0);
    nl_put16(ip + 10, nl_cksum(ip, ihl, 0));
    nl_log(n, NL_L_NET, 3, p->id, T_INFO "IP" T_RESET " forward %s → %s via %s dev %s ttl %d",
           nl_ip_str(nl_get32(ip + 12)), nl_ip_str(dst), r->gw ? nl_ip_str(r->gw) : "direct",
           n->ifc[r->ifx].name, ip[8]);
    nl_count(n, "ip.forwarded", 1);
    {
        uint8_t h[NL_IP_HLEN];
        memcpy(h, ip, NL_IP_HLEN);
        if (ip_emit(n, r->ifx, r->gw ? r->gw : dst, h, ip + ihl, total - ihl, p->id, p->hops, in_if) < 0) {
            int mtu = n->ifc[r->ifx].link->cfg.mtu;
            nl_log(n, NL_L_NET, 2, p->id, T_WARN "IP" T_RESET " %s (MTU %d)",
                   _("too big and DF set: dropped, ICMP sent"), mtu);
            nl_count(n, "ip.df_drop", 1);
            icmp_error(n, NL_ICMP_UNREACH, 4, ip, total, mtu);
        }
    }
drop:
    nl_pkt_free(p);
}

/* ------------------------------------------------------------------ */
/* Frame input                                                        */
/* ------------------------------------------------------------------ */

void nl_stack_rx(nl_node *n, int ifx, nl_pkt *p)
{
    stack *s = S(n);
    nl_iface *f = &n->ifc[ifx];
    uint16_t type;
    int i;

    if (p->len < NL_ETH_HLEN) {
        nl_pkt_free(p);
        return;
    }
    f->rx++;
    f->rx_bytes += p->len;
    type = nl_get16(p->data + 12);

    for (i = 0; i < s->neth; i++)
        if (s->eth[i].type == type) {
            nl_ethmeta m = { p->data, p->data + 6, type, p->data + NL_ETH_HLEN, p->len - NL_ETH_HLEN, ifx, p };
            s->eth[i].fn(n, s->eth[i].ctx, &m);
            nl_pkt_free(p);
            return;
        }
    if (n->kind == NL_SWITCH) {
        if (f->blocked)
            nl_pkt_free(p);
        else
            bridge_input(n, ifx, p);
        return;
    }
    if (memcmp(p->data, f->mac, 6) != 0 && !(p->data[0] & 1)) {
        nl_count(n, "eth.not_for_me", 1);
        nl_pkt_free(p);
        return;
    }
    if (type == NL_ETH_ARP) {
        arp_input(n, ifx, p->data + NL_ETH_HLEN, p->len - NL_ETH_HLEN);
        nl_pkt_free(p);
    } else if (type == NL_ETH_IP) {
        ip_input(n, ifx, p);
    } else if (type == NL_ETH_IP6 && W.ip6) {
        nl_ip6_input(n, ifx, p);
    } else {
        nl_pkt_free(p);
    }
}

/* ------------------------------------------------------------------ */
/* Editable packet view (NAT and friends)                             */
/* ------------------------------------------------------------------ */

int nl_ipv_parse(const nl_pkt *p, nl_ipview *v)
{
    const uint8_t *ip = p->data + NL_ETH_HLEN, *l4;

    memset(v, 0, sizeof(*v));
    if (p->len < NL_ETH_HLEN + NL_IP_HLEN || (ip[0] >> 4) != 4)
        return 0;
    v->ihl = (ip[0] & 15) * 4;
    v->src = nl_get32(ip + 12);
    v->dst = nl_get32(ip + 16);
    v->proto = ip[9];
    v->ttl = ip[8];
    v->tos = ip[1];
    v->l4off = NL_ETH_HLEN + v->ihl;
    v->l4len = nl_get16(ip + 2) - v->ihl;
    if (v->l4off + v->l4len > p->len)
        v->l4len = p->len - v->l4off;
    v->frag = (nl_get16(ip + 6) & 0x1fff) != 0;
    l4 = p->data + v->l4off;
    if (v->frag)
        return 1;
    if ((v->proto == NL_PROTO_TCP && v->l4len >= 20) || (v->proto == NL_PROTO_UDP && v->l4len >= 8)) {
        v->sport = nl_get16(l4);
        v->dport = nl_get16(l4 + 2);
        v->has_ports = 1;
        if (v->proto == NL_PROTO_TCP)
            v->tcp_flags = l4[13];
    } else if (v->proto == NL_PROTO_ICMP && v->l4len >= 8) {
        v->icmp_type = l4[0];
        v->icmp_code = l4[1];
        if (l4[0] == NL_ICMP_ECHO || l4[0] == NL_ICMP_ECHO_REPLY) {
            v->sport = v->dport = nl_get16(l4 + 4);
            v->has_ports = 1;
        }
    }
    return 1;
}

void nl_ipv_commit(nl_pkt *p, const nl_ipview *v)
{
    uint8_t *ip = p->data + NL_ETH_HLEN, *l4 = p->data + v->l4off;
    int more = nl_get16(ip + 6) & 0x2000;

    ip[1] = v->tos;
    ip[8] = v->ttl;
    nl_put32(ip + 12, v->src);
    nl_put32(ip + 16, v->dst);
    nl_put16(ip + 10, 0);
    nl_put16(ip + 10, nl_cksum(ip, v->ihl, 0));
    if (v->frag || !v->has_ports)
        return;
    if (v->proto == NL_PROTO_TCP) {
        nl_put16(l4, v->sport);
        nl_put16(l4 + 2, v->dport);
        if (!more) {
            nl_put16(l4 + 16, 0);
            nl_put16(l4 + 16, nl_l4_cksum(v->src, v->dst, NL_PROTO_TCP, l4, v->l4len));
        }
    } else if (v->proto == NL_PROTO_UDP) {
        nl_put16(l4, v->sport);
        nl_put16(l4 + 2, v->dport);
        if (more) {
            nl_put16(l4 + 6, 0);            /* cannot recompute over a fragment: no checksum */
        } else if (nl_get16(l4 + 6) != 0) {
            uint16_t c;
            nl_put16(l4 + 6, 0);
            c = nl_l4_cksum(v->src, v->dst, NL_PROTO_UDP, l4, v->l4len);
            nl_put16(l4 + 6, c ? c : 0xffff);
        }
    } else if (v->proto == NL_PROTO_ICMP) {
        nl_put16(l4 + 4, v->sport);
        if (!more) {
            nl_put16(l4 + 2, 0);
            nl_put16(l4 + 2, nl_cksum(l4, v->l4len, 0));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Dissectors                                                         */
/* ------------------------------------------------------------------ */

#define DIS_MAX 48
static struct { int kind; uint32_t key; nl_dissect_fn fn; } g_dis[DIS_MAX];
static int g_ndis;
enum { DK_UDP, DK_TCP, DK_IP, DK_ETH };

static void dis_reg(int kind, uint32_t key, nl_dissect_fn fn)
{
    if (g_ndis < DIS_MAX) {
        g_dis[g_ndis].kind = kind;
        g_dis[g_ndis].key = key;
        g_dis[g_ndis].fn = fn;
        g_ndis++;
    }
}

void nl_dissector_udp(uint16_t port, nl_dissect_fn fn) { dis_reg(DK_UDP, port, fn); }
void nl_dissector_tcp(uint16_t port, nl_dissect_fn fn) { dis_reg(DK_TCP, port, fn); }
void nl_dissector_ip(uint8_t proto, nl_dissect_fn fn)  { dis_reg(DK_IP, proto, fn); }
void nl_dissector_eth(uint16_t type, nl_dissect_fn fn) { dis_reg(DK_ETH, type, fn); }

static nl_dissect_fn dis_find(int kind, uint32_t key)
{
    int i;

    for (i = 0; i < g_ndis; i++)
        if (g_dis[i].kind == kind && g_dis[i].key == key)
            return g_dis[i].fn;
    return NULL;
}

void nl_dis_field(nl_dissect *o, const char *layer, const char *name, int off, int len, const char *fmt, ...)
{
    nl_field *f;
    va_list ap;

    if (o->nf >= (int)(sizeof(o->f) / sizeof(o->f[0])))
        return;
    f = &o->f[o->nf++];
    snprintf(f->layer, sizeof(f->layer), "%s", layer);
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->off = (short)off;
    f->len = (short)len;
    va_start(ap, fmt);
    vsnprintf(f->value, sizeof(f->value), fmt, ap);
    va_end(ap);
}

void nl_dis_summary(nl_dissect *o, const char *proto, short color, const char *fmt, ...)
{
    va_list ap;

    snprintf(o->proto, sizeof(o->proto), "%s", proto);
    o->color = color;
    va_start(ap, fmt);
    vsnprintf(o->summary, sizeof(o->summary), fmt, ap);
    va_end(ap);
}

void nl_dissect_tcp_payload(nl_dissect *o, uint16_t sport, uint16_t dport, const uint8_t *d, int len, int off)
{
    nl_dissect_fn fn = dis_find(DK_TCP, dport);

    if (!fn)
        fn = dis_find(DK_TCP, sport);
    if (fn && len > 0)
        fn(o, d, len, off);
}

void nl_dissect_udp_payload(nl_dissect *o, uint16_t sport, uint16_t dport, const uint8_t *d, int len, int off)
{
    nl_dissect_fn fn = dis_find(DK_UDP, dport);

    if (!fn)
        fn = dis_find(DK_UDP, sport);
    if (fn && len > 0)
        fn(o, d, len, off);
}

int nl_dissect_ipproto(nl_dissect *o, uint8_t proto, const uint8_t *d, int len, int off)
{
    nl_dissect_fn fn = dis_find(DK_IP, proto);

    if (fn)
        fn(o, d, len, off);
    return fn != NULL;
}

static const char *icmp_name(int type, int code)
{
    switch (type) {
    case NL_ICMP_ECHO:          return "Echo request";
    case NL_ICMP_ECHO_REPLY:    return "Echo reply";
    case NL_ICMP_TIME_EXCEEDED: return "Time exceeded (TTL)";
    case NL_ICMP_REDIRECT:      return "Redirect";
    case NL_ICMP_UNREACH:
        return code == 0 ? "Network unreachable" : code == 1 ? "Host unreachable"
             : code == 2 ? "Protocol unreachable" : code == 3 ? "Port unreachable"
             : code == 4 ? "Fragmentation needed" : "Destination unreachable";
    default:                    return "ICMP";
    }
}

static void dis_ip(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int ihl, total, fo, l4len;
    nl_ip src, dst;
    const uint8_t *l4;
    nl_dissect_fn fn;

    if (len < NL_IP_HLEN) {
        nl_dis_summary(o, "IP", 114, "truncated IP packet");
        return;
    }
    ihl = (d[0] & 15) * 4;
    total = nl_get16(d + 2);
    fo = nl_get16(d + 6);
    src = nl_get32(d + 12);
    dst = nl_get32(d + 16);
    nl_dis_field(o, "IPv4", "Version / IHL", off, 1, "%d / %d (%d bytes)", d[0] >> 4, d[0] & 15, ihl);
    nl_dis_field(o, "IPv4", "DSCP / ECN", off + 1, 1, "%d / %s", d[1] >> 2,
                 (d[1] & 3) == 0 ? "Not-ECT" : (d[1] & 3) == 3 ? "CE (congestion experienced)" : "ECT");
    nl_dis_field(o, "IPv4", "Total length", off + 2, 2, "%d", total);
    nl_dis_field(o, "IPv4", "Identification", off + 4, 2, "%u", nl_get16(d + 4));
    nl_dis_field(o, "IPv4", "Flags / Offset", off + 6, 2, "%s%s%s, offset %d", fo & 0x4000 ? "DF " : "",
                 fo & 0x2000 ? "MF" : "", fo & 0x6000 ? "" : "none", (fo & 0x1fff) * 8);
    nl_dis_field(o, "IPv4", "TTL", off + 8, 1, "%d", d[8]);
    nl_dis_field(o, "IPv4", "Protocol", off + 9, 1, "%d (%s)", d[9],
                 d[9] == 1 ? "ICMP" : d[9] == 6 ? "TCP" : d[9] == 17 ? "UDP" : d[9] == 89 ? "OSPF" : "?");
    nl_dis_field(o, "IPv4", "Header checksum", off + 10, 2, "0x%04x %s", nl_get16(d + 10),
                 nl_cksum(d, ihl < len ? ihl : len, 0) == 0 ? "✓" : "✗ WRONG");
    nl_dis_field(o, "IPv4", "Source", off + 12, 4, "%s", nl_ip_str(src));
    nl_dis_field(o, "IPv4", "Destination", off + 16, 4, "%s", nl_ip_str(dst));
    snprintf(o->addr, sizeof(o->addr), "%s → %s", nl_ip_str(src), nl_ip_str(dst));

    l4 = d + ihl;
    l4len = (total < len ? total : len) - ihl;
    off += ihl;
    if (fo & 0x1fff) {
        nl_dis_summary(o, "IP", 114, "Fragment id=%u offset=%d len=%d%s", nl_get16(d + 4), (fo & 0x1fff) * 8,
                       l4len, fo & 0x2000 ? " MF" : " (last)");
        return;
    }
    if (d[9] == NL_PROTO_ICMP && l4len >= 8) {
        nl_dis_field(o, "ICMP", "Type / Code", off, 2, "%d / %d (%s)", l4[0], l4[1], icmp_name(l4[0], l4[1]));
        nl_dis_field(o, "ICMP", "Checksum", off + 2, 2, "0x%04x %s", nl_get16(l4 + 2),
                     (fo & 0x2000) ? "" : nl_cksum(l4, l4len, 0) == 0 ? "✓" : "✗ WRONG");
        if (l4[0] == NL_ICMP_ECHO || l4[0] == NL_ICMP_ECHO_REPLY) {
            nl_dis_field(o, "ICMP", "Identifier", off + 4, 2, "%u", nl_get16(l4 + 4));
            nl_dis_field(o, "ICMP", "Sequence", off + 6, 2, "%u", nl_get16(l4 + 6));
            nl_dis_field(o, "ICMP", "Data", off + 8, l4len - 8, "%d bytes", l4len - 8);
            nl_dis_summary(o, "ICMP", 213, "%s id=%u seq=%u ttl=%d", icmp_name(l4[0], l4[1]),
                           nl_get16(l4 + 4), nl_get16(l4 + 6), d[8]);
        } else {
            if (l4len >= 8 + NL_IP_HLEN)
                nl_dis_field(o, "ICMP", "Original packet", off + 8, l4len - 8, "%s → %s proto %d",
                             nl_ip_str(nl_get32(l4 + 8 + 12)), nl_ip_str(nl_get32(l4 + 8 + 16)), l4[8 + 9]);
            nl_dis_summary(o, "ICMP", 213, "%s", icmp_name(l4[0], l4[1]));
        }
    } else if (d[9] == NL_PROTO_UDP && l4len >= 8) {
        uint16_t sp = nl_get16(l4), dp = nl_get16(l4 + 2);

        nl_dis_field(o, "UDP", "Source port", off, 2, "%u", sp);
        nl_dis_field(o, "UDP", "Destination port", off + 2, 2, "%u", dp);
        nl_dis_field(o, "UDP", "Length", off + 4, 2, "%u", nl_get16(l4 + 4));
        nl_dis_field(o, "UDP", "Checksum", off + 6, 2, "0x%04x %s", nl_get16(l4 + 6),
                     nl_get16(l4 + 6) == 0 ? "(none)" : (fo & 0x2000) ? ""
                     : nl_l4_cksum(src, dst, NL_PROTO_UDP, l4, l4len) == 0 ? "✓" : "✗ WRONG");
        nl_dis_summary(o, "UDP", 117, "%u → %u len=%d", sp, dp, l4len - 8);
        if (l4len > 8)
            nl_dis_field(o, "UDP", "Payload", off + 8, l4len - 8, "%d bytes", l4len - 8);
        fn = dis_find(DK_UDP, dp);
        if (!fn)
            fn = dis_find(DK_UDP, sp);
        if (fn && l4len > 8)
            fn(o, l4 + 8, l4len - 8, off + 8);
    } else if ((fn = dis_find(DK_IP, d[9])) != NULL) {
        fn(o, l4, l4len, off);
    } else {
        nl_dis_summary(o, "IP", 114, "protocol %d, %d bytes", d[9], l4len);
    }
}

void nl_dissect_frame(const uint8_t *d, int len, nl_dissect *o)
{
    uint16_t type;
    int off = NL_ETH_HLEN;
    nl_dissect_fn fn;

    memset(o, 0, offsetof(nl_dissect, f));
    o->color = C_DIM;
    if (len < NL_ETH_HLEN) {
        nl_dis_summary(o, "?", C_DIM, "short frame (%d bytes)", len);
        return;
    }
    type = nl_get16(d + 12);
    nl_dis_field(o, "Ethernet", "Destination", 0, 6, "%s%s", nl_mac_str(d),
                 memcmp(d, NL_MAC_BROADCAST, 6) == 0 ? " (broadcast)" : (d[0] & 1) ? " (multicast)" : "");
    nl_dis_field(o, "Ethernet", "Source", 6, 6, "%s", nl_mac_str(d + 6));
    snprintf(o->addr, sizeof(o->addr), "%s → %s", nl_mac_str(d + 6), nl_mac_str(d));
    if (type == NL_ETH_VLAN && len >= 18) {
        nl_dis_field(o, "802.1Q", "VLAN id", 14, 2, "%d (priority %d)", nl_get16(d + 14) & 0xfff, d[14] >> 5);
        type = nl_get16(d + 16);
        d += 4;
        len -= 4;
        off += 4;
    }
    nl_dis_field(o, "Ethernet", "EtherType", off - 2, 2, "0x%04x (%s)", type,
                 type == NL_ETH_IP ? "IPv4" : type == NL_ETH_ARP ? "ARP" : type == NL_ETH_IP6 ? "IPv6" : "other");
    if (type == NL_ETH_ARP && len >= NL_ETH_HLEN + 28) {
        const uint8_t *a = d + NL_ETH_HLEN;
        int op = nl_get16(a + 6);

        nl_dis_field(o, "ARP", "Operation", off + 6, 2, "%d (%s)", op, op == 1 ? "request" : "reply");
        nl_dis_field(o, "ARP", "Sender MAC", off + 8, 6, "%s", nl_mac_str(a + 8));
        nl_dis_field(o, "ARP", "Sender IP", off + 14, 4, "%s", nl_ip_str(nl_get32(a + 14)));
        nl_dis_field(o, "ARP", "Target MAC", off + 18, 6, "%s", nl_mac_str(a + 18));
        nl_dis_field(o, "ARP", "Target IP", off + 24, 4, "%s", nl_ip_str(nl_get32(a + 24)));
        if (op == 1)
            nl_dis_summary(o, "ARP", 221, "Who has %s? Tell %s", nl_ip_str(nl_get32(a + 24)),
                           nl_ip_str(nl_get32(a + 14)));
        else
            nl_dis_summary(o, "ARP", 221, "%s is at %s", nl_ip_str(nl_get32(a + 14)), nl_mac_str(a + 8));
    } else if (type == NL_ETH_IP) {
        dis_ip(o, d + NL_ETH_HLEN, len - NL_ETH_HLEN, off);
    } else if ((fn = dis_find(DK_ETH, type)) != NULL) {
        fn(o, d + NL_ETH_HLEN, len - NL_ETH_HLEN, off);
    } else {
        nl_dis_summary(o, "ETH", C_DIM, "EtherType 0x%04x, %d bytes", type, len - NL_ETH_HLEN);
    }
}
