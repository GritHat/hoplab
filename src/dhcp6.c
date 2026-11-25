/*
 * dhcp6.c - DHCPv6 (RFC 8415): server and client.
 *
 * Message: type (1 byte), transaction id (3), then options (code 16 bit,
 * length 16 bit, value). Clients speak from their link-local address on UDP
 * port 546 to the group ff02::1:2 (All_DHCP_Relay_Agents_and_Servers), port
 * 547; servers answer the client's link-local address directly.
 *
 * Stateful: SOLICIT, ADVERTISE, REQUEST, REPLY give the client an address
 * inside an identity association (IA_NA) with preferred and valid lifetimes;
 * RENEW at T1, REBIND at T2, RELEASE. Stateless: INFORMATION-REQUEST, REPLY
 * carry only settings such as the DNS servers. Which of the two a host uses
 * is decided by the M and O flags of the router advertisements; the default
 * route always comes from the advertisement, never from DHCPv6.
 *
 * Left out: relay agents, prefix delegation (IA_PD), temporary addresses
 * (IA_TA), CONFIRM, DECLINE, RECONFIGURE, authentication, and server
 * preference: with several servers the first ADVERTISE wins.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D6_CLIENT_PORT 546
#define D6_SERVER_PORT 547
#define D6_COLOR       215
#define D6_LEASES      64

enum { M_SOLICIT = 1, M_ADVERTISE, M_REQUEST, M_CONFIRM, M_RENEW, M_REBIND, M_REPLY, M_RELEASE, M_DECLINE,
       M_RECONFIGURE, M_INFOREQ };
enum { O_CLIENTID = 1, O_SERVERID = 2, O_IA_NA = 3, O_IAADDR = 5, O_ORO = 6, O_ELAPSED = 8, O_STATUS = 13,
       O_RAPID = 14, O_DNS = 23 };
enum { ST_SUCCESS = 0, ST_NOADDRS = 2, ST_NOBINDING = 3 };

static const nl_ip6 ALL_DHCP = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2 } };

/* Provided by dns.c: set the resolver of a host (option 23) */
void nl_dns_set_default6(nl_node *n, const nl_ip6 *server);

