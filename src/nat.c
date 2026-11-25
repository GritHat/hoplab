/*
 * nat.c - Network address and port translation (NAPT) with connection
 *         tracking, the four classic NAT behaviours, static port forwarding
 *         and hairpinning; plus `rendezvous`, a tiny UDP hole-punching
 *         service to see which pairs of NAT types let two hosts talk
 *         directly.
 *
 * Simplifications: one outside interface and one public address per NAT;
 * non-first fragments get the address rewritten only; TCP state is tracked
 * loosely (FIN/RST shorten the timeout); the checksum of the packet quoted
 * inside ICMP errors is not recomputed.
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* nat                                                                */
/* ------------------------------------------------------------------ */

#define NAT_MAX   256
#define NAT_PERM  8
#define NAT_PORT0 61001

enum { NT_FULL, NT_RESTRICTED, NT_PORT, NT_SYMMETRIC };

typedef struct {
    int      used, is_static, closing;
    uint8_t  proto;
    nl_ip    in_ip;
    uint16_t in_port, ext_port;
    nl_ip    rem_ip;                    /* first destination (the only one when symmetric) */
    uint16_t rem_port;
    struct { nl_ip ip; uint16_t port; } perm[NAT_PERM];    /* destinations contacted */
    int      nperm, perm_next;
    nl_time  created, last;
    unsigned out, in;
} nat_map;

typedef struct {
    nl_node *n;
    int      out;                       /* outside interface */
    int      type, hairpin, preserve;
    nl_time  timeout;
    uint16_t next_port;
    nat_map  m[NAT_MAX];
    nl_ev    timer;
} nat_state;

static const nl_module nat_mod;

static const char *nat_type_name(int t)
{
    switch (t) {
    case NT_FULL:       return "full cone";
    case NT_RESTRICTED: return "address-restricted cone";
    case NT_PORT:       return "port-restricted cone";
    default:            return "symmetric";
    }
}

static const char *proto_name(int p)
{
    return p == NL_PROTO_UDP ? "udp" : p == NL_PROTO_TCP ? "tcp" : p == NL_PROTO_ICMP ? "icmp" : "ip";
}

static nat_map *map_in(nat_state *st, int proto, uint16_t ext_port)
{
    int i;

    for (i = 0; i < NAT_MAX; i++)
        if (st->m[i].used && st->m[i].proto == proto && st->m[i].ext_port == ext_port)
            return &st->m[i];
    return NULL;
}

static uint16_t alloc_port(nat_state *st, int proto, uint16_t want)
{
    int tries;

    if (st->preserve && want && !map_in(st, proto, want))
        return want;
    for (tries = 0; tries < 5000; tries++) {
        uint16_t p;

        if (st->type == NT_SYMMETRIC) {
            p = (uint16_t)(20000 + nl_rand(40000));
        } else {
            p = st->next_port++;
            if (st->next_port < NAT_PORT0)
                st->next_port = NAT_PORT0;
        }
        if (!map_in(st, proto, p))
            return p;
    }
    return 0;
}

static nat_map *map_out(nat_state *st, int proto, nl_ip in_ip, uint16_t in_port, nl_ip rem_ip,
                        uint16_t rem_port, int create)
{
    nat_map *m, *slot = NULL;
    uint16_t port;
    int i;

    for (i = 0; i < NAT_MAX; i++) {
        m = &st->m[i];
        if (!m->used) {
            if (!slot)
                slot = m;
            continue;
        }
        if (m->proto != proto || m->in_ip != in_ip || m->in_port != in_port)
            continue;
        /* endpoint-independent mapping reuses the entry for every
         * destination; a symmetric NAT needs one per destination */
        if (m->is_static || st->type != NT_SYMMETRIC || (m->rem_ip == rem_ip && m->rem_port == rem_port))
            return m;
    }
    if (!create || !slot || (port = alloc_port(st, proto, in_port)) == 0)
        return NULL;
    m = slot;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    m->proto = (uint8_t)proto;
    m->in_ip = in_ip;
    m->in_port = in_port;
    m->ext_port = port;
    m->rem_ip = rem_ip;
    m->rem_port = rem_port;
    m->created = m->last = nl_now();
    nl_count(st->n, "nat.mappings", 1);
    nl_log(st->n, NL_L_NET, 2, 0, T_INFO "NAT" T_RESET " %s %s %s:%u ↔ %s:%u " T_DIM "(→ %s:%u)",
           _("new mapping"), proto_name(proto), nl_ip_str(in_ip), in_port,
           nl_ip_str(st->n->ifc[st->out].ip), port, nl_ip_str(rem_ip), rem_port);
    return m;
}

