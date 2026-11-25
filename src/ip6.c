/*
 * ip6.c - IPv6 for the nodes of a lab that asks for it: addresses and
 *         prefixes, the routing table, the IPv6 header with source
 *         fragmentation and reassembly (RFC 8200), ICMPv6 (RFC 4443),
 *         Neighbor Discovery with duplicate address detection and router
 *         advertisements (RFC 4861), stateless autoconfiguration
 *         (RFC 4862), path MTU discovery (RFC 8201), UDP over IPv6, the
 *         `ra` module for routers, and the dissectors.
 *
 * Left out, on purpose: MLD (multicast listeners are not announced; the
 * bridges flood multicast anyway), extension headers other than
 * Fragment (they are skipped, not acted upon), redirects, privacy
 * addresses, and the DELAY state of neighbor unreachability detection
 * (folded into PROBE).
 */
#include "nl_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ND_RETRANS     NL_SEC(1)
#define ND_REACHABLE   NL_SEC(30)
#define ND_PROBE_DELAY NL_SEC(5)
#define DAD_TIME       NL_SEC(1)
#define RS_INTERVAL    NL_SEC(4)
#define FRAG6_MAX      16384

typedef struct {
    int      used;
    nl_ip6   src, dst;
    uint32_t id;
    nl_time  t;
    int      total;
    uint8_t  proto;
    uint8_t  buf[FRAG6_MAX];
    uint8_t  have[FRAG6_MAX / 64];
} reasm6;

typedef struct {
    struct { uint8_t proto; nl_ip6_fn fn; void *ctx; } ipp[8];
    struct { uint16_t port; nl_udp6_fn fn; void *ctx; } udp[128];
    struct { int point; nl_hook_fn fn; void *ctx; } hook[8];
    struct { nl_icmp6_fn fn; void *ctx; } icw[8];
    int      nipp, nudp, nhook, nicw;
    uint16_t eph;
    uint32_t frag_id;
    struct { nl_ip6 dst; int mtu; } pmtu[8];
    int      npmtu;
    reasm6  *fr[4];
    nl_ev    tick;
    nl_time  tick_at;
    nl_time  dad_until[NL_MAX_IF][NL_MAX_IP6];
    int      rs_left[NL_MAX_IF];
    nl_time  rs_next[NL_MAX_IF];
    int      started;
    nl_ip6   group[6];                  /* multicast groups joined by modules (nl_ip6_join) */
    int      ngroup;
    struct { nl_ra_fn fn; void *ctx; } raw[2];
    int      nraw;
} stack6;

static const nl_ip6 ZERO6;
static const nl_ip6 ALL_NODES = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
static const nl_ip6 ALL_ROUTERS = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 } };

static stack6 *S6(const nl_node *n) { return n->stack6; }
static void nd_arm(nl_node *n);
static void ra_solicited(nl_node *n, int ifx);

int nl_ip6_enabled(void) { return W.ip6; }

/* ------------------------------------------------------------------ */
/* Addresses                                                          */
/* ------------------------------------------------------------------ */

int nl_ip6_eq(const nl_ip6 *a, const nl_ip6 *b) { return memcmp(a->b, b->b, 16) == 0; }
int nl_ip6_zero(const nl_ip6 *a)                { return memcmp(a->b, ZERO6.b, 16) == 0; }
int nl_ip6_linklocal(const nl_ip6 *a)           { return a->b[0] == 0xfe && (a->b[1] & 0xc0) == 0x80; }
int nl_ip6_multicast(const nl_ip6 *a)           { return a->b[0] == 0xff; }

int nl_ip6_match(const nl_ip6 *a, const nl_ip6 *p, int plen)
{
    int full = plen / 8, rest = plen % 8;

    if (memcmp(a->b, p->b, (size_t)full) != 0)
        return 0;
    return !rest || ((a->b[full] ^ p->b[full]) & (0xff << (8 - rest))) == 0;
}

int nl_parse_ip6(const char *s, nl_ip6 *out)
{
    uint16_t g[8];
    int n = 0, gap = -1, i;

    if (!strchr(s, ':'))
        return 0;
    if (s[0] == ':' && s[1] == ':') {
        gap = 0;
        s += 2;
    }
    while (*s) {
        char *end;
        unsigned long v;

        if (n == 8 || !isxdigit((unsigned char)*s))
            return 0;
        v = strtoul(s, &end, 16);
        if (v > 0xffff || end - s > 4)
            return 0;
        g[n++] = (uint16_t)v;
        s = end;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0)
                    return 0;
                gap = n;
                s++;
            } else if (!*s) {
                return 0;
            }
        } else if (*s) {
            return 0;
        }
    }
    if ((gap < 0 && n != 8) || (gap >= 0 && n == 8))
        return 0;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < n; i++) {
        int pos = gap < 0 || i < gap ? i : 8 - (n - i);
        nl_put16(out->b + 2 * pos, g[i]);
    }
    return 1;
}

int nl_parse_prefix6(const char *s, nl_ip6 *ip, int *plen)
{
    char buf[64];
    char *slash;

    snprintf(buf, sizeof(buf), "%s", s);
    slash = strchr(buf, '/');
    *plen = 128;
    if (slash) {
        *slash = '\0';
        *plen = atoi(slash + 1);
        if (*plen < 0 || *plen > 128)
            return 0;
    }
    if (strcmp(buf, "default") == 0) {
        memset(ip, 0, sizeof(*ip));
        *plen = 0;
        return 1;
    }
    return nl_parse_ip6(buf, ip);
}

const char *nl_ip6_str(const nl_ip6 *ip)
{
    static char bufs[8][48];
    static int k;
    char *b = bufs[k = (k + 1) % 8];
    int best = -1, bestlen = 1, i, len = 0;

    for (i = 0; i < 8; ) {              /* the longest run of zero groups (two or more) becomes :: */
        int j = i;
        while (j < 8 && nl_get16(ip->b + 2 * j) == 0)
            j++;
        if (j - i > bestlen) {
            best = i;
            bestlen = j - i;
        }
        i = j > i ? j : i + 1;
    }
    for (i = 0; i < 8; i++) {
        if (i == best) {
            len += snprintf(b + len, 48 - (size_t)len, "::");
            i += bestlen - 1;
            continue;
        }
        len += snprintf(b + len, 48 - (size_t)len, "%s%x", i && i != best + bestlen ? ":" : "", nl_get16(ip->b + 2 * i));
    }
    return b;
}

static void mask6(nl_ip6 *a, int plen)
{
    int i;

    for (i = 0; i < 16; i++) {
        int bits = plen - 8 * i;
        if (bits <= 0)
            a->b[i] = 0;
        else if (bits < 8)
            a->b[i] &= (uint8_t)(0xff << (8 - bits));
    }
}

static void linklocal_of(const uint8_t mac[6], nl_ip6 *out)
{
    memset(out, 0, sizeof(*out));
    out->b[0] = 0xfe;
    out->b[1] = 0x80;
    out->b[8] = mac[0] ^ 2;             /* modified EUI-64 */
    out->b[9] = mac[1];
    out->b[10] = mac[2];
    out->b[11] = 0xff;
    out->b[12] = 0xfe;
    out->b[13] = mac[3];
    out->b[14] = mac[4];
    out->b[15] = mac[5];
}

static void solicited_node(const nl_ip6 *a, nl_ip6 *out)
{
    static const uint8_t pfx[13] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff };

    memcpy(out->b, pfx, 13);
    memcpy(out->b + 13, a->b + 13, 3);
}

static void mcast_mac(const nl_ip6 *a, uint8_t mac[6])
{
    mac[0] = mac[1] = 0x33;
    memcpy(mac + 2, a->b + 12, 4);
}

/* Index of the address on the interface, or -1 */
static int addr_index(const nl_node *n, int ifx, const nl_ip6 *a)
{
    int k;

    for (k = 0; k < n->ifc[ifx].nip6; k++)
        if (nl_ip6_eq(&n->ifc[ifx].ip6[k].a, a))
            return k;
    return -1;
}

int nl_ip6_is_local(const nl_node *n, const nl_ip6 *ip)
{
    int i, k;

    for (i = 0; i < n->nif; i++)
        if ((k = addr_index(n, i, ip)) >= 0 && n->ifc[i].ip6[k].state == NL_A6_OK)
            return 1;
    return 0;
}

nl_node *nl_node_by_ip6(const nl_ip6 *ip)
{
    int i, k;

    for (i = 0; i < W.nnodes; i++)
        for (k = 0; k < W.nodes[i].nif; k++)
            if (addr_index(&W.nodes[i], k, ip) >= 0)
                return &W.nodes[i];
    return NULL;
}

