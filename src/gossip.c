/*
 * gossip.c - Epidemic protocols: rumor spreading (push, pull, push-pull,
 *            with loss of interest) and SWIM membership with failure
 *            detection (direct and indirect probes, suspicion, incarnation
 *            numbers, infection-style dissemination of updates).
 *
 * One UDP port (7946) carries everything. Every message starts with
 *
 *   0      1      2             6             10            14
 *   +------+------+-------------+-------------+-------------+-----------
 *   | type | nupd | seq         | a           | b           | updates...
 *   +------+------+-------------+-------------+-------------+-----------
 *
 * followed by nupd piggybacked membership updates of 9 bytes each
 * (address(4) state(1) incarnation(4)) and then the type-specific payload:
 *
 *   Ping      seq, a = address the answer must be relayed to (0 = direct)
 *   Ack       seq, a = as in the ping, b = address of who answers
 *   PingReq   seq, a = address to probe on behalf of the sender
 *   Rumor     seq = hop count, a = rumor id, b = 1 when sent as an answer
 *             to a pull; payload: len(1) text
 *   RumorAck  a = rumor id, b = 1 when the receiver already knew it
 *   Pull      payload: n(1) then n rumor ids(4): "this is what I know"
 */
#include "nl_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G_PORT   7946
#define G_MAXM   32
#define G_MAXUPD 24
#define G_MAXRUM 8
#define G_HDR    14
#define G_PIGGY  6

enum { G_PING = 1, G_ACK, G_PINGREQ, G_RUMOR, G_RACK, G_PULL };
enum { ST_ALIVE, ST_SUSPECT, ST_DEAD };
enum { MODE_PUSH, MODE_PULL, MODE_PUSHPULL };

typedef struct { nl_ip ip; char name[16]; int state; uint32_t inc; nl_time since; } member;
typedef struct { nl_ip ip; uint8_t state; uint32_t inc; int left; } update;
typedef struct {
    uint32_t id;
    char     text[40];
    int      state;                     /* 1 infective, 2 removed (lost interest) */
    int      redundant, hop, origin, rounds, done;
} rumor;

typedef struct gossip {
    nl_node *n;
    nl_ip    self_ip;
    nl_ip    seed[G_MAXM];
    char     seed_name[G_MAXM][16];
    int      nseed;
    int      fanout, mode, stop_k, swim, helpers;
    nl_time  interval, period, ping_to, suspect_to;
    uint32_t inc;                       /* incarnation: persistent */

    member   m[G_MAXM];
    int      nm;
    update   upd[G_MAXUPD];
    int      nupd;
    rumor    r[G_MAXRUM];
    int      nr;
    uint32_t rumor_seq, seq;

    int      probe_idx, probe_active, probe_acked;
    nl_ip    probe_target;
    uint32_t probe_seq;
    nl_ev    round_timer, period_timer, ping_timer;
    unsigned msgs, refutations;
} gossip;

static const nl_module gossip_mod;

static const char *state_name(int s)
{
    return s == ST_ALIVE ? "alive" : s == ST_SUSPECT ? "suspect" : "dead";
}

static const char *mode_name(int m)
{
    return m == MODE_PUSH ? "push" : m == MODE_PULL ? "pull" : "push-pull";
}

static member *member_find(gossip *g, nl_ip ip)
{
    int i;

    for (i = 0; i < g->nm; i++)
        if (g->m[i].ip == ip)
            return &g->m[i];
    return NULL;
}

static member *member_add(gossip *g, nl_ip ip, const char *name, int state, uint32_t inc)
{
    member *m;
    const nl_node *who;

    if (g->nm >= G_MAXM)
        return NULL;
    m = &g->m[g->nm++];
    memset(m, 0, sizeof(*m));
    m->ip = ip;
    who = nl_node_by_ip(ip);
    snprintf(m->name, sizeof(m->name), "%s", name ? name : who ? who->name : nl_ip_str(ip));
    m->state = state;
    m->inc = inc;
    m->since = nl_now();
    return m;
}

static const char *ip_name(gossip *g, nl_ip ip)
{
    member *m = member_find(g, ip);
    const nl_node *who;

    if (m)
        return m->name;
    who = nl_node_by_ip(ip);
    return who ? who->name : nl_ip_str(ip);
}

static void counters(gossip *g)
{
    int i, alive = 1, suspect = 0, dead = 0, known = 0;

    for (i = 0; i < g->nm; i++) {
        alive += g->m[i].state == ST_ALIVE;
        suspect += g->m[i].state == ST_SUSPECT;
        dead += g->m[i].state == ST_DEAD;
    }
    for (i = 0; i < g->nr; i++)
        known += g->r[i].state != 0;
    if (g->swim) {
        nl_count_set(g->n, "swim.alive", alive);
        nl_count_set(g->n, "swim.suspects", suspect);
        nl_count_set(g->n, "swim.dead", dead);
        nl_count_set(g->n, "swim.incarnation", g->inc);
        nl_count_set(g->n, "swim.refutations", g->refutations);
    }
    nl_count_set(g->n, "gossip.infected", known);
    nl_count_set(g->n, "gossip.messages", g->msgs);
}

