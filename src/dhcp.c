/*
 * dhcp.c - The Dynamic Host Configuration Protocol with the real BOOTP
 *          message layout and DHCP options, over UDP 67 (server) and 68
 *          (client): DISCOVER / OFFER / REQUEST / ACK / NAK / RELEASE, a
 *          lease table with expiry, and a client state machine that renews,
 *          rebinds and expires.
 *
 * Simplifications: one subnet per server (no relay agents); addresses are
 * handed out in pool order with a simple reuse of the client's previous
 * lease; the client identifier is the interface MAC; T1 = lease/2 and
 * T2 = lease*7/8 as in the RFC defaults; no option 55 parameter list.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOTP_REQUEST 1
#define BOOTP_REPLY   2
#define DHCP_SPORT    68          /* client */
#define DHCP_DPORT    67          /* server */
#define DHCP_MAGIC    0x63825363u
#define DHCP_COLOR    215

/* message types (option 53) */
enum { DHCPDISCOVER = 1, DHCPOFFER, DHCPREQUEST, DHCPDECLINE, DHCPACK, DHCPNAK, DHCPRELEASE };
/* options */
enum { OPT_MASK = 1, OPT_ROUTER = 3, OPT_DNS = 6, OPT_REQIP = 50, OPT_LEASE = 51, OPT_MSGTYPE = 53,
       OPT_SERVERID = 54, OPT_T1 = 58, OPT_T2 = 59, OPT_END = 255 };

/* Provided by dns.c: set the resolver of a host from DHCP option 6 */
void nl_dns_set_default(nl_node *n, nl_ip server);