static void perm_add(nat_map *m, nl_ip ip, uint16_t port)
{
    int i;

    for (i = 0; i < m->nperm; i++)
        if (m->perm[i].ip == ip && m->perm[i].port == port)
            return;
    i = m->nperm < NAT_PERM ? m->nperm++ : (m->perm_next++ % NAT_PERM);
    m->perm[i].ip = ip;
    m->perm[i].port = port;
}

/* Filtering: may this remote endpoint use the mapping from outside? */
static int allowed(const nat_state *st, const nat_map *m, nl_ip ip, uint16_t port)
{
    int i, by_port = m->proto != NL_PROTO_ICMP;

    if (m->is_static || st->type == NT_FULL)
        return 1;
    if (st->type == NT_SYMMETRIC)
        return m->rem_ip == ip && (!by_port || m->rem_port == port);
    for (i = 0; i < m->nperm; i++)
        if (m->perm[i].ip == ip && (st->type == NT_RESTRICTED || !by_port || m->perm[i].port == port))
            return 1;
    return 0;
}

static void touch(nat_map *m, const nl_ipview *v)
{
    m->last = nl_now();
    if (v->proto == NL_PROTO_TCP && (v->tcp_flags & 0x05))      /* FIN or RST */
        m->closing = 1;
}

/* Inside -> outside: rewrite the source */
static int nat_post(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    nat_state *st = ctx;
    nl_ipview v;
    nat_map *m;
    nl_ip ext = n->ifc[st->out].ip;
    uint16_t rport;

    if (out_if != st->out || in_if < 0 || in_if == st->out || !ext || !nl_ipv_parse(p, &v) || v.src == ext)
        return NL_HOOK_OK;
    if (!v.has_ports) {                 /* fragments, ICMP errors: the address only */
        v.src = ext;
        nl_ipv_commit(p, &v);
        return NL_HOOK_OK;
    }
    rport = v.proto == NL_PROTO_ICMP ? 0 : v.dport;
    m = map_out(st, v.proto, v.src, v.sport, v.dst, rport, 1);
    if (!m) {
        nl_log(n, NL_L_NET, 2, p->id, T_ERR "NAT" T_RESET " %s", _("translation table full: packet dropped"));
        nl_count(n, "nat.table_full", 1);
        return NL_HOOK_DROP;
    }
    perm_add(m, v.dst, rport);
    touch(m, &v);
    m->out++;
    nl_log(n, NL_L_NET, 3, p->id, T_INFO "NAT" T_RESET " out %s %s:%u → %s:%u", proto_name(v.proto),
           nl_ip_str(v.src), v.sport, nl_ip_str(ext), m->ext_port);
    v.src = ext;
    v.sport = m->ext_port;
    nl_ipv_commit(p, &v);
    nl_count(n, "nat.translated", 1);
    return NL_HOOK_OK;
}

/* An ICMP error about a translated packet: fix the quoted header too */
static int nat_icmp_error(nat_state *st, nl_pkt *p, nl_ipview *v, nl_ip ext)
{
    uint8_t *l4 = p->data + v->l4off, *q = l4 + 8, *qp;
    nat_map *m;
    int qihl;

    if (v->l4len < 8 + NL_IP_HLEN + 8 || (l4[0] != NL_ICMP_UNREACH && l4[0] != NL_ICMP_TIME_EXCEEDED))
        return NL_HOOK_OK;
    qihl = (q[0] & 15) * 4;
    if (v->l4len < 8 + qihl + 8 || nl_get32(q + 12) != ext)
        return NL_HOOK_OK;
    qp = q + qihl;
    m = map_in(st, q[9], q[9] == NL_PROTO_ICMP ? nl_get16(qp + 4) : nl_get16(qp));
    if (!m)
        return NL_HOOK_OK;
    nl_put32(q + 12, m->in_ip);
    nl_put16(q[9] == NL_PROTO_ICMP ? qp + 4 : qp, m->in_port);
    nl_put16(q + 10, 0);
    nl_put16(q + 10, nl_cksum(q, qihl, 0));
    nl_put16(l4 + 2, 0);
    nl_put16(l4 + 2, nl_cksum(l4, v->l4len, 0));
    v->dst = m->in_ip;
    nl_ipv_commit(p, v);
    m->in++;
    nl_log(st->n, NL_L_NET, 3, p->id, T_INFO "NAT" T_RESET " in  icmp error → %s", nl_ip_str(m->in_ip));
    nl_count(st->n, "nat.translated", 1);
    return NL_HOOK_OK;
}