static const char *msg_name(int t)
{
    switch (t) {
    case M_SOLICIT:   return "SOLICIT";
    case M_ADVERTISE: return "ADVERTISE";
    case M_REQUEST:   return "REQUEST";
    case M_RENEW:     return "RENEW";
    case M_REBIND:    return "REBIND";
    case M_REPLY:     return "REPLY";
    case M_RELEASE:   return "RELEASE";
    case M_INFOREQ:   return "INFORMATION-REQUEST";
    default:          return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Messages                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int      type;
    uint32_t xid;
    const uint8_t *cid, *sid;
    int      cidlen, sidlen;
    int      has_ia, has_addr, rapid, has_dns, oro_dns;
    uint32_t iaid, t1, t2, pref, valid;
    nl_ip6   addr, dns;
    int      status, ia_status;         /* -1 = no status option */
} d6msg;

static int d6_parse(const uint8_t *b, int len, d6msg *m)
{
    int p = 4;

    memset(m, 0, sizeof(*m));
    m->status = m->ia_status = -1;
    if (len < 4)
        return 0;
    m->type = b[0];
    m->xid = (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    while (p + 4 <= len) {
        int code = nl_get16(b + p), l = nl_get16(b + p + 2);
        const uint8_t *v = b + p + 4;

        if (p + 4 + l > len)
            return 0;
        switch (code) {
        case O_CLIENTID: m->cid = v; m->cidlen = l; break;
        case O_SERVERID: m->sid = v; m->sidlen = l; break;
        case O_RAPID:    m->rapid = 1; break;
        case O_STATUS:   if (l >= 2) m->status = nl_get16(v); break;
        case O_DNS:      if (l >= 16) { memcpy(m->dns.b, v, 16); m->has_dns = 1; } break;
        case O_ORO: {
            int k;
            for (k = 0; k + 2 <= l; k += 2)
                if (nl_get16(v + k) == O_DNS)
                    m->oro_dns = 1;
            break;
        }
        case O_IA_NA: {
            int q = 12;

            if (l < 12)
                return 0;
            m->has_ia = 1;
            m->iaid = nl_get32(v);
            m->t1 = nl_get32(v + 4);
            m->t2 = nl_get32(v + 8);
            while (q + 4 <= l) {
                int c2 = nl_get16(v + q), l2 = nl_get16(v + q + 2);

                if (q + 4 + l2 > l)
                    return 0;
                if (c2 == O_IAADDR && l2 >= 24) {
                    memcpy(m->addr.b, v + q + 4, 16);
                    m->pref = nl_get32(v + q + 20);
                    m->valid = nl_get32(v + q + 24);
                    m->has_addr = 1;
                } else if (c2 == O_STATUS && l2 >= 2) {
                    m->ia_status = nl_get16(v + q + 4);
                }
                q += 4 + l2;
            }
            break;
        }
        default: break;
        }
        p += 4 + l;
    }
    return 1;
}

typedef struct { uint8_t b[400]; int len; } d6buf;

static void d6_start(d6buf *o, int type, uint32_t xid)
{
    o->b[0] = (uint8_t)type;
    o->b[1] = (uint8_t)(xid >> 16);
    o->b[2] = (uint8_t)(xid >> 8);
    o->b[3] = (uint8_t)xid;
    o->len = 4;
}

static void d6_opt(d6buf *o, int code, const void *v, int l)
{
    if (o->len + 4 + l > (int)sizeof(o->b))
        return;
    nl_put16(o->b + o->len, (uint16_t)code);
    nl_put16(o->b + o->len + 2, (uint16_t)l);
    if (l)
        memcpy(o->b + o->len + 4, v, (size_t)l);
    o->len += 4 + l;
}

/* IA_NA with at most one address and an optional status */
static void d6_ia(d6buf *o, uint32_t iaid, uint32_t t1, uint32_t t2, const nl_ip6 *addr, uint32_t pref,
                  uint32_t valid, int status)
{
    uint8_t v[12 + 28 + 6];
    int l = 12;

    nl_put32(v, iaid);
    nl_put32(v + 4, t1);
    nl_put32(v + 8, t2);
    if (addr) {
        nl_put16(v + l, O_IAADDR);
        nl_put16(v + l + 2, 24);
        memcpy(v + l + 4, addr->b, 16);
        nl_put32(v + l + 20, pref);
        nl_put32(v + l + 24, valid);
        l += 28;
    }
    if (status >= 0) {
        nl_put16(v + l, O_STATUS);
        nl_put16(v + l + 2, 2);
        nl_put16(v + l + 4, (uint16_t)status);
        l += 6;
    }
    d6_opt(o, O_IA_NA, v, l);
}

/* DUID-LL: type 3, hardware type 1 (Ethernet), the MAC address */
static void duid_ll(const nl_node *n, int ifx, uint8_t out[10])
{
    nl_put16(out, 3);
    nl_put16(out + 2, 1);
    memcpy(out + 4, n->ifc[ifx].mac, 6);
}

static const char *duid_str(const uint8_t *d, int len)
{
    static char b[2][64];
    static int k;
    char *o = b[k = !k];
    int i, p = 0;

    for (i = 0; i < len && p < 60; i++)
        p += snprintf(o + p, 64 - (size_t)p, "%s%02x", i ? ":" : "", d[i]);
    if (!len)
        o[0] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int      used, bound;
    uint8_t  duid[20];
    int      duidlen;
    uint32_t iaid;
    nl_ip6   addr;
    nl_time  expire;
} lease6;

enum { CL_IDLE, CL_SOLICIT, CL_REQUEST, CL_BOUND, CL_RENEW, CL_REBIND, CL_INFO, CL_INFO_DONE, CL_RELEASED };
enum { MODE_AUTO, MODE_STATEFUL, MODE_STATELESS };

typedef struct {
    nl_node *n;
    int      server, client;
    /* server */
    int      sifx;                      /* -1 = every interface */
    nl_ip6   first, dns;
    int      count, has_dns, rapid;
    nl_time  lease;
    lease6   l[D6_LEASES];
    uint8_t  duid[10];
    unsigned n_adv, n_reply, n_info;
    /* client */
    int      cifx, mode, state, want_rapid, tries, ra_seen, ra_m, ra_o, has_addr, has_dns6;
    uint32_t xid, iaid;
    uint8_t  cduid[10], sduid[20];
    int      sduidlen;
    nl_ip6   addr, dns6, srv;
    nl_time  t1, t2, valid_until, bound_at;
    nl_ev    timer;
} d6_state;

static const nl_module d6_mod;

static const char *state_name(int s)
{
    switch (s) {
    case CL_IDLE:      return _("waiting for a router advertisement");
    case CL_SOLICIT:   return "SOLICIT";
    case CL_REQUEST:   return "REQUEST";
    case CL_BOUND:     return "BOUND";
    case CL_RENEW:     return "RENEW";
    case CL_REBIND:    return "REBIND";
    case CL_INFO:      return "INFORMATION-REQUEST";
    case CL_INFO_DONE: return _("configured (stateless)");
    default:          return _("released");
    }
}

static void d6_send(nl_node *n, int ifx, const nl_ip6 *dst, uint16_t sport, uint16_t dport, const d6buf *o)
{
    nl_ip6tx tx;

    memset(&tx, 0, sizeof(tx));
    tx.dst = *dst;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_udp6_send_ex(n, &tx, sport, dport, o->b, o->len);
}

/* ------------------------------------------------------------------ */
/* Server                                                             */
/* ------------------------------------------------------------------ */

static void pool_addr(const d6_state *s, int i, nl_ip6 *out)
{
    *out = s->first;
    nl_put16(out->b + 14, (uint16_t)(nl_get16(s->first.b + 14) + i));
}

static lease6 *lease_find(d6_state *s, const d6msg *m)
{
    int i;

    for (i = 0; i < D6_LEASES; i++)
        if (s->l[i].used && s->l[i].iaid == m->iaid && s->l[i].duidlen == m->cidlen &&
            memcmp(s->l[i].duid, m->cid, (size_t)m->cidlen) == 0)
            return &s->l[i];
    return NULL;
}

/* The binding of this client, or a free address of the pool reserved for it */
static lease6 *lease_get(d6_state *s, const d6msg *m)
{
    lease6 *l = lease_find(s, m), *slot = NULL;
    nl_time now = nl_now();
    int i, k;

    if (l)
        return l;
    if (m->cidlen > (int)sizeof(l->duid))
        return NULL;
    for (i = 0; i < D6_LEASES; i++) {
        if (s->l[i].used && s->l[i].expire <= now)
            s->l[i].used = 0;           /* expired: the address returns to the pool */
        if (!s->l[i].used && !slot)
            slot = &s->l[i];
    }
    if (!slot)
        return NULL;
    for (k = 0; k < s->count; k++) {
        nl_ip6 a;

        pool_addr(s, k, &a);
        for (i = 0; i < D6_LEASES; i++)
            if (s->l[i].used && nl_ip6_eq(&s->l[i].addr, &a))
                break;
        if (i == D6_LEASES) {
            memset(slot, 0, sizeof(*slot));
            slot->used = 1;
            memcpy(slot->duid, m->cid, (size_t)m->cidlen);
            slot->duidlen = m->cidlen;
            slot->iaid = m->iaid;
            slot->addr = a;
            slot->expire = now + NL_SEC(10);    /* offered, not yet requested */
            return slot;
        }
    }
    return NULL;
}

static int leases_bound(d6_state *s)
{
    int i, c = 0;

    for (i = 0; i < D6_LEASES; i++)
        if (s->l[i].used && s->l[i].bound && s->l[i].expire > nl_now())
            c++;
    return c;
}

static void server_rx(nl_node *n, void *ctx, const nl_udp6msg *um)
{
    d6_state *s = ctx;
    d6msg m;
    d6buf o;
    lease6 *l = NULL;
    uint32_t lease_s = (uint32_t)(s->lease / NL_SEC(1));
    int reply = M_REPLY;

    if (!s->server || (s->sifx >= 0 && um->in_if != s->sifx) || !d6_parse(um->data, um->len, &m) || !m.cid)
        return;
    /* messages addressed to one server carry its identifier */
    if (m.sid && (m.sidlen != 10 || memcmp(m.sid, s->duid, 10) != 0))
        return;
    if ((m.type == M_REQUEST || m.type == M_RENEW || m.type == M_RELEASE) && !m.sid)
        return;
    nl_log(n, NL_L_APP, 2, um->pkt_id, T_FIN "DHCPv6" T_RESET " %s %s %s " T_DIM "(DUID %s)", msg_name(m.type),
           _("from"), nl_ip6_str(&um->src), duid_str(m.cid, m.cidlen));
    switch (m.type) {
    case M_SOLICIT:
        reply = m.rapid && s->rapid ? M_REPLY : M_ADVERTISE;
        d6_start(&o, reply, m.xid);
        d6_opt(&o, O_CLIENTID, m.cid, m.cidlen);
        d6_opt(&o, O_SERVERID, s->duid, 10);
        if (m.has_ia) {
            l = s->count ? lease_get(s, &m) : NULL;
            if (!l) {
                d6_ia(&o, m.iaid, 0, 0, NULL, 0, 0, ST_NOADDRS);
                nl_count(n, "dhcp6.noaddr", 1);
                nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " " T_WARN "%s", s->count
                       ? _("pool exhausted: no address to offer") : _("no pool configured: no address to offer"));
            } else {
                if (reply == M_REPLY) {
                    l->bound = 1;
                    l->expire = nl_now() + s->lease * 3 / 2;
                    d6_opt(&o, O_RAPID, NULL, 0);
                }
                d6_ia(&o, m.iaid, lease_s / 2, lease_s * 4 / 5, &l->addr, lease_s, lease_s * 3 / 2, -1);
            }
        }
        s->n_adv++;
        break;
    case M_REQUEST: case M_RENEW: case M_REBIND:
        if (!m.has_ia)
            return;
        l = m.type == M_REQUEST ? (s->count ? lease_get(s, &m) : NULL) : lease_find(s, &m);
        if (!l && m.type == M_REBIND)
            return;                     /* not ours: stay silent */
        d6_start(&o, M_REPLY, m.xid);
        d6_opt(&o, O_CLIENTID, m.cid, m.cidlen);
        d6_opt(&o, O_SERVERID, s->duid, 10);
        if (!l) {
            d6_ia(&o, m.iaid, 0, 0, NULL, 0, 0, m.type == M_REQUEST ? ST_NOADDRS : ST_NOBINDING);
            nl_count(n, m.type == M_REQUEST ? "dhcp6.noaddr" : "dhcp6.nobinding", 1);
        } else {
            int fresh = !l->bound;

            l->bound = 1;
            l->expire = nl_now() + s->lease * 3 / 2;
            d6_ia(&o, m.iaid, lease_s / 2, lease_s * 4 / 5, &l->addr, lease_s, lease_s * 3 / 2, -1);
            nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s %s %s %s " T_DIM "(%s %u s)",
                   fresh ? _("address") : _("lease renewed:"), nl_ip6_str(&l->addr), _("for"),
                   duid_str(m.cid, m.cidlen), _("valid"), lease_s * 3 / 2);
        }
        s->n_reply++;
        break;
    case M_RELEASE:
        l = lease_find(s, &m);
        if (l) {
            nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s %s", nl_ip6_str(&l->addr),
                   _("released: back in the pool"));
            l->used = 0;
        }
        d6_start(&o, M_REPLY, m.xid);
        d6_opt(&o, O_CLIENTID, m.cid, m.cidlen);
        d6_opt(&o, O_SERVERID, s->duid, 10);
        {
            uint8_t st[2] = { 0, ST_SUCCESS };
            d6_opt(&o, O_STATUS, st, 2);
        }
        break;
    case M_INFOREQ:
        d6_start(&o, M_REPLY, m.xid);
        d6_opt(&o, O_CLIENTID, m.cid, m.cidlen);
        d6_opt(&o, O_SERVERID, s->duid, 10);
        s->n_info++;
        nl_count(n, "dhcp6.info_replies", 1);
        break;
    default:
        return;
    }
    if (s->has_dns)
        d6_opt(&o, O_DNS, s->dns.b, 16);
    nl_count_set(n, "dhcp6.leases", leases_bound(s));
    nl_log(n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s → %s", msg_name(reply), nl_ip6_str(&um->src));
    d6_send(n, um->in_if, &um->src, D6_SERVER_PORT, um->sport, &o);
}

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