static const char *msgtype_name(int t)
{
    switch (t) {
    case DHCPDISCOVER: return "DISCOVER";
    case DHCPOFFER:    return "OFFER";
    case DHCPREQUEST:  return "REQUEST";
    case DHCPDECLINE:  return "DECLINE";
    case DHCPACK:      return "ACK";
    case DHCPNAK:      return "NAK";
    case DHCPRELEASE:  return "RELEASE";
    default:           return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Message building and parsing                                       */
/* ------------------------------------------------------------------ */

#define BOOTP_FIX 236            /* fixed part before the options */

typedef struct {
    int      op, msgtype;
    uint32_t xid;
    nl_ip    ciaddr, yiaddr, siaddr;
    uint8_t  chaddr[6];
    nl_ip    mask, router, dns, serverid, reqip;
    uint32_t lease, t1, t2;
    int      ok;
} dhcp_msg;

static int build(uint8_t *b, int op, uint32_t xid, const uint8_t chaddr[6], nl_ip ci, nl_ip yi, int msgtype)
{
    int n;

    memset(b, 0, BOOTP_FIX);
    b[0] = (uint8_t)op;
    b[1] = 1;                    /* htype: Ethernet */
    b[2] = 6;                    /* hlen */
    nl_put32(b + 4, xid);
    nl_put32(b + 12, ci);        /* ciaddr */
    nl_put32(b + 16, yi);        /* yiaddr */
    memcpy(b + 28, chaddr, 6);
    nl_put32(b + BOOTP_FIX, DHCP_MAGIC);
    n = BOOTP_FIX + 4;
    b[n++] = OPT_MSGTYPE;
    b[n++] = 1;
    b[n++] = (uint8_t)msgtype;
    return n;
}

static int opt_u32(uint8_t *b, int n, int code, uint32_t v)
{
    b[n++] = (uint8_t)code;
    b[n++] = 4;
    nl_put32(b + n, v);
    return n + 4;
}

static int opt_end(uint8_t *b, int n)
{
    b[n++] = OPT_END;
    return n;
}

static int parse(const uint8_t *b, int len, dhcp_msg *m)
{
    int n = BOOTP_FIX + 4;

    memset(m, 0, sizeof(*m));
    if (len < BOOTP_FIX + 4 || nl_get32(b + BOOTP_FIX) != DHCP_MAGIC)
        return 0;
    m->op = b[0];
    m->xid = nl_get32(b + 4);
    m->ciaddr = nl_get32(b + 12);
    m->yiaddr = nl_get32(b + 16);
    m->siaddr = nl_get32(b + 20);
    memcpy(m->chaddr, b + 28, 6);
    while (n < len) {
        int code = b[n++], l;

        if (code == OPT_END)
            break;
        if (code == 0)
            continue;            /* pad */
        if (n >= len)
            break;
        l = b[n++];
        if (n + l > len)
            break;
        switch (code) {
        case OPT_MSGTYPE: m->msgtype = b[n]; break;
        case OPT_MASK:     if (l == 4) m->mask = nl_get32(b + n); break;
        case OPT_ROUTER:   if (l >= 4) m->router = nl_get32(b + n); break;
        case OPT_DNS:      if (l >= 4) m->dns = nl_get32(b + n); break;
        case OPT_SERVERID: if (l == 4) m->serverid = nl_get32(b + n); break;
        case OPT_REQIP:    if (l == 4) m->reqip = nl_get32(b + n); break;
        case OPT_LEASE:    if (l == 4) m->lease = nl_get32(b + n); break;
        case OPT_T1:       if (l == 4) m->t1 = nl_get32(b + n); break;
        case OPT_T2:       if (l == 4) m->t2 = nl_get32(b + n); break;
        default: break;
        }
        n += l;
    }
    m->ok = 1;
    return 1;
}

/* Broadcast a message out of one interface, from 0.0.0.0 when we have no
 * address yet; the core delivers 255.255.255.255 to every local node. */
static void dhcp_bcast(nl_node *n, int ifx, uint16_t sport, uint16_t dport, nl_ip src, const uint8_t *b, int len)
{
    nl_iptx tx;

    memset(&tx, 0, sizeof(tx));
    tx.src = src;
    tx.dst = NL_IP_BROADCAST;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_udp_send_ex(n, &tx, sport, dport, b, len);
}

/* ------------------------------------------------------------------ */
/* Server                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  mac[6];
    nl_ip    ip;
    nl_time  expire;
    int      used, bound;        /* bound 0 = only offered */
} lease;

typedef struct {
    nl_node *n;
    int      ifx;
    nl_ip    first, last, mask, router, dns, serverid;
    uint32_t leasetime;
    lease    pool[64];
    int      npool;
    unsigned offers, acks, naks;
    nl_ev    timer;
} dhcp_server;

static const nl_module dhcps_mod;

static lease *lease_by_mac(dhcp_server *s, const uint8_t *mac)
{
    int i;

    for (i = 0; i < s->npool; i++)
        if (s->pool[i].used && memcmp(s->pool[i].mac, mac, 6) == 0)
            return &s->pool[i];
    return NULL;
}

static lease *lease_by_ip(dhcp_server *s, nl_ip ip)
{
    int i;

    for (i = 0; i < s->npool; i++)
        if (s->pool[i].used && s->pool[i].ip == ip)
            return &s->pool[i];
    return NULL;
}

/* Pick (or reuse) an address for a client; NULL when the pool is full */
static lease *lease_alloc(dhcp_server *s, const uint8_t *mac, nl_ip prefer)
{
    nl_time now = nl_now();
    lease *e = lease_by_mac(s, mac);
    nl_ip ip;
    int i;

    for (i = 0; i < s->npool; i++)
        if (s->pool[i].used && s->pool[i].expire <= now)
            s->pool[i].used = 0;
    if (e)
        return e;
    if (prefer && prefer >= s->first && prefer <= s->last && !lease_by_ip(s, prefer))
        ip = prefer;
    else {
        ip = 0;
        for (ip = s->first; ip <= s->last; ip++)
            if (!lease_by_ip(s, ip))
                break;
        if (ip > s->last)
            return NULL;
    }
    if (s->npool >= (int)(sizeof(s->pool) / sizeof(s->pool[0])))
        return NULL;
    e = &s->pool[s->npool++];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    return e;
}

static void server_send(dhcp_server *s, int type, const uint8_t *mac, uint32_t xid, nl_ip yi)
{
    uint8_t b[400];
    int n = build(b, BOOTP_REPLY, xid, mac, 0, yi, type);

    if (type != DHCPNAK) {
        n = opt_u32(b, n, OPT_MASK, s->mask);
        if (s->router)
            n = opt_u32(b, n, OPT_ROUTER, s->router);
        if (s->dns)
            n = opt_u32(b, n, OPT_DNS, s->dns);
        n = opt_u32(b, n, OPT_LEASE, s->leasetime);
        n = opt_u32(b, n, OPT_T1, s->leasetime / 2);
        n = opt_u32(b, n, OPT_T2, s->leasetime * 7 / 8);
    }
    n = opt_u32(b, n, OPT_SERVERID, s->serverid);
    n = opt_end(b, n);
    dhcp_bcast(s->n, s->ifx, DHCP_DPORT, DHCP_SPORT, s->serverid, b, n);
}

static void server_rx(nl_node *n, void *ctx, const nl_udpmsg *um)
{
    dhcp_server *s = ctx;
    dhcp_msg m;
    lease *e;

    if (!parse(um->data, um->len, &m) || m.op != BOOTP_REQUEST)
        return;
    switch (m.msgtype) {
    case DHCPDISCOVER:
        e = lease_alloc(s, m.chaddr, m.reqip);
        if (!e) {
            nl_log(n, NL_L_APP, 1, um->pkt_id, T_WARN "DHCP" T_RESET " DISCOVER %s %s — %s", _("from"),
                   nl_mac_str(m.chaddr), _("pool exhausted, no offer"));
            nl_count(n, "dhcp.exhausted", 1);
            return;
        }
        e->expire = nl_now() + NL_SEC(s->leasetime);     /* hold the offer */
        s->offers++;
        nl_count(n, "dhcp.offers", 1);
        nl_log(n, NL_L_APP, 2, um->pkt_id, T_WARN "DHCP" T_RESET " DISCOVER %s %s → OFFER %s", _("from"),
               nl_mac_str(m.chaddr), nl_ip_str(e->ip));
        server_send(s, DHCPOFFER, m.chaddr, m.xid, e->ip);
        break;
    case DHCPREQUEST: {
        nl_ip want = m.reqip ? m.reqip : m.ciaddr;

        if (m.serverid && m.serverid != s->serverid)
            return;                                      /* the client chose another server */
        e = lease_by_mac(s, m.chaddr);
        if (!e || (want && e->ip != want)) {
            s->naks++;
            nl_count(n, "dhcp.naks", 1);
            nl_log(n, NL_L_APP, 2, um->pkt_id, T_ERR "DHCP" T_RESET " REQUEST %s %s %s → NAK", _("from"),
                   nl_mac_str(m.chaddr), want ? nl_ip_str(want) : "");
            server_send(s, DHCPNAK, m.chaddr, m.xid, 0);
            return;
        }
        e->bound = 1;
        e->expire = nl_now() + NL_SEC(s->leasetime);
        s->acks++;
        nl_count(n, "dhcp.acks", 1);
        nl_count_set(n, "dhcp.leases", s->npool);
        nl_log(n, NL_L_APP, 2, um->pkt_id, T_WARN "DHCP" T_RESET " REQUEST %s → ACK %s " T_DIM "(%s %u s)",
               nl_mac_str(m.chaddr), nl_ip_str(e->ip), _("lease"), s->leasetime);
        server_send(s, DHCPACK, m.chaddr, m.xid, e->ip);
        break;
    }
    case DHCPRELEASE:
        e = lease_by_mac(s, m.chaddr);
        if (e) {
            nl_log(n, NL_L_APP, 2, um->pkt_id, T_WARN "DHCP" T_RESET " RELEASE %s %s", nl_mac_str(m.chaddr),
                   nl_ip_str(e->ip));
            e->used = 0;
            nl_count_set(n, "dhcp.leases", s->npool);
        }
        break;
    default:
        break;
    }
}

static void server_sweep(void *ctx)
{
    dhcp_server *s = ctx;
    nl_time now = nl_now();
    int i, live = 0;

    for (i = 0; i < s->npool; i++)
        if (s->pool[i].used) {
            if (s->pool[i].expire <= now)
                s->pool[i].used = 0;
            else if (s->pool[i].bound)
                live++;
        }
    nl_count_set(s->n, "dhcp.bound", live);
    s->timer = nl_after(s->n, NL_SEC(1), server_sweep, s);
}

static int server_run(nl_node *n, const nl_args *a)
{
    dhcp_server *s = nl_mod_get(n, &dhcps_mod);
    const char *pool = nl_arg_str(a, "pool", NULL), *ifn = nl_arg_str(a, "if", NULL);
    const char *dns = nl_arg_str(a, "dns", NULL), *router = nl_arg_str(a, "router", NULL);
    char buf[64];
    char *dash;
    int ifx = ifn ? nl_if_find(n, ifn) : 0;

    if (ifx < 0 || ifx >= n->nif) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp: %s '%s'", _("unknown interface"), ifn ? ifn : "eth0");
        return -1;
    }
    if (!pool) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp: %s", _("server needs pool=FIRST-LAST"));
        return -1;
    }
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        s->n = n;
        nl_mod_set(n, &dhcps_mod, s);
        nl_udp_bind(n, DHCP_DPORT, server_rx, s);
        s->timer = nl_after(n, NL_SEC(1), server_sweep, s);
    }
    s->ifx = ifx;
    snprintf(buf, sizeof(buf), "%s", pool);
    dash = strchr(buf, '-');
    if (dash)
        *dash = '\0';
    if (!dash || !nl_parse_ip(buf, &s->first) || !nl_parse_ip(dash + 1, &s->last) || s->last < s->first) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp: %s", _("bad pool (use FIRST-LAST)"));
        return -1;
    }
    s->mask = nl_mask(n->ifc[ifx].plen ? n->ifc[ifx].plen : 24);
    s->serverid = n->ifc[ifx].ip;
    s->router = n->ifc[ifx].ip;
    if (router)
        nl_resolve(router, &s->router);
    if (dns)
        nl_resolve(dns, &s->dns);
    s->leasetime = (uint32_t)(nl_arg_time(a, "lease", NL_SEC(60)) / 1000000);
    if (!s->leasetime)
        s->leasetime = 1;
    nl_log(n, NL_L_APP, 2, 0, T_WARN "DHCP" T_RESET " %s %s–%s, %s %u s", _("server ready, pool"),
           nl_ip_str(s->first), nl_ip_str(s->last), _("lease"), s->leasetime);
    return 0;
}

