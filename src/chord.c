/*
 * chord.c - The Chord distributed hash table: consistent hashing on an
 *           identifier ring, finger tables, recursive lookups in O(log N)
 *           hops, join, stabilization, successor lists and replication.
 *
 * Identifiers have m bits (default 8): a node id is the hash of the node
 * name, a key id the hash of the key. The hash is 32-bit FNV-1a followed by
 * the MurmurHash3 finalizer, truncated to m bits (Chord proper uses SHA-1
 * and m = 160; with small m two nodes can collide: pick other names or more
 * bits).
 *
 * UDP port 7003. Wire format (big endian): type, hops, purpose, count,
 * request id (4), target id (4), node id (4), node address (4); then, by
 * type, a successor list (count x (id, address)) or key and value strings
 * (one length byte each).
 */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_PORT  7003
#define CH_MAXM  16
#define CH_SUCC  4
#define CH_KEYS  32
#define CH_PEND  24
#define CH_PROBE 24
#define CH_HDR   20

enum { C_FIND = 1, C_FOUND, C_GETPRED, C_PRED, C_NOTIFY, C_PING, C_PONG, C_PUT, C_GET, C_VALUE, C_STORE,
       C_MOVE };
#define C_LAST C_MOVE
enum { P_JOIN = 1, P_FINGER, P_LOOKUP, P_PUT, P_GET, P_GETWAIT };

static const char *const CH_NAMES[] = { "?", "FIND_SUCCESSOR", "FOUND", "GET_PREDECESSOR", "PREDECESSOR",
                                        "NOTIFY", "PING", "PONG", "PUT", "GET", "VALUE", "STORE", "MOVE" };

typedef struct { uint32_t id; nl_ip ip; } cref;

typedef struct { int used; uint32_t id; char key[20], val[28]; } ckey;

typedef struct {
    int      used, purpose, arg, tries;
    uint32_t reqid, target;
    nl_time  t;
    char     key[20], val[28];
} cpend;

typedef struct chord {
    nl_node *n;
    int      m, nsucc, joined, next_finger;
    uint32_t mask, id, next_req;
    nl_ip    ip, boot, contact;         /* contact: a ring member remembered across crashes */
    char     join[16];
    cref     succ[CH_SUCC], pred, fing[CH_MAXM];
    nl_time  stab;
    nl_ev    timer;
    ckey     keys[CH_KEYS];
    cpend    pend[CH_PEND];
    struct { cref r; int waiting; } probe[CH_PROBE];
    int      nprobe;
    unsigned lookups, found, lasthops, collisions;
    uint32_t repl_sig;                  /* neighbourhood for which the replicas were last pushed */
} chord;

static const nl_module chord_mod;

static nl_ip peer_addr(const nl_node *n, const char *name)
{
    const nl_node *p = nl_node_find(name);
    nl_ip ip = 0;
    int i, k;

    if (!p) {
        nl_resolve(name, &ip);
        return ip;
    }
    for (i = 0; i < n->nif; i++)
        for (k = 0; k < p->nif; k++)
            if (n->ifc[i].ip && p->ifc[k].ip && n->ifc[i].plen == p->ifc[k].plen &&
                ((n->ifc[i].ip ^ p->ifc[k].ip) & nl_mask(n->ifc[i].plen)) == 0)
                return p->ifc[k].ip;
    return nl_node_ip(p);
}

static uint32_t chd_hash(const char *s)
{
    uint32_t h = 2166136261u;

    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* x in (a, b] on the ring */
static int in_half_open(uint32_t x, uint32_t a, uint32_t b)
{
    if (a == b)
        return 1;
    return a < b ? (x > a && x <= b) : (x > a || x <= b);
}

/* x in (a, b) on the ring */
static int in_open(uint32_t x, uint32_t a, uint32_t b)
{
    if (a == b)
        return x != a;
    return a < b ? (x > a && x < b) : (x > a || x < b);
}

static const char *who(nl_ip ip)
{
    const nl_node *p = nl_node_by_ip(ip);
    return p ? p->name : nl_ip_str(ip);
}

static void publish(chord *c)
{
    int i, k = 0;

    for (i = 0; i < CH_KEYS; i++)
        k += c->keys[i].used;
    nl_count_set(c->n, "chord.keys", k);
    nl_count_set(c->n, "chord.lookups", c->lookups);
    nl_count_set(c->n, "chord.found", c->found);
    nl_count_set(c->n, "chord.answered", c->found + nl_count_get(c->n, "chord.notfound"));
    nl_count_set(c->n, "chord.collisions", c->collisions);
    nl_count_set(c->n, "chord.hops", c->lasthops);
    nl_count_set(c->n, "chord.joined", c->joined);
}

/* ---- messages ---- */

static int put_str(uint8_t *b, const char *s)
{
    int n = (int)strlen(s);

    b[0] = (uint8_t)n;
    memcpy(b + 1, s, (size_t)n);
    return n + 1;
}

static int get_str(const uint8_t *b, int len, char *out, size_t size)
{
    int n;

    out[0] = '\0';
    if (len < 1 || b[0] > len - 1)
        return len;
    n = b[0];
    snprintf(out, size, "%.*s", n, (const char *)b + 1);
    return n + 1;
}

static void chd_send(chord *c, nl_ip to, int type, int hops, int purpose, uint32_t reqid, uint32_t target,
                    cref node, const char *key, const char *val)
{
    uint8_t b[CH_HDR + 8 * CH_SUCC + 64];
    int len = CH_HDR, i;

    if (!to)
        return;
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)hops;
    b[2] = (uint8_t)purpose;
    b[3] = 0;
    nl_put32(b + 4, reqid);
    nl_put32(b + 8, target);
    nl_put32(b + 12, node.id);
    nl_put32(b + 16, node.ip);
    if (type == C_PRED) {
        for (i = 0; i < c->nsucc && c->succ[i].ip; i++, len += 8) {
            nl_put32(b + len, c->succ[i].id);
            nl_put32(b + len + 4, c->succ[i].ip);
        }
        b[3] = (uint8_t)i;
    }
    if (key)
        len += put_str(b + len, key);
    if (val)
        len += put_str(b + len, val);
    nl_udp_send(c->n, CH_PORT, to, CH_PORT, b, len);
}

