/*
 * lb.c - A layer-4 load balancer: one virtual address (VIP) in front of a
 *        pool of servers. Scheduling algorithms (round robin, weighted,
 *        least connections, hashing, consistent hashing, Maglev, power of
 *        two choices), a flow table for connection affinity, active health
 *        checks; plus the two small UDP programs the labs use to see where
 *        requests end up: `whoami` (server) and `reqgen` (client).
 *
 * Simplifications: health checks are ICMP echo (they see a dead server or
 * link, not a dead service); "connections" are flows of the 5-tuple with an
 * idle timeout, also for UDP; direct server return is not emulated (the
 * modes are nat and fullnat).
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LB_MAXB    16
#define LB_FLOWS   512
#define LB_VNODES  100
#define LB_MAXW    10
#define LB_MAGLEV  251                  /* prime; real deployments use 65537 */
#define LB_PROBE   0x4c42

enum { A_RR, A_WRR, A_LEAST, A_HASH, A_CONSISTENT, A_P2C, A_MAGLEV };
static const char *const ALGO_NAME[] = { "rr", "wrr", "leastconn", "hash", "consistent", "p2c", "maglev" };

typedef struct {
    char     name[16];
    nl_ip    ip;
    int      used, healthy, weight, fails, awaiting, cur, flows;
    unsigned hits;
} lb_backend;

typedef struct {
    int      used, closing, be;
    uint8_t  proto;
    nl_ip    cip;
    uint16_t cport, lbport;
    nl_time  last;
} lb_flow;

typedef struct { uint32_t h; int be; } ring_pt;

typedef struct {
    nl_node   *n;
    nl_ip      vip;
    int        vport;                   /* 0 = every port */
    int        algo, fullnat, affinity, key_src, fall;
    nl_time    check, flow_timeout;
    lb_backend b[LB_MAXB];
    lb_flow    f[LB_FLOWS];
    int        rr;
    uint16_t   next_port, round;
    ring_pt    ring[LB_MAXB * LB_VNODES * LB_MAXW];
    int        nring;
    int        maglev[LB_MAGLEV];
    nl_ev      t_check, t_sweep;
} lb_state;

static const nl_module lb_mod;

static uint32_t mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static uint32_t flow_hash(const lb_state *st, int proto, nl_ip cip, uint16_t cport)
{
    return mix32(mix32(cip) ^ ((uint32_t)(st->key_src ? 0 : cport) * 0x9e3779b1u) ^ ((uint32_t)proto << 24));
}

static int usable(const lb_state *st, int i)
{
    return i >= 0 && i < LB_MAXB && st->b[i].used && st->b[i].healthy;
}

static int hash_family(int algo)
{
    return algo == A_HASH || algo == A_CONSISTENT || algo == A_MAGLEV;
}

static int cmp_ring(const void *a, const void *b)
{
    uint32_t x = ((const ring_pt *)a)->h, y = ((const ring_pt *)b)->h;
    return x < y ? -1 : x > y;
}

/* Rebuild the consistent-hashing ring and the Maglev table */
static void rebuild(lb_state *st)
{
    uint32_t off[LB_MAXB], skip[LB_MAXB], next[LB_MAXB];
    int live[LB_MAXB], nlive = 0, i, v, filled = 0;

    st->nring = 0;
    for (i = 0; i < LB_MAXB; i++) {
        if (!usable(st, i))
            continue;
        live[nlive++] = i;
        for (v = 0; v < LB_VNODES * st->b[i].weight; v++) {
            st->ring[st->nring].h = mix32(st->b[i].ip * 2654435761u + (uint32_t)v * 40503u + 17);
            st->ring[st->nring++].be = i;
        }
    }
    qsort(st->ring, (size_t)st->nring, sizeof(ring_pt), cmp_ring);

    for (i = 0; i < LB_MAGLEV; i++)
        st->maglev[i] = -1;
    for (i = 0; i < nlive; i++) {
        off[i] = mix32(st->b[live[i]].ip ^ 0x1234567u) % LB_MAGLEV;
        skip[i] = mix32(st->b[live[i]].ip ^ 0x89abcdeu) % (LB_MAGLEV - 1) + 1;
        next[i] = 0;
    }
    while (nlive && filled < LB_MAGLEV)
        for (i = 0; i < nlive && filled < LB_MAGLEV; i++) {
            uint32_t c;

            /* each backend takes the next free slot of its own permutation */
            do
                c = (off[i] + next[i]++ * skip[i]) % LB_MAGLEV;
            while (st->maglev[c] >= 0);
            st->maglev[c] = live[i];
            filled++;
        }
}