static void server_stop(nl_node *n)
{
    dhcp_server *s = nl_mod_get(n, &dhcps_mod);

    if (!s)
        return;
    nl_cancel(&s->timer);
    nl_udp_unbind(n, DHCP_DPORT);
    nl_mod_set(n, &dhcps_mod, NULL);
    free(s);
}

static void server_recover(nl_node *n)
{
    dhcp_server *s = nl_mod_get(n, &dhcps_mod);

    if (s)                              /* leases are soft state: a reboot forgets them */
        s->timer = nl_after(n, NL_SEC(1), server_sweep, s);
}

static int server_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    dhcp_server *s = nl_mod_get(n, &dhcps_mod);
    nl_time now = nl_now();
    int row = 0, i;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("pool"), "%s – %s", nl_ip_str(s->first), nl_ip_str(s->last));
    row += nl_ui_kv(x, y + row, w, _("offers / ACK / NAK"), "%u / %u / %u", s->offers, s->acks, s->naks);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("leases (remaining)"));
    for (i = 0; i < s->npool && row < h; i++) {
        const lease *e = &s->pool[i];
        if (!e->used)
            continue;
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%-15s %-18s %s%4lld s", nl_ip_str(e->ip),
                  nl_mac_str(e->mac), e->bound ? T_OK "●" T_RESET " " : T_WARN "offered " T_RESET,
                  e->bound ? (long long)((e->expire - now) / 1000000) : 0);
    }
    return row;
}