/* ---- dissemination buffer ---- */

static void queue_update(gossip *g, nl_ip ip, int state, uint32_t inc)
{
    update *u = NULL;
    int i, total = g->nm + 1, times = 3;

    while (total > 1) {                 /* lambda * ceil(log2(n)) retransmissions */
        total = (total + 1) / 2;
        times += 3;
    }
    for (i = 0; i < g->nupd; i++)
        if (g->upd[i].ip == ip)
            u = &g->upd[i];
    if (!u && g->nupd < G_MAXUPD)
        u = &g->upd[g->nupd++];
    if (!u) {                           /* full: replace the most disseminated one */
        u = &g->upd[0];
        for (i = 1; i < g->nupd; i++)
            if (g->upd[i].left < u->left)
                u = &g->upd[i];
    }
    u->ip = ip;
    u->state = (uint8_t)state;
    u->inc = inc;
    u->left = times;
}

static void gsend(gossip *g, nl_ip to, int type, uint32_t seq, uint32_t a, uint32_t b,
                  const uint8_t *payload, int plen)
{
    uint8_t buf[G_HDR + (G_PIGGY + 1) * 9 + 512];
    int n = G_HDR, cnt = 0, i;
    member *t = member_find(g, to);

    buf[0] = (uint8_t)type;
    nl_put32(buf + 2, seq);
    nl_put32(buf + 6, a);
    nl_put32(buf + 10, b);
    if (g->swim) {
        /* what we believe about the receiver itself comes first: a suspected
         * or dead member that is in fact alive must get the chance to refute */
        if (t && t->state != ST_ALIVE) {
            nl_put32(buf + n, t->ip);
            buf[n + 4] = (uint8_t)t->state;
            nl_put32(buf + n + 5, t->inc);
            n += 9;
            cnt++;
        }
        for (i = 0; i < g->nupd && cnt < G_PIGGY + 1; i++) {
            update *u = &g->upd[(i + (int)g->seq) % g->nupd];
            if (u->left <= 0)
                continue;
            u->left--;
            nl_put32(buf + n, u->ip);
            buf[n + 4] = u->state;
            nl_put32(buf + n + 5, u->inc);
            n += 9;
            cnt++;
        }
    }
    buf[1] = (uint8_t)cnt;
    if (plen > 0 && plen <= 512) {
        memcpy(buf + n, payload, (size_t)plen);
        n += plen;
    }
    g->msgs++;
    nl_udp_send(g->n, G_PORT, to, G_PORT, buf, n);
}

/* ---- membership ---- */

static void set_dead(gossip *g, member *m, int local_decision)
{
    const nl_node *who = nl_node_by_ip(m->ip);
    int wrong;

    m->state = ST_DEAD;
    m->since = nl_now();
    queue_update(g, m->ip, ST_DEAD, m->inc);
    /* the simulator knows the truth: was it a mistake? */
    wrong = who && who->local && who->up;

    nl_count(g->n, "swim.detected", 1);
    if (wrong)
        nl_count(g->n, "swim.false_positives", 1);
    if (local_decision)
        nl_log(g->n, NL_L_DIST, 1, 0, T_ERR "swim: %s %s" T_RESET " — %s%s%s", _("declares DEAD"), m->name,
               _("suspicion not refuted in time"), wrong ? T_WARN "  " : "",
               wrong ? _("(false positive: it is alive but unreachable)") : "");
    else
        nl_log(g->n, NL_L_DIST, wrong ? 2 : 1, 0, T_ERR "swim: %s %s", _("learns by gossip that this member is dead:"),
               m->name);
}