/* Choice that depends only on the flow key and the backend set */
static int pick_stateless(const lb_state *st, uint32_t h)
{
    int i, n = 0, lo, hi;

    if (st->algo == A_MAGLEV)
        return st->maglev[h % LB_MAGLEV];
    if (st->algo == A_CONSISTENT) {
        if (!st->nring)
            return -1;
        lo = 0;
        hi = st->nring;
        while (lo < hi) {               /* first point clockwise from h */
            int mid = (lo + hi) / 2;
            if (st->ring[mid].h < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        return st->ring[lo == st->nring ? 0 : lo].be;
    }
    for (i = 0; i < LB_MAXB; i++)
        n += usable(st, i);
    if (!n)
        return -1;
    n = (int)(h % (uint32_t)n);
    for (i = 0; i < LB_MAXB; i++)
        if (usable(st, i) && n-- == 0)
            return i;
    return -1;
}

static int pick(lb_state *st, uint32_t h)
{
    int i, best = -1, total = 0, a, b, n = 0, live[LB_MAXB];

    if (hash_family(st->algo))
        return pick_stateless(st, h);
    for (i = 0; i < LB_MAXB; i++)
        if (usable(st, i))
            live[n++] = i;
    if (!n)
        return -1;
    switch (st->algo) {
    case A_WRR:                         /* smooth weighted round robin */
        for (i = 0; i < n; i++) {
            lb_backend *be = &st->b[live[i]];
            be->cur += be->weight;
            total += be->weight;
            if (best < 0 || be->cur > st->b[best].cur)
                best = live[i];
        }
        st->b[best].cur -= total;
        return best;
    case A_LEAST:
        for (i = 0; i < n; i++)
            if (best < 0 || st->b[live[i]].flows * st->b[best].weight < st->b[best].flows * st->b[live[i]].weight)
                best = live[i];
        return best;
    case A_P2C:
        /* two candidates drawn in proportion to their weight; the one with
         * fewer active flows per unit of weight wins */
        for (i = 0; i < n; i++)
            total += st->b[live[i]].weight > 0 ? st->b[live[i]].weight : 1;
        a = b = live[0];
        for (best = 0; best < 2; best++) {
            int r = (int)nl_rand((unsigned)total), c = live[n - 1];
            for (i = 0; i < n; i++) {
                r -= st->b[live[i]].weight > 0 ? st->b[live[i]].weight : 1;
                if (r < 0) {
                    c = live[i];
                    break;
                }
            }
            if (best == 0) a = c; else b = c;
        }
        return st->b[b].flows * (st->b[a].weight > 0 ? st->b[a].weight : 1) <
               st->b[a].flows * (st->b[b].weight > 0 ? st->b[b].weight : 1) ? b : a;
    default:
        for (i = 1; i <= LB_MAXB; i++)
            if (usable(st, (st->rr + i) % LB_MAXB)) {
                st->rr = (st->rr + i) % LB_MAXB;
                return st->rr;
            }
        return -1;
    }
}

static lb_flow *flow_find(lb_state *st, int proto, nl_ip cip, uint16_t cport)
{
    int i;

    for (i = 0; i < LB_FLOWS; i++)
        if (st->f[i].used && st->f[i].proto == proto && st->f[i].cip == cip && st->f[i].cport == cport)
            return &st->f[i];
    return NULL;
}

static lb_flow *flow_new(lb_state *st, int proto, nl_ip cip, uint16_t cport)
{
    lb_flow *f = NULL, *oldest = &st->f[0];
    int i;

    for (i = 0; i < LB_FLOWS && !f; i++) {
        if (!st->f[i].used)
            f = &st->f[i];
        else if (st->f[i].last < oldest->last)
            oldest = &st->f[i];
    }
    if (!f) {
        f = oldest;
        if (usable(st, f->be) && st->b[f->be].flows > 0)
            st->b[f->be].flows--;
    }
    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->be = -1;
    f->proto = (uint8_t)proto;
    f->cip = cip;
    f->cport = cport;
    f->lbport = st->next_port++;
    if (st->next_port < 30000)
        st->next_port = 30000;
    nl_count(st->n, "lb.flows", 1);
    return f;
}

static void flow_assign(lb_state *st, lb_flow *f, int be)
{
    if (f->be >= 0 && f->be < LB_MAXB && st->b[f->be].flows > 0)
        st->b[f->be].flows--;
    f->be = be;
    if (be >= 0)
        st->b[be].flows++;
}

static int backend_by_ip(const lb_state *st, nl_ip ip)
{
    int i;

    for (i = 0; i < LB_MAXB; i++)
        if (st->b[i].used && st->b[i].ip == ip)
            return i;
    return -1;
}

/* The pool changed (health, add, remove): rebuild the tables and see what
 * happens to the flows we know */
static void pool_changed(lb_state *st)
{
    int i, up = 0, tracked = 0, lost = 0, moved = 0;

    rebuild(st);
    for (i = 0; i < LB_MAXB; i++)
        up += usable(st, i);
    nl_count_set(st->n, "lb.backends_up", up);
    for (i = 0; i < LB_FLOWS; i++) {
        lb_flow *f = &st->f[i];

        if (!f->used)
            continue;
        tracked++;
        if (!usable(st, f->be)) {
            /* its server is gone: the next packet picks another one */
            if (f->be >= 0 && st->b[f->be].flows > 0)
                st->b[f->be].flows--;
            f->used = 0;
            lost++;
        } else if (hash_family(st->algo) &&
                   pick_stateless(st, flow_hash(st, f->proto, f->cip, f->cport)) != f->be) {
            moved++;
        }
    }
    nl_count(st->n, "lb.remapped", lost + moved);
    if (hash_family(st->algo) && tracked)
        nl_log(st->n, NL_L_TRANSPORT, 1, 0, T_INFO "LB" T_RESET " %s: %d/%d %s, %d/%d %s%s", ALGO_NAME[st->algo],
               lost, tracked, _("flows lost their server"), moved, tracked,
               _("more now hash to a different server"),
               moved && st->affinity ? _(" (the flow table keeps them where they are)") : "");
    else if (lost)
        nl_log(st->n, NL_L_TRANSPORT, 1, 0, T_INFO "LB" T_RESET " %s: %d/%d %s", ALGO_NAME[st->algo], lost, tracked,
               _("flows lost their server and will be scheduled again"));
}

static int lb_pre(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    lb_state *st = ctx;
    nl_ipview v;
    lb_flow *f;
    int be;

    (void)in_if;
    (void)out_if;
    if (!nl_ipv_parse(p, &v) || v.frag || (v.proto != NL_PROTO_UDP && v.proto != NL_PROTO_TCP))
        return NL_HOOK_OK;

    if (v.dst == st->vip && (!st->vport || v.dport == st->vport)) {         /* client -> VIP */
        uint32_t h = flow_hash(st, v.proto, v.src, v.sport);

        f = flow_find(st, v.proto, v.src, v.sport);
        if (!f)
            f = flow_new(st, v.proto, v.src, v.sport);
        be = f->be;
        if (!usable(st, be) || !st->affinity) {
            be = pick(st, h);
            if (be != f->be) {
                if (f->be >= 0)
                    nl_log(n, NL_L_TRANSPORT, 2, p->id, T_WARN "LB" T_RESET " %s %s:%u: %s → %s", _("flow"),
                           nl_ip_str(v.src), v.sport, st->b[f->be].name, be >= 0 ? st->b[be].name : "-");
                else if (be >= 0)
                    nl_log(n, NL_L_TRANSPORT, 2, p->id, T_INFO "LB" T_RESET " %s %s:%u → %s " T_DIM "(%s)",
                           _("new flow"), nl_ip_str(v.src), v.sport, st->b[be].name, ALGO_NAME[st->algo]);
                flow_assign(st, f, be);
            }
        }
        if (be < 0) {
            nl_log(n, NL_L_TRANSPORT, 2, p->id, T_ERR "LB" T_RESET " %s", _("no healthy server: request dropped"));
            nl_count(n, "lb.nobackend", 1);
            f->used = 0;
            return NL_HOOK_DROP;
        }
        f->last = nl_now();
        if (v.proto == NL_PROTO_TCP && (v.tcp_flags & 0x05))
            f->closing = 1;
        st->b[be].hits++;
        {
            char cname[28];
            snprintf(cname, sizeof(cname), "lb.hits.%s", st->b[be].name);
            nl_count(n, cname, 1);
        }
        v.dst = st->b[be].ip;
        if (st->fullnat) {
            const nl_route *r = nl_route_lookup(n, v.dst);
            if (r) {
                v.src = n->ifc[r->ifx].ip;
                v.sport = f->lbport;
            }
        }
        nl_ipv_commit(p, &v);
        return NL_HOOK_OK;
    }

    be = backend_by_ip(st, v.src);                                          /* server -> client */
    if (be < 0 || (st->vport && v.sport != st->vport))
        return NL_HOOK_OK;
    if (st->fullnat) {
        int i;

        if (!nl_ip_is_local(n, v.dst))
            return NL_HOOK_OK;
        for (i = 0, f = NULL; i < LB_FLOWS && !f; i++)
            if (st->f[i].used && st->f[i].proto == v.proto && st->f[i].lbport == v.dport && st->f[i].be == be)
                f = &st->f[i];
        if (!f)
            return NL_HOOK_OK;
        v.dst = f->cip;
        v.dport = f->cport;
    } else {
        f = flow_find(st, v.proto, v.dst, v.dport);
        if (st->affinity && (!f || f->be != be))
            return NL_HOOK_OK;          /* not an answer to something we balanced */
    }
    if (f) {
        f->last = nl_now();
        if (v.proto == NL_PROTO_TCP && (v.tcp_flags & 0x05))
            f->closing = 1;
    }
    v.src = st->vip;
    nl_ipv_commit(p, &v);
    return NL_HOOK_OK;
}

static void lb_sweep(void *ctx)
{
    lb_state *st = ctx;
    nl_time now = nl_now();
    int i, active = 0;

    for (i = 0; i < LB_FLOWS; i++) {
        lb_flow *f = &st->f[i];

        if (!f->used)
            continue;
        if (now - f->last > (f->closing ? NL_SEC(2) : st->flow_timeout)) {
            if (f->be >= 0 && st->b[f->be].flows > 0)
                st->b[f->be].flows--;
            f->used = 0;
        } else {
            active++;
        }
    }
    nl_sample(st->n, "active flows", active);
    st->t_sweep = nl_after(st->n, NL_SEC(1), lb_sweep, st);
}

static void lb_check(void *ctx)
{
    lb_state *st = ctx;
    int i, changed = 0;

    st->round++;
    for (i = 0; i < LB_MAXB; i++) {
        lb_backend *b = &st->b[i];

        if (!b->used)
            continue;
        if (b->awaiting && ++b->fails >= st->fall && b->healthy) {
            b->healthy = 0;
            changed = 1;
            nl_count(st->n, "lb.health_down", 1);
            nl_log(st->n, NL_L_TRANSPORT, 1, 0, T_ERR "LB" T_RESET " %s %s (%s): %s", _("server"), b->name,
                   nl_ip_str(b->ip), _("health check failed, taken out of the pool"));
        }
        b->awaiting = 1;
        nl_icmp_echo(st->n, b->ip, LB_PROBE, (uint16_t)(st->round * LB_MAXB + i), 0, 8, 0);
    }
    if (changed)
        pool_changed(st);
    st->t_check = nl_after(st->n, st->check, lb_check, st);
}

static void lb_icmp(nl_node *n, void *ctx, const nl_icmpmeta *m)
{
    lb_state *st = ctx;
    lb_backend *b;

    if (m->type != NL_ICMP_ECHO_REPLY || m->id != LB_PROBE)
        return;
    b = &st->b[m->seq % LB_MAXB];
    if (!b->used || b->ip != m->from)
        return;
    b->awaiting = 0;
    b->fails = 0;
    if (!b->healthy) {
        b->healthy = 1;
        nl_count(n, "lb.health_up", 1);
        nl_log(n, NL_L_TRANSPORT, 1, m->pkt_id, T_OK "LB" T_RESET " %s %s (%s): %s", _("server"), b->name,
               nl_ip_str(b->ip), _("healthy again, back in the pool"));
        pool_changed(st);
    }
}

static int parse_algo(const char *s)
{
    int i;

    for (i = 0; i < (int)(sizeof(ALGO_NAME) / sizeof(ALGO_NAME[0])); i++)
        if (!strcmp(s, ALGO_NAME[i]))
            return i;
    return -1;
}

static int backend_add(lb_state *st, const char *name, int weight)
{
    nl_ip ip;
    int i;

    if (!nl_resolve(name, &ip)) {
        nl_log(st->n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s '%s'", _("unknown server"), name);
        return -1;
    }
    if (backend_by_ip(st, ip) >= 0)
        return 0;
    for (i = 0; i < LB_MAXB; i++)
        if (!st->b[i].used) {
            lb_backend *b = &st->b[i];

            memset(b, 0, sizeof(*b));
            snprintf(b->name, sizeof(b->name), "%.11s", name);
            b->ip = ip;
            b->used = b->healthy = 1;
            b->weight = weight < 1 ? 1 : weight > LB_MAXW ? LB_MAXW : weight;
            return 0;
        }
    nl_log(st->n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s (%d)", _("too many servers"), LB_MAXB);
    return -1;
}

static void lb_stop(nl_node *n)
{
    lb_state *st = nl_mod_get(n, &lb_mod);

    if (!st)
        return;
    nl_cancel(&st->t_check);
    nl_cancel(&st->t_sweep);
    nl_hook_del(n, NL_HOOK_PRE, lb_pre, st);
    nl_icmp_unwatch(n, lb_icmp, st);
    nl_mod_set(n, &lb_mod, NULL);
    free(st);
}

static int lb_run(nl_node *n, const nl_args *a)
{
    lb_state *st = nl_mod_get(n, &lb_mod);
    const char *sub = nl_arg_pos(a, 0), *arg = nl_arg_pos(a, 1), *s;
    int algo, i;

    if (st && sub && (!strcmp(sub, "add") || !strcmp(sub, "remove"))) {
        nl_ip ip;

        if (!arg)
            return -1;
        if (sub[0] == 'a') {
            if (backend_add(st, arg, (int)nl_arg_int(a, "weight", 1)) != 0)
                return -1;
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_INFO "LB" T_RESET " %s %s", _("server added to the pool:"), arg);
        } else {
            i = nl_resolve(arg, &ip) ? backend_by_ip(st, ip) : -1;
            if (i < 0) {
                nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s '%s'", _("unknown server"), arg);
                return -1;
            }
            st->b[i].used = 0;
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_INFO "LB" T_RESET " %s %s", _("server removed from the pool:"), arg);
        }
        pool_changed(st);
        return 0;
    }
    if (!st) {
        const char *vip = nl_arg_str(a, "vip", NULL), *bl = nl_arg_str(a, "backends", NULL);
        const char *wl = nl_arg_str(a, "weights", "");
        char list[256], host[40];
        char *tok, *save = NULL;
        const char *colon;

        if (!bl) {
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s", _("usage: lb [vip=ADDR[:PORT]] backends=s1,s2,..."));
            return -1;
        }
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->n = n;
        st->affinity = 1;
        st->fall = 2;
        st->check = NL_SEC(1);
        st->flow_timeout = NL_SEC(10);
        st->next_port = 30000;
        st->vip = nl_node_ip(n);
        if (vip) {
            /* ADDR:PORT when the part after the last colon is a number */
            colon = strrchr(vip, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9') {
                st->vport = atoi(colon + 1);
                snprintf(host, sizeof(host), "%.*s", (int)(colon - vip), vip);
            } else {
                snprintf(host, sizeof(host), "%s", vip);
            }
            if (!nl_resolve(host, &st->vip)) {
                nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s '%s'", _("bad virtual address"), vip);
                free(st);
                return -1;
            }
        }
        snprintf(list, sizeof(list), "%s", bl);
        for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            int w = atoi(wl);

            if (backend_add(st, tok, w ? w : 1) != 0) {
                free(st);
                return -1;
            }
            wl = strchr(wl, ',') ? strchr(wl, ',') + 1 : "";
        }
        n->forward = 1;
        nl_mod_set(n, &lb_mod, st);
        nl_hook_add(n, NL_HOOK_PRE, lb_pre, st);
        nl_icmp_watch(n, lb_icmp, st);
        st->t_sweep = nl_after(n, NL_SEC(1), lb_sweep, st);
    }
    if ((s = nl_arg_str(a, "algo", NULL)) != NULL) {
        if ((algo = parse_algo(s)) < 0) {
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "lb: %s '%s' (rr, wrr, leastconn, hash, consistent, p2c, maglev)",
                   _("unknown algorithm"), s);
            return -1;
        }
        st->algo = algo;
    }
    if ((s = nl_arg_str(a, "mode", NULL)) != NULL)
        st->fullnat = !strcmp(s, "fullnat");
    if ((s = nl_arg_str(a, "key", NULL)) != NULL)
        st->key_src = !strcmp(s, "src");
    st->vport = (int)nl_arg_int(a, "port", st->vport);
    st->affinity = (int)nl_arg_int(a, "affinity", st->affinity);
    st->fall = (int)nl_arg_int(a, "fall", st->fall);
    st->flow_timeout = nl_arg_time(a, "flowtimeout", st->flow_timeout);
    st->check = nl_arg_time(a, "check", st->check);
    nl_cancel(&st->t_check);
    if (st->check > 0)
        st->t_check = nl_after(n, st->check, lb_check, st);
    rebuild(st);
    for (i = 0, algo = 0; i < LB_MAXB; i++)
        algo += usable(st, i);
    nl_count_set(n, "lb.backends_up", algo);
    nl_log(n, NL_L_TRANSPORT, 1, 0, T_INFO "LB" T_RESET " %s %s:%d → %d %s, %s, %s%s", _("virtual address"),
           nl_ip_str(st->vip), st->vport, algo, _("servers"), ALGO_NAME[st->algo],
           st->fullnat ? "fullnat" : "nat", st->affinity ? "" : _(", no flow table"));
    return 0;
}