int nl_resolve6(const char *s, nl_ip6 *out)
{
    char buf[40];
    char *colon;
    nl_node *n;
    int i, k, ifx = -1;

    if (!s || !*s)
        return 0;
    if (nl_parse_ip6(s, out))
        return 1;
    snprintf(buf, sizeof(buf), "%s", s);
    colon = strchr(buf, ':');
    if (colon)
        *colon = '\0';
    n = nl_node_find(buf);
    if (!n || (colon && (ifx = nl_if_find(n, colon + 1)) < 0))
        return 0;
    for (k = 1; k >= 0; k--)            /* a global address first, then a link-local one */
        for (i = 0; i < n->nif; i++) {
            int a;
            if (ifx >= 0 && i != ifx)
                continue;
            for (a = 0; a < n->ifc[i].nip6; a++)
                if (nl_ip6_linklocal(&n->ifc[i].ip6[a].a) != k && n->ifc[i].ip6[a].state != NL_A6_DUPLICATE) {
                    *out = n->ifc[i].ip6[a].a;
                    return 1;
                }
        }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Routing table                                                      */
/* ------------------------------------------------------------------ */

void nl_route6_set(nl_node *n, const nl_ip6 *dst0, int plen, const nl_ip6 *gw, int ifx, int metric, int proto)
{
    nl_ip6 dst = *dst0;
    nl_route6 *r = NULL;
    int i;

    mask6(&dst, plen);
    for (i = 0; i < n->nrt6; i++)
        if (nl_ip6_eq(&n->rt6[i].dst, &dst) && n->rt6[i].plen == plen && n->rt6[i].proto == proto)
            r = &n->rt6[i];
    if (r && nl_ip6_eq(&r->gw, gw ? gw : &ZERO6) && r->ifx == ifx && r->metric == metric)
        return;
    if (!r) {
        if (n->nrt6 >= NL_MAX_ROUTES6)
            return;
        r = &n->rt6[n->nrt6++];
        r->expire = 0;
    }
    r->dst = dst;
    r->plen = plen;
    r->gw = gw ? *gw : ZERO6;
    r->ifx = ifx;
    r->metric = metric;
    r->proto = proto;
    r->since = W.now;
    if (proto != NL_RT_CONNECTED)
        nl_log(n, NL_L_NET, proto == NL_RT_STATIC ? 3 : 2, 0,
               T_INFO "route" T_RESET " %s/%d via %s dev %s metric %d " T_DIM "[%s]", nl_ip6_str(&dst), plen,
               nl_ip6_zero(&r->gw) ? "direct" : nl_ip6_str(&r->gw),
               ifx >= 0 && ifx < n->nif ? n->ifc[ifx].name : "?", metric, nl_route_proto_name(proto));
}

void nl_route6_del(nl_node *n, const nl_ip6 *dst0, int plen, int proto)
{
    nl_ip6 dst = *dst0;
    int i;

    mask6(&dst, plen);
    for (i = 0; i < n->nrt6; i++)
        if (nl_ip6_eq(&n->rt6[i].dst, &dst) && n->rt6[i].plen == plen && n->rt6[i].proto == proto) {
            n->rt6[i] = n->rt6[--n->nrt6];
            return;
        }
}

void nl_route6_flush(nl_node *n, int proto)
{
    int i;

    for (i = 0; i < n->nrt6; )
        if (n->rt6[i].proto == proto)
            n->rt6[i] = n->rt6[--n->nrt6];
        else
            i++;
}

const nl_route6 *nl_route6_lookup(const nl_node *n, const nl_ip6 *dst)
{
    const nl_route6 *best = NULL;
    int i;

    for (i = 0; i < n->nrt6; i++) {
        const nl_route6 *r = &n->rt6[i];
        if (!nl_ip6_match(dst, &r->dst, r->plen) || !nl_if_usable(n, r->ifx) || (r->expire && r->expire <= W.now))
            continue;
        if (!best || r->plen > best->plen ||
            (r->plen == best->plen && (r->proto < best->proto ||
                                       (r->proto == best->proto && r->metric < best->metric))))
            best = r;
    }
    return best;
}

/* Outgoing interface and next hop for dst; hint_if >= 0 forces the interface */
static int route6(const nl_node *n, const nl_ip6 *dst, int hint_if, int *ifx, nl_ip6 *nh)
{
    const nl_route6 *r;
    int i;

    *nh = *dst;
    if (hint_if >= 0 && hint_if < n->nif) {
        *ifx = hint_if;
        return nl_if_usable(n, hint_if);
    }
    if (nl_ip6_linklocal(dst) || nl_ip6_multicast(dst)) {
        for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])); i++)
            if (n->nbr[i].state && nl_ip6_eq(&n->nbr[i].ip, dst)) {
                *ifx = n->nbr[i].ifx;
                return 1;
            }
        for (i = 0; i < n->nif; i++)
            if (n->ifc[i].nip6 && nl_if_usable(n, i)) {
                *ifx = i;
                return 1;
            }
        return 0;
    }
    r = nl_route6_lookup(n, dst);
    if (!r)
        return 0;
    *ifx = r->ifx;
    if (!nl_ip6_zero(&r->gw))
        *nh = r->gw;
    return 1;
}

static int pick_src(const nl_node *n, int ifx, const nl_ip6 *dst, nl_ip6 *src)
{
    int want_ll = nl_ip6_linklocal(dst) || (nl_ip6_multicast(dst) && (dst->b[1] & 0x0f) <= 2), pass, i, k;

    for (pass = 0; pass < 3; pass++)    /* right scope on this interface, right scope anywhere, anything */
        for (i = 0; i < n->nif; i++) {
            if (pass == 0 && i != ifx)
                continue;
            for (k = 0; k < n->ifc[i].nip6; k++) {
                if (n->ifc[i].ip6[k].state != NL_A6_OK)
                    continue;
                if (pass < 2 && nl_ip6_linklocal(&n->ifc[i].ip6[k].a) != want_ll)
                    continue;
                if (pass == 1 && want_ll)
                    continue;           /* a link-local address is only valid on its own link */
                if (pass == 2 && i != ifx && nl_ip6_linklocal(&n->ifc[i].ip6[k].a))
                    continue;
                *src = n->ifc[i].ip6[k].a;
                return 1;
            }
        }
    return 0;
}

int nl_ip6_src(const nl_node *n, const nl_ip6 *dst, nl_ip6 *src)
{
    nl_ip6 nh;
    int ifx;

    if (nl_ip6_is_local(n, dst)) {
        *src = *dst;
        return 1;
    }
    return route6(n, dst, -1, &ifx, &nh) && pick_src(n, ifx, dst, src);
}

int nl_ip6_mtu(const nl_node *n, const nl_ip6 *dst)
{
    const stack6 *s = S6(n);
    nl_ip6 nh;
    int ifx, mtu = NL_DEFAULT_MTU, i;

    if (route6(n, dst, -1, &ifx, &nh) && n->ifc[ifx].link)
        mtu = n->ifc[ifx].link->cfg.mtu;
    for (i = 0; i < s->npmtu; i++)
        if (nl_ip6_eq(&s->pmtu[i].dst, dst) && s->pmtu[i].mtu < mtu)
            mtu = s->pmtu[i].mtu;
    return mtu;
}

/* ------------------------------------------------------------------ */
/* Handlers and hooks                                                 */
/* ------------------------------------------------------------------ */

void nl_ip6_bind(nl_node *n, uint8_t proto, nl_ip6_fn fn, void *ctx)
{
    stack6 *s = S6(n);
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

void nl_hook6_add(nl_node *n, int point, nl_hook_fn fn, void *ctx)
{
    stack6 *s = S6(n);

    if (s->nhook < (int)(sizeof(s->hook) / sizeof(s->hook[0]))) {
        s->hook[s->nhook].point = point;
        s->hook[s->nhook].fn = fn;
        s->hook[s->nhook].ctx = ctx;
        s->nhook++;
    }
}

void nl_hook6_del(nl_node *n, int point, nl_hook_fn fn, void *ctx)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; i < s->nhook; i++)
        if (s->hook[i].point == point && s->hook[i].fn == fn && s->hook[i].ctx == ctx) {
            memmove(&s->hook[i], &s->hook[i + 1], (size_t)(s->nhook - i - 1) * sizeof(s->hook[0]));
            s->nhook--;
            return;
        }
}

static int hooks6(nl_node *n, int point, nl_pkt *p, int in_if, int out_if)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; i < s->nhook; i++)
        if (s->hook[i].point == point && s->hook[i].fn(n, s->hook[i].ctx, p, in_if, out_if) == NL_HOOK_DROP)
            return NL_HOOK_DROP;
    return NL_HOOK_OK;
}

void nl_icmp6_watch(nl_node *n, nl_icmp6_fn fn, void *ctx)
{
    stack6 *s = S6(n);

    if (s->nicw < (int)(sizeof(s->icw) / sizeof(s->icw[0]))) {
        s->icw[s->nicw].fn = fn;
        s->icw[s->nicw].ctx = ctx;
        s->nicw++;
    }
}

void nl_icmp6_unwatch(nl_node *n, nl_icmp6_fn fn, void *ctx)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; i < s->nicw; i++)
        if (s->icw[i].fn == fn && s->icw[i].ctx == ctx) {
            memmove(&s->icw[i], &s->icw[i + 1], (size_t)(s->nicw - i - 1) * sizeof(s->icw[0]));
            s->nicw--;
            return;
        }
}

uint16_t nl_l4_cksum6(const nl_ip6 *src, const nl_ip6 *dst, uint8_t proto, const void *seg, int len)
{
    uint32_t sum = (uint32_t)len + proto;
    int i;

    for (i = 0; i < 16; i += 2)
        sum += nl_get16(src->b + i) + nl_get16(dst->b + i);
    return nl_cksum(seg, len, sum);
}

/* ------------------------------------------------------------------ */
/* Output                                                             */
/* ------------------------------------------------------------------ */

static void icmp6_error(nl_node *n, int type, int code, uint32_t param, const uint8_t *orig, int olen);
static void nd_send_ns(nl_node *n, int ifx, const nl_ip6 *target, const nl_ip6 *dst, int dad);

static void frame_out(nl_node *n, int ifx, const uint8_t mac[6], nl_pkt *p)
{
    memcpy(p->data, mac, 6);
    memcpy(p->data + 6, n->ifc[ifx].mac, 6);
    nl_put16(p->data + 12, NL_ETH_IP6);
    nl_link_tx(n, ifx, p);
}

static nl_nbr *nbr_find(nl_node *n, const nl_ip6 *ip, int create)
{
    nl_nbr *freeslot = NULL, *oldest = NULL;
    int i;

    for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])); i++) {
        nl_nbr *e = &n->nbr[i];
        if (e->state && nl_ip6_eq(&e->ip, ip))
            return e;
        if (!e->state && !freeslot)
            freeslot = e;
        if (e->state >= NL_NBR_REACHABLE && (!oldest || e->confirmed < oldest->confirmed))
            oldest = e;
    }
    if (!create)
        return NULL;
    if (!freeslot)
        freeslot = oldest;
    if (freeslot) {
        memset(freeslot, 0, sizeof(*freeslot));
        freeslot->ip = *ip;
    }
    return freeslot;
}

/* Send an IPv6 frame to next hop nh out of ifx, resolving its link address */
static void nbr_xmit(nl_node *n, int ifx, const nl_ip6 *nh, nl_pkt *p)
{
    uint8_t mac[6];
    nl_nbr *e;

    if (nl_ip6_multicast(nh)) {
        mcast_mac(nh, mac);
        frame_out(n, ifx, mac, p);
        return;
    }
    e = nbr_find(n, nh, 1);
    if (!e) {
        nl_pkt_free(p);
        return;
    }
    if (e->state == NL_NBR_FREE) {
        e->state = NL_NBR_INCOMPLETE;
        e->ifx = ifx;
        e->tries = 1;
        e->next = W.now + ND_RETRANS;
        e->wait[e->nwait++] = p;
        nd_send_ns(n, ifx, nh, NULL, 0);
        nd_arm(n);
        return;
    }
    if (e->state == NL_NBR_INCOMPLETE) {
        if (e->nwait < 3)
            e->wait[e->nwait++] = p;
        else
            nl_pkt_free(p);
        return;
    }
    if (e->state == NL_NBR_REACHABLE && W.now - e->confirmed > ND_REACHABLE) {
        e->state = NL_NBR_STALE;
        nl_log(n, NL_L_LINK, 3, 0, T_WARN "NDP" T_RESET " %s: %s", nl_ip6_str(nh), _("reachable → stale"));
    }
    if (e->state == NL_NBR_STALE) {     /* use it, and check that it is still there */
        e->state = NL_NBR_PROBE;
        e->tries = 0;
        e->next = W.now + ND_PROBE_DELAY;
        nd_arm(n);
    }
    frame_out(n, e->ifx, e->mac, p);
}

static void ip6_hdr(uint8_t *h, int plen, int nxt, int hlim, int tc, const nl_ip6 *src, const nl_ip6 *dst)
{
    h[0] = (uint8_t)(0x60 | (tc >> 4));
    h[1] = (uint8_t)(tc << 4);
    h[2] = h[3] = 0;
    nl_put16(h + 4, (uint16_t)plen);
    h[6] = (uint8_t)nxt;
    h[7] = (uint8_t)hlim;
    memcpy(h + 8, src->b, 16);
    memcpy(h + 24, dst->b, 16);
}