static void client_send(d6_state *s);

static void client_arm(d6_state *s, nl_time after, void (*fn)(void *))
{
    nl_cancel(&s->timer);
    s->timer = nl_after(s->n, after, fn, s);
}

static void client_retry(void *ctx)
{
    d6_state *s = ctx;

    s->timer = 0;
    s->tries++;
    if (s->state == CL_REQUEST && s->tries >= 5) {
        s->state = CL_SOLICIT;           /* the chosen server went silent: look for another */
        s->tries = 0;
        s->xid = nl_rand(1u << 24);
    }
    client_send(s);
}

static void client_drop_addr(d6_state *s)
{
    if (s->has_addr)
        nl_if_del_addr6(s->n, s->cifx, &s->addr);
    s->has_addr = 0;
    nl_count_set(s->n, "dhcp6.bound", 0);
}

static void client_begin(d6_state *s, int state)
{
    s->state = state;
    s->tries = 0;
    s->xid = nl_rand(1u << 24);
    client_send(s);
}

/* Lifetime events of a bound address: T1 renew, T2 rebind, valid lifetime end */
static void client_lifetime(void *ctx)
{
    d6_state *s = ctx;
    nl_time now = nl_now();

    s->timer = 0;
    if (now >= s->valid_until) {
        nl_log(s->n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " " T_ERR "%s %s", nl_ip6_str(&s->addr),
               _("expired: the address is removed, starting again"));
        nl_count(s->n, "dhcp6.expired", 1);
        client_drop_addr(s);
        client_begin(s, CL_SOLICIT);
    } else if (now >= s->t2 && s->state != CL_REBIND) {
        nl_log(s->n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s", _("T2: the server did not answer, REBIND with any server"));
        client_begin(s, CL_REBIND);
    } else if (now >= s->t1 && s->state == CL_BOUND) {
        nl_log(s->n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s", _("T1: time to RENEW the lease"));
        client_begin(s, CL_RENEW);
    } else {
        nl_time next = s->state == CL_BOUND ? s->t1 : s->state == CL_RENEW ? s->t2 : s->valid_until;
        client_arm(s, next - now, client_lifetime);
    }
}

static void client_send(d6_state *s)
{
    d6buf o;
    uint8_t oro[2], el[2] = { 0, 0 };
    nl_time wait = NL_SEC(1) << (s->tries < 3 ? s->tries : 3);
    int type;

    switch (s->state) {
    case CL_SOLICIT: type = M_SOLICIT; break;
    case CL_REQUEST: type = M_REQUEST; break;
    case CL_RENEW:   type = M_RENEW; break;
    case CL_REBIND:  type = M_REBIND; break;
    case CL_INFO:    type = M_INFOREQ; break;
    default: return;
    }
    d6_start(&o, type, s->xid);
    d6_opt(&o, O_CLIENTID, s->cduid, 10);
    if (type == M_REQUEST || type == M_RENEW)
        d6_opt(&o, O_SERVERID, s->sduid, s->sduidlen);
    nl_put16(el, (uint16_t)(s->tries * 100));
    d6_opt(&o, O_ELAPSED, el, 2);
    nl_put16(oro, O_DNS);
    d6_opt(&o, O_ORO, oro, 2);
    if (type == M_SOLICIT) {
        if (s->want_rapid)
            d6_opt(&o, O_RAPID, NULL, 0);
        d6_ia(&o, s->iaid, 0, 0, NULL, 0, 0, -1);
    } else if (type != M_INFOREQ) {
        d6_ia(&o, s->iaid, 0, 0, &s->addr, 0, 0, -1);
    }
    nl_log(s->n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s → ff02::1:2 " T_DIM "(xid %06x%s)", msg_name(type),
           s->xid, s->tries ? _(", again") : "");
    nl_count(s->n, "dhcp6.sent", 1);
    d6_send(s->n, s->cifx, &ALL_DHCP, D6_CLIENT_PORT, D6_SERVER_PORT, &o);
    if (s->state == CL_RENEW || s->state == CL_REBIND) {
        /* keep trying, but never past the next lifetime event */
        nl_time limit = (s->state == CL_RENEW ? s->t2 : s->valid_until) - nl_now();
        if (limit < wait) {
            client_arm(s, limit > 0 ? limit : 0, client_lifetime);
            return;
        }
    }
    client_arm(s, wait, client_retry);
}

static void client_dns(d6_state *s, const d6msg *m)
{
    if (!m->has_dns)
        return;
    s->dns6 = m->dns;
    s->has_dns6 = 1;
    nl_dns_set_default6(s->n, &m->dns);
    nl_count_set(s->n, "dhcp6.dns", 1);
}

static void client_rx(nl_node *n, void *ctx, const nl_udp6msg *um)
{
    d6_state *s = ctx;
    d6msg m;

    if (!s->client || um->in_if != s->cifx || !d6_parse(um->data, um->len, &m) || m.xid != s->xid || !m.cid ||
        m.cidlen != 10 || memcmp(m.cid, s->cduid, 10) != 0 || !m.sid || m.sidlen > (int)sizeof(s->sduid))
        return;
    if (m.type == M_ADVERTISE && s->state == CL_SOLICIT) {
        if (!m.has_addr) {
            nl_log(n, NL_L_APP, 1, um->pkt_id, T_FIN "DHCPv6" T_RESET " " T_WARN "%s %s: %s", _("server"),
                   nl_ip6_str(&um->src), _("no addresses available"));
            nl_count(n, "dhcp6.refused", 1);
            return;                     /* keep soliciting: another server, or later */
        }
        memcpy(s->sduid, m.sid, (size_t)m.sidlen);
        s->sduidlen = m.sidlen;
        s->addr = m.addr;
        s->srv = um->src;
        nl_log(n, NL_L_APP, 2, um->pkt_id, T_FIN "DHCPv6" T_RESET " ADVERTISE %s %s: %s", _("from"),
               nl_ip6_str(&um->src), nl_ip6_str(&m.addr));
        client_begin(s, CL_REQUEST);
        return;
    }
    if (m.type != M_REPLY)
        return;
    if (s->state == CL_INFO) {
        nl_cancel(&s->timer);
        s->state = CL_INFO_DONE;
        client_dns(s, &m);
        nl_count(n, "dhcp6.info", 1);
        nl_log(n, NL_L_APP, 1, um->pkt_id, T_OK "DHCPv6: %s" T_RESET " %s %s", _("stateless configuration:"),
               _("DNS server"), m.has_dns ? nl_ip6_str(&m.dns) : "—");
        return;
    }
    if (s->state == CL_RELEASED)
        return;
    if (s->state != CL_REQUEST && s->state != CL_RENEW && s->state != CL_REBIND && !(s->state == CL_SOLICIT && m.rapid))
        return;
    if (!m.has_addr) {
        /* NoBinding, NoAddrsAvail: the lease is gone, start again */
        nl_log(n, NL_L_APP, 1, um->pkt_id, T_FIN "DHCPv6" T_RESET " " T_WARN "%s", m.ia_status == ST_NOBINDING
               ? _("the server no longer knows this lease: starting again") : _("no addresses available"));
        nl_count(n, "dhcp6.refused", 1);
        if (s->state != CL_REQUEST && s->state != CL_SOLICIT)
            client_drop_addr(s);
        s->state = CL_SOLICIT;
        s->tries = 2;
        s->xid = nl_rand(1u << 24);
        client_arm(s, NL_SEC(4), client_retry);
        return;
    }
    {
        int fresh = !s->has_addr || !nl_ip6_eq(&s->addr, &m.addr);
        nl_time now = nl_now();

        if (fresh) {
            int k;

            client_drop_addr(s);
            s->addr = m.addr;
            if (nl_if_add_addr6(n, s->cifx, &m.addr, 128) != 0)
                return;
            for (k = 0; k < n->ifc[s->cifx].nip6; k++)
                if (nl_ip6_eq(&n->ifc[s->cifx].ip6[k].a, &m.addr))
                    n->ifc[s->cifx].ip6[k].autoconf = 1;     /* forgotten when the node loses power */
            s->has_addr = 1;
            s->bound_at = now;
            nl_count(n, "dhcp6.bound_events", 1);
        } else {
            nl_count(n, "dhcp6.renewals", 1);
        }
        memcpy(s->sduid, m.sid, (size_t)m.sidlen);
        s->sduidlen = m.sidlen;
        s->srv = um->src;
        s->t1 = now + NL_SEC(m.t1 ? m.t1 : m.pref / 2);
        s->t2 = now + NL_SEC(m.t2 ? m.t2 : m.pref * 4 / 5);
        s->valid_until = now + NL_SEC(m.valid);
        s->state = CL_BOUND;
        client_dns(s, &m);
        nl_count_set(n, "dhcp6.bound", 1);
        nl_log(n, NL_L_APP, 1, um->pkt_id, T_OK "DHCPv6: %s %s" T_RESET " %s %s " T_DIM "(%s %u s, T1 %u s, T2 %u s%s%s)",
               fresh ? _("address") : _("lease renewed:"), nl_ip6_str(&m.addr), _("from server"), nl_ip6_str(&um->src),
               _("valid"), m.valid, m.t1, m.t2, m.has_dns ? ", DNS " : "", m.has_dns ? nl_ip6_str(&m.dns) : "");
        client_arm(s, s->t1 - now, client_lifetime);
    }
}

/* The M and O flags of the router say what DHCPv6 is for on this link */
static void client_ra(nl_node *n, void *ctx, int ifx, const nl_ip6 *router, int managed, int other)
{
    d6_state *s = ctx;

    if (!s->client || ifx != s->cifx)
        return;
    if (s->ra_seen && s->ra_m == managed && s->ra_o == other)
        return;
    s->ra_seen = 1;
    s->ra_m = managed;
    s->ra_o = other;
    if (s->mode != MODE_AUTO || s->state != CL_IDLE)
        return;
    if (managed) {
        nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s %s: %s", _("router"), nl_ip6_str(router),
               _("M flag set, asking a DHCPv6 server for an address"));
        client_begin(s, CL_SOLICIT);
    } else if (other) {
        nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s %s: %s", _("router"), nl_ip6_str(router),
               _("O flag set, asking a DHCPv6 server for the other settings only"));
        client_begin(s, CL_INFO);
    } else {
        nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " %s %s: %s", _("router"), nl_ip6_str(router),
               _("M and O flags clear, DHCPv6 is not used on this link (SLAAC only)"));
        nl_count_set(n, "dhcp6.unused", 1);
    }
}