static void lb_crash(nl_node *n)
{
    lb_state *st = nl_mod_get(n, &lb_mod);
    int i;

    if (!st)
        return;
    memset(st->f, 0, sizeof(st->f));    /* the flow table is volatile */
    for (i = 0; i < LB_MAXB; i++)
        st->b[i].flows = st->b[i].awaiting = st->b[i].fails = 0;
    st->t_check = st->t_sweep = 0;
}

static void lb_recover(nl_node *n)
{
    lb_state *st = nl_mod_get(n, &lb_mod);

    if (!st)
        return;
    st->t_sweep = nl_after(n, NL_SEC(1), lb_sweep, st);
    if (st->check > 0)
        st->t_check = nl_after(n, st->check, lb_check, st);
}

static int lb_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    lb_state *st = nl_mod_get(n, &lb_mod);
    unsigned total = 0, maxhits = 1;
    int row = 0, i, bw = w - 44;

    for (i = 0; i < LB_MAXB; i++)
        if (st->b[i].used) {
            total += st->b[i].hits;
            if (st->b[i].hits > maxhits)
                maxhits = st->b[i].hits;
        }
    row += nl_ui_kv(x, y + row, w, _("virtual address"), "%s:%d", nl_ip_str(st->vip), st->vport);
    row += nl_ui_kv(x, y + row, w, _("algorithm"), "%s%s  (%s)", ALGO_NAME[st->algo],
                    st->affinity ? "" : _(", no flow table"), st->fullnat ? "fullnat" : "nat");
    row += nl_ui_kv(x, y + row, w, _("requests / remapped flows"), "%u / %.0f", total,
                    nl_count_get(n, "lb.remapped"));
    for (i = 0; i < LB_MAXB && row < h; i++) {
        const lb_backend *b = &st->b[i];

        if (!b->used)
            continue;
        tui_print(x, y + row, w, TS(C_TEXT, C_DEFAULT, 0),
                  "%s" T_RESET " " T_BOLD "%-8s" T_RESET " %-15s " T_DIM "w" T_RESET "%-2d " T_DIM "%s" T_RESET
                  " %-3d %5u", b->healthy ? T_OK "●" : T_ERR "✗", b->name, nl_ip_str(b->ip), b->weight,
                  _("flows"), b->flows, b->hits);
        if (bw >= 6)
            tui_bar(x + 44, y + row, bw, (double)b->hits / maxhits, b->healthy ? C_ACK : C_ERR);
        row++;
    }
    if (detail >= 2 && h - row >= 5)
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "active flows"), C_INFO, _("active flows"));
    return detail >= 2 && h - row >= 5 ? h : row;
}