static const nl_module dhcps_mod = {
    .name = "dhcp", .title = N_("DHCP server and client: hand out and renew addresses"), .layer = NL_L_APP,
    .usage = N_("run NODE dhcp server pool=FIRST-LAST [lease=60s] [dns=ADDR] [router=ADDR] [if=eth0]\n"
                "run NODE dhcp client [if=eth0]      obtain and keep an address\n"
                "run NODE dhcp release               give the address back\n"
                "  Hosts in a DHCP scenario start with no address; give the server a static ip."),
    .theory = NULL,                     /* the theory is on the registered front module below */
    .run = server_run, .stop = server_stop, .recover = server_recover, .crash = server_stop, .panel = server_panel,
};
/* not registered: the `dhcp` name is owned by the front dispatcher below */

/* ------------------------------------------------------------------ */
/* Client                                                             */
/* ------------------------------------------------------------------ */

enum { CL_INIT, CL_SELECTING, CL_REQUESTING, CL_BOUND, CL_RENEWING, CL_REBINDING };

typedef struct {
    nl_node *n;
    int      ifx, state;
    uint32_t xid;
    nl_ip    offered, server, mask, router, dns;
    int      plen;
    uint32_t lease, t1, t2;
    nl_time  got_at;
    int      tries;
    nl_ev    timer;
    unsigned discovers;
} dhcp_client;