/* ------------------------------------------------------------------ */
/* Module                                                             */
/* ------------------------------------------------------------------ */

static d6_state *d6_get(nl_node *n)
{
    d6_state *s = nl_mod_get(n, &d6_mod);

    if (s)
        return s;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->n = n;
    s->sifx = -1;
    nl_mod_set(n, &d6_mod, s);
    return s;
}

static int d6_run(nl_node *n, const nl_args *a)
{
    const char *role = nl_arg_pos(a, 0), *ifname = nl_arg_str(a, "if", NULL);
    d6_state *s;
    int ifx = ifname ? nl_if_find(n, ifname) : 0;

    if (!nl_ip6_enabled()) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp6: %s", _("this lab does not use IPv6 (see `ipv6 on`)"));
        return -1;
    }
    if (!role || ifx < 0 || ifx >= n->nif) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp6: %s", _("usage: dhcp6 server|client|release [if=IFACE] ..."));
        return -1;
    }
    s = d6_get(n);
    if (!s)
        return -1;
    if (strcmp(role, "server") == 0) {
        const char *pool = nl_arg_str(a, "pool", NULL), *dns = nl_arg_str(a, "dns", NULL);

        s->server = 1;
        s->sifx = ifname ? ifx : -1;
        s->lease = nl_arg_time(a, "lease", NL_SEC(60));
        if (s->lease < NL_SEC(4))
            s->lease = NL_SEC(4);
        s->rapid = (int)nl_arg_int(a, "rapid", 0);
        duid_ll(n, 0, s->duid);
        s->count = 0;
        if (pool) {
            char first[64];
            const char *dash = strchr(pool, '-');
            nl_ip6 last;

            snprintf(first, sizeof(first), "%.*s", dash ? (int)(dash - pool) : 63, pool);
            if (!nl_parse_ip6(first, &s->first) || (dash && !nl_parse_ip6(dash + 1, &last))) {
                nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp6: %s '%s'", _("bad pool (FIRST-LAST, IPv6 addresses)"), pool);
                return -1;
            }
            s->count = dash ? nl_get16(last.b + 14) - nl_get16(s->first.b + 14) + 1 : 1;
            if (s->count < 1) s->count = 1;
            if (s->count > D6_LEASES) s->count = D6_LEASES;
        }
        s->has_dns = dns && (nl_parse_ip6(dns, &s->dns) || nl_resolve6(dns, &s->dns));
        if (dns && !s->has_dns) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp6: %s '%s'", _("unknown DNS server"), dns);
            return -1;
        }
        nl_ip6_join(n, &ALL_DHCP);
        nl_udp6_bind(n, D6_SERVER_PORT, server_rx, s);
        if (s->count)
            nl_log(n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s %s (%d %s), %s %s", _("server ready, pool from"),
                   nl_ip6_str(&s->first), s->count, _("addresses"), _("preferred lifetime"), nl_time_str(s->lease));
        else
            nl_log(n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s", _("server ready, stateless: settings only, no addresses"));
        return 0;
    }
    if (strcmp(role, "release") == 0) {
        d6buf o;

        if (!s->client || !s->has_addr)
            return 0;
        nl_cancel(&s->timer);
        s->xid = nl_rand(1u << 24);
        d6_start(&o, M_RELEASE, s->xid);
        d6_opt(&o, O_CLIENTID, s->cduid, 10);
        d6_opt(&o, O_SERVERID, s->sduid, s->sduidlen);
        d6_ia(&o, s->iaid, 0, 0, &s->addr, 0, 0, -1);
        nl_log(n, NL_L_APP, 1, 0, T_FIN "DHCPv6" T_RESET " RELEASE %s", nl_ip6_str(&s->addr));
        d6_send(n, s->cifx, &ALL_DHCP, D6_CLIENT_PORT, D6_SERVER_PORT, &o);
        client_drop_addr(s);
        s->state = CL_RELEASED;
        return 0;
    }
    if (strcmp(role, "client") == 0) {
        const char *mode = nl_arg_str(a, "mode", "auto");

        nl_cancel(&s->timer);
        s->client = 1;
        s->cifx = ifx;
        s->mode = !strcmp(mode, "stateful") ? MODE_STATEFUL : !strcmp(mode, "stateless") ? MODE_STATELESS : MODE_AUTO;
        s->want_rapid = (int)nl_arg_int(a, "rapid", 0);
        s->iaid = (uint32_t)(n->idx * 16 + ifx + 1);
        duid_ll(n, ifx, s->cduid);
        s->state = CL_IDLE;
        s->ra_seen = 0;
        nl_udp6_bind(n, D6_CLIENT_PORT, client_rx, s);
        nl_ra_watch(n, client_ra, s);
        if (s->mode == MODE_STATEFUL)
            client_begin(s, CL_SOLICIT);
        else if (s->mode == MODE_STATELESS)
            client_begin(s, CL_INFO);
        else
            nl_log(n, NL_L_APP, 2, 0, T_FIN "DHCPv6" T_RESET " %s",
                   _("client waiting for a router advertisement to learn whether DHCPv6 is used here"));
        return 0;
    }
    nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp6: %s", _("usage: dhcp6 server|client|release [if=IFACE] ..."));
    return -1;
}