static const nl_module lb_mod = {
    .name = "lb", .title = N_("Load balancer: one address, many servers"), .layer = NL_L_TRANSPORT,
    .usage = N_("run NODE lb backends=s1,s2,... [vip=ADDR[:PORT]] [port=N]\n"
                "            [algo=rr|wrr|leastconn|hash|consistent|p2c|maglev] [weights=3,1,1]\n"
                "            [key=flow|src] [affinity=1] [mode=nat|fullnat] [check=1s] [fall=2]\n"
                "            [flowtimeout=10s]\n"
                "run NODE lb add SERVER [weight=N]   |   run NODE lb remove SERVER\n"
                "  vip defaults to the node's first address; port 0 balances every UDP/TCP port.\n"
                "  mode=nat rewrites only the destination: the servers must route their answers\n"
                "  back through the balancer. mode=fullnat also rewrites the source.\n"
                "  affinity=0 turns the flow table off: every packet is scheduled again.\n"
                "  key=src hashes the client address only (same client, same server).\n"
                "  check=0 disables health checks (ICMP echo; `fall` misses mark a server down)."),
    .theory = N_("A layer-4 load balancer owns a virtual IP address (VIP). Clients talk to the VIP; for "
                 "the first packet of each flow the balancer picks a real server and rewrites the "
                 "destination address, and for the answers it puts the VIP back as the source, so the "
                 "client never learns which server it reached. A flow table (client address and port -> "
                 "server) keeps every later packet of the connection on the same server: without it a TCP "
                 "connection would break as soon as the choice changed. Health checks probe the servers "
                 "and remove from the pool those that stop answering.\n\n"
                 "How to pick the server is the interesting part. Round robin cycles through the pool; "
                 "the weighted variant gives bigger servers proportionally more; least connections sends "
                 "the new flow to the server with fewest active ones, which adapts to long-lived or slow "
                 "connections; power of two choices samples two servers at random and takes the less "
                 "loaded one, which gets most of the benefit with almost no shared state. Hashing the "
                 "flow (or the client address) needs no state at all and lets several balancers make the "
                 "same choice independently, but hash mod N has a flaw: when N changes almost every flow "
                 "maps to a different server. Consistent hashing places servers (as many virtual nodes) "
                 "and flows on a ring and assigns each flow to the next server clockwise, so that adding "
                 "or removing one server moves only about 1/N of the flows. Maglev hashing reaches the "
                 "same goal with a precomputed lookup table filled from per-server permutations, giving "
                 "very even load and O(1) lookups. The lab counts how many flows are remapped by each "
                 "scheme when the pool changes."),
    .run = lb_run, .stop = lb_stop, .crash = lb_crash, .recover = lb_recover, .panel = lb_panel,
};
NL_MODULE(lb_mod)