static cref self_ref(const chord *c)
{
    cref r = { c->id, c->ip };
    return r;
}

/* ---- keys ---- */

static ckey *key_find(chord *c, const char *key, int create)
{
    ckey *freeslot = NULL;
    int i;

    for (i = 0; i < CH_KEYS; i++) {
        if (c->keys[i].used && strcmp(c->keys[i].key, key) == 0)
            return &c->keys[i];
        if (!c->keys[i].used && !freeslot)
            freeslot = &c->keys[i];
    }
    if (!create || !freeslot)
        return NULL;
    freeslot->used = 1;
    freeslot->id = chd_hash(key) & c->mask;
    snprintf(freeslot->key, sizeof(freeslot->key), "%s", key);
    return freeslot;
}

static void key_store(chord *c, const char *key, const char *val, int replicate)
{
    ckey *k = key_find(c, key, 1);
    int i;

    if (!k)
        return;
    snprintf(k->val, sizeof(k->val), "%s", val);
    nl_log(c->n, NL_L_DIST, replicate ? 1 : 2, 0, T_OK "chord" T_RESET " %s %s=%s " T_DIM "(id %u)",
           replicate ? _("stored") : _("stored a copy of"), key, val, k->id);
    /* r copies in all: here and on the next r - 1 successors */
    if (replicate)
        for (i = 0; i < c->nsucc - 1; i++)
            if (c->succ[i].ip && c->succ[i].ip != c->ip)
                chd_send(c, c->succ[i].ip, C_STORE, 0, 0, 0, k->id, self_ref(c), key, val);
    publish(c);
}

/* When the neighbourhood changes (a successor died or joined, or we
 * inherited the arc of a dead predecessor) the keys we own must again
 * exist on our first r - 1 successors. */
static void replicas_check(chord *c)
{
    uint32_t sig = c->pred.ip * 31u;
    int i, k, pushed = 0;

    if (!c->pred.ip || c->nsucc < 2)
        return;
    for (i = 0; i < c->nsucc - 1; i++)
        sig = sig * 131u + c->succ[i].ip;
    if (sig == c->repl_sig)
        return;
    c->repl_sig = sig;
    for (k = 0; k < CH_KEYS; k++) {
        if (!c->keys[k].used || !in_half_open(c->keys[k].id, c->pred.id, c->id))
            continue;
        for (i = 0; i < c->nsucc - 1; i++)
            if (c->succ[i].ip && c->succ[i].ip != c->ip)
                chd_send(c, c->succ[i].ip, C_STORE, 0, 0, 0, c->keys[k].id, self_ref(c), c->keys[k].key,
                         c->keys[k].val);
        pushed++;
    }
    if (pushed)
        nl_log(c->n, NL_L_DIST, 2, 0, T_INFO "chord" T_RESET " %s: %d %s", _("my neighbours changed"), pushed,
               _("keys copied again to my successors"));
}

static void chd_start(chord *c);

/* Two nodes hashed to the same identifier: this one moves to the next free
 * position and joins again. */
static void collision_move(chord *c, const char *with)
{
    c->collisions++;
    nl_log(c->n, NL_L_DIST, 1, 0, T_WARN "● chord" T_RESET " %s %u %s %s: %s %u (%s)", _("identifier"), c->id,
           _("is also the identifier of"), with, _("moving to"), (c->id + 1) & c->mask,
           _("with few bits two names can hash to the same point"));
    c->id = (c->id + 1) & c->mask;
    if (!c->boot)
        c->boot = c->contact;
    chd_start(c);
}