static void ip6_out(nl_node *n, int ifx, const nl_ip6 *nh, const nl_ip6 *src, const nl_ip6 *dst, int nxt, int hlim,
                    int tc, const uint8_t *pl, int len, uint32_t id)
{
    nl_pkt *p = nl_pkt_new(id);

    ip6_hdr(p->data + NL_ETH_HLEN, len, nxt, hlim, tc, src, dst);
    memcpy(p->data + NL_ETH_HLEN + NL_IP6_HLEN, pl, (size_t)len);
    p->len = (uint16_t)(NL_ETH_HLEN + NL_IP6_HLEN + len);
    if (hooks6(n, NL_HOOK_POST, p, -1, ifx) == NL_HOOK_DROP)
        nl_pkt_free(p);
    else
        nbr_xmit(n, ifx, nh, p);
}

static void ip6_local(nl_node *n, int in_if, nl_pkt *p);

typedef struct { nl_node *n; nl_pkt *p; } loop6;

static void loop_deliver(void *ctx)
{
    loop6 *c = ctx;

    ip6_local(c->n, -1, c->p);
    nl_pkt_free(c->p);
    free(c);
}

uint32_t nl_ip6_send(nl_node *n, const nl_ip6tx *tx, const void *l4, int len)
{
    stack6 *s = S6(n);
    nl_ip6 src = tx->src, nh;
    uint32_t id = tx->pkt_id ? tx->pkt_id : nl_pkt_newid();
    int ifx, hlim = tx->hlim ? tx->hlim : 64, mtu, i;

    if (!n->up || !W.ip6 || len < 0 || len > 65000)
        return 0;
    if (!tx->use_if && nl_ip6_is_local(n, &tx->dst)) {
        loop6 *c;
        nl_pkt *p;

        if (len > NL_FRAME_MAX - NL_ETH_HLEN - NL_IP6_HLEN || !(c = malloc(sizeof(*c))))
            return 0;
        p = nl_pkt_new(id);
        ip6_hdr(p->data + NL_ETH_HLEN, len, tx->proto, hlim, tx->tclass, nl_ip6_zero(&src) ? &tx->dst : &src, &tx->dst);
        memcpy(p->data + NL_ETH_HLEN + NL_IP6_HLEN, l4, (size_t)len);
        p->len = (uint16_t)(NL_ETH_HLEN + NL_IP6_HLEN + len);
        c->n = n;
        c->p = p;
        nl_after(n, 0, loop_deliver, c);
        return id;
    }
    if (!route6(n, &tx->dst, tx->use_if ? tx->ifx : -1, &ifx, &nh)) {
        nl_log(n, NL_L_NET, 2, 0, T_ERR "IPv6" T_RESET " %s %s", _("no route to"), nl_ip6_str(&tx->dst));
        nl_count(n, "ip6.noroute", 1);
        return 0;
    }
    if (nl_ip6_zero(&src) && !pick_src(n, ifx, &tx->dst, &src)) {
        nl_log(n, NL_L_NET, 2, 0, T_ERR "IPv6" T_RESET " %s %s", _("no usable source address yet for"),
               nl_ip6_str(&tx->dst));
        nl_count(n, "ip6.nosrc", 1);
        return 0;
    }
    mtu = n->ifc[ifx].link ? n->ifc[ifx].link->cfg.mtu : NL_DEFAULT_MTU;
    for (i = 0; i < s->npmtu; i++)
        if (nl_ip6_eq(&s->pmtu[i].dst, &tx->dst) && s->pmtu[i].mtu < mtu)
            mtu = s->pmtu[i].mtu;
    if (NL_IP6_HLEN + len <= mtu) {
        ip6_out(n, ifx, &nh, &src, &tx->dst, tx->proto, hlim, tx->tclass, l4, len, id);
    } else {
        /* only the source fragments: a Fragment header in front of every piece */
        static uint8_t buf[8 + 2048];
        int chunk = (mtu - NL_IP6_HLEN - 8) & ~7, off;
        uint32_t fid = ++s->frag_id;

        nl_log(n, NL_L_NET, 2, id, T_INFO "IPv6" T_RESET " %s: %d B > %s %d → %d %s", _("source fragmentation"),
               NL_IP6_HLEN + len, _("path MTU"), mtu, (len + chunk - 1) / chunk, _("fragments"));
        nl_count(n, "ip6.fragmented", 1);
        for (off = 0; off < len; off += chunk) {
            int part = len - off < chunk ? len - off : chunk;

            buf[0] = tx->proto;
            buf[1] = 0;
            nl_put16(buf + 2, (uint16_t)(off | (off + part < len ? 1 : 0)));
            nl_put32(buf + 4, fid);
            memcpy(buf + 8, (const uint8_t *)l4 + off, (size_t)part);
            ip6_out(n, ifx, &nh, &src, &tx->dst, NL_PROTO_FRAG6, hlim, tx->tclass, buf, 8 + part, id);
        }
    }
    return id;
}

/* ------------------------------------------------------------------ */
/* ICMPv6                                                             */
/* ------------------------------------------------------------------ */

static uint32_t icmp6_send(nl_node *n, const nl_ip6tx *tx0, uint8_t *msg, int len)
{
    nl_ip6tx tx = *tx0;
    nl_ip6 nh;
    int ifx;

    tx.proto = NL_PROTO_ICMP6;
    if (nl_ip6_zero(&tx.src) && !tx.use_if) {
        if (!nl_ip6_src(n, &tx.dst, &tx.src))
            return 0;
    } else if (nl_ip6_zero(&tx.src) && tx.use_if == 1) {
        if (!route6(n, &tx.dst, tx.ifx, &ifx, &nh) || !pick_src(n, tx.ifx, &tx.dst, &tx.src))
            return 0;
    }
    nl_put16(msg + 2, 0);
    nl_put16(msg + 2, nl_l4_cksum6(&tx.src, &tx.dst, NL_PROTO_ICMP6, msg, len));
    return nl_ip6_send(n, &tx, msg, len);
}

static const char *icmp6_name(int type, int code)
{
    switch (type) {
    case NL_ICMP6_UNREACH:
        return code == 0 ? "No route to destination" : code == 3 ? "Address unreachable"
             : code == 4 ? "Port unreachable" : "Destination unreachable";
    case NL_ICMP6_TOO_BIG:       return "Packet too big";
    case NL_ICMP6_TIME_EXCEEDED: return "Time exceeded (hop limit)";
    case 4:                      return "Parameter problem";
    case NL_ICMP6_ECHO:          return "Echo request";
    case NL_ICMP6_ECHO_REPLY:    return "Echo reply";
    case 133:                    return "Router solicitation";
    case 134:                    return "Router advertisement";
    case 135:                    return "Neighbor solicitation";
    case 136:                    return "Neighbor advertisement";
    default:                     return "ICMPv6";
    }
}

/* orig: the offending IPv6 packet from its header */
static void icmp6_error(nl_node *n, int type, int code, uint32_t param, const uint8_t *orig, int olen)
{
    static uint8_t b[NL_IP6_MIN_MTU];
    nl_ip6tx tx;
    int quote = olen < NL_IP6_MIN_MTU - NL_IP6_HLEN - 8 ? olen : NL_IP6_MIN_MTU - NL_IP6_HLEN - 8;

    if (olen < NL_IP6_HLEN)
        return;
    memset(&tx, 0, sizeof(tx));
    memcpy(tx.dst.b, orig + 8, 16);
    /* never about an ICMPv6 error, nor to a multicast or unspecified source */
    if (nl_ip6_zero(&tx.dst) || nl_ip6_multicast(&tx.dst) ||
        (orig[6] == NL_PROTO_ICMP6 && olen > NL_IP6_HLEN && orig[NL_IP6_HLEN] < 128))
        return;
    if (orig[24] == 0xff && type != NL_ICMP6_TOO_BIG)
        return;
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)code;
    nl_put32(b + 4, param);
    memcpy(b + 8, orig, (size_t)quote);
    nl_log(n, NL_L_NET, 2, 0, T_TIME "ICMPv6" T_RESET " → %s: %s%s", nl_ip6_str(&tx.dst), icmp6_name(type, code),
           type == NL_ICMP6_TOO_BIG ? " (MTU)" : "");
    nl_count(n, "icmp6.errors_sent", 1);
    icmp6_send(n, &tx, b, 8 + quote);
}

uint32_t nl_icmp6_echo(nl_node *n, const nl_ip6 *dst, uint16_t id, uint16_t seq, int hlim, int size)
{
    static uint8_t b[8 + 8192];
    nl_ip6tx tx;
    int i;

    if (size < 0) size = 0;
    if (size > 8192) size = 8192;
    b[0] = NL_ICMP6_ECHO;
    b[1] = 0;
    nl_put16(b + 4, id);
    nl_put16(b + 6, seq);
    for (i = 0; i < size; i++)
        b[8 + i] = (uint8_t)('a' + i % 26);
    memset(&tx, 0, sizeof(tx));
    tx.dst = *dst;
    tx.hlim = (uint8_t)hlim;
    return icmp6_send(n, &tx, b, 8 + size);
}

/* ---- Neighbor Discovery ---- */

static void nd_send(nl_node *n, int ifx, const nl_ip6 *src, const nl_ip6 *dst, uint8_t *msg, int len)
{
    nl_put16(msg + 2, 0);
    nl_put16(msg + 2, nl_l4_cksum6(src, dst, NL_PROTO_ICMP6, msg, len));
    ip6_out(n, ifx, dst, src, dst, NL_PROTO_ICMP6, 255, 0, msg, len, nl_pkt_newid());
}

/* dst NULL = the solicited-node group of the target (address resolution);
 * dad = 1: the probe of duplicate address detection, from :: */
static void nd_send_ns(nl_node *n, int ifx, const nl_ip6 *target, const nl_ip6 *dst, int dad)
{
    uint8_t b[32];
    nl_ip6 group, src = ZERO6;
    int len = 24;

    memset(b, 0, sizeof(b));
    b[0] = 135;
    memcpy(b + 8, target->b, 16);
    solicited_node(target, &group);
    if (!dad) {
        if (!pick_src(n, ifx, dst ? dst : target, &src))
            return;
        b[24] = 1;                      /* source link-layer address */
        b[25] = 1;
        memcpy(b + 26, n->ifc[ifx].mac, 6);
        len = 32;
        if (!dst)
            nl_log(n, NL_L_LINK, 2, 0, T_WARN "NDP" T_RESET " who has %s? tell %s", nl_ip6_str(target),
                   nl_ip6_str(&src));
    } else {
        nl_log(n, NL_L_LINK, 2, 0, T_WARN "NDP" T_RESET " DAD: %s %s?", _("is anybody already using"),
               nl_ip6_str(target));
    }
    nl_count(n, "nd.ns_sent", 1);
    nd_send(n, ifx, &src, dst ? dst : &group, b, len);
}