/* ------------------------------------------------------------------ */
/* whoami: a UDP server that answers with its own name                */
/* ------------------------------------------------------------------ */

#define WHO_PORT 8000

typedef struct { int port; unsigned served; } who_state;
static const nl_module who_mod;

static void who_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    who_state *s = ctx;

    s->served++;
    nl_count(n, "whoami.served", 1);
    nl_log(n, NL_L_APP, 3, m->pkt_id, T_FIN "whoami" T_RESET " %s %s:%u", _("request from"), nl_ip_str(m->src),
           m->sport);
    nl_udp_send(n, (uint16_t)s->port, m->src, m->sport, n->name, (int)strlen(n->name));
}

static int who_run(nl_node *n, const nl_args *a)
{
    who_state *s = nl_mod_get(n, &who_mod);

    if (s)
        return 0;
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->port = (int)nl_arg_int(a, "port", WHO_PORT);
    nl_mod_set(n, &who_mod, s);
    nl_udp_bind(n, (uint16_t)s->port, who_rx, s);
    return 0;
}

static void who_stop(nl_node *n)
{
    who_state *s = nl_mod_get(n, &who_mod);

    if (!s)
        return;
    nl_udp_unbind(n, (uint16_t)s->port);
    nl_mod_set(n, &who_mod, NULL);
    free(s);
}