static void apply_update(gossip *g, nl_ip ip, int state, uint32_t inc)
{
    member *m;

    if (ip == g->self_ip) {
        if (state != ST_ALIVE && inc >= g->inc) {
            g->inc = inc + 1;
            g->refutations++;
            queue_update(g, g->self_ip, ST_ALIVE, g->inc);
            nl_log(g->n, NL_L_DIST, 1, 0, T_OK "swim: %s" T_RESET " — %s %u", state == ST_DEAD
                   ? _("someone thinks I am dead: refuting") : _("someone suspects me: refuting"),
                   _("announcing incarnation"), g->inc);
        }
        return;
    }
    m = member_find(g, ip);
    if (!m) {
        m = member_add(g, ip, NULL, state, inc);
        if (!m)
            return;
        nl_log(g->n, NL_L_DIST, 2, 0, T_CHAN "swim" T_RESET " %s %s (%s)", _("learns about member"), m->name,
               state_name(state));
        queue_update(g, ip, state, inc);
        return;
    }
    switch (state) {
    case ST_ALIVE:
        if (inc > m->inc) {
            if (m->state != ST_ALIVE)
                nl_log(g->n, NL_L_DIST, m->state == ST_DEAD ? 1 : 2, 0, T_OK "swim: %s %s" T_RESET " (%s %u)", m->name,
                       m->state == ST_DEAD ? _("is back among the living") : _("refuted the suspicion: alive"),
                       _("incarnation"), inc);
            m->state = ST_ALIVE;
            m->inc = inc;
            m->since = nl_now();
            queue_update(g, ip, ST_ALIVE, inc);
        }
        break;
    case ST_SUSPECT:
        if ((m->state == ST_ALIVE && inc >= m->inc) || (m->state == ST_SUSPECT && inc > m->inc)) {
            if (m->state == ST_ALIVE)
                nl_log(g->n, NL_L_DIST, 2, 0, T_CHAN "swim" T_RESET " %s %s", _("hears that this member is suspected:"),
                       m->name);
            m->state = ST_SUSPECT;
            m->inc = inc;
            m->since = nl_now();
            queue_update(g, ip, ST_SUSPECT, inc);
        }
        break;
    default:
        if (m->state != ST_DEAD && inc >= m->inc) {
            m->inc = inc;
            set_dead(g, m, 0);
        }
        break;
    }
}

static nl_ip random_peer(gossip *g, nl_ip except1, nl_ip except2)
{
    int cand[G_MAXM], nc = 0, i;

    for (i = 0; i < g->nm; i++)
        if (g->m[i].state != ST_DEAD && g->m[i].ip != except1 && g->m[i].ip != except2)
            cand[nc++] = i;
    return nc ? g->m[cand[nl_rand((unsigned)nc)]].ip : 0;
}

static void on_ping_timeout(void *ctx)
{
    gossip *g = ctx;
    nl_ip used[8];
    int k, nu = 0;

    g->ping_timer = 0;
    if (!g->probe_active || g->probe_acked)
        return;
    nl_log(g->n, NL_L_DIST, 2, 0, T_CHAN "swim" T_RESET " %s %s: %s", _("no ack from"),
           ip_name(g, g->probe_target), _("asking other members to probe it (ping-req)"));
    for (k = 0; k < g->helpers && k < 8; k++) {
        nl_ip h = 0;
        int tries, j, dup;

        for (tries = 0; tries < 8; tries++) {
            h = random_peer(g, g->probe_target, 0);
            for (dup = 0, j = 0; j < nu; j++)
                dup |= used[j] == h;
            if (h && !dup)
                break;
            h = 0;
        }
        if (!h)
            break;
        used[nu++] = h;
        gsend(g, h, G_PINGREQ, g->probe_seq, g->probe_target, 0, NULL, 0);
    }
}

static void on_period(void *ctx)
{
    gossip *g = ctx;
    member *m;
    int i, tries;

    g->period_timer = nl_after(g->n, g->period, on_period, g);
    /* 1. verdict on the previous probe */
    if (g->probe_active && !g->probe_acked) {
        m = member_find(g, g->probe_target);
        if (m && m->state == ST_ALIVE) {
            m->state = ST_SUSPECT;
            m->since = nl_now();
            queue_update(g, m->ip, ST_SUSPECT, m->inc);
            nl_log(g->n, NL_L_DIST, 1, 0, T_WARN "swim: %s %s" T_RESET " — %s", _("SUSPECTS"), m->name,
                   _("no ack, neither direct nor through helpers"));
        }
    }
    g->probe_active = 0;
    /* 2. suspicions that nobody refuted */
    for (i = 0; i < g->nm; i++)
        if (g->m[i].state == ST_SUSPECT && nl_now() - g->m[i].since >= g->suspect_to)
            set_dead(g, &g->m[i], 1);
    /* 3. next member, round robin; dead ones only now and then, in case
     *    they came back (or a partition healed) */
    for (tries = 0; tries < g->nm * 2 && g->nm > 0; tries++) {
        g->probe_idx = (g->probe_idx + 1) % g->nm;
        m = &g->m[g->probe_idx];
        if (m->state == ST_DEAD && nl_rand(4) != 0)
            continue;
        g->probe_active = 1;
        g->probe_acked = 0;
        g->probe_target = m->ip;
        g->probe_seq = ++g->seq;
        nl_log(g->n, NL_L_DIST, 3, 0, T_CHAN "swim" T_RESET " ping %s seq=%u", m->name, g->probe_seq);
        gsend(g, m->ip, G_PING, g->probe_seq, 0, 0, NULL, 0);
        nl_cancel(&g->ping_timer);
        g->ping_timer = nl_after(g->n, g->ping_to, on_ping_timeout, g);
        break;
    }
    counters(g);
}

/* ---- rumors ---- */