static void nd_send_na(nl_node *n, int ifx, const nl_ip6 *target, const nl_ip6 *dst, int solicited)
{
    uint8_t b[32];

    memset(b, 0, sizeof(b));
    b[0] = 136;
    b[4] = (uint8_t)((n->forward ? 0x80 : 0) | (solicited ? 0x40 : 0) | 0x20);
    memcpy(b + 8, target->b, 16);
    b[24] = 2;                          /* target link-layer address */
    b[25] = 1;
    memcpy(b + 26, n->ifc[ifx].mac, 6);
    nd_send(n, ifx, target, dst, b, 32);
}

static void nbr_learn(nl_node *n, int ifx, const nl_ip6 *ip, const uint8_t *mac, int reachable, int router)
{
    nl_nbr *e = nbr_find(n, ip, 1);
    nl_pkt *w[3];
    int k, nw, changed;

    if (!e || nl_ip6_zero(ip))
        return;
    changed = e->state <= NL_NBR_INCOMPLETE || memcmp(e->mac, mac, 6) != 0;
    if (changed)
        nl_log(n, NL_L_LINK, 3, 0, T_WARN "NDP" T_RESET " %s: %s → %s", _("neighbor cache"), nl_ip6_str(ip),
               nl_mac_str(mac));
    memcpy(e->mac, mac, 6);
    e->ifx = ifx;
    if (router >= 0)
        e->router = router;
    if (reachable) {
        e->state = NL_NBR_REACHABLE;
        e->confirmed = W.now;
    } else if (changed || e->state == NL_NBR_FREE) {
        e->state = NL_NBR_STALE;
        e->confirmed = W.now;
    }
    nw = e->nwait;
    memcpy(w, e->wait, sizeof(w));
    e->nwait = 0;
    for (k = 0; k < nw; k++)
        frame_out(n, ifx, mac, w[k]);
}

/* Option of the given type in an ND message, or NULL */
static const uint8_t *nd_option(const uint8_t *opt, int len, int type)
{
    while (len >= 8 && opt[1] && opt[1] * 8 <= len) {
        if (opt[0] == type)
            return opt;
        len -= opt[1] * 8;
        opt += opt[1] * 8;
    }
    return NULL;
}

static void dad_failed(nl_node *n, int ifx, int k)
{
    n->ifc[ifx].ip6[k].state = NL_A6_DUPLICATE;
    nl_log(n, NL_L_NET, 1, 0, T_ERR "IPv6" T_RESET " %s %s %s: %s", _("address"),
           nl_ip6_str(&n->ifc[ifx].ip6[k].a), _("is already in use on this link (DAD)"), _("not configured"));
    nl_count(n, "nd.dad_failed", 1);
}

static void slaac(nl_node *n, int ifx, const nl_ip6 *router, const uint8_t *b, int len)
{
    const uint8_t *opt = b + 16;
    int olen = len - 16, lifetime = nl_get16(b + 6);
    stack6 *s = S6(n);
    nl_route6 *r;
    int i;

    s->rs_left[ifx] = 0;
    if (lifetime) {
        nl_route6_set(n, &ZERO6, 0, router, ifx, 1, NL_RT_RA);
        for (i = 0; i < n->nrt6; i++) {
            r = &n->rt6[i];
            if (r->proto == NL_RT_RA && r->plen == 0 && r->ifx == ifx)
                r->expire = W.now + NL_SEC(lifetime);
        }
    }
    while (olen >= 8 && opt[1] && opt[1] * 8 <= olen) {
        if (opt[0] == 3 && opt[1] == 4 && opt[2] == 64 && (opt[3] & 0xc0) == 0x80) {
            /* on-link but not autonomous: neighbours are reached directly, the
             * address comes from elsewhere (DHCPv6) */
            nl_ip6 pfx;

            memcpy(pfx.b, opt + 16, 16);
            nl_route6_set(n, &pfx, 64, NULL, ifx, 0, NL_RT_RA);
        }
        if (opt[0] == 3 && opt[1] == 4 && opt[2] == 64 && (opt[3] & 0x40) && n->ifc[ifx].nip6) {
            nl_ip6 a;

            memcpy(a.b, opt + 16, 8);
            memcpy(a.b + 8, n->ifc[ifx].ip6[0].a.b + 8, 8);     /* prefix + our interface identifier */
            if (addr_index(n, ifx, &a) < 0 && nl_if_add_addr6(n, ifx, &a, 64) == 0) {
                n->ifc[ifx].ip6[n->ifc[ifx].nip6 - 1].autoconf = 1;
                nl_log(n, NL_L_NET, 1, 0, T_OK "SLAAC" T_RESET " %s: %s %s/64 (%s %s)", n->ifc[ifx].name,
                       _("autoconfigured address"), nl_ip6_str(&a), _("prefix advertised by"),
                       nl_ip6_str(router));
                nl_count(n, "slaac.addresses", 1);
            }
        }
        olen -= opt[1] * 8;
        opt += opt[1] * 8;
    }
    for (i = 0; i < s->nraw; i++)
        s->raw[i].fn(n, s->raw[i].ctx, ifx, router, (b[5] & 0x80) != 0, (b[5] & 0x40) != 0);
}

static void nd_input(nl_node *n, const nl_ip6meta *m)
{
    const uint8_t *b = m->l4, *opt;
    int type = b[0], ifx = m->in_if, k;
    nl_ip6 target;

    if (m->hlim != 255 || ifx < 0)
        return;                         /* must come from this very link */
    if (type == 133) {
        if (m->l4len >= 8 && (opt = nd_option(b + 8, m->l4len - 8, 1)) != NULL && !nl_ip6_zero(&m->src))
            nbr_learn(n, ifx, &m->src, opt + 2, 0, 0);
        if (n->forward)
            ra_solicited(n, ifx);
        return;
    }
    if (type == 134) {
        if (m->l4len < 16 || !nl_ip6_linklocal(&m->src))
            return;
        if ((opt = nd_option(b + 16, m->l4len - 16, 1)) != NULL)
            nbr_learn(n, ifx, &m->src, opt + 2, 0, 1);
        if (!n->forward)
            slaac(n, ifx, &m->src, b, m->l4len);
        return;
    }
    if (m->l4len < 24)
        return;
    memcpy(target.b, b + 8, 16);
    k = addr_index(n, ifx, &target);
    if (type == 135) {
        if (k < 0)
            return;
        if (nl_ip6_zero(&m->src)) {     /* somebody else is checking this address */
            if (n->ifc[ifx].ip6[k].state == NL_A6_TENTATIVE)
                dad_failed(n, ifx, k);
            else if (n->ifc[ifx].ip6[k].state == NL_A6_OK)
                nd_send_na(n, ifx, &target, &ALL_NODES, 0);
            return;
        }
        if (n->ifc[ifx].ip6[k].state != NL_A6_OK)
            return;
        if ((opt = nd_option(b + 24, m->l4len - 24, 1)) != NULL)
            nbr_learn(n, ifx, &m->src, opt + 2, 0, -1);
        nl_log(n, NL_L_LINK, 2, m->pkt_id, T_WARN "NDP" T_RESET " %s is at %s", nl_ip6_str(&target),
               nl_mac_str(n->ifc[ifx].mac));
        nd_send_na(n, ifx, &target, &m->src, 1);
    } else if (type == 136) {
        nl_nbr *e;

        if (k >= 0) {
            if (n->ifc[ifx].ip6[k].state == NL_A6_TENTATIVE)
                dad_failed(n, ifx, k);
            return;
        }
        e = nbr_find(n, &target, 0);
        opt = nd_option(b + 24, m->l4len - 24, 2);
        if (!e)
            return;
        if (opt)
            nbr_learn(n, ifx, &target, opt + 2, b[4] & 0x40, (b[4] & 0x80) != 0);
        else if (e->state >= NL_NBR_REACHABLE && (b[4] & 0x40)) {
            e->state = NL_NBR_REACHABLE;
            e->confirmed = W.now;
        }
    }
}

static void icmp6_input(nl_node *n, const nl_ip6meta *m)
{
    stack6 *s = S6(n);
    const uint8_t *b = m->l4;
    nl_icmp6meta im;
    int i;

    if (m->l4len < 8)
        return;
    if (nl_l4_cksum6(&m->src, &m->dst, NL_PROTO_ICMP6, b, m->l4len) != 0) {
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_ERR "ICMPv6" T_RESET " %s", _("wrong checksum: discarded"));
        nl_count(n, "icmp6.bad_checksum", 1);
        return;
    }
    if (b[0] >= 133 && b[0] <= 136) {
        nd_input(n, m);
        return;
    }
    memset(&im, 0, sizeof(im));
    im.from = m->src;
    im.type = b[0];
    im.code = b[1];
    im.hlim = m->hlim;
    im.pkt_id = m->pkt_id;
    im.data = b + 8;
    im.len = m->l4len - 8;
    if (b[0] >= 128) {
        im.id = nl_get16(b + 4);
        im.seq = nl_get16(b + 6);
    } else if (im.len >= NL_IP6_HLEN) {
        const uint8_t *o = im.data, *u = o + NL_IP6_HLEN;
        int left = im.len - NL_IP6_HLEN, proto = o[6];

        memcpy(im.orig_src.b, o + 8, 16);
        memcpy(im.orig_dst.b, o + 24, 16);
        im.mtu = (int)nl_get32(b + 4);
        if (proto == NL_PROTO_FRAG6 && left >= 8) {
            proto = u[0];
            u += 8;
            left -= 8;
        }
        im.orig_proto = (uint8_t)proto;
        if (left >= 8) {
            if (proto == NL_PROTO_ICMP6) {
                im.orig_id = nl_get16(u + 4);
                im.orig_seq = nl_get16(u + 6);
            } else {
                im.orig_sport = nl_get16(u);
                im.orig_dport = nl_get16(u + 2);
            }
        }
        if (b[0] == NL_ICMP6_TOO_BIG && nl_ip6_is_local(n, &im.orig_src) && im.mtu >= NL_IP6_MIN_MTU) {
            /* path MTU discovery: remember it and fragment at the source from now on */
            for (i = 0; i < s->npmtu; i++)
                if (nl_ip6_eq(&s->pmtu[i].dst, &im.orig_dst))
                    break;
            if (i == s->npmtu && s->npmtu < (int)(sizeof(s->pmtu) / sizeof(s->pmtu[0])))
                s->npmtu++;
            if (i < s->npmtu) {
                s->pmtu[i].dst = im.orig_dst;
                s->pmtu[i].mtu = im.mtu;
            }
            nl_log(n, NL_L_NET, 2, m->pkt_id, T_INFO "IPv6" T_RESET " %s %s: %d (%s %s)", _("path MTU towards"),
                   nl_ip6_str(&im.orig_dst), im.mtu, _("reported by"), nl_ip6_str(&m->src));
            nl_count(n, "ip6.pmtu_updates", 1);
        }
    }
    if (b[0] == NL_ICMP6_ECHO) {
        static uint8_t r[8 + 8192];
        nl_ip6tx tx;

        if (m->l4len > (int)sizeof(r))
            return;
        memcpy(r, b, (size_t)m->l4len);
        r[0] = NL_ICMP6_ECHO_REPLY;
        memset(&tx, 0, sizeof(tx));
        if (!nl_ip6_multicast(&m->dst))
            tx.src = m->dst;
        tx.dst = m->src;
        if (nl_ip6_linklocal(&m->src) && m->in_if >= 0) {
            tx.ifx = m->in_if;
            tx.use_if = 1;
            if (nl_ip6_zero(&tx.src))
                pick_src(n, m->in_if, &tx.dst, &tx.src);
        }
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_TIME "ICMPv6" T_RESET " echo request %s %s seq=%u → reply", _("from"),
               nl_ip6_str(&m->src), im.seq);
        nl_count(n, "icmp6.echo_served", 1);
        icmp6_send(n, &tx, r, m->l4len);
    }
    for (i = 0; i < s->nicw; i++)
        s->icw[i].fn(n, s->icw[i].ctx, &im);
}