static int who_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    who_state *s = nl_mod_get(n, &who_mod);

    (void)h;
    (void)detail;
    nl_ui_kv(x, y, w, _("UDP port"), "%d", s->port);
    nl_ui_kv(x, y + 1, w, _("requests served"), "%u", s->served);
    return 2;
}

static const nl_module who_mod = {
    .name = "whoami", .title = N_("UDP server that answers with its own name"), .layer = NL_L_APP,
    .usage = N_("run NODE whoami [port=8000]\n"
                "  Put several behind a load balancer and ask with reqgen to see who answers."),
    .run = who_run, .stop = who_stop, .panel = who_panel,
};
NL_MODULE(who_mod)

/* ------------------------------------------------------------------ */
/* reqgen: many client flows, and who answered each                   */
/* ------------------------------------------------------------------ */

#define RG_FLOWS 40                     /* one UDP socket each */
#define RG_NAMES 16

typedef struct {
    nl_node *n;
    nl_ip    dst;
    int      port, count, flows, sent, answers, moved, done, life;
    double   rate;
    uint16_t sport[RG_FLOWS];
    int      used[RG_FLOWS];            /* requests sent on the current port of each flow */
    char     last[RG_FLOWS][16];
    struct { char name[16]; int hits; } tally[RG_NAMES];
    int      ntally;
    nl_ev    timer;
} rg_state;