static const nl_module dhcpc_mod;
static void client_discover(dhcp_client *c);

static int masklen(nl_ip m)
{
    int l = 0;

    while (m & 0x80000000u) {
        l++;
        m <<= 1;
    }
    return l;
}

static void client_send(dhcp_client *c, int type, nl_ip ci)
{
    uint8_t b[400];
    nl_node *n = c->n;
    int nn = build(b, BOOTP_REQUEST, c->xid, n->ifc[c->ifx].mac, ci, 0, type);

    if (type == DHCPREQUEST) {
        if (c->offered)
            nn = opt_u32(b, nn, OPT_REQIP, c->offered);
        if (c->state == CL_REQUESTING && c->server)
            nn = opt_u32(b, nn, OPT_SERVERID, c->server);
    }
    nn = opt_end(b, nn);
    if (type == DHCPRELEASE) {
        nl_iptx tx;                     /* unicast to the server */
        memset(&tx, 0, sizeof(tx));
        tx.dst = c->server;
        nl_udp_send_ex(n, &tx, DHCP_SPORT, DHCP_DPORT, b, nn);
    } else if ((c->state == CL_RENEWING) && c->server) {
        nl_iptx tx;
        memset(&tx, 0, sizeof(tx));
        tx.src = ci;
        tx.dst = c->server;
        tx.ifx = c->ifx;
        nl_udp_send_ex(n, &tx, DHCP_SPORT, DHCP_DPORT, b, nn);
    } else {
        dhcp_bcast(n, c->ifx, DHCP_SPORT, DHCP_DPORT, ci, b, nn);
    }
}

static void client_apply(dhcp_client *c)
{
    nl_node *n = c->n;

    nl_if_set_addr(n, c->ifx, c->offered, c->plen);
    if (c->router) {
        nl_route_del(n, 0, 0, NL_RT_DHCP);
        nl_route_set(n, 0, 0, c->router, c->ifx, 1, NL_RT_DHCP);
    }
    if (c->dns)
        nl_dns_set_default(n, c->dns);
    nl_count_set(n, "dhcp.bound", 1);
}

static void client_timer(void *ctx)
{
    dhcp_client *c = ctx;
    nl_time now = nl_now();

    c->timer = 0;
    switch (c->state) {
    case CL_SELECTING:
    case CL_REQUESTING:
        if (++c->tries > 4) {
            nl_log(c->n, NL_L_APP, 1, 0, T_ERR "DHCP" T_RESET " %s", _("no answer, starting over"));
            client_discover(c);
            return;
        }
        if (c->state == CL_SELECTING)
            client_send(c, DHCPDISCOVER, 0);
        else
            client_send(c, DHCPREQUEST, 0);
        c->timer = nl_after(c->n, NL_SEC(2), client_timer, c);
        break;
    case CL_BOUND:                      /* reached T1: renew */
        c->state = CL_RENEWING;
        c->tries = 0;
        nl_log(c->n, NL_L_APP, 2, 0, T_WARN "DHCP" T_RESET " T1: %s %s", _("renewing with"), nl_ip_str(c->server));
        client_send(c, DHCPREQUEST, c->offered);
        c->timer = nl_after(c->n, (c->got_at + NL_SEC(c->t2)) - now, client_timer, c);
        break;
    case CL_RENEWING:                   /* reached T2: rebind to any server */
        c->state = CL_REBINDING;
        nl_log(c->n, NL_L_APP, 2, 0, T_WARN "DHCP" T_RESET " T2: %s", _("rebinding (broadcast)"));
        client_send(c, DHCPREQUEST, c->offered);
        c->timer = nl_after(c->n, (c->got_at + NL_SEC(c->lease)) - now, client_timer, c);
        break;
    case CL_REBINDING:                  /* lease expired */
        nl_log(c->n, NL_L_APP, 1, 0, T_ERR "DHCP" T_RESET " %s %s", _("lease expired, dropping"),
               nl_ip_str(c->offered));
        nl_route_del(c->n, 0, 0, NL_RT_DHCP);
        nl_if_set_addr(c->n, c->ifx, 0, 0);
        nl_count_set(c->n, "dhcp.bound", 0);
        client_discover(c);
        break;
    default:
        break;
    }
}