/* ---- routing ---- */

static cref closest_preceding(const chord *c, uint32_t target)
{
    cref best = self_ref(c);
    int i;

    for (i = c->m - 1; i >= 0; i--)
        if (c->fing[i].ip && in_open(c->fing[i].id, c->id, target) &&
            (best.ip == c->ip || in_open(best.id, c->id, c->fing[i].id)))
            best = c->fing[i];
    for (i = 0; i < c->nsucc; i++)
        if (c->succ[i].ip && in_open(c->succ[i].id, c->id, target) &&
            (best.ip == c->ip || in_open(best.id, c->id, c->succ[i].id)))
            best = c->succ[i];
    return best;
}

static void on_found(chord *c, uint32_t reqid, cref node, int hops);

static void handle_find(chord *c, uint32_t target, nl_ip origin, uint32_t reqid, int hops, int purpose)
{
    cref s = c->succ[0], answer, next;
    /* a node owns the ids between its predecessor and itself */
    int mine = target == c->id || (c->pred.ip && in_half_open(target, c->pred.id, c->id));

    if (mine || !s.ip || s.ip == c->ip || in_half_open(target, c->id, s.id)) {
        answer = mine || !s.ip ? self_ref(c) : s;
        if (origin == c->ip)
            on_found(c, reqid, answer, hops);
        else
            chd_send(c, origin, C_FOUND, hops, purpose, reqid, target, answer, NULL, NULL);
        return;
    }
    next = closest_preceding(c, target);
    if (next.ip == c->ip)
        next = s;
    nl_log(c->n, NL_L_DIST, purpose == P_FINGER ? 3 : 2, 0,
           T_INFO "chord" T_RESET " find_successor(%u) → %s %s (id %u)", target, _("forwarded to"),
           who(next.ip), next.id);
    {
        cref o = { 0, origin };
        chd_send(c, next.ip, C_FIND, hops + 1, purpose, reqid, target, o, NULL, NULL);
    }
}

static cpend *pend_new(chord *c, int purpose, int arg, uint32_t target, const char *key, const char *val)
{
    int i;

    for (i = 0; i < CH_PEND; i++)
        if (!c->pend[i].used) {
            cpend *p = &c->pend[i];
            memset(p, 0, sizeof(*p));
            p->used = 1;
            p->purpose = purpose;
            p->arg = arg;
            p->target = target;
            p->reqid = ++c->next_req;
            p->t = nl_now();
            snprintf(p->key, sizeof(p->key), "%s", key ? key : "");
            snprintf(p->val, sizeof(p->val), "%s", val ? val : "");
            return p;
        }
    return NULL;
}

static void start_find(chord *c, cpend *p)
{
    p->t = nl_now();
    if (p->purpose == P_JOIN) {
        cref o = { 0, c->ip };
        chd_send(c, c->boot, C_FIND, 1, P_JOIN, p->reqid, p->target, o, NULL, NULL);
    } else {
        handle_find(c, p->target, c->ip, p->reqid, 0, p->purpose);
    }
}

static void set_successor(chord *c, cref s)
{
    int i;

    if (c->succ[0].ip == s.ip)
        return;
    for (i = c->nsucc - 1; i > 0; i--)
        c->succ[i] = c->succ[i - 1];
    c->succ[0] = s;
    c->fing[0] = s;
    if (s.ip != c->ip)
        c->contact = s.ip;
    nl_log(c->n, NL_L_DIST, 2, 0, T_INFO "chord" T_RESET " %s: %s (id %u)", _("new successor"), who(s.ip), s.id);
}

static void on_value(chord *c, cpend *p, int ok, const char *val, nl_ip from)
{
    if (ok) {
        c->found++;
        nl_log(c->n, NL_L_DIST, 1, 0, T_OK "✓ chord get %s" T_RESET " = %s  " T_DIM "(%s %s)", p->key, val,
               _("answered by"), who(from));
    } else {
        nl_count(c->n, "chord.notfound", 1);
        nl_log(c->n, NL_L_DIST, 1, 0, T_ERR "✗ chord get %s" T_RESET ": %s %s", p->key, _("not found on"),
               who(from));
    }
    p->used = 0;
    publish(c);
}