static rumor *rumor_find(gossip *g, uint32_t id)
{
    int i;

    for (i = 0; i < g->nr; i++)
        if (g->r[i].id == id)
            return &g->r[i];
    return NULL;
}

static void rumor_send(gossip *g, const rumor *r, nl_ip to, int pulled)
{
    uint8_t p[1 + sizeof(r->text)];
    int len = (int)strlen(r->text);

    p[0] = (uint8_t)len;
    memcpy(p + 1, r->text, (size_t)len);
    gsend(g, to, G_RUMOR, (uint32_t)r->hop + 1, r->id, (uint32_t)pulled, p, 1 + len);
}

/* How many live members know the rumor: seen from outside, by looking at
 * the other nodes simulated by this process */
static int rumor_census(gossip *g, uint32_t id, int *total)
{
    int i, know = 1;

    *total = 1;
    for (i = 0; i < g->nm; i++) {
        const nl_node *pn = nl_node_by_ip(g->m[i].ip);
        gossip *pg = pn && pn->local && pn->up ? nl_mod_get(pn, &gossip_mod) : NULL;

        if (!pg)
            continue;
        (*total)++;
        know += rumor_find(pg, id) != NULL;
    }
    return know;
}

static void on_round(void *ctx)
{
    gossip *g = ctx;
    int i, k;

    g->round_timer = nl_after(g->n, g->interval, on_round, g);
    for (i = 0; i < g->nr; i++) {
        rumor *r = &g->r[i];

        if (r->origin && !r->done) {
            int total, know = rumor_census(g, r->id, &total);

            nl_sample(g->n, "nodes that know the rumor", know);
            nl_log(g->n, NL_L_DIST, 1, 0, T_INFO "gossip: %s %d" T_RESET " — %d/%d %s «%s»", _("round"), r->rounds, know,
                   total, _("nodes know"), r->text);
            if (know >= total) {
                r->done = 1;
                nl_count_set(g->n, "gossip.rounds", r->rounds);
                nl_log(g->n, NL_L_DIST, 1, 0, T_OK "gossip: %s %d %s" T_RESET " (log2 N = %.1f)",
                       _("everybody knows after"), r->rounds, _("rounds"), log2((double)total));
            }
            r->rounds++;
        }
        if (r->state != 1 || g->mode == MODE_PULL)
            continue;
        for (k = 0; k < g->fanout; k++) {
            nl_ip to = random_peer(g, 0, 0);
            if (!to)
                break;
            nl_log(g->n, NL_L_DIST, 2, 0, T_CHAN "gossip" T_RESET " push «%s» → %s", r->text, ip_name(g, to));
            rumor_send(g, r, to, 0);
        }
    }
    if (g->mode != MODE_PUSH) {
        /* anti-entropy: tell a few peers what we know, they send what we miss */
        uint8_t p[1 + 4 * G_MAXRUM];

        p[0] = (uint8_t)g->nr;
        for (i = 0; i < g->nr; i++)
            nl_put32(p + 1 + 4 * i, g->r[i].id);
        for (k = 0; k < g->fanout; k++) {
            nl_ip to = random_peer(g, 0, 0);
            if (!to)
                break;
            nl_log(g->n, NL_L_DIST, 3, 0, T_CHAN "gossip" T_RESET " pull ← %s (%d %s)", ip_name(g, to), g->nr,
                   _("rumors known"));
            gsend(g, to, G_PULL, 0, 0, 0, p, 1 + 4 * g->nr);
        }
    }
    counters(g);
}