static void client_discover(dhcp_client *c)
{
    c->state = CL_SELECTING;
    c->xid = nl_pkt_newid();
    c->offered = c->server = 0;
    c->tries = 1;
    c->discovers++;
    nl_count(c->n, "dhcp.discovers", 1);
    nl_log(c->n, NL_L_APP, 2, 0, T_WARN "DHCP" T_RESET " DISCOVER %s %s (%s)", _("broadcast from"),
           nl_mac_str(c->n->ifc[c->ifx].mac), c->n->ifc[c->ifx].name);
    client_send(c, DHCPDISCOVER, 0);
    nl_cancel(&c->timer);
    c->timer = nl_after(c->n, NL_SEC(2), client_timer, c);
}

static void client_rx(nl_node *n, void *ctx, const nl_udpmsg *um)
{
    dhcp_client *c = ctx;
    dhcp_msg m;

    if (!parse(um->data, um->len, &m) || m.op != BOOTP_REPLY || m.xid != c->xid ||
        memcmp(m.chaddr, n->ifc[c->ifx].mac, 6) != 0)
        return;
    if (m.msgtype == DHCPOFFER && (c->state == CL_SELECTING)) {
        c->offered = m.yiaddr;
        c->server = m.serverid;
        c->mask = m.mask;
        c->router = m.router;
        c->dns = m.dns;
        c->plen = m.mask ? masklen(m.mask) : 24;
        c->state = CL_REQUESTING;
        c->tries = 1;
        nl_log(n, NL_L_APP, 2, um->pkt_id, T_WARN "DHCP" T_RESET " OFFER %s %s %s → REQUEST", nl_ip_str(m.yiaddr),
               _("from"), nl_ip_str(m.serverid));
        client_send(c, DHCPREQUEST, 0);
        nl_cancel(&c->timer);
        c->timer = nl_after(n, NL_SEC(2), client_timer, c);
    } else if (m.msgtype == DHCPACK) {
        int renew = c->state == CL_RENEWING || c->state == CL_REBINDING;

        c->lease = m.lease ? m.lease : 60;
        c->t1 = m.t1 ? m.t1 : c->lease / 2;
        c->t2 = m.t2 ? m.t2 : c->lease * 7 / 8;
        c->got_at = nl_now();
        c->state = CL_BOUND;
        c->offered = m.yiaddr;
        if (m.router) c->router = m.router;
        if (m.dns) c->dns = m.dns;
        if (m.mask) c->plen = masklen(m.mask);
        client_apply(c);
        nl_count(n, "dhcp.bound_events", 1);
        nl_log(n, NL_L_APP, 1, um->pkt_id, T_OK "DHCP" T_RESET " %s %s/%d " T_DIM "(%s %s, %s %s, %s %u s)",
               renew ? _("lease renewed:") : _("bound:"), nl_ip_str(c->offered), c->plen, _("gw"),
               c->router ? nl_ip_str(c->router) : "—", "dns", c->dns ? nl_ip_str(c->dns) : "—", _("lease"),
               c->lease);
        nl_cancel(&c->timer);
        c->timer = nl_after(n, NL_SEC(c->t1), client_timer, c);
    } else if (m.msgtype == DHCPNAK) {
        nl_log(n, NL_L_APP, 1, um->pkt_id, T_ERR "DHCP" T_RESET " NAK %s", _("from the server, starting over"));
        client_discover(c);
    }
}