/* Outside -> inside: rewrite the destination when a mapping allows it */
static int nat_pre(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    nat_state *st = ctx;
    nl_ipview v;
    nat_map *m;
    nl_ip ext = n->ifc[st->out].ip;
    int hair = in_if != st->out;

    (void)out_if;
    if (!ext || in_if < 0 || !nl_ipv_parse(p, &v) || v.dst != ext || v.frag)
        return NL_HOOK_OK;
    if (hair && (!st->hairpin || v.proto == NL_PROTO_ICMP))
        return NL_HOOK_OK;
    if (v.proto == NL_PROTO_ICMP && !v.has_ports)
        return nat_icmp_error(st, p, &v, ext);
    if (!v.has_ports || (v.proto == NL_PROTO_ICMP && v.icmp_type == NL_ICMP_ECHO))
        return NL_HOOK_OK;              /* e.g. a ping to the router itself */
    m = map_in(st, v.proto, v.dport);
    if (!m) {
        if (v.proto != NL_PROTO_ICMP && !hair) {
            nl_log(n, NL_L_NET, 2, p->id, T_WARN "NAT" T_RESET " %s %s:%u → %s %u: %s", proto_name(v.proto),
                   nl_ip_str(v.src), v.sport, _("port"), v.dport,
                   _("no mapping, nothing inside to deliver it to"));
            nl_count(n, "nat.blocked", 1);
        }
        return NL_HOOK_OK;              /* the router's own stack decides */
    }
    if (!hair && !allowed(st, m, v.src, v.sport)) {
        nl_log(n, NL_L_NET, 2, p->id, T_ERR "NAT" T_RESET " %s %s:%u → %s %u: %s (%s)", proto_name(v.proto),
               nl_ip_str(v.src), v.sport, _("port"), v.dport,
               _("unsolicited sender filtered out"), nat_type_name(st->type));
        nl_count(n, "nat.blocked", 1);
        return NL_HOOK_DROP;
    }
    if (hair) {
        /* inside host talking to our public address: translate both ends
         * so that the answer comes back through us */
        nat_map *self = map_out(st, v.proto, v.src, v.sport, v.dst, v.dport, 1);

        if (!self)
            return NL_HOOK_DROP;
        perm_add(self, v.dst, v.dport);
        touch(self, &v);
        self->out++;
        v.src = ext;
        v.sport = self->ext_port;
        nl_log(n, NL_L_NET, 2, p->id, T_INFO "NAT" T_RESET " %s", _("hairpin: inside host reached via the public address"));
    }
    nl_log(n, NL_L_NET, 3, p->id, T_INFO "NAT" T_RESET " in  %s %s:%u → %s:%u", proto_name(v.proto),
           nl_ip_str(ext), m->ext_port, nl_ip_str(m->in_ip), m->in_port);
    v.dst = m->in_ip;
    if (v.proto == NL_PROTO_ICMP)
        v.sport = m->in_port;           /* the echo identifier */
    else
        v.dport = m->in_port;
    touch(m, &v);
    m->in++;
    nl_ipv_commit(p, &v);
    nl_count(n, "nat.translated", 1);
    return NL_HOOK_OK;
}

static void nat_sweep(void *ctx)
{
    nat_state *st = ctx;
    nl_time now = nl_now();
    int i, active = 0;

    for (i = 0; i < NAT_MAX; i++) {
        nat_map *m = &st->m[i];
        nl_time limit = m->closing ? NL_SEC(2) : m->proto == NL_PROTO_TCP ? 4 * st->timeout : st->timeout;

        if (!m->used)
            continue;
        if (!m->is_static && now - m->last > limit) {
            nl_log(st->n, NL_L_NET, 2, 0, T_DIM "NAT %s %s %s:%u ↔ :%u", _("mapping expired"),
                   proto_name(m->proto), nl_ip_str(m->in_ip), m->in_port, m->ext_port);
            m->used = 0;
            continue;
        }
        active++;
    }
    nl_count_set(st->n, "nat.active", active);
    st->timer = nl_after(st->n, NL_SEC(1), nat_sweep, st);
}

static int parse_type(const char *s, int *type)
{
    if (!strcmp(s, "full") || !strcmp(s, "cone"))  *type = NT_FULL;
    else if (!strcmp(s, "restricted"))             *type = NT_RESTRICTED;
    else if (!strcmp(s, "port"))                   *type = NT_PORT;
    else if (!strcmp(s, "symmetric"))              *type = NT_SYMMETRIC;
    else return 0;
    return 1;
}