static rumor *rumor_learn(gossip *g, uint32_t id, const char *text, int hop, int origin)
{
    rumor *r;

    if (g->nr >= G_MAXRUM)
        return NULL;
    r = &g->r[g->nr++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    snprintf(r->text, sizeof(r->text), "%s", text);
    r->state = 1;
    r->hop = hop;
    r->origin = origin;
    counters(g);
    return r;
}

/* ---- receiving ---- */

static void gossip_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    gossip *g = ctx;
    const uint8_t *b = m->data, *p;
    int type, nupd, i, plen;
    uint32_t seq, a, bb;
    const char *from;

    if (m->len < G_HDR)
        return;
    type = b[0];
    nupd = b[1];
    seq = nl_get32(b + 2);
    a = nl_get32(b + 6);
    bb = nl_get32(b + 10);
    if (m->len < G_HDR + nupd * 9)
        return;
    if (g->swim) {
        if (!member_find(g, m->src) && m->src != g->self_ip) {
            member *nm = member_add(g, m->src, NULL, ST_ALIVE, 0);
            if (nm) {
                nl_log(n, NL_L_DIST, 1, m->pkt_id, T_OK "swim: %s %s", _("new member joined:"), nm->name);
                queue_update(g, m->src, ST_ALIVE, 0);
            }
        }
        for (i = 0; i < nupd; i++) {
            const uint8_t *u = b + G_HDR + 9 * i;
            apply_update(g, nl_get32(u), u[4], nl_get32(u + 5));
        }
    }
    p = b + G_HDR + nupd * 9;
    plen = m->len - G_HDR - nupd * 9;
    from = ip_name(g, m->src);

    switch (type) {
    case G_PING:
        nl_log(n, NL_L_DIST, 3, m->pkt_id, T_CHAN "swim" T_RESET " ← %s ping seq=%u%s → ack", from, seq,
               a ? " (indirect)" : "");
        gsend(g, m->src, G_ACK, seq, a, g->self_ip, NULL, 0);
        break;
    case G_PINGREQ:
        nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "swim" T_RESET " ← %s ping-req: %s %s", from,
               _("probing on its behalf"), ip_name(g, a));
        gsend(g, a, G_PING, seq, m->src, 0, NULL, 0);
        break;
    case G_ACK:
        if (a && a != g->self_ip) {
            gsend(g, a, G_ACK, seq, 0, bb, NULL, 0);     /* relay to who asked */
        } else if (g->probe_active && seq == g->probe_seq && bb == g->probe_target) {
            if (!g->probe_acked && m->src != g->probe_target)
                nl_log(n, NL_L_DIST, 2, m->pkt_id, T_OK "swim" T_RESET " %s %s %s: %s", ip_name(g, bb),
                       _("answered through"), from, _("alive after all (the direct path lost a packet)"));
            g->probe_acked = 1;
        }
        break;
    case G_RUMOR: {
        char text[40];
        rumor *r = rumor_find(g, a);
        int knew = r != NULL;

        if (plen < 1 || p[0] >= sizeof(text) || plen < 1 + p[0])
            return;
        snprintf(text, sizeof(text), "%.*s", p[0], (const char *)p + 1);
        if (!r) {
            r = rumor_learn(g, a, text, (int)seq, 0);
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_OK "gossip: %s" T_RESET " «%s» %s %s (%s %u)", _("NEW rumor"), text,
                   _("from"), from, bb ? _("pulled, hop") : _("pushed, hop"), seq);
        } else {
            nl_log(n, NL_L_DIST, 3, m->pkt_id, T_CHAN "gossip" T_RESET " «%s» %s %s: %s", text, _("from"), from,
                   _("already known"));
        }
        if (!bb)
            gsend(g, m->src, G_RACK, 0, a, (uint32_t)knew, NULL, 0);
        break;
    }
    case G_RACK: {
        rumor *r = rumor_find(g, a);

        if (r && bb && r->state == 1 && g->mode != MODE_PULL && ++r->redundant >= g->stop_k) {
            r->state = 2;
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "gossip" T_RESET " «%s»: %s (%d %s)", r->text,
                   _("loses interest, stops pushing"), r->redundant, _("peers already knew"));
        }
        break;
    }
    case G_PULL: {
        int cnt, k, j;

        if (plen < 1 || plen < 1 + 4 * p[0])
            return;
        cnt = p[0];
        for (k = 0; k < g->nr; k++) {
            int has = 0;
            for (j = 0; j < cnt; j++)
                has |= nl_get32(p + 1 + 4 * j) == g->r[k].id;
            if (!has) {
                nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "gossip" T_RESET " %s %s «%s»: %s", from, _("is missing"),
                       g->r[k].text, _("sending it (pull answer)"));
                rumor_send(g, &g->r[k], m->src, 1);
            }
        }
        break;
    }
    default:
        break;
    }
    counters(g);
}

/* ---- commands ---- */

static void arm(gossip *g)
{
    /* rounds are aligned on multiples of the interval, so that all nodes
     * act together and "round" means the same thing as in the analysis */
    g->round_timer = nl_after(g->n, g->interval - nl_now() % g->interval, on_round, g);
    if (g->swim)
        g->period_timer = nl_after(g->n, (nl_time)(nl_rand01() * (double)g->period), on_period, g);
}

static void reset_members(gossip *g)
{
    int i;

    g->nm = 0;
    for (i = 0; i < g->nseed; i++)
        if (g->seed[i] != g->self_ip)
            member_add(g, g->seed[i], g->seed_name[i], ST_ALIVE, 0);
    g->probe_idx = g->nm ? (int)nl_rand((unsigned)g->nm) : 0;
}