static void on_found(chord *c, uint32_t reqid, cref node, int hops)
{
    cpend *p = NULL;
    int i;

    for (i = 0; i < CH_PEND; i++)
        if (c->pend[i].used && c->pend[i].reqid == reqid)
            p = &c->pend[i];
    if (!p)
        return;
    switch (p->purpose) {
    case P_JOIN:
        if (node.id == c->id && node.ip != c->ip) {
            p->used = 0;
            collision_move(c, who(node.ip));
            return;
        }
        set_successor(c, node);
        c->joined = 1;
        nl_log(c->n, NL_L_DIST, 1, 0, T_OK "● chord" T_RESET " %s (id %u): %s %s (id %u)", _("joined the ring"),
               c->id, _("successor"), who(node.ip), node.id);
        p->used = 0;
        break;
    case P_FINGER:
        if (c->fing[p->arg].ip != node.ip)
            nl_log(c->n, NL_L_DIST, 3, 0, T_INFO "chord" T_RESET " finger[%d] (%u) = %s (id %u)", p->arg,
                   p->target, who(node.ip), node.id);
        c->fing[p->arg] = node;
        p->used = 0;
        break;
    case P_LOOKUP:
    case P_PUT:
    case P_GET:
        c->lookups++;
        c->lasthops = (unsigned)hops;
        nl_sample(c->n, "lookup hops", hops);
        nl_log(c->n, NL_L_DIST, 1, 0, T_INFO "chord" T_RESET " %s %s (id %u) → %s %s (id %u), %d %s", _("key"),
               p->key, p->target, _("owner"), who(node.ip), node.id, hops, _("hops"));
        if (p->purpose == P_LOOKUP) {
            p->used = 0;
        } else if (p->purpose == P_PUT) {
            if (node.ip == c->ip)
                key_store(c, p->key, p->val, 1);
            else
                chd_send(c, node.ip, C_PUT, 0, 0, p->reqid, p->target, self_ref(c), p->key, p->val);
            p->used = 0;
        } else if (node.ip == c->ip) {
            ckey *k = key_find(c, p->key, 0);
            on_value(c, p, k != NULL, k ? k->val : "", c->ip);
        } else {
            p->purpose = P_GETWAIT;
            p->t = nl_now();
            chd_send(c, node.ip, C_GET, 0, 0, p->reqid, p->target, self_ref(c), p->key, NULL);
        }
        break;
    default:
        break;
    }
    publish(c);
}

/* Forget a node that stopped answering */
static void purge(chord *c, cref dead)
{
    int i, k, was_succ = c->succ[0].ip == dead.ip;

    nl_log(c->n, NL_L_DIST, 2, 0, T_WARN "chord" T_RESET " %s (id %u) %s", who(dead.ip), dead.id,
           _("does not answer: removed from my tables"));
    for (i = 0; i < c->m; i++)
        if (c->fing[i].ip == dead.ip)
            memset(&c->fing[i], 0, sizeof(cref));
    if (c->pred.ip == dead.ip)
        memset(&c->pred, 0, sizeof(cref));
    for (i = 0, k = 0; i < c->nsucc; i++)
        if (c->succ[i].ip && c->succ[i].ip != dead.ip)
            c->succ[k++] = c->succ[i];
    for (i = k; i < c->nsucc; i++)
        memset(&c->succ[i], 0, sizeof(cref));
    if (!c->succ[0].ip) {
        /* no successor left: any node we still know, else we are alone */
        c->succ[0] = self_ref(c);
        for (i = 0; i < c->m; i++)
            if (c->fing[i].ip && c->fing[i].ip != c->ip) {
                c->succ[0] = c->fing[i];
                break;
            }
        if (c->succ[0].ip == c->ip && c->pred.ip)
            c->succ[0] = c->pred;
    }
    if (was_succ)
        nl_log(c->n, NL_L_DIST, 1, 0, T_WARN "● chord" T_RESET " %s %s: %s %s (id %u)", who(dead.ip),
               _("failed"), _("my successor is now"), who(c->succ[0].ip), c->succ[0].id);
    c->fing[0] = c->succ[0];
}

static void probe_add(chord *c, cref r)
{
    int i;

    if (!r.ip || r.ip == c->ip)
        return;
    for (i = 0; i < c->nprobe; i++)
        if (c->probe[i].r.ip == r.ip)
            return;
    if (c->nprobe < CH_PROBE) {
        c->probe[c->nprobe].r = r;
        c->probe[c->nprobe].waiting = 1;
        c->nprobe++;
        chd_send(c, r.ip, C_PING, 0, 0, 0, 0, self_ref(c), NULL, NULL);
    }
}