static int nat_forward(nat_state *st, const nl_args *a)
{
    const char *ps = nl_arg_str(a, "proto", "udp"), *to = nl_arg_str(a, "to", NULL), *colon;
    int proto = !strcmp(ps, "tcp") ? NL_PROTO_TCP : NL_PROTO_UDP, port = (int)nl_arg_int(a, "port", 0), i;
    char host[40];
    nl_ip ip;
    nat_map *m = NULL;

    colon = to ? strrchr(to, ':') : NULL;
    snprintf(host, sizeof(host), "%.*s", colon ? (int)(colon - to) : 39, to ? to : "");
    if (!to || port <= 0 || port > 65535 || !nl_resolve(host, &ip)) {
        nl_log(st->n, NL_L_NET, 1, 0, T_ERR "nat: %s", _("usage: nat forward proto=udp|tcp port=N to=HOST[:PORT]"));
        return -1;
    }
    if (map_in(st, proto, (uint16_t)port)) {
        nl_log(st->n, NL_L_NET, 1, 0, T_ERR "nat: %s %d", _("outside port already in use:"), port);
        return -1;
    }
    for (i = 0; i < NAT_MAX && !m; i++)
        if (!st->m[i].used)
            m = &st->m[i];
    if (!m)
        return -1;
    memset(m, 0, sizeof(*m));
    m->used = m->is_static = 1;
    m->proto = (uint8_t)proto;
    m->in_ip = ip;
    m->in_port = (uint16_t)(colon ? atoi(colon + 1) : port);
    m->ext_port = (uint16_t)port;
    m->created = m->last = nl_now();
    nl_log(st->n, NL_L_NET, 1, 0, T_INFO "NAT" T_RESET " %s %s %s:%d → %s:%u", _("port forwarding:"),
           proto_name(proto), nl_ip_str(st->n->ifc[st->out].ip), port, nl_ip_str(ip), m->in_port);
    return 0;
}

static int nat_run(nl_node *n, const nl_args *a)
{
    nat_state *st = nl_mod_get(n, &nat_mod);
    const char *sub = nl_arg_pos(a, 0), *type = nl_arg_str(a, "type", NULL);
    int fresh = !st;

    if (sub && !strcmp(sub, "forward")) {
        if (!st) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "nat: %s", _("start it first: run NODE nat out=IFACE"));
            return -1;
        }
        return nat_forward(st, a);
    }
    if (!st) {
        const char *out = nl_arg_str(a, "out", NULL);
        const nl_route *def = nl_route_lookup(n, NL_IP(198, 51, 100, 254));
        int ifx = out ? nl_if_find(n, out) : def ? def->ifx : -1;

        if (ifx < 0) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "nat: %s", _("which is the outside interface? (out=IFACE)"));
            return -1;
        }
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->n = n;
        st->out = ifx;
        st->type = NT_PORT;
        st->timeout = NL_SEC(30);
        st->next_port = NAT_PORT0;
        n->forward = 1;
        nl_mod_set(n, &nat_mod, st);
        nl_hook_add(n, NL_HOOK_PRE, nat_pre, st);
        nl_hook_add(n, NL_HOOK_POST, nat_post, st);
        st->timer = nl_after(n, NL_SEC(1), nat_sweep, st);
    }
    if (type && !parse_type(type, &st->type)) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "nat: %s '%s' (full, restricted, port, symmetric)", _("unknown type"), type);
        return -1;
    }
    st->timeout = nl_arg_time(a, "timeout", st->timeout);
    st->hairpin = (int)nl_arg_int(a, "hairpin", st->hairpin);
    st->preserve = (int)nl_arg_int(a, "preserve", st->preserve);
    nl_log(n, NL_L_NET, fresh ? 1 : 2, 0, T_INFO "NAT" T_RESET " %s %s (%s), %s, %s %s", _("on"),
           n->ifc[st->out].name, nl_ip_str(n->ifc[st->out].ip), nat_type_name(st->type), _("idle timeout"),
           nl_time_str(st->timeout));
    return 0;
}

static void nat_stop(nl_node *n)
{
    nat_state *st = nl_mod_get(n, &nat_mod);

    if (!st)
        return;
    nl_cancel(&st->timer);
    nl_hook_del(n, NL_HOOK_PRE, nat_pre, st);
    nl_hook_del(n, NL_HOOK_POST, nat_post, st);
    nl_mod_set(n, &nat_mod, NULL);
    free(st);
}