/* ------------------------------------------------------------------ */
/* UDP over IPv6                                                      */
/* ------------------------------------------------------------------ */

int nl_udp6_bind(nl_node *n, uint16_t port, nl_udp6_fn fn, void *ctx)
{
    stack6 *s = S6(n);
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

void nl_udp6_unbind(nl_node *n, uint16_t port)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; i < s->nudp; i++)
        if (s->udp[i].port == port) {
            s->udp[i] = s->udp[--s->nudp];
            return;
        }
}

uint32_t nl_udp6_send_ex(nl_node *n, const nl_ip6tx *tx0, uint16_t sport, uint16_t dport, const void *data, int len)
{
    static uint8_t b[NL_UDP_HLEN + 16384];
    nl_ip6tx tx = *tx0;
    nl_ip6 nh;
    int ifx;
    uint16_t c;

    if (len < 0 || len > 16384)
        return 0;
    tx.proto = NL_PROTO_UDP;
    if (nl_ip6_zero(&tx.src)) {         /* the checksum needs the source address now */
        if (tx.use_if ? !(route6(n, &tx.dst, tx.ifx, &ifx, &nh) && pick_src(n, ifx, &tx.dst, &tx.src))
                      : !nl_ip6_src(n, &tx.dst, &tx.src)) {
            nl_count(n, "ip6.nosrc", 1);
            return 0;
        }
    }
    nl_put16(b, sport);
    nl_put16(b + 2, dport);
    nl_put16(b + 4, (uint16_t)(NL_UDP_HLEN + len));
    nl_put16(b + 6, 0);
    memcpy(b + NL_UDP_HLEN, data, (size_t)len);
    c = nl_l4_cksum6(&tx.src, &tx.dst, NL_PROTO_UDP, b, NL_UDP_HLEN + len);
    nl_put16(b + 6, c ? c : 0xffff);
    return nl_ip6_send(n, &tx, b, NL_UDP_HLEN + len);
}

uint32_t nl_udp6_send(nl_node *n, uint16_t sport, const nl_ip6 *dst, uint16_t dport, const void *data, int len)
{
    nl_ip6tx tx;

    memset(&tx, 0, sizeof(tx));
    tx.dst = *dst;
    return nl_udp6_send_ex(n, &tx, sport, dport, data, len);
}

static void udp6_input(nl_node *n, const nl_ip6meta *m, const uint8_t *pkt6, int pkt6len)
{
    stack6 *s = S6(n);
    const uint8_t *b = m->l4;
    nl_udp6msg um;
    int i, ulen;

    if (m->l4len < NL_UDP_HLEN)
        return;
    ulen = nl_get16(b + 4);
    if (ulen < NL_UDP_HLEN || ulen > m->l4len)
        ulen = m->l4len;
    if (nl_get16(b + 6) == 0 || nl_l4_cksum6(&m->src, &m->dst, NL_PROTO_UDP, b, ulen) != 0) {
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_ERR "UDP" T_RESET " %s", _("wrong checksum: datagram discarded"));
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
    um.hlim = m->hlim;
    um.pkt_id = m->pkt_id;
    for (i = 0; i < s->nudp; i++)
        if (s->udp[i].port == um.dport) {
            s->udp[i].fn(n, s->udp[i].ctx, &um);
            return;
        }
    nl_count(n, "udp.no_port", 1);
    if (!nl_ip6_multicast(&m->dst) && pkt6)
        icmp6_error(n, NL_ICMP6_UNREACH, 4, 0, pkt6, pkt6len);
}

/* ------------------------------------------------------------------ */
/* Input, reassembly, forwarding                                      */
/* ------------------------------------------------------------------ */

static int reasm6_add(nl_node *n, const nl_ip6 *src, const nl_ip6 *dst, const uint8_t *fh, const uint8_t *data,
                      int len, const uint8_t **out, int *outlen, int *proto)
{
    stack6 *s = S6(n);
    uint32_t id = nl_get32(fh + 4);
    int off = nl_get16(fh + 2) & 0xfff8, more = fh[3] & 1, i, blocks;
    reasm6 *r = NULL, **slot = NULL;

    for (i = 0; i < 4; i++) {
        reasm6 *c = s->fr[i];
        if (c && c->used && W.now - c->t > NL_SEC(60))
            c->used = 0;
        if (c && c->used && c->id == id && nl_ip6_eq(&c->src, src) && nl_ip6_eq(&c->dst, dst))
            r = c;
        if (!slot && (!c || !c->used))
            slot = &s->fr[i];
    }
    if (!r) {
        if (!slot)
            slot = &s->fr[nl_rand(4)];
        if (!*slot)
            *slot = malloc(sizeof(reasm6));
        r = *slot;
        if (!r)
            return 0;
        memset(r, 0, offsetof(reasm6, buf));
        memset(r->have, 0, sizeof(r->have));
        r->used = 1;
        r->src = *src;
        r->dst = *dst;
        r->id = id;
        r->t = W.now;
        r->total = -1;
    }
    if (off + len > FRAG6_MAX)
        return 0;
    if (off == 0)
        r->proto = fh[0];
    memcpy(r->buf + off, data, (size_t)len);
    for (i = off / 8; i < (off + len + 7) / 8; i++)
        r->have[i / 8] |= (uint8_t)(1 << (i % 8));
    if (!more)
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
    *proto = r->proto;
    return 1;
}

static void ip6_local(nl_node *n, int in_if, nl_pkt *p)
{
    stack6 *s = S6(n);
    const uint8_t *h = p->data + NL_ETH_HLEN, *u = h + NL_IP6_HLEN;
    int plen = nl_get16(h + 4), left = plen, proto = h[6], i;
    nl_ip6meta m;

    memset(&m, 0, sizeof(m));
    memcpy(m.src.b, h + 8, 16);
    memcpy(m.dst.b, h + 24, 16);
    m.hlim = h[7];
    m.tclass = (uint8_t)((h[0] << 4) | (h[1] >> 4));
    m.in_if = in_if;
    m.pkt_id = p->id;
    m.pkt = p;
    /* walk the extension headers: only Fragment is acted upon */
    while (proto == 0 || proto == 43 || proto == 60) {
        int el;
        if (left < 8 || (el = (u[1] + 1) * 8) > left)
            return;
        proto = u[0];
        u += el;
        left -= el;
    }
    if (proto == NL_PROTO_FRAG6) {
        if (left < 8 || !reasm6_add(n, &m.src, &m.dst, u, u + 8, left - 8, &u, &left, &proto))
            return;
        nl_log(n, NL_L_NET, 2, p->id, T_INFO "IPv6" T_RESET " %s (%d B)", _("datagram reassembled"), left);
        nl_count(n, "ip6.reassembled", 1);
    }
    m.proto = (uint8_t)proto;
    m.l4 = u;
    m.l4len = left;
    if (proto == NL_PROTO_ICMP6) {
        icmp6_input(n, &m);
        return;
    }
    if (proto == NL_PROTO_UDP) {
        udp6_input(n, &m, h, NL_IP6_HLEN + plen);
        return;
    }
    for (i = 0; i < s->nipp; i++)
        if (s->ipp[i].proto == proto) {
            s->ipp[i].fn(n, s->ipp[i].ctx, &m);
            return;
        }
    if (!nl_ip6_multicast(&m.dst))
        icmp6_error(n, 4, 1, 6, h, NL_IP6_HLEN + plen);          /* unrecognised next header */
}

/* Is the packet for this node? (tentative addresses count for their
 * solicited-node group only, so that DAD probes are heard) */
static int for_me(const nl_node *n, int ifx, const nl_ip6 *dst)
{
    int i, k;

    if (nl_ip6_is_local(n, dst) || nl_ip6_eq(dst, &ALL_NODES) || (n->forward && nl_ip6_eq(dst, &ALL_ROUTERS)))
        return 1;
    if (!nl_ip6_multicast(dst))
        return 0;
    for (i = 0; i < n->nif; i++)
        for (k = 0; k < n->ifc[i].nip6; k++) {
            nl_ip6 g;
            solicited_node(&n->ifc[i].ip6[k].a, &g);
            if (nl_ip6_eq(dst, &g))
                return 1;
        }
    if (S6(n))
        for (i = 0; i < S6(n)->ngroup; i++)
            if (nl_ip6_eq(dst, &S6(n)->group[i]))
                return 1;
    (void)ifx;
    return 0;
}

void nl_ip6_join(nl_node *n, const nl_ip6 *group)
{
    stack6 *s = S6(n);
    int i;

    if (!s)
        return;
    for (i = 0; i < s->ngroup; i++)
        if (nl_ip6_eq(&s->group[i], group))
            return;
    if (s->ngroup < (int)(sizeof(s->group) / sizeof(s->group[0])))
        s->group[s->ngroup++] = *group;
}

void nl_ip6_leave(nl_node *n, const nl_ip6 *group)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; s && i < s->ngroup; i++)
        if (nl_ip6_eq(&s->group[i], group)) {
            s->group[i] = s->group[--s->ngroup];
            return;
        }
}

void nl_ra_watch(nl_node *n, nl_ra_fn fn, void *ctx)
{
    stack6 *s = S6(n);
    int i;

    if (!s)
        return;
    for (i = 0; i < s->nraw; i++)
        if (s->raw[i].fn == fn && s->raw[i].ctx == ctx)
            return;
    if (s->nraw < (int)(sizeof(s->raw) / sizeof(s->raw[0]))) {
        s->raw[s->nraw].fn = fn;
        s->raw[s->nraw].ctx = ctx;
        s->nraw++;
    }
}

void nl_ra_unwatch(nl_node *n, nl_ra_fn fn, void *ctx)
{
    stack6 *s = S6(n);
    int i;

    for (i = 0; s && i < s->nraw; i++)
        if (s->raw[i].fn == fn && s->raw[i].ctx == ctx) {
            s->raw[i] = s->raw[--s->nraw];
            return;
        }
}