static int gossip_start(nl_node *n, const nl_args *a)
{
    const char *peers = nl_arg_str(a, "peers", NULL), *mode = nl_arg_str(a, "mode", "pushpull");
    char list[400];
    char *tok, *save = NULL;
    gossip *g;

    if (!peers) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "gossip: %s", _("the peer list is missing (peers=n1,n2,...)"));
        return -1;
    }
    g = calloc(1, sizeof(*g));
    if (!g)
        return -1;
    g->n = n;
    g->self_ip = nl_node_ip(n);
    snprintf(list, sizeof(list), "%s", peers);
    for (tok = strtok_r(list, ",", &save); tok && g->nseed < G_MAXM; tok = strtok_r(NULL, ",", &save)) {
        if (!nl_resolve(tok, &g->seed[g->nseed])) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "gossip: %s '%s'", _("unknown peer"), tok);
            free(g);
            return -1;
        }
        snprintf(g->seed_name[g->nseed++], sizeof(g->seed_name[0]), "%s", tok);
    }
    g->mode = !strcmp(mode, "push") ? MODE_PUSH : !strcmp(mode, "pull") ? MODE_PULL : MODE_PUSHPULL;
    g->fanout = (int)nl_arg_int(a, "fanout", 2);
    if (g->fanout < 1) g->fanout = 1;
    if (g->fanout > 8) g->fanout = 8;
    g->stop_k = (int)nl_arg_int(a, "stop", 3);
    g->interval = nl_arg_time(a, "interval", NL_MS(200));
    g->swim = (int)nl_arg_int(a, "swim", 0);
    g->period = nl_arg_time(a, "period", NL_SEC(1));
    g->ping_to = nl_arg_time(a, "timeout", g->period * 3 / 10);
    g->suspect_to = nl_arg_time(a, "suspect", g->period * 4);
    g->helpers = (int)nl_arg_int(a, "helpers", 3);
    reset_members(g);
    nl_mod_set(n, &gossip_mod, g);
    nl_udp_bind(n, G_PORT, gossip_rx, g);
    nl_log(n, NL_L_DIST, 2, 0, T_CHAN "gossip" T_RESET " %d %s, %s, fanout %d, %s %s%s", g->nm, _("peers"),
           mode_name(g->mode), g->fanout, _("round every"), nl_time_str(g->interval),
           g->swim ? ", SWIM" : "");
    arm(g);
    counters(g);
    return 0;
}

static int gossip_run(nl_node *n, const nl_args *a)
{
    gossip *g = nl_mod_get(n, &gossip_mod);
    const char *sub = nl_arg_pos(a, 0);

    if (!g)
        return gossip_start(n, a);
    if (sub && strcmp(sub, "rumor") == 0) {
        const char *text = nl_arg_str(a, "text", nl_arg_pos(a, 1) ? nl_arg_pos(a, 1) : "hello");
        rumor *r = rumor_learn(g, ((uint32_t)(n->idx + 1) << 16) | ++g->rumor_seq, text, 0, 1);

        if (!r) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "gossip: %s", _("too many rumors"));
            return -1;
        }
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "gossip: %s" T_RESET " «%s» (%s, fanout %d)", _("starts a rumor"), r->text,
               mode_name(g->mode), g->fanout);
        return 0;
    }
    if (sub && strcmp(sub, "members") == 0) {
        int i;
        for (i = 0; i < g->nm; i++)
            nl_log(n, NL_L_DIST, 1, 0, "  %-8s %s inc=%u", g->m[i].name, state_name(g->m[i].state), g->m[i].inc);
        return 0;
    }
    nl_log(n, NL_L_DIST, 1, 0, T_ERR "gossip: %s", _("usage: gossip rumor text=TEXT | gossip members"));
    return -1;
}

static void gossip_crash(nl_node *n)
{
    gossip *g = nl_mod_get(n, &gossip_mod);

    /* everything is in memory except the incarnation number */
    g->round_timer = g->period_timer = g->ping_timer = 0;
    g->nr = g->nupd = g->nm = 0;
    g->probe_active = 0;
    nl_count_set(n, "gossip.infected", 0);
}

static void gossip_recover(nl_node *n)
{
    gossip *g = nl_mod_get(n, &gossip_mod);

    reset_members(g);
    if (g->swim) {
        g->inc++;
        queue_update(g, g->self_ip, ST_ALIVE, g->inc);
        nl_log(n, NL_L_DIST, 1, 0, T_INFO "swim: %s %u", _("restarted: rejoining with incarnation"), g->inc);
    }
    arm(g);
    counters(g);
}

static void gossip_stop(nl_node *n)
{
    gossip *g = nl_mod_get(n, &gossip_mod);

    if (!g)
        return;
    nl_cancel(&g->round_timer);
    nl_cancel(&g->period_timer);
    nl_cancel(&g->ping_timer);
    nl_udp_unbind(n, G_PORT);
    nl_mod_set(n, &gossip_mod, NULL);
    free(g);
}