static const nl_module rg_mod;

static void rg_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    rg_state *g = ctx;
    char name[16], cname[28];
    int i, k;

    snprintf(name, sizeof(name), "%.*s", m->len > 12 ? 12 : m->len, (const char *)m->data);
    for (i = 0; i < g->flows && g->sport[i] != m->dport; i++)
        ;
    if (i == g->flows)
        return;
    g->answers++;
    nl_count_set(n, "reqgen.answers", g->answers);
    for (k = 0; k < g->ntally && strcmp(g->tally[k].name, name); k++)
        ;
    if (k == g->ntally) {
        if (g->ntally >= RG_NAMES)
            return;
        snprintf(g->tally[g->ntally++].name, sizeof(g->tally[0].name), "%s", name);
        nl_count_set(n, "reqgen.backends_seen", g->ntally);
    }
    g->tally[k].hits++;
    snprintf(cname, sizeof(cname), "reqgen.%s", name);
    nl_count(n, cname, 1);
    if (g->last[i][0] && strcmp(g->last[i], name)) {
        g->moved++;
        nl_count_set(n, "reqgen.moved", g->moved);
        nl_log(n, NL_L_APP, 2, m->pkt_id, T_WARN "reqgen" T_RESET " %s %u: %s → %s", _("flow from port"),
               m->dport, g->last[i], name);
    }
    snprintf(g->last[i], sizeof(g->last[i]), "%s", name);
}

static void rg_tick(void *ctx)
{
    rg_state *g = ctx;
    char msg[32], line[220];
    int k, len = 0, i;

    g->timer = 0;
    if (g->sent >= g->count) {
        if (g->done)
            return;
        g->done = 1;
        for (k = 0; k < g->ntally && len < (int)sizeof(line) - 40; k++)
            len += snprintf(line + len, sizeof(line) - (size_t)len, "  %s %d (%.0f%%)", g->tally[k].name,
                            g->tally[k].hits, g->answers ? 100.0 * g->tally[k].hits / g->answers : 0.0);
        line[len] = '\0';
        nl_count_set(g->n, "reqgen.lost", g->sent - g->answers);
        nl_log(g->n, NL_L_APP, 1, 0, "%sreqgen: %d %s, %d %s, %d %s —" T_RESET "%s",
               g->answers == g->sent ? T_OK : T_WARN, g->sent, _("requests"), g->answers, _("answers"),
               g->moved, _("flows changed server"), line);
        return;
    }
    i = g->sent % g->flows;
    if (g->life && g->used[i] >= g->life) {
        /* this "connection" is over: a new one starts from a new port */
        nl_udp_unbind(g->n, g->sport[i]);
        g->sport[i] = (uint16_t)nl_udp_bind(g->n, 0, rg_rx, g);
        g->used[i] = 0;
        g->last[i][0] = '\0';
        nl_count(g->n, "reqgen.flows_opened", 1);
    }
    g->used[i]++;
    snprintf(msg, sizeof(msg), "REQ %d", g->sent + 1);
    nl_udp_send(g->n, g->sport[i], g->dst, (uint16_t)g->port, msg, (int)strlen(msg));
    g->sent++;
    nl_count_set(g->n, "reqgen.sent", g->sent);
    /* after the last request wait a little for the answers */
    g->timer = nl_after(g->n, g->sent >= g->count ? NL_SEC(1) : (nl_time)(1e6 / g->rate), rg_tick, g);
}