static void d6_stop(nl_node *n)
{
    d6_state *s = nl_mod_get(n, &d6_mod);

    if (!s)
        return;
    nl_cancel(&s->timer);
    if (s->server) {
        nl_udp6_unbind(n, D6_SERVER_PORT);
        nl_ip6_leave(n, &ALL_DHCP);
    }
    if (s->client) {
        nl_udp6_unbind(n, D6_CLIENT_PORT);
        nl_ra_unwatch(n, client_ra, s);
        client_drop_addr(s);
    }
    nl_mod_set(n, &d6_mod, NULL);
    free(s);
}

/* Power loss: a client forgets its lease (the core drops the address); a
 * server keeps its bindings, which are on stable storage */
static void d6_crash(nl_node *n)
{
    d6_state *s = nl_mod_get(n, &d6_mod);

    nl_cancel(&s->timer);
    if (s->client) {
        s->has_addr = 0;
        s->state = CL_IDLE;
        s->ra_seen = 0;
        nl_count_set(n, "dhcp6.bound", 0);
    }
}

static void d6_recover(nl_node *n)
{
    d6_state *s = nl_mod_get(n, &d6_mod);

    if (s->server) {
        nl_ip6_join(n, &ALL_DHCP);
        nl_udp6_bind(n, D6_SERVER_PORT, server_rx, s);
    }
    if (s->client) {
        nl_udp6_bind(n, D6_CLIENT_PORT, client_rx, s);
        nl_ra_watch(n, client_ra, s);
        if (s->mode == MODE_STATEFUL)
            client_begin(s, CL_SOLICIT);
        else if (s->mode == MODE_STATELESS)
            client_begin(s, CL_INFO);
    }
}