void nl_ip6_input(nl_node *n, int in_if, nl_pkt *p)
{
    uint8_t *h = p->data + NL_ETH_HLEN;
    const nl_route6 *r;
    nl_ip6 src, dst, nh;
    int plen;

    if (!n->stack6 || p->len < NL_ETH_HLEN + NL_IP6_HLEN || (h[0] >> 4) != 6)
        goto drop;
    plen = nl_get16(h + 4);
    if (NL_ETH_HLEN + NL_IP6_HLEN + plen > p->len)
        goto drop;
    p->len = (uint16_t)(NL_ETH_HLEN + NL_IP6_HLEN + plen);
    if (hooks6(n, NL_HOOK_PRE, p, in_if, -1) == NL_HOOK_DROP)
        goto drop;
    memcpy(src.b, h + 8, 16);
    memcpy(dst.b, h + 24, 16);
    if (for_me(n, in_if, &dst)) {
        ip6_local(n, in_if, p);
        goto drop;
    }
    if (!n->forward || nl_ip6_multicast(&dst) || nl_ip6_linklocal(&dst) || nl_ip6_linklocal(&src) ||
        nl_ip6_zero(&src)) {
        nl_count(n, "ip6.not_for_me", 1);
        goto drop;
    }
    if (h[7] <= 1) {
        nl_log(n, NL_L_NET, 2, p->id, T_WARN "IPv6" T_RESET " hop limit %s (%s → %s)", _("expired"),
               nl_ip6_str(&src), nl_ip6_str(&dst));
        nl_count(n, "ip6.hlim_expired", 1);
        icmp6_error(n, NL_ICMP6_TIME_EXCEEDED, 0, 0, h, NL_IP6_HLEN + plen);
        goto drop;
    }
    r = nl_route6_lookup(n, &dst);
    if (!r) {
        nl_log(n, NL_L_NET, 2, p->id, T_ERR "IPv6" T_RESET " %s %s", _("no route to"), nl_ip6_str(&dst));
        nl_count(n, "ip6.noroute", 1);
        icmp6_error(n, NL_ICMP6_UNREACH, 0, 0, h, NL_IP6_HLEN + plen);
        goto drop;
    }
    if (n->ifc[r->ifx].link && NL_IP6_HLEN + plen > n->ifc[r->ifx].link->cfg.mtu) {
        int mtu = n->ifc[r->ifx].link->cfg.mtu;
        /* a router never fragments IPv6: it tells the source */
        nl_log(n, NL_L_NET, 2, p->id, T_WARN "IPv6" T_RESET " %d B > MTU %d: %s", NL_IP6_HLEN + plen, mtu,
               _("routers do not fragment IPv6: dropped, Packet Too Big sent"));
        nl_count(n, "ip6.too_big", 1);
        icmp6_error(n, NL_ICMP6_TOO_BIG, 0, (uint32_t)mtu, h, NL_IP6_HLEN + plen);
        goto drop;
    }
    if (hooks6(n, NL_HOOK_FWD, p, in_if, r->ifx) == NL_HOOK_DROP)
        goto drop;
    h[7]--;
    nh = nl_ip6_zero(&r->gw) ? dst : r->gw;
    nl_log(n, NL_L_NET, 3, p->id, T_INFO "IPv6" T_RESET " forward %s → %s via %s dev %s hlim %d", nl_ip6_str(&src),
           nl_ip6_str(&dst), nl_ip6_zero(&r->gw) ? "direct" : nl_ip6_str(&r->gw), n->ifc[r->ifx].name, h[7]);
    nl_count(n, "ip6.forwarded", 1);
    if (hooks6(n, NL_HOOK_POST, p, in_if, r->ifx) == NL_HOOK_DROP)
        goto drop;
    nbr_xmit(n, r->ifx, &nh, p);
    return;
drop:
    nl_pkt_free(p);
}

/* ------------------------------------------------------------------ */
/* Address configuration, DAD, the periodic work                      */
/* ------------------------------------------------------------------ */

static void dad_begin(nl_node *n, int ifx, int k)
{
    stack6 *s = S6(n);

    n->ifc[ifx].ip6[k].state = NL_A6_TENTATIVE;
    s->dad_until[ifx][k] = W.now + DAD_TIME;
    nd_send_ns(n, ifx, &n->ifc[ifx].ip6[k].a, NULL, 1);
    nd_arm(n);
}

int nl_if_add_addr6(nl_node *n, int ifx, const nl_ip6 *ip, int plen)
{
    nl_iface *f = &n->ifc[ifx];
    stack6 *s = S6(n);
    int k = addr_index(n, ifx, ip);

    if (k < 0) {
        if (f->nip6 >= NL_MAX_IP6)
            return -1;
        k = f->nip6++;
    }
    f->ip6[k].a = *ip;
    f->ip6[k].plen = (uint8_t)plen;
    f->ip6[k].autoconf = 0;
    f->ip6[k].state = NL_A6_OK;
    if (!nl_ip6_linklocal(ip) && plen < 128)
        nl_route6_set(n, ip, plen, NULL, ifx, 0, NL_RT_CONNECTED);
    if (s && s->started && n->local && n->up)
        dad_begin(n, ifx, k);
    return 0;
}

void nl_if_del_addr6(nl_node *n, int ifx, const nl_ip6 *ip)
{
    nl_iface *f = &n->ifc[ifx];
    stack6 *s = S6(n);
    int k = addr_index(n, ifx, ip);

    if (k <= 0)                         /* not there, or the link-local address */
        return;
    if (f->ip6[k].plen < 128)
        nl_route6_del(n, &f->ip6[k].a, f->ip6[k].plen, NL_RT_CONNECTED);
    memmove(&f->ip6[k], &f->ip6[k + 1], (size_t)(f->nip6 - k - 1) * sizeof(f->ip6[0]));
    if (s)
        memmove(&s->dad_until[ifx][k], &s->dad_until[ifx][k + 1],
                (size_t)(f->nip6 - k - 1) * sizeof(s->dad_until[0][0]));
    f->nip6--;
}

static void send_rs(nl_node *n, int ifx)
{
    uint8_t b[16];
    nl_ip6 src;

    if (!pick_src(n, ifx, &ALL_ROUTERS, &src))
        return;
    memset(b, 0, sizeof(b));
    b[0] = 133;
    b[8] = 1;
    b[9] = 1;
    memcpy(b + 10, n->ifc[ifx].mac, 6);
    nl_log(n, NL_L_NET, 2, 0, T_WARN "NDP" T_RESET " %s (%s)", _("router solicitation: is there a router?"),
           n->ifc[ifx].name);
    nd_send(n, ifx, &src, &ALL_ROUTERS, b, 16);
}

static void nd_tick(void *ctx)
{
    nl_node *n = ctx;
    stack6 *s = S6(n);
    int i, k;

    s->tick = 0;
    for (i = 0; i < n->nif; i++) {
        for (k = 0; k < n->ifc[i].nip6; k++)
            if (n->ifc[i].ip6[k].state == NL_A6_TENTATIVE && s->dad_until[i][k] && s->dad_until[i][k] <= W.now) {
                n->ifc[i].ip6[k].state = NL_A6_OK;
                s->dad_until[i][k] = 0;
                nl_log(n, NL_L_NET, 2, 0, T_INFO "IPv6" T_RESET " %s %s/%d: %s", n->ifc[i].name,
                       nl_ip6_str(&n->ifc[i].ip6[k].a), n->ifc[i].ip6[k].plen,
                       _("nobody objected (DAD): address in use"));
                if (k == 0 && !n->forward) {    /* link-local ready: look for routers */
                    s->rs_left[i] = 3;
                    s->rs_next[i] = W.now;
                }
            }
        if (s->rs_left[i] > 0 && s->rs_next[i] <= W.now && nl_if_usable(n, i)) {
            s->rs_left[i]--;
            s->rs_next[i] = W.now + RS_INTERVAL;
            send_rs(n, i);
        }
    }
    for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])); i++) {
        nl_nbr *e = &n->nbr[i];

        if ((e->state != NL_NBR_INCOMPLETE && e->state != NL_NBR_PROBE) || e->next > W.now)
            continue;
        if (e->tries >= 3) {
            if (e->state == NL_NBR_INCOMPLETE) {
                nl_log(n, NL_L_LINK, 2, 0, T_ERR "NDP" T_RESET " %s %s", _("no answer for"), nl_ip6_str(&e->ip));
                nl_count(n, "nd.failed", 1);
                for (k = 0; k < e->nwait; k++) {
                    nl_pkt *w = e->wait[k];
                    if (n->forward)
                        icmp6_error(n, NL_ICMP6_UNREACH, 3, 0, w->data + NL_ETH_HLEN, w->len - NL_ETH_HLEN);
                    nl_pkt_free(w);
                }
            } else {
                nl_log(n, NL_L_LINK, 2, 0, T_WARN "NDP" T_RESET " %s %s", nl_ip6_str(&e->ip),
                       _("does not answer any more: removed from the neighbor cache"));
            }
            memset(e, 0, sizeof(*e));
            continue;
        }
        e->tries++;
        e->next = W.now + ND_RETRANS;
        nd_send_ns(n, e->ifx, &e->ip, e->state == NL_NBR_PROBE ? &e->ip : NULL, 0);
    }
    nd_arm(n);
}

/* Schedule the next piece of pending work, if any */
static void nd_arm(nl_node *n)
{
    stack6 *s = S6(n);
    nl_time next = -1;
    int i, k;

    if (!s || !n->up)
        return;
#define SOON(t) do { if ((t) > 0 && (next < 0 || (t) < next)) next = (t); } while (0)
    for (i = 0; i < n->nif; i++) {
        for (k = 0; k < n->ifc[i].nip6; k++)
            if (n->ifc[i].ip6[k].state == NL_A6_TENTATIVE)
                SOON(s->dad_until[i][k]);
        if (s->rs_left[i] > 0)
            SOON(s->rs_next[i] > W.now ? s->rs_next[i] : W.now + 1);
    }
    for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])); i++)
        if (n->nbr[i].state == NL_NBR_INCOMPLETE || n->nbr[i].state == NL_NBR_PROBE)
            SOON(n->nbr[i].next);
#undef SOON
    if (next < 0 || (s->tick && s->tick_at <= next))
        return;                         /* nothing to do, or already due early enough */
    nl_cancel(&s->tick);
    s->tick_at = next > W.now ? next : W.now + 1;
    s->tick = nl_after(n, s->tick_at - W.now, nd_tick, n);
}