static void nat_crash(nl_node *n)
{
    nat_state *st = nl_mod_get(n, &nat_mod);
    int i;

    /* the table lives in memory: a reboot forgets every dynamic mapping */
    for (i = 0; st && i < NAT_MAX; i++)
        if (!st->m[i].is_static)
            st->m[i].used = 0;
    if (st)
        st->timer = 0;
}

static void nat_recover(nl_node *n)
{
    nat_state *st = nl_mod_get(n, &nat_mod);

    if (st)
        st->timer = nl_after(n, NL_SEC(1), nat_sweep, st);
}

static int nat_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    nat_state *st = nl_mod_get(n, &nat_mod);
    nl_ip ext = n->ifc[st->out].ip;
    int row = 0, i, shown = 0, total = 0;

    row += nl_ui_kv(x, y + row, w, _("outside"), "%s  %s", n->ifc[st->out].name, nl_ip_str(ext));
    row += nl_ui_kv(x, y + row, w, _("behaviour"), "%s%s", nat_type_name(st->type),
                    st->hairpin ? " + hairpin" : "");
    row += nl_ui_kv(x, y + row, w, _("translated / blocked"), "%.0f / %.0f", nl_count_get(n, "nat.translated"),
                    nl_count_get(n, "nat.blocked"));
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-4s %-21s %-7s %-21s %5s %s", "", _("inside"),
                  _("public"), _("remote"), _("idle"), _("out/in"));
    for (i = 0; i < NAT_MAX; i++) {
        const nat_map *m = &st->m[i];
        char in[32], rem[32];

        if (!m->used)
            continue;
        total++;
        if (row >= h)
            continue;
        snprintf(in, sizeof(in), "%s:%u", nl_ip_str(m->in_ip), m->in_port);
        if (m->is_static)
            snprintf(rem, sizeof(rem), "%s", _("(port forwarding)"));
        else
            snprintf(rem, sizeof(rem), "%s:%u%s", nl_ip_str(m->rem_ip), m->rem_port, m->nperm > 1 ? " +" : "");
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0),
                  T_INFO "%-4s" T_RESET " %-21s " T_WARN ":%-6u" T_RESET " %-21s " T_DIM "%4.0fs" T_RESET " %u/%u",
                  proto_name(m->proto), in, m->ext_port, rem, (double)(nl_now() - m->last) / 1e6, m->out, m->in);
        shown++;
        if (detail >= 3 && m->nperm > 1 && row < h) {
            char line[160];
            int k, len = 0;

            for (k = 0; k < m->nperm && len < (int)sizeof(line) - 30; k++)
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %s:%u", nl_ip_str(m->perm[k].ip),
                                m->perm[k].port);
            tui_print(x + 5, y + row++, w - 5, TS(C_DIM, C_DEFAULT, 0), "%s%s", _("contacted:"), line);
        }
    }
    if (shown < total && row > 0)
        tui_print(x, y + row - 1, w, TS(C_DIM, C_DEFAULT, 0), "… %d %s", total - shown + 1, _("more mappings"));
    if (!total && row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("(no mappings yet)"));
    return row;
}

static const nl_module nat_mod = {
    .name = "nat", .title = N_("NAT: many private hosts behind one public address"), .layer = NL_L_NET,
    .usage = N_("run ROUTER nat out=IFACE [type=full|restricted|port|symmetric] [timeout=30s]\n"
                "               [hairpin=1] [preserve=1]\n"
                "run ROUTER nat forward proto=udp|tcp port=N to=HOST[:PORT]\n"
                "  out is the interface towards the Internet; its address becomes the public one.\n"
                "  type chooses mapping and filtering (default port: port-restricted cone).\n"
                "  preserve=1 keeps the original source port when it is free.\n"
                "  forward publishes an inside server on a port of the public address."),
    .theory = N_("Private addresses (10/8, 172.16/12, 192.168/16) are not routed on the Internet: a packet "
                 "from 192.168.1.10 can reach a server, but the answer has nowhere to go. A NAT router fixes "
                 "this by rewriting the source of every outgoing packet to its own public address and to a "
                 "port it picks (NAPT), and by remembering the pair in a translation table: inside "
                 "address:port <-> public port. When the answer arrives on that public port the router looks "
                 "the entry up, rewrites the destination back and forwards the packet inside. IP and "
                 "transport checksums are recomputed, ICMP echo uses its identifier as the port, and ICMP "
                 "errors need the quoted packet translated too. Entries are created by outgoing traffic "
                 "and expire when idle, so nobody outside can open a conversation: that is why servers "
                 "behind a NAT need an explicit port forwarding rule.\n\n"
                 "NATs differ in two choices. Mapping: an endpoint-independent NAT reuses the same public "
                 "port for everything an inside socket sends, while a symmetric NAT allocates a new port "
                 "for each destination. Filtering: who may send to a mapped port? Anyone (full cone), only "
                 "addresses the inside host has contacted (address-restricted), only the exact "
                 "address:port pairs contacted (port-restricted), or only the one destination of the "
                 "mapping (symmetric). These choices decide whether two hosts that are both behind NATs "
                 "can talk directly: with UDP hole punching each learns the other's public endpoint from "
                 "a rendezvous server and both send at the same time, so each NAT sees an outgoing packet "
                 "first and lets the peer's packets in. It works with cone NATs and fails when a "
                 "symmetric NAT meets a symmetric or port-restricted one, because the port the peer was "
                 "told about is not the port actually used; then a relay (TURN) is needed."),
    .run = nat_run, .stop = nat_stop, .crash = nat_crash, .recover = nat_recover, .panel = nat_panel,
};
NL_MODULE(nat_mod)