static int gossip_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    gossip *g = nl_mod_get(n, &gossip_mod);
    tstyle txt = TS(C_TEXT, C_DEFAULT, 0);
    int row = 0, i, col = 0;

    tui_print(x, y + row++, w, txt, T_DIM "%s" T_RESET " %s  " T_DIM "fanout" T_RESET " %d  " T_DIM "%s" T_RESET " %s  "
              T_DIM "%s" T_RESET " %u", _("mode"), mode_name(g->mode), g->fanout, _("round"), nl_time_str(g->interval),
              _("messages"), g->msgs);
    for (i = 0; i < g->nr && row < h; i++) {
        const rumor *r = &g->r[i];
        tui_print(x, y + row++, w, txt, "%s " T_RESET " «%s» " T_DIM "hop %d%s", r->state == 1 ? T_CHIP_WARN " infective"
                  : T_CHIP_OFF " removed  ", r->text, r->hop, r->origin ? " · origin" : "");
    }
    if (!g->nr && row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("(no rumor heard yet)"));
    if (g->swim && row < h) {
        tui_print(x, y + row++, w, txt, T_DIM "SWIM · %s" T_RESET " %u  " T_DIM "%s" T_RESET " %s  " T_DIM "%s" T_RESET " %s",
                  _("my incarnation"), g->inc, _("period"), nl_time_str(g->period), _("suspicion"),
                  nl_time_str(g->suspect_to));
        for (i = 0; i < g->nm && row < h; i++) {
            const member *m = &g->m[i];
            int cw = detail >= 2 ? 22 : 12;

            if (col + cw > w) {
                col = 0;
                row++;
                if (row >= h)
                    break;
            }
            tui_print(x + col, y + row, cw - 1, txt, "%s%s" T_RESET " %s" T_DIM "%s",
                      m->state == ST_ALIVE ? T_OK : m->state == ST_SUSPECT ? T_WARN : T_ERR,
                      m->state == ST_ALIVE ? "●" : m->state == ST_SUSPECT ? "?" : "✗", m->name,
                      detail >= 2 ? (m->state == ST_ALIVE ? " alive" : m->state == ST_SUSPECT ? " suspect" : " dead")
                      : "");
            col += cw;
        }
        row++;
    }
    if (h - row >= 5 && nl_series_find(n, "nodes that know the rumor")) {
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "nodes that know the rumor"), C_WARN,
                    _("nodes that know the rumor"));
        return h;
    }
    return row;
}

/* ---- dissector ---- */

static void dis_gossip(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const char *const names[] = { "?", "Ping", "Ack", "PingReq", "Rumor", "RumorAck", "Pull" };
    int type, nupd, i, p;
    uint32_t seq, a, b;

    if (len < G_HDR)
        return;
    type = d[0];
    nupd = d[1];
    seq = nl_get32(d + 2);
    a = nl_get32(d + 6);
    b = nl_get32(d + 10);
    nl_dis_field(o, "Gossip", "Type", off, 1, "%d (%s)", type, type >= 1 && type <= 6 ? names[type] : "?");
    nl_dis_field(o, "Gossip", "Piggybacked updates", off + 1, 1, "%d", nupd);
    for (i = 0; i < nupd && G_HDR + 9 * (i + 1) <= len; i++) {
        const uint8_t *u = d + G_HDR + 9 * i;
        nl_dis_field(o, "Gossip", "Update", off + G_HDR + 9 * i, 9, "%s is %s (incarnation %u)",
                     nl_ip_str(nl_get32(u)), state_name(u[4]), nl_get32(u + 5));
    }
    p = G_HDR + 9 * nupd;
    switch (type) {
    case G_PING:
        nl_dis_field(o, "Gossip", "Sequence", off + 2, 4, "%u", seq);
        if (a)
            nl_dis_field(o, "Gossip", "On behalf of", off + 6, 4, "%s", nl_ip_str(a));
        nl_dis_summary(o, "SWIM", 209, "Ping seq=%u%s%s +%d upd", seq, a ? " for " : "", a ? nl_ip_str(a) : "", nupd);
        break;
    case G_ACK:
        nl_dis_field(o, "Gossip", "Sequence", off + 2, 4, "%u", seq);
        nl_dis_field(o, "Gossip", "Alive member", off + 10, 4, "%s", nl_ip_str(b));
        nl_dis_summary(o, "SWIM", 209, "Ack seq=%u from %s%s +%d upd", seq, nl_ip_str(b), a ? " (to relay)" : "", nupd);
        break;
    case G_PINGREQ:
        nl_dis_field(o, "Gossip", "Sequence", off + 2, 4, "%u", seq);
        nl_dis_field(o, "Gossip", "Target", off + 6, 4, "%s", nl_ip_str(a));
        nl_dis_summary(o, "SWIM", 209, "PingReq seq=%u: please probe %s", seq, nl_ip_str(a));
        break;
    case G_RUMOR:
        if (p < len && p + 1 + d[p] <= len) {
            nl_dis_field(o, "Gossip", "Rumor id", off + 6, 4, "0x%08x", a);
            nl_dis_field(o, "Gossip", "Hop", off + 2, 4, "%u", seq);
            nl_dis_field(o, "Gossip", "Text", off + p, 1 + d[p], "%.*s", d[p], (const char *)d + p + 1);
            nl_dis_summary(o, "GOSSIP", 209, "Rumor «%.*s» hop=%u (%s)", d[p], (const char *)d + p + 1, seq,
                           b ? "pull answer" : "push");
        }
        break;
    case G_RACK:
        nl_dis_field(o, "Gossip", "Rumor id", off + 6, 4, "0x%08x", a);
        nl_dis_field(o, "Gossip", "Already known", off + 10, 4, "%s", b ? "yes" : "no");
        nl_dis_summary(o, "GOSSIP", 209, "RumorAck %s", b ? "already knew it" : "new to me");
        break;
    case G_PULL:
        if (p < len)
            nl_dis_field(o, "Gossip", "Rumors known", off + p, 1 + 4 * d[p], "%d", d[p]);
        nl_dis_summary(o, "GOSSIP", 209, "Pull: I know %d rumors, send what I miss", p < len ? d[p] : 0);
        break;
    default:
        nl_dis_summary(o, "GOSSIP", 209, "type %d", type);
        break;
    }
}