static void node_start(nl_node *n)
{
    stack6 *s = S6(n);
    int i, k;

    if (n->kind == NL_SWITCH)
        return;
    for (i = 0; i < n->nif; i++) {
        nl_iface *f = &n->ifc[i];
        nl_ip6 ll;

        if (!f->link)
            continue;
        linklocal_of(f->mac, &ll);
        if (f->nip6 == 0 || !nl_ip6_eq(&f->ip6[0].a, &ll)) {    /* the link-local address comes first */
            if (f->nip6 < NL_MAX_IP6)
                f->nip6++;
            memmove(&f->ip6[1], &f->ip6[0], (size_t)(f->nip6 - 1) * sizeof(f->ip6[0]));
            f->ip6[0].a = ll;
            f->ip6[0].plen = 64;
            f->ip6[0].autoconf = 0;
            f->ip6[0].state = NL_A6_OK;
        }
        if (!n->local || !n->up)
            continue;                   /* another process does the talking */
        for (k = 0; k < f->nip6; k++)
            dad_begin(n, i, k);
    }
    s->started = 1;
}

static void start_event(void *ctx)
{
    int i;

    (void)ctx;
    for (i = 0; i < W.nnodes; i++)
        node_start(&W.nodes[i]);
}

void nl_ip6_start(void)
{
    if (W.ip6)
        nl_after(NULL, 0, start_event, NULL);
}

void nl_ip6_init(nl_node *n)
{
    stack6 *s = calloc(1, sizeof(*s));

    if (!s)
        exit(EXIT_FAILURE);
    s->eph = 49152;
    s->frag_id = (uint32_t)n->idx << 16;
    n->stack6 = s;
}

void nl_ip6_node_down(nl_node *n)
{
    stack6 *s = S6(n);
    int i, k;

    if (!s)
        return;
    for (i = 0; i < (int)(sizeof(n->nbr) / sizeof(n->nbr[0])); i++) {
        for (k = 0; k < n->nbr[i].nwait; k++)
            nl_pkt_free(n->nbr[i].wait[k]);
        memset(&n->nbr[i], 0, sizeof(n->nbr[i]));
    }
    s->tick = 0;
    s->npmtu = 0;
    /* what was learned from routers is forgotten with the power */
    nl_route6_flush(n, NL_RT_RA);
    for (i = 0; i < n->nif; i++) {
        nl_iface *f = &n->ifc[i];
        for (k = 0; k < f->nip6; )
            if (f->ip6[k].autoconf) {
                nl_route6_del(n, &f->ip6[k].a, f->ip6[k].plen, NL_RT_CONNECTED);
                memmove(&f->ip6[k], &f->ip6[k + 1], (size_t)(f->nip6 - k - 1) * sizeof(f->ip6[0]));
                f->nip6--;
            } else {
                k++;
            }
        s->rs_left[i] = 0;
    }
}

void nl_ip6_node_up(nl_node *n)
{
    stack6 *s = S6(n);
    int i, k;

    if (!W.ip6 || !s || !s->started || !n->local || n->kind == NL_SWITCH)
        return;
    for (i = 0; i < n->nif; i++)
        for (k = 0; k < n->ifc[i].nip6; k++)
            dad_begin(n, i, k);
}

/* ------------------------------------------------------------------ */
/* Router advertisements                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_node *n;
    nl_time  interval;
    int      lifetime;                  /* seconds */
    int      managed, other, autonomous;        /* the M and O flags, and the A flag of the prefixes */
    unsigned sent;
    nl_ev    timer;
} ra_state;

static const nl_module ra_mod;

static void ra_send(nl_node *n, int ifx)
{
    ra_state *st = nl_mod_get(n, &ra_mod);
    const nl_iface *f = &n->ifc[ifx];
    uint8_t b[16 + 8 + 8 + 32 * NL_MAX_IP6];
    int len = 16, k, prefixes = 0;

    if (!st || !f->link || f->nip6 == 0 || f->ip6[0].state != NL_A6_OK || !nl_if_usable(n, ifx))
        return;
    memset(b, 0, sizeof(b));
    b[0] = 134;
    b[4] = 64;                          /* hop limit suggested to the hosts */
    b[5] = (uint8_t)((st->managed ? 0x80 : 0) | (st->other ? 0x40 : 0));
    nl_put16(b + 6, (uint16_t)st->lifetime);
    b[len] = 1;                         /* source link-layer address */
    b[len + 1] = 1;
    memcpy(b + len + 2, f->mac, 6);
    len += 8;
    b[len] = 5;                         /* MTU */
    b[len + 1] = 1;
    nl_put32(b + len + 4, (uint32_t)f->link->cfg.mtu);
    len += 8;
    for (k = 1; k < f->nip6; k++) {
        nl_ip6 pfx = f->ip6[k].a;

        if (f->ip6[k].state != NL_A6_OK || f->ip6[k].plen != 64 || nl_ip6_linklocal(&pfx))
            continue;
        mask6(&pfx, 64);
        b[len] = 3;                     /* prefix information */
        b[len + 1] = 4;
        b[len + 2] = 64;
        b[len + 3] = (uint8_t)(0x80 | (st->autonomous ? 0x40 : 0));     /* on-link; autonomous configuration */
        nl_put32(b + len + 4, (uint32_t)st->lifetime * 10);
        nl_put32(b + len + 8, (uint32_t)st->lifetime * 5);
        memcpy(b + len + 16, pfx.b, 16);
        len += 32;
        prefixes++;
    }
    if (!prefixes)
        return;
    st->sent++;
    nl_count(n, "ra.sent", 1);
    nl_log(n, NL_L_NET, 2, 0, T_WARN "NDP" T_RESET " %s %s: %d %s, %s %d s%s%s%s", _("router advertisement on"),
           f->name, prefixes, _("prefix(es)"), _("router lifetime"), st->lifetime, st->managed ? ", M" : "",
           st->other ? ", O" : "", st->autonomous ? "" : ", A=0");
    nd_send(n, ifx, &f->ip6[0].a, &ALL_NODES, b, len);
}

static void ra_solicited(nl_node *n, int ifx)
{
    ra_send(n, ifx);
}

static void ra_tick(void *ctx)
{
    ra_state *st = ctx;
    int i;

    for (i = 0; i < st->n->nif; i++)
        ra_send(st->n, i);
    st->timer = nl_after(st->n, st->interval, ra_tick, st);
}

static int ra_run(nl_node *n, const nl_args *a)
{
    ra_state *st = nl_mod_get(n, &ra_mod);

    if (!W.ip6 || !n->forward) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "ra: %s", _("needs a router in a lab that uses IPv6"));
        return -1;
    }
    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->n = n;
        nl_mod_set(n, &ra_mod, st);
    }
    nl_cancel(&st->timer);
    st->interval = nl_arg_time(a, "interval", NL_SEC(10));
    st->lifetime = (int)(nl_arg_time(a, "lifetime", NL_SEC(1800)) / NL_SEC(1));
    if (st->lifetime > 9000)
        st->lifetime = 9000;
    st->managed = (int)nl_arg_int(a, "managed", 0);
    st->other = (int)nl_arg_int(a, "other", 0);
    st->autonomous = (int)nl_arg_int(a, "autonomous", 1);
    {   /* the three usual combinations by name */
        const char *mode = nl_arg_str(a, "mode", "");

        if (!strcmp(mode, "stateful")) {
            st->managed = st->other = 1;
            st->autonomous = 0;
        } else if (!strcmp(mode, "stateless")) {
            st->managed = 0;
            st->other = st->autonomous = 1;
        } else if (!strcmp(mode, "slaac")) {
            st->managed = st->other = 0;
            st->autonomous = 1;
        }
    }
    /* the first one after our own addresses have passed DAD */
    st->timer = nl_after(n, DAD_TIME + NL_MS(200), ra_tick, st);
    return 0;
}

static void ra_stop(nl_node *n)
{
    ra_state *st = nl_mod_get(n, &ra_mod);

    if (!st)
        return;
    nl_cancel(&st->timer);
    nl_mod_set(n, &ra_mod, NULL);
    free(st);
}

static void ra_recover(nl_node *n)
{
    ra_state *st = nl_mod_get(n, &ra_mod);

    if (st)
        st->timer = nl_after(n, DAD_TIME + NL_MS(200), ra_tick, st);
}

static int ra_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    ra_state *st = nl_mod_get(n, &ra_mod);
    int row = 0, i, k;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("advertisements sent"), "%u", st->sent);
    row += nl_ui_kv(x, y + row, w, _("interval / router lifetime"), "%s / %d s", nl_time_str(st->interval),
                    st->lifetime);
    for (i = 0; i < n->nif && row < h; i++)
        for (k = 1; k < n->ifc[i].nip6 && row < h; k++) {
            nl_ip6 pfx = n->ifc[i].ip6[k].a;
            if (n->ifc[i].ip6[k].plen != 64)
                continue;
            mask6(&pfx, 64);
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_BOLD "%-5s" T_RESET " %s/64  " T_DIM "%s%s%s",
                      n->ifc[i].name, nl_ip6_str(&pfx), st->autonomous ? _("on-link, autonomous") : _("on-link"),
                      st->managed ? "  M" : "", st->other ? "  O" : "");
        }
    return row;
}

static const nl_module ra_mod = {
    .name = "ra", .title = N_("Router advertisements: IPv6 hosts configure themselves (SLAAC)"),
    .layer = NL_L_NET,
    .usage = N_("run ROUTER ra [interval=10s] [lifetime=1800s] [managed=0] [other=0] [autonomous=1]\n"
                "  managed=1 sets the M flag (addresses come from DHCPv6), other=1 the O flag (other\n"
                "  settings, such as DNS servers, come from DHCPv6); autonomous=0 clears the A flag of\n"
                "  the prefixes, so hosts do not form addresses by themselves.\n"
                "  mode=slaac|stateless|stateful sets the three at once (stateless: O; stateful: M, O\n"
                "  and no autonomous addresses).\n"
                "  Advertises, on every interface, the /64 prefixes configured there, and answers\n"
                "  router solicitations. Hosts of a lab that uses IPv6 listen without any command."),
    .theory = N_("An IPv6 host needs no server to get an address. Every interface first gives itself a "
                 "link-local address (fe80::/64 plus an identifier derived from its MAC address) and checks "
                 "with duplicate address detection that nobody else on the link uses it. Then it asks "
                 "\"is there a router?\" with a Router Solicitation sent to the all-routers group. A router "
                 "answers with a Router Advertisement carrying the prefixes of the link: for each prefix "
                 "marked autonomous the host builds prefix + its own identifier, checks that address too, "
                 "and uses the router's link-local address as default gateway for as long as the "
                 "advertised router lifetime. Routers also repeat the advertisement periodically. This is "
                 "stateless address autoconfiguration (RFC 4862) on top of Neighbor Discovery (RFC 4861). "
                 "Two flags of the advertisement point to DHCPv6: M (managed) says addresses are "
                 "handed out by a DHCPv6 server, O (other) says only the remaining settings, such as "
                 "DNS servers, are. The default route always comes from the advertisement. "
                 "Simplified here: preferred and valid lifetimes are advertised but autoconfigured "
                 "addresses do not expire, and there are no privacy addresses."),
    .run = ra_run, .stop = ra_stop, .recover = ra_recover, .panel = ra_panel,
};
NL_MODULE(ra_mod)