static int d6_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    d6_state *s = nl_mod_get(n, &d6_mod);
    nl_time now = nl_now();
    int row = 0, i;

    (void)detail;
    if (s->server) {
        if (s->count)
            row += nl_ui_kv(x, y + row, w, _("pool"), "%s … (%d)", nl_ip6_str(&s->first), s->count);
        else
            row += nl_ui_kv(x, y + row, w, _("pool"), "%s", _("none: stateless server"));
        row += nl_ui_kv(x, y + row, w, _("DNS server handed out"), "%s", s->has_dns ? nl_ip6_str(&s->dns) : "—");
        row += nl_ui_kv(x, y + row, w, _("advertise / reply / information"), "%u / %u / %u", s->n_adv, s->n_reply,
                        s->n_info);
        for (i = 0; i < D6_LEASES && row < h; i++)
            if (s->l[i].used && s->l[i].expire > now)
                tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%s%-24s" T_RESET " " T_DIM "%s" T_RESET " " T_TIME "%s",
                          s->l[i].bound ? T_OK : T_WARN, nl_ip6_str(&s->l[i].addr),
                          duid_str(s->l[i].duid + (s->l[i].duidlen > 6 ? s->l[i].duidlen - 6 : 0),
                                   s->l[i].duidlen > 6 ? 6 : s->l[i].duidlen),
                          nl_time_str(s->l[i].expire - now));
    }
    if (s->client && row < h) {
        row += nl_ui_kv(x, y + row, w, _("state"), "%s", state_name(s->state));
        row += nl_ui_kv(x, y + row, w, _("router flags"), "%s", !s->ra_seen ? "—" : s->ra_m ? "M (managed)"
                        : s->ra_o ? "O (other configuration)" : _("none (SLAAC only)"));
        row += nl_ui_kv(x, y + row, w, "DUID", "%s", duid_str(s->cduid, 10));
        if (s->has_addr) {
            double total = (double)(s->valid_until - s->bound_at), left = (double)(s->valid_until - now);

            row += nl_ui_kv(x, y + row, w, _("address"), "%s", nl_ip6_str(&s->addr));
            row += nl_ui_kv(x, y + row, w, _("renew / rebind / expire in"), "%s / %s / %s",
                            nl_time_str(s->t1 > now ? s->t1 - now : 0), nl_time_str(s->t2 > now ? s->t2 - now : 0),
                            nl_time_str(s->valid_until > now ? s->valid_until - now : 0));
            if (row < h && total > 0)
                tui_bar(x, y + row++, w > 40 ? 40 : w, left > 0 ? left / total : 0, C_OK);
        }
        if (s->has_dns6)
            row += nl_ui_kv(x, y + row, w, _("DNS server"), "%s", nl_ip6_str(&s->dns6));
    }
    return row;
}