/* ------------------------------------------------------------------ */
/* rendezvous: UDP hole punching                                      */
/* ------------------------------------------------------------------ */

#define RV_PORT  3478
#define RV_PEERS 16

typedef struct {
    nl_node *n;
    int      server, port, sport;
    /* server */
    struct { char name[16], peer[16]; nl_ip ip; uint16_t port; int used; } reg[RV_PEERS];
    /* client */
    nl_ip    srv, peer_ip;
    uint16_t peer_port;
    char     name[16], peer[16];
    int      have_peer, tries, sent, regs, rx, ok, done;
    nl_time  interval, wait;
    nl_ev    timer;
} rv_state;

static const nl_module rv_mod;

static void rv_send(rv_state *r, nl_ip ip, uint16_t port, const char *fmt, ...)
{
    char msg[96];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    nl_udp_send(r->n, (uint16_t)r->sport, ip, port, msg, (int)strlen(msg));
}

static void rv_server_rx(rv_state *r, const nl_udpmsg *m, const char *msg)
{
    char name[16], peer[16];
    int i, me = -1, other = -1;

    if (sscanf(msg, "REG %15s %15s", name, peer) != 2)
        return;
    for (i = 0; i < RV_PEERS; i++)
        if (r->reg[i].used && !strcmp(r->reg[i].name, name))
            me = i;
    for (i = 0; i < RV_PEERS && me < 0; i++)
        if (!r->reg[i].used)
            me = i;
    if (me < 0)
        return;
    if (!r->reg[me].used || r->reg[me].ip != m->src || r->reg[me].port != m->sport)
        nl_log(r->n, NL_L_APP, 1, m->pkt_id, T_FIN "rendezvous" T_RESET " %s %s %s:%u", name,
               _("registered: its packets arrive from"), nl_ip_str(m->src), m->sport);
    r->reg[me].used = 1;
    snprintf(r->reg[me].name, sizeof(r->reg[me].name), "%s", name);
    snprintf(r->reg[me].peer, sizeof(r->reg[me].peer), "%s", peer);
    r->reg[me].ip = m->src;
    r->reg[me].port = m->sport;
    for (i = 0; i < RV_PEERS; i++)
        if (r->reg[i].used && !strcmp(r->reg[i].name, peer) && !strcmp(r->reg[i].peer, name))
            other = i;
    if (other < 0)
        return;
    nl_log(r->n, NL_L_APP, 2, 0, T_FIN "rendezvous" T_RESET " %s %s ↔ %s", _("introducing"), name, peer);
    rv_send(r, r->reg[me].ip, r->reg[me].port, "PEER %s %s %u", peer, nl_ip_str(r->reg[other].ip),
            r->reg[other].port);
    rv_send(r, r->reg[other].ip, r->reg[other].port, "PEER %s %s %u", name, nl_ip_str(r->reg[me].ip),
            r->reg[me].port);
    nl_count(r->n, "rendezvous.introductions", 1);
}