static int client_run(nl_node *n, const nl_args *a)
{
    dhcp_client *c = nl_mod_get(n, &dhcpc_mod);
    const char *sub = nl_arg_pos(a, 0), *ifn = nl_arg_str(a, "if", NULL);
    int ifx = ifn ? nl_if_find(n, ifn) : 0;

    if (sub && strcmp(sub, "release") == 0) {
        if (c && c->state >= CL_BOUND && c->offered) {
            nl_log(n, NL_L_APP, 1, 0, T_WARN "DHCP" T_RESET " %s %s", _("releasing"), nl_ip_str(c->offered));
            client_send(c, DHCPRELEASE, c->offered);
            nl_route_del(n, 0, 0, NL_RT_DHCP);
            nl_if_set_addr(n, c->ifx, 0, 0);
            nl_cancel(&c->timer);
            c->state = CL_INIT;
            nl_count_set(n, "dhcp.bound", 0);
        }
        return 0;
    }
    if (ifx < 0 || ifx >= n->nif) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dhcp: %s '%s'", _("unknown interface"), ifn ? ifn : "eth0");
        return -1;
    }
    if (!c) {
        c = calloc(1, sizeof(*c));
        if (!c)
            return -1;
        c->n = n;
        nl_mod_set(n, &dhcpc_mod, c);
        nl_udp_bind(n, DHCP_SPORT, client_rx, c);
    }
    c->ifx = ifx;
    client_discover(c);
    return 0;
}

static void client_stop(nl_node *n)
{
    dhcp_client *c = nl_mod_get(n, &dhcpc_mod);

    if (!c)
        return;
    nl_cancel(&c->timer);
    nl_udp_unbind(n, DHCP_SPORT);
    nl_mod_set(n, &dhcpc_mod, NULL);
    free(c);
}

/* A crash loses the lease; on recovery the client starts a fresh DISCOVER */
static void client_crash(nl_node *n)
{
    dhcp_client *c = nl_mod_get(n, &dhcpc_mod);

    if (c)
        c->state = CL_INIT;
}

static void client_recover(nl_node *n)
{
    dhcp_client *c = nl_mod_get(n, &dhcpc_mod);

    if (c)
        client_discover(c);
}

static int client_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    static const char *const names[] = { "INIT", "SELECTING", "REQUESTING", "BOUND", "RENEWING", "REBINDING" };
    dhcp_client *c = nl_mod_get(n, &dhcpc_mod);
    nl_time now = nl_now();
    int row = 0;

    (void)detail;
    (void)h;
    row += nl_ui_kv(x, y + row, w, _("state"), "%s", _(names[c->state]));
    row += nl_ui_kv(x, y + row, w, _("address"), "%s", c->offered ? nl_ip_str(c->offered) : "—");
    row += nl_ui_kv(x, y + row, w, _("server / gateway"), "%s / %s", c->server ? nl_ip_str(c->server) : "—",
                    c->router ? nl_ip_str(c->router) : "—");
    if (c->state >= CL_BOUND && c->lease) {
        nl_time left = (c->got_at + NL_SEC(c->lease)) - now;
        row += nl_ui_kv(x, y + row, w, _("lease remaining"), "%lld / %u s", (long long)(left / 1000000), c->lease);
        row += nl_ui_kv(x, y + row, w, _("T1 renew / T2 rebind"), "%u / %u s", c->t1, c->t2);
    }
    return row;
}

static const nl_module dhcpc_mod = {
    .name = "dhcp-client-internal", .title = N_("DHCP client state"), .layer = NL_L_APP,
    .usage = NULL, .theory = NULL,
    .run = client_run, .stop = client_stop, .crash = client_crash, .recover = client_recover,
    .panel = client_panel,
};

/* The `dhcp` name reaches the server module; `dhcp client/release` is routed
 * here by a tiny front that dispatches on the first word. */
static int dhcp_front(nl_node *n, const nl_args *a)
{
    const char *role = nl_arg_pos(a, 0);

    if (role && (strcmp(role, "client") == 0 || strcmp(role, "release") == 0))
        return client_run(n, a);
    return server_run(n, a);
}