static void chd_tick(void *ctx)
{
    chord *c = ctx;
    int i;

    c->timer = nl_after(c->n, c->stab, chd_tick, c);

    /* 1. who did not answer the last ping? */
    for (i = 0; i < c->nprobe; i++)
        if (c->probe[i].waiting)
            purge(c, c->probe[i].r);
    c->nprobe = 0;
    for (i = 0; i < c->nsucc; i++)
        probe_add(c, c->succ[i]);
    probe_add(c, c->pred);
    for (i = 0; i < c->m; i++)
        probe_add(c, c->fing[i]);

    /* 2. requests without an answer */
    for (i = 0; i < CH_PEND; i++) {
        cpend *p = &c->pend[i];
        if (!p->used || nl_now() - p->t < 2 * c->stab)
            continue;
        if (p->purpose == P_FINGER || ++p->tries > 4) {
            if (p->purpose != P_FINGER && p->purpose != P_JOIN) {
                nl_count(c->n, "chord.failed", 1);
                nl_log(c->n, NL_L_DIST, 1, 0, T_ERR "✗ chord" T_RESET " %s %s: %s", _("key"), p->key,
                       _("no answer, giving up"));
            }
            if (p->purpose == P_JOIN)
                p->tries = 0;           /* never give up joining */
            else
                p->used = 0;
            if (!p->used)
                continue;
        }
        nl_log(c->n, NL_L_DIST, 2, 0, T_WARN "chord" T_RESET " %s (%s %u)", _("no answer, trying again"),
               _("target"), p->target);
        if (p->purpose == P_GETWAIT)
            p->purpose = P_GET;
        start_find(c, p);
    }
    if (!c->joined)
        return;
    replicas_check(c);

    /* 3. stabilize: ask the successor for its predecessor */
    if (c->succ[0].ip && c->succ[0].ip != c->ip)
        chd_send(c, c->succ[0].ip, C_GETPRED, 0, 0, 0, 0, self_ref(c), NULL, NULL);

    /* 4. fix two fingers per round */
    for (i = 0; i < 2 && c->succ[0].ip != c->ip; i++) {
        cpend *p;

        c->next_finger = c->next_finger % (c->m - 1) + 1;
        p = pend_new(c, P_FINGER, c->next_finger, (c->id + (1u << c->next_finger)) & c->mask, NULL, NULL);
        if (p)
            start_find(c, p);
    }
    publish(c);
}

static void chd_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    chord *c = ctx;
    const uint8_t *d = m->data;
    int type, hops, purpose, cnt, off = CH_HDR, i;
    uint32_t reqid, target;
    cref node;
    char key[20], val[28];

    if (m->len < CH_HDR)
        return;
    type = d[0];
    hops = d[1];
    purpose = d[2];
    cnt = d[3];
    reqid = nl_get32(d + 4);
    target = nl_get32(d + 8);
    node.id = nl_get32(d + 12);
    node.ip = nl_get32(d + 16);
    if (type < 1 || type > C_LAST || off + cnt * 8 > m->len)
        return;
    if (type == C_MOVE) {
        if (c->id == target)
            collision_move(c, who(node.ip));
        return;
    }
    if ((type == C_NOTIFY || type == C_PING || type == C_PONG) && node.id == c->id && node.ip != c->ip &&
        c->ip > node.ip && c->joined) {
        collision_move(c, who(node.ip));
        return;
    }
    if (type != C_PING && type != C_PONG)
        nl_log(n, NL_L_DIST, 3, m->pkt_id, T_INFO "chord" T_RESET " ← %s %s %s", CH_NAMES[type], _("from"),
               who(m->src));

    switch (type) {
    case C_FIND:
        handle_find(c, target, node.ip, reqid, hops, purpose);
        break;
    case C_FOUND:
        on_found(c, reqid, node, hops);
        break;
    case C_GETPRED:
        chd_send(c, m->src, C_PRED, 0, 0, 0, 0, c->pred, NULL, NULL);
        break;
    case C_PRED: {
        cref s = c->succ[0];
        int k = 1;

        if (m->src != s.ip)
            break;
        if (node.ip && node.ip != c->ip && in_open(node.id, c->id, s.id)) {
            set_successor(c, node);
        } else {
            /* my successor list = my successor followed by its list */
            for (i = 0; i < cnt && k < c->nsucc; i++) {
                cref r = { nl_get32(d + off + 8 * i), nl_get32(d + off + 8 * i + 4) };
                if (r.ip != c->ip && r.ip != s.ip)
                    c->succ[k++] = r;
            }
            for (; k < c->nsucc; k++)
                memset(&c->succ[k], 0, sizeof(cref));
        }
        chd_send(c, c->succ[0].ip, C_NOTIFY, 0, 0, 0, 0, self_ref(c), NULL, NULL);
        break;
    }
    case C_NOTIFY:
        if (c->pred.ip && node.ip != c->pred.ip && node.id == c->pred.id) {
            /* two predecessors with one identifier: tell the one with the
             * higher address to move */
            cref keep = node.ip < c->pred.ip ? node : c->pred, move = node.ip < c->pred.ip ? c->pred : node;

            chd_send(c, move.ip, C_MOVE, 0, 0, 0, move.id, keep, NULL, NULL);
            c->pred = keep;
            break;
        }
        if (!c->pred.ip || in_open(node.id, c->pred.id, c->id)) {
            c->pred = node;
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_INFO "chord" T_RESET " %s: %s (id %u)", _("new predecessor"),
                   who(node.ip), node.id);
            /* hand over the keys the newcomer is now responsible for */
            for (i = 0; i < CH_KEYS; i++)
                if (c->keys[i].used && !in_half_open(c->keys[i].id, node.id, c->id))
                    chd_send(c, node.ip, C_STORE, 0, 0, 0, c->keys[i].id, self_ref(c), c->keys[i].key,
                            c->keys[i].val);
        }
        if (c->succ[0].ip == c->ip) {   /* the first node of the ring learns it is not alone */
            set_successor(c, node);
            c->contact = node.ip;
        }
        break;
    case C_PING:
        chd_send(c, m->src, C_PONG, 0, 0, 0, 0, self_ref(c), NULL, NULL);
        break;
    case C_PONG:
        for (i = 0; i < c->nprobe; i++)
            if (c->probe[i].r.ip == m->src)
                c->probe[i].waiting = 0;
        break;
    case C_PUT:
    case C_STORE:
        off += get_str(d + off, m->len - off, key, sizeof(key));
        get_str(d + off, m->len - off, val, sizeof(val));
        if (key[0])
            key_store(c, key, val, type == C_PUT);
        break;
    case C_GET: {
        ckey *k;

        get_str(d + off, m->len - off, key, sizeof(key));
        k = key_find(c, key, 0);
        nl_log(n, NL_L_DIST, 2, m->pkt_id, T_INFO "chord" T_RESET " get %s %s %s: %s", key, _("from"),
               who(m->src), k ? k->val : _("not here"));
        chd_send(c, m->src, C_VALUE, k ? 1 : 0, 0, reqid, target, self_ref(c), key, k ? k->val : "");
        break;
    }
    case C_VALUE:
        off += get_str(d + off, m->len - off, key, sizeof(key));
        get_str(d + off, m->len - off, val, sizeof(val));
        for (i = 0; i < CH_PEND; i++)
            if (c->pend[i].used && c->pend[i].reqid == reqid && c->pend[i].purpose == P_GETWAIT) {
                on_value(c, &c->pend[i], hops, val, m->src);
                break;
            }
        break;
    default:
        break;
    }
}