static const nl_module gossip_mod = {
    .name = "gossip", .title = N_("Gossip — epidemic dissemination and SWIM failure detection"),
    .layer = NL_L_DIST,
    .usage = N_("run NODE gossip peers=n1,n2,... [mode=push|pull|pushpull] [fanout=2] [interval=200ms] [stop=3]\n"
                "                [swim=1] [period=1s] [timeout=300ms] [suspect=4s] [helpers=3]\n"
                "  Start it on every node; peers are the members known at start (more are learnt).\n"
                "  stop: a node stops pushing a rumor after that many peers answered \"I knew it\".\n"
                "  swim=1 adds membership and failure detection.\n"
                "run NODE gossip rumor text=TEXT   start spreading a rumor from NODE\n"
                "run NODE gossip members           print the membership view of NODE"),
    .theory = N_("Gossip (epidemic) protocols spread information the way a rumor or a virus spreads: at "
                 "every round each node talks to a few peers chosen at random. Nobody coordinates and "
                 "nobody knows the whole topology, yet the number of informed nodes roughly doubles each "
                 "round, so N nodes are reached in O(log N) rounds with O(N log N) messages. Random choice "
                 "is also what makes the scheme robust: lost messages and crashed nodes only slow it "
                 "down a little, there is no single point whose failure stops it.\n\n"
                 "Three styles exist. In push, a node that knows the rumor sends it to random peers: very "
                 "fast at the start, wasteful at the end, when almost every push hits a node that already "
                 "knows. In pull, every node periodically asks random peers what is new: slow to start "
                 "(the few informed nodes are rarely asked) but, once half the nodes know, the uninformed "
                 "fraction squares at every round. Push-pull combines them and needs only O(N log log N) "
                 "messages. A pushing node must also decide when to stop: here it loses interest after a "
                 "number of peers answered that they already knew (the feedback/counter variant of Demers "
                 "et al.). Stopping early leaves a small residue of nodes that never hear the rumor; this "
                 "is why practical systems pair rumor mongering with anti-entropy, a periodic exchange of "
                 "digests that repairs any difference, heals partitions and updates nodes that were down.\n\n"
                 "SWIM applies the same idea to membership: who is in the group and who has failed. "
                 "Classic heartbeating to everybody costs O(N²) messages; in SWIM every node probes one "
                 "random member per period, so the load per node is constant. If the ping is not "
                 "acknowledged in time the prober does not accuse yet: it asks k other members to ping the "
                 "target for it (ping-req). Only when direct and indirect probes all fail does the member "
                 "become suspect, and only if the suspicion survives a timeout is it declared dead. The "
                 "indirect probe filters out failures of a single path; the suspicion period filters out "
                 "slow or briefly overloaded nodes.\n\n"
                 "A suspected node that is alive defends itself: each member has an incarnation number "
                 "that only the member itself may increment. Hearing that it is suspected at incarnation "
                 "i, it announces alive at i+1, which overrides the suspicion everywhere; a restarted node "
                 "rejoins the same way. Membership updates (alive, suspect, dead) are not broadcast: they "
                 "are piggybacked on the pings and acks already flowing, each a logarithmic number of "
                 "times, so news spreads epidemically at no extra cost. Detection time is constant in "
                 "expectation and independent of the group size, and false positives can be traded against "
                 "it by tuning the period, k and the suspicion timeout. Serf/Consul memberlist, Cassandra "
                 "and many service meshes use this family of protocols, with refinements such as "
                 "Lifeguard's adaptive timeouts."),
    .run = gossip_run, .stop = gossip_stop, .crash = gossip_crash, .recover = gossip_recover,
    .panel = gossip_panel,
};
NL_MODULE(gossip_mod)

NL_INIT(gossip_dis)
{
    nl_dissector_udp(G_PORT, dis_gossip);
}