static void rv_tick(void *ctx)
{
    rv_state *r = ctx;

    r->timer = 0;
    if (!r->have_peer) {
        if (r->regs >= 10) {
            nl_log(r->n, NL_L_APP, 1, 0, T_ERR "rendezvous %s: %s", r->name, _("no answer from the server"));
            r->done = 1;
            return;
        }
        r->regs++;
        nl_log(r->n, NL_L_APP, 2, 0, T_FIN "rendezvous" T_RESET " %s → %s: REG %s %s", r->name,
               nl_ip_str(r->srv), r->name, r->peer);
        rv_send(r, r->srv, (uint16_t)r->port, "REG %s %s", r->name, r->peer);
        r->timer = nl_after(r->n, NL_SEC(1), rv_tick, r);
        return;
    }
    if (r->sent >= r->tries) {
        r->done = 1;
        if (!r->ok)
            nl_log(r->n, NL_L_APP, 1, 0, T_ERR "rendezvous %s: %s %s — %s", r->name,
                   _("no direct path to"), r->peer, _("the NATs do not let the punch through (a relay is needed)"));
        return;
    }
    r->sent++;
    nl_log(r->n, NL_L_APP, 2, 0, T_FIN "rendezvous" T_RESET " %s → %s:%u PUNCH %d", r->name,
           nl_ip_str(r->peer_ip), r->peer_port, r->sent);
    rv_send(r, r->peer_ip, r->peer_port, "PUNCH %s %d", r->name, r->sent);
    nl_count(r->n, "punch.sent", 1);
    r->timer = nl_after(r->n, r->interval, rv_tick, r);
}

static void rv_client_rx(rv_state *r, const nl_udpmsg *m, const char *msg)
{
    char who[16], ip[20];
    unsigned port;
    int seq;

    if (sscanf(msg, "PEER %15s %19s %u", who, ip, &port) == 3) {
        nl_ip pip;

        if (r->have_peer || strcmp(who, r->peer) || !nl_parse_ip(ip, &pip))
            return;
        r->have_peer = 1;
        r->peer_ip = pip;
        r->peer_port = (uint16_t)port;
        nl_log(r->n, NL_L_APP, 1, m->pkt_id, T_FIN "rendezvous" T_RESET " %s: %s %s %s:%u — %s", r->name,
               r->peer, _("is reachable at"), ip, port, _("punching"));
        nl_cancel(&r->timer);
        r->timer = nl_after(r->n, r->wait, rv_tick, r);
        return;
    }
    if (sscanf(msg, "PUNCH %15s %d", who, &seq) != 2 && sscanf(msg, "ACK %15s %d", who, &seq) != 2)
        return;
    if (strcmp(who, r->peer))
        return;
    r->rx++;
    nl_count(r->n, "punch.rx", 1);
    if (r->have_peer && (m->src != r->peer_ip || m->sport != r->peer_port)) {
        nl_log(r->n, NL_L_APP, 1, m->pkt_id, T_WARN "rendezvous %s: %s %s:%u, %s %s:%u — %s", r->name,
               _("the peer reaches us from"), nl_ip_str(m->src), m->sport, _("not"), nl_ip_str(r->peer_ip),
               r->peer_port, _("its NAT is symmetric; answering there"));
        r->peer_ip = m->src;
        r->peer_port = m->sport;
    }
    if (!r->ok) {
        r->ok = 1;
        nl_count_set(r->n, "punch.ok", 1);
        nl_log(r->n, NL_L_APP, 1, m->pkt_id, T_OK "rendezvous %s: %s %s (%s:%u)", r->name,
               _("hole punched, direct path to"), r->peer, nl_ip_str(m->src), m->sport);
    }
    if (msg[0] == 'P')
        rv_send(r, m->src, m->sport, "ACK %s %d", r->name, seq);
}

static void rv_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    rv_state *r = ctx;
    char msg[96];

    (void)n;
    snprintf(msg, sizeof(msg), "%.*s", m->len > 95 ? 95 : m->len, (const char *)m->data);
    if (r->server)
        rv_server_rx(r, m, msg);
    else
        rv_client_rx(r, m, msg);
}

static void rv_stop(nl_node *n)
{
    rv_state *r = nl_mod_get(n, &rv_mod);

    if (!r)
        return;
    nl_cancel(&r->timer);
    nl_udp_unbind(n, (uint16_t)r->sport);
    nl_mod_set(n, &rv_mod, NULL);
    free(r);
}