/* ---- module ---- */

static void chd_start(chord *c)
{
    cpend *p;

    memset(c->succ, 0, sizeof(c->succ));
    memset(c->fing, 0, sizeof(c->fing));
    memset(&c->pred, 0, sizeof(c->pred));
    memset(c->pend, 0, sizeof(c->pend));
    memset(c->keys, 0, sizeof(c->keys));
    c->nprobe = 0;
    c->next_finger = 0;
    if (c->boot) {
        c->joined = 0;
        p = pend_new(c, P_JOIN, 0, c->id, NULL, NULL);
        if (p)
            start_find(c, p);
    } else {
        c->joined = 1;
        c->succ[0] = self_ref(c);
        nl_log(c->n, NL_L_DIST, 1, 0, T_OK "● chord" T_RESET " %s (id %u, %d bit)", _("new ring created"), c->id,
               c->m);
    }
    nl_cancel(&c->timer);
    c->timer = nl_after(c->n, c->stab, chd_tick, c);
    publish(c);
}

static int chd_run(nl_node *n, const nl_args *a)
{
    chord *c = nl_mod_get(n, &chord_mod);
    const char *cmd = nl_arg_pos(a, 0), *key = nl_arg_str(a, "key", NULL);
    cpend *p;

    if (!c) {
        const char *join = nl_arg_str(a, "join", "");

        c = calloc(1, sizeof(*c));
        if (!c)
            return -1;
        c->n = n;
        c->m = (int)nl_arg_int(a, "bits", 8);
        if (c->m < 3) c->m = 3;
        if (c->m > CH_MAXM) c->m = CH_MAXM;
        c->mask = (1u << c->m) - 1;
        c->id = chd_hash(n->name) & c->mask;
        c->ip = nl_node_ip(n);
        c->nsucc = (int)nl_arg_int(a, "succlist", 3);
        if (c->nsucc < 1) c->nsucc = 1;
        if (c->nsucc > CH_SUCC) c->nsucc = CH_SUCC;
        c->stab = nl_arg_time(a, "stabilize", NL_MS(500));
        if (c->stab < NL_MS(50))
            c->stab = NL_MS(50);
        snprintf(c->join, sizeof(c->join), "%s", join);
        if (join[0] && strcmp(join, n->name) != 0) {
            c->boot = peer_addr(n, join);
            if (!c->boot) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "chord: %s '%s'", _("unknown node to join through"), join);
                free(c);
                return -1;
            }
        }
        nl_mod_set(n, &chord_mod, c);
        nl_udp_bind(n, CH_PORT, chd_rx, c);
        chd_start(c);
        if (!cmd)
            return 0;
    }
    if (!cmd)
        return 0;
    if (!key || (strcmp(cmd, "put") && strcmp(cmd, "get") && strcmp(cmd, "lookup"))) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "chord: %s", _("usage: put key=K value=V | get key=K | lookup key=K"));
        return -1;
    }
    p = pend_new(c, cmd[0] == 'p' ? P_PUT : cmd[0] == 'g' ? P_GET : P_LOOKUP, 0, chd_hash(key) & c->mask, key,
                 nl_arg_str(a, "value", ""));
    if (!p) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "chord: %s", _("too many requests in progress"));
        return -1;
    }
    nl_log(n, NL_L_DIST, 2, 0, T_INFO "chord" T_RESET " %s %s: hash → id %u", cmd, key, p->target);
    start_find(c, p);
    return 0;
}