/* ------------------------------------------------------------------ */
/* Dissector                                                          */
/* ------------------------------------------------------------------ */

static const char *nh_name(int nh)
{
    switch (nh) {
    case 0:              return "Hop-by-Hop options";
    case 6:              return "TCP";
    case 17:             return "UDP";
    case 43:             return "Routing";
    case 44:             return "Fragment";
    case 58:             return "ICMPv6";
    case 59:             return "No next header";
    case 60:             return "Destination options";
    default:             return "?";
    }
}

static void dis_nd_options(nl_dissect *o, const uint8_t *opt, int len, int off)
{
    while (len >= 8 && opt[1] && opt[1] * 8 <= len) {
        nl_ip6 a;

        if (opt[0] == 1 || opt[0] == 2)
            nl_dis_field(o, "ICMPv6", opt[0] == 1 ? "Source link addr" : "Target link addr", off, 8, "%s",
                         nl_mac_str(opt + 2));
        else if (opt[0] == 5)
            nl_dis_field(o, "ICMPv6", "MTU option", off, 8, "%u", nl_get32(opt + 4));
        else if (opt[0] == 3 && opt[1] == 4) {
            memcpy(a.b, opt + 16, 16);
            nl_dis_field(o, "ICMPv6", "Prefix", off, 32, "%s/%d%s%s valid %u s", nl_ip6_str(&a), opt[2],
                         opt[3] & 0x80 ? " on-link" : "", opt[3] & 0x40 ? " autonomous" : "", nl_get32(opt + 4));
        } else
            nl_dis_field(o, "ICMPv6", "Option", off, opt[1] * 8, "type %d", opt[0]);
        len -= opt[1] * 8;
        off += opt[1] * 8;
        opt += opt[1] * 8;
    }
}

static void dis_icmp6(nl_dissect *o, const nl_ip6 *src, const nl_ip6 *dst, const uint8_t *d, int len, int off,
                      int whole, int hlim)
{
    nl_ip6 t;

    if (len < 8) {
        nl_dis_summary(o, "ICMPv6", 213, "truncated");
        return;
    }
    nl_dis_field(o, "ICMPv6", "Type / Code", off, 2, "%d / %d (%s)", d[0], d[1], icmp6_name(d[0], d[1]));
    nl_dis_field(o, "ICMPv6", "Checksum", off + 2, 2, "0x%04x %s", nl_get16(d + 2),
                 !whole ? "" : nl_l4_cksum6(src, dst, NL_PROTO_ICMP6, d, len) == 0 ? "✓" : "✗ WRONG");
    switch (d[0]) {
    case NL_ICMP6_ECHO: case NL_ICMP6_ECHO_REPLY:
        nl_dis_field(o, "ICMPv6", "Identifier", off + 4, 2, "%u", nl_get16(d + 4));
        nl_dis_field(o, "ICMPv6", "Sequence", off + 6, 2, "%u", nl_get16(d + 6));
        nl_dis_field(o, "ICMPv6", "Data", off + 8, len - 8, "%d bytes", len - 8);
        nl_dis_summary(o, "ICMPv6", 213, "%s id=%u seq=%u hlim=%d", icmp6_name(d[0], 0), nl_get16(d + 4),
                       nl_get16(d + 6), hlim);
        break;
    case 135: case 136:
        if (len < 24)
            break;
        memcpy(t.b, d + 8, 16);
        if (d[0] == 136)
            nl_dis_field(o, "ICMPv6", "Flags", off + 4, 1, "%s%s%s", d[4] & 0x80 ? "Router " : "",
                         d[4] & 0x40 ? "Solicited " : "", d[4] & 0x20 ? "Override" : "");
        nl_dis_field(o, "ICMPv6", "Target", off + 8, 16, "%s", nl_ip6_str(&t));
        dis_nd_options(o, d + 24, len - 24, off + 24);
        if (d[0] == 135 && nl_ip6_zero(src))
            nl_dis_summary(o, "NDP", 221, "DAD: is %s already in use?", nl_ip6_str(&t));
        else if (d[0] == 135)
            nl_dis_summary(o, "NDP", 221, "Who has %s? Tell %s", nl_ip6_str(&t), nl_ip6_str(src));
        else
            nl_dis_summary(o, "NDP", 221, "%s is at %s%s", nl_ip6_str(&t),
                           len >= 32 ? nl_mac_str(d + 26) : "?", d[4] & 0x40 ? "" : " (unsolicited)");
        break;
    case 133:
        dis_nd_options(o, d + 8, len - 8, off + 8);
        nl_dis_summary(o, "NDP", 221, "Router solicitation");
        break;
    case 134:
        if (len < 16)
            break;
        nl_dis_field(o, "ICMPv6", "Hop limit / Flags", off + 4, 2, "%d / M=%d O=%d", d[4], d[5] >> 7, (d[5] >> 6) & 1);
        nl_dis_field(o, "ICMPv6", "Router lifetime", off + 6, 2, "%u s", nl_get16(d + 6));
        dis_nd_options(o, d + 16, len - 16, off + 16);
        nl_dis_summary(o, "NDP", 221, "Router advertisement, lifetime %u s", nl_get16(d + 6));
        break;
    default:
        if (d[0] == NL_ICMP6_TOO_BIG)
            nl_dis_field(o, "ICMPv6", "MTU", off + 4, 4, "%u", nl_get32(d + 4));
        if (len >= 8 + NL_IP6_HLEN) {
            nl_ip6 os, od;
            memcpy(os.b, d + 16, 16);
            memcpy(od.b, d + 32, 16);
            nl_dis_field(o, "ICMPv6", "Original packet", off + 8, len - 8, "%s → %s", nl_ip6_str(&os),
                         nl_ip6_str(&od));
        }
        if (d[0] == NL_ICMP6_TOO_BIG)
            nl_dis_summary(o, "ICMPv6", 213, "Packet too big, MTU %u", nl_get32(d + 4));
        else
            nl_dis_summary(o, "ICMPv6", 213, "%s", icmp6_name(d[0], d[1]));
        break;
    }
}

static void dis_ip6(nl_dissect *o, const uint8_t *d, int len, int off)
{
    nl_ip6 src, dst;
    int plen, nh, left, whole = 1;
    const uint8_t *u;

    if (len < NL_IP6_HLEN) {
        nl_dis_summary(o, "IPv6", 114, "truncated IPv6 packet");
        return;
    }
    plen = nl_get16(d + 4);
    nh = d[6];
    memcpy(src.b, d + 8, 16);
    memcpy(dst.b, d + 24, 16);
    nl_dis_field(o, "IPv6", "Version/Class/Flow", off, 4, "%d / 0x%02x / 0x%05x", d[0] >> 4,
                 ((d[0] << 4) | (d[1] >> 4)) & 0xff, ((d[1] & 15) << 16) | nl_get16(d + 2));
    nl_dis_field(o, "IPv6", "Payload length", off + 4, 2, "%d", plen);
    nl_dis_field(o, "IPv6", "Next header", off + 6, 1, "%d (%s)", nh, nh_name(nh));
    nl_dis_field(o, "IPv6", "Hop limit", off + 7, 1, "%d", d[7]);
    nl_dis_field(o, "IPv6", "Source", off + 8, 16, "%s", nl_ip6_str(&src));
    nl_dis_field(o, "IPv6", "Destination", off + 24, 16, "%s", nl_ip6_str(&dst));
    snprintf(o->addr, sizeof(o->addr), "%s → %s", nl_ip6_str(&src), nl_ip6_str(&dst));
    u = d + NL_IP6_HLEN;
    left = (plen < len - NL_IP6_HLEN ? plen : len - NL_IP6_HLEN);
    off += NL_IP6_HLEN;
    while ((nh == 0 || nh == 43 || nh == 60) && left >= 8 && (u[1] + 1) * 8 <= left) {
        int el = (u[1] + 1) * 8;
        nl_dis_field(o, "IPv6", "Extension header", off, el, "%s, next %d", nh_name(nh), u[0]);
        nh = u[0];
        u += el;
        left -= el;
        off += el;
    }
    if (nh == NL_PROTO_FRAG6 && left >= 8) {
        int fo = nl_get16(u + 2) & 0xfff8, more = u[3] & 1;

        nl_dis_field(o, "Fragment", "Next header", off, 1, "%d (%s)", u[0], nh_name(u[0]));
        nl_dis_field(o, "Fragment", "Offset / More", off + 2, 2, "%d / %s", fo, more ? "M (more follow)" : "last");
        nl_dis_field(o, "Fragment", "Identification", off + 4, 4, "0x%08x", nl_get32(u + 4));
        nl_dis_summary(o, "IPv6", 114, "Fragment id=0x%x offset=%d len=%d%s", nl_get32(u + 4), fo, left - 8,
                       more ? " M" : " (last)");
        if (fo != 0)
            return;
        whole = 0;
        nh = u[0];
        u += 8;
        left -= 8;
        off += 8;
        if (nh != NL_PROTO_ICMP6 && nh != NL_PROTO_UDP)
            return;
    }
    if (nh == NL_PROTO_ICMP6) {
        char keep[150];
        snprintf(keep, sizeof(keep), "%s", o->summary);
        dis_icmp6(o, &src, &dst, u, left, off, whole, d[7]);
        if (!whole) {                   /* keep saying it is a fragment */
            char both[150];
            snprintf(both, sizeof(both), "%.70s · %.70s", keep, o->summary);
            snprintf(o->summary, sizeof(o->summary), "%s", both);
        }
    } else if (nh == NL_PROTO_UDP && left >= 8) {
        uint16_t sp = nl_get16(u), dp = nl_get16(u + 2);

        nl_dis_field(o, "UDP", "Source port", off, 2, "%u", sp);
        nl_dis_field(o, "UDP", "Destination port", off + 2, 2, "%u", dp);
        nl_dis_field(o, "UDP", "Length", off + 4, 2, "%u", nl_get16(u + 4));
        nl_dis_field(o, "UDP", "Checksum", off + 6, 2, "0x%04x %s", nl_get16(u + 6),
                     !whole ? "" : nl_l4_cksum6(&src, &dst, NL_PROTO_UDP, u, left) == 0 ? "✓" : "✗ WRONG");
        if (whole) {
            nl_dis_summary(o, "UDP", 117, "%u → %u len=%d", sp, dp, left - 8);
            if (left > 8)
                nl_dis_field(o, "UDP", "Payload", off + 8, left - 8, "%d bytes", left - 8);
            nl_dissect_udp_payload(o, sp, dp, u + 8, left - 8, off + 8);
        }
    } else if (!nl_dissect_ipproto(o, (uint8_t)nh, u, left, off)) {
        nl_dis_summary(o, "IPv6", 114, "next header %d, %d bytes", nh, left);
    }
}

NL_INIT(ip6)
{
    nl_dissector_eth(NL_ETH_IP6, dis_ip6);
}