static void rg_stop(nl_node *n)
{
    rg_state *g = nl_mod_get(n, &rg_mod);
    int i;

    if (!g)
        return;
    nl_cancel(&g->timer);
    for (i = 0; i < g->flows; i++)
        nl_udp_unbind(n, g->sport[i]);
    nl_mod_set(n, &rg_mod, NULL);
    free(g);
}

static int rg_run(nl_node *n, const nl_args *a)
{
    const char *target = nl_arg_pos(a, 0);
    rg_state *g;
    nl_ip dst;
    int i;

    if (!target || !nl_resolve(target, &dst)) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "reqgen: %s '%s'", _("unknown destination"), target ? target : "");
        return -1;
    }
    rg_stop(n);
    g = calloc(1, sizeof(*g));
    if (!g)
        return -1;
    g->n = n;
    g->dst = dst;
    g->port = (int)nl_arg_int(a, "port", WHO_PORT);
    g->count = (int)nl_arg_int(a, "count", 200);
    g->flows = (int)nl_arg_int(a, "flows", 30);
    g->rate = nl_arg_dbl(a, "rate", 20);
    g->life = (int)nl_arg_int(a, "life", 0);
    if (g->flows < 1) g->flows = 1;
    if (g->flows > RG_FLOWS) g->flows = RG_FLOWS;
    if (g->rate < 0.1) g->rate = 0.1;
    for (i = 0; i < g->flows; i++) {
        g->sport[i] = (uint16_t)nl_udp_bind(n, 0, rg_rx, g);
        if (!g->sport[i]) {             /* socket table full */
            g->flows = i;
            break;
        }
    }
    if (!g->flows) {
        free(g);
        return -1;
    }
    nl_mod_set(n, &rg_mod, g);
    nl_log(n, NL_L_APP, 1, 0, T_INFO "reqgen" T_RESET " → %s:%d  %d %s, %d %s, %.4g/s", nl_ip_str(dst), g->port,
           g->count, _("requests"), g->flows, _("flows (source ports)"), g->rate);
    rg_tick(g);
    return 0;
}

static void rg_recover(nl_node *n)
{
    rg_state *g = nl_mod_get(n, &rg_mod);

    if (g && !g->done)
        g->timer = nl_after(n, NL_MS(100), rg_tick, g);
}

static int rg_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    rg_state *g = nl_mod_get(n, &rg_mod);
    int row = 0, k, maxhits = 1, bw = w - 24;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("requests / answers"), "%d/%d / %d", g->sent, g->count, g->answers);
    row += nl_ui_kv(x, y + row, w, _("flows / changed server"), "%d / %d", g->flows, g->moved);
    for (k = 0; k < g->ntally; k++)
        if (g->tally[k].hits > maxhits)
            maxhits = g->tally[k].hits;
    for (k = 0; k < g->ntally && row < h; k++) {
        tui_print(x, y + row, w, TS(C_TEXT, C_DEFAULT, 0), T_BOLD "%-8s" T_RESET " %5d " T_DIM "%3.0f%%",
                  g->tally[k].name, g->tally[k].hits, g->answers ? 100.0 * g->tally[k].hits / g->answers : 0.0);
        if (bw >= 6)
            tui_bar(x + 22, y + row, bw, (double)g->tally[k].hits / maxhits, C_DATA);
        row++;
    }
    return row;
}

static const nl_module rg_mod = {
    .name = "reqgen", .title = N_("Request generator: many client flows, and which server answered"),
    .layer = NL_L_APP,
    .usage = N_("run NODE reqgen DEST [port=8000] [count=200] [rate=20] [flows=30] [life=0]\n"
                "  Sends `count` UDP requests at `rate` per second, cycling over `flows` source\n"
                "  ports (at most 40), to whoami servers; tallies the names that answer and\n"
                "  counts the flows whose server changed. life=K ends a flow after K requests and\n"
                "  opens a new one from a fresh port (0 = flows last forever)."),
    .run = rg_run, .stop = rg_stop, .recover = rg_recover, .panel = rg_panel,
};
NL_MODULE(rg_mod)

static void dis_who(nl_dissect *o, const uint8_t *d, int len, int off)
{
    char text[40];

    tui_payload(d, (size_t)len, text, sizeof(text), 24);
    nl_dis_field(o, "Whoami", "Text", off, len, "%s", text);
    nl_dis_summary(o, "WHO", 177, "%s", text);
}

NL_INIT(lb)
{
    nl_dissector_udp(WHO_PORT, dis_who);
}