static void chd_stop(nl_node *n)
{
    chord *c = nl_mod_get(n, &chord_mod);

    if (!c)
        return;
    nl_cancel(&c->timer);
    nl_udp_unbind(n, CH_PORT);
    nl_mod_set(n, &chord_mod, NULL);
    free(c);
}

static void chd_crash(nl_node *n)
{
    chord *c = nl_mod_get(n, &chord_mod);

    if (c) {
        c->timer = 0;
        c->joined = 0;
    }
}

/* Everything was in memory: the node rejoins as a new one, through its
 * original contact or, for the node that created the ring, the last
 * successor it knew. */
static void chd_recover(nl_node *n)
{
    chord *c = nl_mod_get(n, &chord_mod);

    if (!c)
        return;
    if (!c->boot)
        c->boot = c->contact;
    chd_start(c);
}

static int chd_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    chord *c = nl_mod_get(n, &chord_mod);
    int row = 0, i, span = w > 4 ? w - 1 : 1;
    nl_ip last = 0;

    row += nl_ui_kv(x, y + row, w, _("node id"), "%u / %u (%d bit)%s", c->id, c->mask + 1, c->m,
                    c->joined ? "" : _("  joining..."));
    row += nl_ui_kv(x, y + row, w, _("successor"), "%s (id %u)", who(c->succ[0].ip), c->succ[0].id);
    row += nl_ui_kv(x, y + row, w, _("predecessor"), "%s (id %u)", c->pred.ip ? who(c->pred.ip) : "—",
                    c->pred.id);

    /* the ring unrolled on a line: fingers, neighbours, this node */
    if (row + 2 < h) {
        tui_hline(x, y + row, w, TS(C_FAINT, C_DEFAULT, 0), "·");
        for (i = 0; i < c->m; i++)
            if (c->fing[i].ip)
                tui_put(x + (int)((uint64_t)c->fing[i].id * (uint64_t)span / (c->mask + 1)), y + row,
                        TS(C_INFO, C_DEFAULT, 0), "◆");
        if (c->pred.ip)
            tui_put(x + (int)((uint64_t)c->pred.id * (uint64_t)span / (c->mask + 1)), y + row,
                    TS(C_WARN, C_DEFAULT, TA_BOLD), "◀");
        if (c->succ[0].ip)
            tui_put(x + (int)((uint64_t)c->succ[0].id * (uint64_t)span / (c->mask + 1)), y + row,
                    TS(C_OK, C_DEFAULT, TA_BOLD), "▶");
        tui_put(x + (int)((uint64_t)c->id * (uint64_t)span / (c->mask + 1)), y + row,
                TS(C_BRIGHT, C_DEFAULT, TA_BOLD), "◉");
        row++;
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0),
                  "0 " T_TEXT "◉" T_DIM " %s  " T_OK "▶" T_DIM " %s  " T_WARN "◀" T_DIM " %s  " T_INFO "◆" T_DIM
                  " finger", _("me"), _("successor"), _("predecessor"));
    }
    if (row < h) {
        char line[160] = "";
        size_t len = 0;

        for (i = 1; i < c->nsucc && c->succ[i].ip; i++)
            len += (size_t)snprintf(line + len, sizeof(line) - len, "%s%s", i > 1 ? ", " : "",
                                    who(c->succ[i].ip));
        row += nl_ui_kv(x, y + row, w, _("successor list"), "%s", line[0] ? line : "—");
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, TA_BOLD), "%s", _("finger table"));
    for (i = 0; i < c->m && row < h; i++) {
        if (!c->fing[i].ip || c->fing[i].ip == last)
            continue;                   /* consecutive fingers often point to the same node */
        if (detail < 3)
            last = c->fing[i].ip;
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " %2d  " T_DIM "id+2^%-2d = %-5u" T_RESET " → %s (id %u)",
                  i, i, (c->id + (1u << i)) & c->mask, who(c->fing[i].ip), c->fing[i].id);
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, TA_BOLD), "%s", _("stored keys"));
    for (i = 0; i < CH_KEYS && row < h; i++)
        if (c->keys[i].used)
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), " %-12s = %-12s " T_DIM "id %u%s", c->keys[i].key,
                      c->keys[i].val, c->keys[i].id,
                      c->pred.ip && !in_half_open(c->keys[i].id, c->pred.id, c->id) ? "  (replica)" : "");
    return row;
}