static const nl_module dhcp_mod = {
    .name = "dhcp", .title = N_("DHCP server and client: hand out and renew addresses"), .layer = NL_L_APP,
    .usage = N_("run NODE dhcp server pool=FIRST-LAST [lease=60s] [dns=ADDR] [router=ADDR] [if=eth0]\n"
                "run NODE dhcp client [if=eth0]      obtain and keep an address\n"
                "run NODE dhcp release               give the address back\n"
                "  Hosts in a DHCP scenario start with no address; give the server a static ip."),
    .theory = N_("A host that joins a network with no address uses DHCP to get one. It cannot send a "
                 "normal unicast yet, so everything starts as a broadcast. The client broadcasts a "
                 "DISCOVER; every server that can serve the subnet replies with an OFFER proposing an "
                 "address, the subnet mask, the default gateway, the DNS servers and a lease time. "
                 "The client picks one offer and broadcasts a REQUEST naming the chosen server (so "
                 "the others take their offer back); that server answers ACK and the client finally "
                 "configures the interface, or NAK if the address is no longer free.\n\n"
                 "A lease is temporary. The client starts two timers: at T1 (half the lease) it tries "
                 "to renew with the same server by unicast; at T2 (seven eighths) it broadcasts to "
                 "any server; if the lease runs out it must drop the address and start over. RELEASE "
                 "returns an address early. The message format is inherited from the older BOOTP "
                 "protocol: a fixed header carrying the client hardware address and the offered "
                 "address, followed by a list of typed options, of which option 53 (the message "
                 "type) turns BOOTP into DHCP. Here one server serves its own subnet directly; real "
                 "networks reach a central server through relay agents."),
    .run = dhcp_front, .stop = server_stop, .recover = server_recover, .crash = server_stop,
    .panel = server_panel,
};
NL_MODULE(dhcp_mod)

/* ------------------------------------------------------------------ */
/* Dissector                                                          */
/* ------------------------------------------------------------------ */

static void dis_dhcp(nl_dissect *o, const uint8_t *d, int len, int off)
{
    dhcp_msg m;

    if (!parse(d, len, &m)) {
        nl_dis_summary(o, "DHCP", DHCP_COLOR, "BOOTP/DHCP (%d bytes)", len);
        return;
    }
    nl_dis_field(o, "DHCP", "Opcode", off, 1, "%d (%s)", m.op, m.op == BOOTP_REQUEST ? "BOOTREQUEST" : "BOOTREPLY");
    nl_dis_field(o, "DHCP", "Transaction ID", off + 4, 4, "0x%08x", m.xid);
    nl_dis_field(o, "DHCP", "Client address", off + 12, 4, "%s", nl_ip_str(m.ciaddr));
    nl_dis_field(o, "DHCP", "Your address", off + 16, 4, "%s", nl_ip_str(m.yiaddr));
    nl_dis_field(o, "DHCP", "Client MAC", off + 28, 6, "%s", nl_mac_str(m.chaddr));
    nl_dis_field(o, "DHCP", "Message type", off + BOOTP_FIX, 0, "%d (%s)", m.msgtype, msgtype_name(m.msgtype));
    if (m.serverid)
        nl_dis_field(o, "DHCP", "Server identifier", off, 0, "%s", nl_ip_str(m.serverid));
    if (m.reqip)
        nl_dis_field(o, "DHCP", "Requested address", off, 0, "%s", nl_ip_str(m.reqip));
    if (m.lease)
        nl_dis_field(o, "DHCP", "Lease time", off, 0, "%u s", m.lease);
    if (m.mask)
        nl_dis_field(o, "DHCP", "Subnet mask", off, 0, "%s", nl_ip_str(m.mask));
    if (m.router)
        nl_dis_field(o, "DHCP", "Router", off, 0, "%s", nl_ip_str(m.router));
    if (m.dns)
        nl_dis_field(o, "DHCP", "DNS server", off, 0, "%s", nl_ip_str(m.dns));
    if (m.msgtype == DHCPOFFER || m.msgtype == DHCPACK)
        nl_dis_summary(o, "DHCP", DHCP_COLOR, "%s %s from %s", msgtype_name(m.msgtype), nl_ip_str(m.yiaddr),
                       nl_ip_str(m.serverid));
    else if (m.msgtype == DHCPREQUEST)
        nl_dis_summary(o, "DHCP", DHCP_COLOR, "REQUEST %s", m.reqip ? nl_ip_str(m.reqip) : nl_ip_str(m.ciaddr));
    else
        nl_dis_summary(o, "DHCP", DHCP_COLOR, "%s", msgtype_name(m.msgtype));
}

NL_INIT(dhcp)
{
    nl_dissector_udp(DHCP_DPORT, dis_dhcp);
    nl_dissector_udp(DHCP_SPORT, dis_dhcp);
}