static int rv_run(nl_node *n, const nl_args *a)
{
    const char *sub = nl_arg_pos(a, 0), *target = nl_arg_pos(a, 1);
    rv_state *r;
    nl_ip srv = 0;
    int server = sub && !strcmp(sub, "server");

    if (!sub || (!server && (strcmp(sub, "join") || !target || !nl_resolve(target, &srv) ||
                             !nl_arg_has(a, "name") || !nl_arg_has(a, "peer")))) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "rendezvous: %s",
               _("usage: rendezvous server | rendezvous join SERVER name=ME peer=OTHER"));
        return -1;
    }
    rv_stop(n);
    r = calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->n = n;
    r->server = server;
    r->port = (int)nl_arg_int(a, "port", RV_PORT);
    r->sport = nl_udp_bind(n, (uint16_t)(server ? r->port : 0), rv_rx, r);
    nl_mod_set(n, &rv_mod, r);
    if (server) {
        nl_log(n, NL_L_APP, 2, 0, T_FIN "rendezvous" T_RESET " %s %d", _("server listening on UDP port"), r->port);
        return 0;
    }
    r->srv = srv;
    snprintf(r->name, sizeof(r->name), "%s", nl_arg_str(a, "name", ""));
    snprintf(r->peer, sizeof(r->peer), "%s", nl_arg_str(a, "peer", ""));
    r->tries = (int)nl_arg_int(a, "tries", 6);
    r->interval = nl_arg_time(a, "interval", NL_MS(500));
    r->wait = nl_arg_time(a, "wait", 0);
    nl_count_set(n, "punch.ok", 0);
    rv_tick(r);
    return 0;
}

static void rv_recover(nl_node *n)
{
    rv_state *r = nl_mod_get(n, &rv_mod);

    if (r && !r->server && !r->done)
        r->timer = nl_after(n, NL_MS(100), rv_tick, r);
}

static int rv_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    rv_state *r = nl_mod_get(n, &rv_mod);
    int row = 0, i;

    (void)detail;
    if (r->server) {
        row += nl_ui_kv(x, y + row, w, _("UDP port"), "%d", r->port);
        for (i = 0; i < RV_PEERS && row < h; i++)
            if (r->reg[i].used)
                tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_BOLD "%-8s" T_RESET " %s:%u  " T_DIM "→ %s",
                          r->reg[i].name, nl_ip_str(r->reg[i].ip), r->reg[i].port, r->reg[i].peer);
        return row;
    }
    row += nl_ui_kv(x, y + row, w, _("me / peer"), "%s / %s", r->name, r->peer);
    row += nl_ui_kv(x, y + row, w, _("local UDP port"), "%d", r->sport);
    if (r->have_peer)
        row += nl_ui_kv(x, y + row, w, _("peer endpoint"), "%s:%u", nl_ip_str(r->peer_ip), r->peer_port);
    else
        row += nl_ui_kv(x, y + row, w, _("peer endpoint"), "%s", _("waiting for the server"));
    row += nl_ui_kv(x, y + row, w, _("punches sent / received"), "%d / %d", r->sent, r->rx);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, TA_BOLD), "%s%s",
                  r->ok ? T_OK "✓ " : r->done ? T_ERR "✗ " : T_DIM "… ",
                  r->ok ? _("direct path open") : r->done ? _("no direct path") : _("trying"));
    return row;
}

static const nl_module rv_mod = {
    .name = "rendezvous", .title = N_("UDP hole punching through NATs, with a rendezvous server"),
    .layer = NL_L_APP,
    .usage = N_("run NODE rendezvous server [port=3478]\n"
                "run NODE rendezvous join SERVER name=ME peer=OTHER [tries=6] [interval=500ms]\n"
                "                    [wait=0ms]\n"
                "  Both peers join; the server tells each the public endpoint of the other and\n"
                "  they send PUNCH datagrams to each other (after `wait`). punch.ok becomes 1 when\n"
                "  one arrives."),
    .theory = N_("Two hosts behind different NATs cannot simply send to each other: neither knows the "
                 "other's public address and port, and both NATs drop unsolicited packets. A rendezvous "
                 "server with a public address solves the first problem: each peer sends it a datagram, "
                 "and the server reads the source address and port as rewritten by the NAT (this is what "
                 "STUN does) and passes each endpoint to the other peer. Then both peers send datagrams "
                 "to the endpoint they were given, at the same time. The first packet of each side may "
                 "be dropped by the other NAT, but on its way out it opened a mapping and a filter entry "
                 "for the peer; the following packets pass in both directions. This relies on the NAT "
                 "reusing the same public port it showed to the server: symmetric NATs do not, so the "
                 "punch fails unless the other side accepts packets from any port. The lab reports "
                 "which combinations work."),
    .run = rv_run, .stop = rv_stop, .recover = rv_recover, .panel = rv_panel,
};
NL_MODULE(rv_mod)

static void dis_rv(nl_dissect *o, const uint8_t *d, int len, int off)
{
    char text[64];

    tui_payload(d, (size_t)len, text, sizeof(text), 48);
    nl_dis_field(o, "Rendezvous", "Message", off, len, "%s", text);
    nl_dis_summary(o, "PUNCH", 209, "%s", text);
}

NL_INIT(nat)
{
    nl_dissector_udp(RV_PORT, dis_rv);
}