static void dis_chord(nl_dissect *o, const uint8_t *d, int len, int off)
{
    if (len < CH_HDR || d[0] < 1 || d[0] > C_LAST)
        return;
    nl_dis_field(o, "Chord", "Message", off, 1, "%d (%s)", d[0], CH_NAMES[d[0]]);
    nl_dis_field(o, "Chord", "Hops", off + 1, 1, "%d", d[1]);
    nl_dis_field(o, "Chord", "Request id", off + 4, 4, "%u", nl_get32(d + 4));
    nl_dis_field(o, "Chord", "Target id", off + 8, 4, "%u", nl_get32(d + 8));
    nl_dis_field(o, "Chord", "Node", off + 12, 8, "id %u at %s", nl_get32(d + 12), nl_ip_str(nl_get32(d + 16)));
    if (d[0] == C_PRED)
        nl_dis_field(o, "Chord", "Successor list", off + CH_HDR, d[3] * 8, "%d entries", d[3]);
    else if (len > CH_HDR + 1)
        nl_dis_field(o, "Chord", "Key", off + CH_HDR, d[CH_HDR] + 1, "%.*s", d[CH_HDR],
                     (const char *)d + CH_HDR + 1);
    if (d[0] == C_FIND || d[0] == C_FOUND)
        nl_dis_summary(o, "CHORD", 209, "%s id=%u hops=%d", CH_NAMES[d[0]], nl_get32(d + 8), d[1]);
    else
        nl_dis_summary(o, "CHORD", 209, "%s", CH_NAMES[d[0]]);
}

static const nl_module chord_mod = {
    .name = "chord", .title = N_("Chord: a distributed hash table on an identifier ring"), .layer = NL_L_DIST,
    .usage = N_("run NODE chord [join=NODE] [bits=8] [stabilize=500ms] [succlist=3]\n"
                "run NODE chord put key=K value=V\n"
                "run NODE chord get key=K\n"
                "run NODE chord lookup key=K          only find the node responsible for K\n"
                "  Without join= the node creates a new ring; the others join through any member.\n"
                "  succlist=r: a node remembers its next r successors and every pair exists in r copies,\n"
                "  on its owner and on the owner's next r - 1 successors (r=1: no fallback, no replica)."),
    .theory = N_("A distributed hash table stores key-value pairs on many machines with no central "
                 "directory: any node can find the machine responsible for a key, and nodes may come "
                 "and go. Chord does it with consistent hashing: nodes and keys are hashed onto the "
                 "same circle of 2^m identifiers, and a key belongs to its successor, the first node "
                 "met going clockwise from the key's identifier. When a node joins or leaves, only "
                 "the keys of one arc of the circle change owner.\n\n"
                 "Knowing only its successor a node could find any key by walking the ring, in O(N) "
                 "hops. The finger table makes it logarithmic: entry i points to the successor of "
                 "id + 2^i, so a node knows its neighbourhood densely and the far side of the ring "
                 "sparsely. A lookup is forwarded to the closest finger preceding the target, which at "
                 "least halves the remaining distance: O(log N) hops with O(log N) state per node.\n\n"
                 "Correctness depends only on successor pointers; fingers are an optimisation. A new "
                 "node asks any member to look up its own identifier and adopts the answer as its "
                 "successor. Then every node periodically runs stabilize - ask your successor for its "
                 "predecessor, adopt it if it sits between the two of you, and notify your successor "
                 "that you exist - and fix_fingers, which refreshes one finger at a time with a "
                 "lookup. These background tasks repair the ring after joins without any global "
                 "coordination.\n\n"
                 "Failures are handled with a successor list: each node remembers its next r "
                 "successors, so that when the first one stops answering it falls back on the second, "
                 "and lookups route around dead fingers. Data survives because every pair exists in r "
                 "copies, on its owner and on the owner's next r - 1 successors: the node that inherits "
                 "the arc of a dead one already holds a copy, and whenever its neighbourhood changes a "
                 "node copies the keys it owns to its new successors, so that r copies exist again "
                 "before the next failure. With r = 1 there is a single copy and a single successor "
                 "pointer: every crash loses the keys of the dead node. Two names that hash to the same "
                 "identifier (likely with few bits) are detected and one of them moves to the next "
                 "position. Here lookups are recursive (each hop forwards the "
                 "query, the last one answers the origin directly), liveness is checked with pings "
                 "every stabilization round, and a node that restarts has lost its memory and joins "
                 "again as a new one."),
    .run = chd_run, .stop = chd_stop, .crash = chd_crash, .recover = chd_recover, .panel = chd_panel,
};
NL_MODULE(chord_mod)

NL_INIT(chord_dissector)
{
    nl_dissector_udp(CH_PORT, dis_chord);
}