static const nl_module d6_mod = {
    .name = "dhcp6", .title = N_("DHCPv6 server and client: addresses and settings for IPv6 hosts"),
    .layer = NL_L_APP,
    .usage = N_("run NODE dhcp6 server [pool=FIRST-LAST] [lease=60s] [dns=ADDR] [rapid=1] [if=eth0]\n"
                "  pool: IPv6 addresses to lease (at most 64); without it the server is stateless\n"
                "  and only hands out settings. lease is the preferred lifetime (valid = 1.5 x).\n"
                "run NODE dhcp6 client [mode=auto|stateful|stateless] [rapid=1] [if=eth0]\n"
                "  auto follows the router advertisements: M flag = ask for an address, O flag =\n"
                "  ask for the settings only, neither = do nothing (the address comes from SLAAC).\n"
                "run NODE dhcp6 release\n"
                "  Pair it with `run ROUTER ra managed=1` or `other=1`; the default route always\n"
                "  comes from the router advertisement."),
    .theory = N_("IPv6 hosts can configure themselves from router advertisements alone (SLAAC), so "
                 "DHCPv6 has a narrower job than DHCP for IPv4, and the router decides whether it is "
                 "used: the M flag of the advertisement says \"addresses are managed, ask a DHCPv6 "
                 "server\", the O flag says \"take your address from SLAAC but ask a server for the other "
                 "settings\", typically the DNS servers. In both cases the default route still comes "
                 "from the advertisement: DHCPv6 has no router option, and no prefix length either, "
                 "so which neighbours are on-link is again the router's word.\n\n"
                 "A client has no broadcast to shout on. It sends from its link-local address to the "
                 "multicast group ff02::1:2, where servers and relays listen. Stateful exchange: "
                 "SOLICIT, then ADVERTISE from every willing server, REQUEST to the chosen one, REPLY "
                 "with the address (with the Rapid Commit option the middle two are skipped). The "
                 "address lives inside an identity association (IA_NA) identified by an IAID, and "
                 "carries a preferred and a valid lifetime; the association has two timers, T1 when "
                 "the client sends RENEW to its server and T2 when it sends REBIND to any server, and "
                 "when the valid lifetime ends the address must go. RELEASE returns it early. "
                 "Stateless exchange: INFORMATION-REQUEST and REPLY, no lease at all.\n\n"
                 "Clients and servers are named by a DUID, an identifier that stays the same on every "
                 "interface and survives a change of network card better than a MAC address; here it "
                 "is the simplest kind, built from the link-layer address. Left out: relay agents "
                 "(the server must be on the client's link), prefix delegation, temporary addresses, "
                 "CONFIRM and DECLINE, and server preference."),
    .run = d6_run, .stop = d6_stop, .crash = d6_crash, .recover = d6_recover, .panel = d6_panel,
};
NL_MODULE(d6_mod)

/* ------------------------------------------------------------------ */
/* Dissector                                                          */
/* ------------------------------------------------------------------ */

static void dis_dhcp6(nl_dissect *o, const uint8_t *d, int len, int off)
{
    d6msg m;
    char extra[120] = "";

    if (!d6_parse(d, len, &m)) {
        nl_dis_summary(o, "DHCPv6", D6_COLOR, "malformed message");
        return;
    }
    nl_dis_field(o, "DHCPv6", "Message type", off, 1, "%d (%s)", m.type, msg_name(m.type));
    nl_dis_field(o, "DHCPv6", "Transaction id", off + 1, 3, "0x%06x", m.xid);
    if (m.cid)
        nl_dis_field(o, "DHCPv6", "Client DUID", off + (int)(m.cid - d), m.cidlen, "%s", duid_str(m.cid, m.cidlen));
    if (m.sid)
        nl_dis_field(o, "DHCPv6", "Server DUID", off + (int)(m.sid - d), m.sidlen, "%s", duid_str(m.sid, m.sidlen));
    if (m.has_ia)
        nl_dis_field(o, "DHCPv6", "IA_NA", off, 0, "IAID %u, T1 %u s, T2 %u s", m.iaid, m.t1, m.t2);
    if (m.has_addr) {
        nl_dis_field(o, "DHCPv6", "IA address", off, 0, "%s preferred %u s valid %u s", nl_ip6_str(&m.addr), m.pref,
                     m.valid);
        snprintf(extra, sizeof(extra), " %s", nl_ip6_str(&m.addr));
    }
    if (m.ia_status > 0) {
        nl_dis_field(o, "DHCPv6", "Status", off, 0, "%d (%s)", m.ia_status,
                     m.ia_status == ST_NOADDRS ? "NoAddrsAvail" : m.ia_status == ST_NOBINDING ? "NoBinding" : "?");
        snprintf(extra, sizeof(extra), " %s", m.ia_status == ST_NOADDRS ? "NoAddrsAvail" : "NoBinding");
    }
    if (m.rapid)
        nl_dis_field(o, "DHCPv6", "Rapid commit", off, 0, "yes");
    if (m.has_dns)
        nl_dis_field(o, "DHCPv6", "DNS server", off, 0, "%s", nl_ip6_str(&m.dns));
    nl_dis_summary(o, "DHCPv6", D6_COLOR, "%s xid=0x%06x%s%s%s", msg_name(m.type), m.xid, extra,
                   m.has_dns ? " DNS " : "", m.has_dns ? nl_ip6_str(&m.dns) : "");
}

NL_INIT(dhcp6)
{
    nl_dissector_udp(D6_CLIENT_PORT, dis_dhcp6);
    nl_dissector_udp(D6_SERVER_PORT, dis_dhcp6);
}
