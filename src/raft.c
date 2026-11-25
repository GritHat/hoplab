/*
 * raft.c - The Raft consensus algorithm (Ongaro & Ousterhout, 2014) over UDP:
 *          leader election, log replication, commitment and a small
 *          key-value state machine. Optional pre-vote.
 *
 * Messages are binary, UDP port 5254:
 *
 *   0      1      2             6
 *   +------+------+-------------+----------------------------
 *   | type | from | term        | type-specific body
 *   +------+------+-------------+----------------------------
 *
 *   RequestVote / PreVote      lastLogIndex(4) lastLogTerm(4)
 *   ...Reply                   granted(1)
 *   AppendEntries              prevIndex(4) prevTerm(4) commit(4) n(1)
 *                              then n entries: term(4) len(1) command
 *   AppendEntriesReply         success(1) index(4)   (match index, or a hint
 *                              where to retry from on failure)
 *   ClientForward              len(1) command
 *
 * `from` is the index of the sender in the member list, which every member
 * sorts by name so that all agree.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAFT_PORT   5254
#define RAFT_MAXN   16
#define RAFT_LOG    256
#define RAFT_CMD    40
#define RAFT_BATCH  8
#define RAFT_KV     16

enum { M_RV = 1, M_RVR, M_PV, M_PVR, M_AE, M_AER, M_CLIENT };
enum { R_FOLLOWER, R_CANDIDATE, R_LEADER };

typedef struct { int term; char cmd[RAFT_CMD]; } rentry;

typedef struct raft {
    nl_node *n;
    int      np, self;
    char     pname[RAFT_MAXN][16];
    nl_ip    pip[RAFT_MAXN];
    nl_time  el_min, el_max, hb;
    int      prevote;

    /* persistent state: survives a crash */
    int      term, voted_for, nlog;
    rentry   log[RAFT_LOG + 1];         /* 1-based */

    /* volatile state */
    int      role, leader, commit, applied;
    int      pre;                       /* collecting pre-votes */
    unsigned votes;                     /* bit mask of granted votes */
    int      next[RAFT_MAXN], match[RAFT_MAXN];
    nl_time  el_start, el_deadline, last_heard;
    nl_ev    el_timer, hb_timer;
    char     key[RAFT_KV][16], val[RAFT_KV][24];
    int      nkv;
    unsigned elections;
} raft;

static const nl_module raft_mod;

static const char *role_name(int role)
{
    return role == R_LEADER ? "leader" : role == R_CANDIDATE ? "candidate" : "follower";
}

static int last_term(const raft *r) { return r->nlog ? r->log[r->nlog].term : 0; }

/* Besides its own counters every member publishes two cluster-wide facts,
 * computed by looking at the other members directly (a privilege of the
 * simulator, limited to the members simulated by this process): how many
 * live members believe they are leader, and whether all live members have
 * applied the same commands. Scenarios assert on them. */
static void counters(raft *r)
{
    int i, leaders = 0, agree = 1, max_term = 0;
    double hash = -1, applied = -1;

    nl_count_set(r->n, "raft.is_leader", r->role == R_LEADER);
    nl_count_set(r->n, "raft.term", r->term);
    nl_count_set(r->n, "raft.commit_index", r->commit);
    nl_count_set(r->n, "raft.applied", r->applied);
    nl_count_set(r->n, "raft.log_len", r->nlog);
    nl_count_set(r->n, "raft.elections", r->elections);
    nl_sample(r->n, "raft term", r->term);
    for (i = 0; i < r->np; i++) {
        const nl_node *pn = nl_node_find(r->pname[i]);
        const raft *pr = pn && pn->local && pn->up ? nl_mod_get(pn, &raft_mod) : NULL;

        if (!pr)
            continue;
        leaders += pr->role == R_LEADER;
        if (pr->term > max_term)
            max_term = pr->term;
        if (applied < 0) {
            applied = pr->applied;
            hash = nl_count_get(pn, "raft.state_hash");
        } else if (applied != pr->applied || hash != nl_count_get(pn, "raft.state_hash")) {
            agree = 0;
        }
    }
    nl_count_set(r->n, "raft.leaders", leaders);
    nl_count_set(r->n, "raft.max_term", max_term);
    nl_count_set(r->n, "raft.agree", agree);
}

/* ---- messages ---- */

static int head(uint8_t *b, const raft *r, int type, int term)
{
    b[0] = (uint8_t)type;
    b[1] = (uint8_t)r->self;
    nl_put32(b + 2, (uint32_t)term);
    return 6;
}

static void send_to(raft *r, int peer, const uint8_t *b, int len)
{
    nl_count(r->n, "raft.messages", 1);
    nl_udp_send(r->n, RAFT_PORT, r->pip[peer], RAFT_PORT, b, len);
}

static void send_vote_req(raft *r, int type, int term)
{
    uint8_t b[16];
    int i, n = head(b, r, type, term);

    nl_put32(b + n, (uint32_t)r->nlog);
    nl_put32(b + n + 4, (uint32_t)last_term(r));
    for (i = 0; i < r->np; i++)
        if (i != r->self)
            send_to(r, i, b, n + 8);
}

static void send_append(raft *r, int peer)
{
    uint8_t b[32 + RAFT_BATCH * (RAFT_CMD + 5)];
    int n = head(b, r, M_AE, r->term), prev = r->next[peer] - 1, cnt = 0, i;

    nl_put32(b + n, (uint32_t)prev);
    nl_put32(b + n + 4, (uint32_t)(prev > 0 ? r->log[prev].term : 0));
    nl_put32(b + n + 8, (uint32_t)r->commit);
    n += 13;
    for (i = r->next[peer]; i <= r->nlog && cnt < RAFT_BATCH; i++, cnt++) {
        int cl = (int)strlen(r->log[i].cmd);
        nl_put32(b + n, (uint32_t)r->log[i].term);
        b[n + 4] = (uint8_t)cl;
        memcpy(b + n + 5, r->log[i].cmd, (size_t)cl);
        n += 5 + cl;
    }
    b[18] = (uint8_t)cnt;
    if (cnt)
        nl_log(r->n, NL_L_DIST, 2, 0, T_CHAN "raft" T_RESET " → %s AppendEntries term=%d prev=%d/%d entries=%d commit=%d",
               r->pname[peer], r->term, prev, prev > 0 ? r->log[prev].term : 0, cnt, r->commit);
    else
        nl_log(r->n, NL_L_DIST, 3, 0, T_CHAN "raft" T_RESET " → %s heartbeat term=%d commit=%d", r->pname[peer],
               r->term, r->commit);
    send_to(r, peer, b, n);
}

/* ---- state machine ---- */

static const char *kv_get(const raft *r, const char *key)
{
    int i;

    for (i = 0; i < r->nkv; i++)
        if (strcmp(r->key[i], key) == 0)
            return r->val[i];
    return NULL;
}

static void apply_entries(raft *r)
{
    while (r->applied < r->commit) {
        const rentry *e = &r->log[++r->applied];
        char k[16], v[24];

        if (sscanf(e->cmd, "put %15[^=]=%23s", k, v) == 2) {
            int i;
            for (i = 0; i < r->nkv; i++)
                if (strcmp(r->key[i], k) == 0)
                    break;
            if (i == r->nkv && r->nkv < RAFT_KV)
                r->nkv++;
            if (i < RAFT_KV) {
                snprintf(r->key[i], sizeof(r->key[i]), "%s", k);
                snprintf(r->val[i], sizeof(r->val[i]), "%s", v);
            }
            /* a tiny fingerprint of the state, so scenarios can compare nodes */
            nl_count_set(r->n, "raft.state_hash", 0);
            for (i = 0; i < r->nkv; i++) {
                const char *s;
                double h = nl_count_get(r->n, "raft.state_hash");
                unsigned x = 17;
                for (s = r->key[i]; *s; s++) x = x * 31 + (unsigned char)*s;
                for (s = r->val[i]; *s; s++) x = x * 31 + (unsigned char)*s;
                nl_count_set(r->n, "raft.state_hash", h + (double)(x % 100003));
            }
        }
        if (r->role == R_LEADER)
            nl_log(r->n, NL_L_DIST, 1, 0, T_OK "raft: %s %d" T_RESET " [%s] — %s", _("committed entry"), r->applied,
                   e->cmd[0] ? e->cmd : "no-op", _("stored on a majority, applied"));
        else
            nl_log(r->n, NL_L_DIST, 2, 0, T_CHAN "raft" T_RESET " %s %d [%s]", _("applied entry"), r->applied,
                   e->cmd[0] ? e->cmd : "no-op");
    }
    counters(r);
}

/* ---- roles ---- */

static void on_election_timeout(void *ctx);

static void reset_election(raft *r)
{
    nl_cancel(&r->el_timer);
    r->el_start = nl_now();
    r->el_deadline = r->el_start + r->el_min +
                     (nl_time)(nl_rand01() * (double)(r->el_max - r->el_min));
    r->el_timer = nl_after(r->n, r->el_deadline - r->el_start, on_election_timeout, r);
}

static void become_follower(raft *r, int term)
{
    if (r->role == R_LEADER)
        nl_log(r->n, NL_L_DIST, 1, 0, T_WARN "raft: %s" T_RESET " (term %d → %d)",
               _("steps down: a higher term exists"), r->term, term);
    nl_cancel(&r->hb_timer);
    if (term > r->term) {
        r->term = term;
        r->voted_for = -1;
    }
    r->role = R_FOLLOWER;
    r->pre = 0;
    r->votes = 0;
    counters(r);
}

static void on_heartbeat(void *ctx)
{
    raft *r = ctx;
    int i;

    r->hb_timer = 0;
    if (r->role != R_LEADER)
        return;
    for (i = 0; i < r->np; i++)
        if (i != r->self)
            send_append(r, i);
    r->hb_timer = nl_after(r->n, r->hb, on_heartbeat, r);
}

static void advance_commit(raft *r)
{
    int idx, i;

    for (idx = r->nlog; idx > r->commit; idx--) {
        int have = 1;

        /* only entries of the current term are committed by counting
         * replicas; older ones are committed indirectly (Raft §5.4.2) */
        if (r->log[idx].term != r->term)
            break;
        for (i = 0; i < r->np; i++)
            if (i != r->self && r->match[i] >= idx)
                have++;
        if (have * 2 > r->np) {
            r->commit = idx;
            break;
        }
    }
    apply_entries(r);
}

static int append_local(raft *r, const char *cmd)
{
    if (r->nlog >= RAFT_LOG)
        return -1;
    r->nlog++;
    r->log[r->nlog].term = r->term;
    snprintf(r->log[r->nlog].cmd, RAFT_CMD, "%s", cmd);
    return r->nlog;
}

static void become_leader(raft *r)
{
    int i;

    r->role = R_LEADER;
    r->leader = r->self;
    r->pre = 0;
    nl_cancel(&r->el_timer);
    for (i = 0; i < r->np; i++) {
        r->next[i] = r->nlog + 1;
        r->match[i] = 0;
    }
    nl_log(r->n, NL_L_DIST, 1, 0, T_OK T_BOLD "raft: %s %d" T_RESET " (%d/%d %s)", _("elected LEADER for term"),
           r->term, __builtin_popcount(r->votes), r->np, _("votes"));
    /* a no-op of the new term: committing it also commits everything
     * inherited from previous terms */
    append_local(r, "");
    counters(r);
    if (r->np == 1)
        advance_commit(r);
    on_heartbeat(r);
}

static void start_election(raft *r)
{
    r->pre = 0;
    r->role = R_CANDIDATE;
    r->term++;
    r->voted_for = r->self;
    r->votes = 1u << r->self;
    r->leader = -1;
    r->elections++;
    nl_log(r->n, NL_L_DIST, 1, 0, T_WARN "raft: %s" T_RESET " — %s %d, %s", _("election timeout"),
           _("candidate for term"), r->term, _("asking for votes"));
    counters(r);
    if (r->np == 1) {
        become_leader(r);
        return;
    }
    send_vote_req(r, M_RV, r->term);
}

static void on_election_timeout(void *ctx)
{
    raft *r = ctx;

    r->el_timer = 0;
    if (r->role == R_LEADER)
        return;
    reset_election(r);
    if (r->prevote && r->np > 1) {
        /* ask first whether an election could succeed, without touching the term */
        r->pre = 1;
        r->votes = 1u << r->self;
        r->leader = -1;
        nl_log(r->n, NL_L_DIST, 2, 0, T_CHAN "raft" T_RESET " %s %d", _("election timeout: pre-vote for term"),
               r->term + 1);
        send_vote_req(r, M_PV, r->term + 1);
        return;
    }
    start_election(r);
}

/* ---- receiving ---- */

static void raft_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    raft *r = ctx;
    const uint8_t *b = m->data;
    int type, from, term;
    uint8_t out[16];
    int on;

    if (m->len < 6)
        return;
    type = b[0];
    from = b[1];
    term = (int)nl_get32(b + 2);
    if (from >= r->np || from == r->self)
        return;

    if (type == M_CLIENT) {
        char cmd[RAFT_CMD];
        int idx;

        if (m->len < 7 || b[6] >= RAFT_CMD || m->len < 7 + b[6])
            return;
        snprintf(cmd, sizeof(cmd), "%.*s", b[6], (const char *)b + 7);
        if (r->role != R_LEADER) {
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " %s [%s]",
                   _("forwarded command dropped: not the leader any more"), cmd);
            return;
        }
        idx = append_local(r, cmd);
        nl_log(n, NL_L_DIST, 1, m->pkt_id, T_INFO "raft: %s %s" T_RESET " [%s] → %s %d", _("command from"),
               r->pname[from], cmd, _("appended at index"), idx);
        counters(r);
        nl_cancel(&r->hb_timer);
        on_heartbeat(r);
        return;
    }

    if (type == M_PV) {
        /* pre-vote: answer what we would do, change nothing */
        int lidx = (int)nl_get32(b + 6), lterm = (int)nl_get32(b + 10);
        int fresh = lterm > last_term(r) || (lterm == last_term(r) && lidx >= r->nlog);
        int quiet = r->role != R_LEADER && nl_now() - r->last_heard >= r->el_min;

        if (m->len < 14)
            return;
        on = head(out, r, M_PVR, term);
        out[on] = (uint8_t)(term > r->term && fresh && quiet);
        nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s PreVote term=%d: %s", r->pname[from], term,
               out[on] ? _("would vote") : _("no (a leader is alive or the log is behind)"));
        send_to(r, from, out, on + 1);
        return;
    }
    if (type == M_PVR) {
        if (!r->pre || term != r->term + 1 || m->len < 7 || !b[6])
            return;
        r->votes |= 1u << from;
        if (__builtin_popcount(r->votes) * 2 > r->np)
            start_election(r);
        return;
    }

    if (term > r->term) {
        int was = r->role;
        become_follower(r, term);
        if (was != R_FOLLOWER)
            reset_election(r);
    }

    switch (type) {
    case M_RV: {
        int lidx, lterm, fresh, grant;

        if (m->len < 14)
            return;
        lidx = (int)nl_get32(b + 6);
        lterm = (int)nl_get32(b + 10);
        fresh = lterm > last_term(r) || (lterm == last_term(r) && lidx >= r->nlog);
        grant = term == r->term && (r->voted_for < 0 || r->voted_for == from) && fresh;
        if (grant) {
            r->voted_for = from;
            reset_election(r);
        }
        nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s RequestVote term=%d lastLog=%d/%d: %s%s",
               r->pname[from], term, lidx, lterm, grant ? T_OK : T_ERR,
               grant ? _("vote granted")
               : term < r->term ? _("refused: stale term")
               : !fresh ? _("refused: candidate's log is behind mine")
               : _("refused: already voted in this term"));
        on = head(out, r, M_RVR, r->term);
        out[on] = (uint8_t)grant;
        send_to(r, from, out, on + 1);
        counters(r);
        break;
    }
    case M_RVR:
        if (r->role != R_CANDIDATE || term != r->term || m->len < 7 || !b[6])
            return;
        r->votes |= 1u << from;
        nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s %s (%d/%d)", r->pname[from], _("vote"),
               __builtin_popcount(r->votes), r->np);
        if (__builtin_popcount(r->votes) * 2 > r->np)
            become_leader(r);
        break;
    case M_AE: {
        int prev, pterm, lcommit, cnt, off = 19, k, ok, idx, hint;

        if (m->len < 19)
            return;
        prev = (int)nl_get32(b + 6);
        pterm = (int)nl_get32(b + 10);
        lcommit = (int)nl_get32(b + 14);
        cnt = b[18];
        if (term < r->term) {
            on = head(out, r, M_AER, r->term);
            out[on] = 0;
            nl_put32(out + on + 1, 0);
            send_to(r, from, out, on + 5);
            return;
        }
        if (r->role != R_FOLLOWER)
            become_follower(r, term);
        if (r->leader != from)
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " %s %s (term %d)", _("recognises leader"),
                   r->pname[from], term);
        r->leader = from;
        r->last_heard = nl_now();
        r->pre = 0;
        reset_election(r);

        ok = prev <= r->nlog && (prev == 0 || r->log[prev].term == pterm);
        hint = 0;
        if (!ok) {
            /* tell the leader where to retry: the start of our conflicting
             * term, or just past the end of our log */
            if (prev > r->nlog) {
                hint = r->nlog + 1;
            } else {
                hint = prev;
                while (hint > 1 && r->log[hint - 1].term == r->log[prev].term)
                    hint--;
            }
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s AppendEntries prev=%d/%d: " T_WARN "%s"
                   T_RESET " (%s %d)", r->pname[from], prev, pterm, _("log does not match"), _("retry from"), hint);
        } else {
            idx = prev;
            for (k = 0; k < cnt; k++) {
                int et, cl;

                if (off + 5 > m->len)
                    break;
                et = (int)nl_get32(b + off);
                cl = b[off + 4];
                if (cl >= RAFT_CMD || off + 5 + cl > m->len)
                    break;
                idx++;
                if (idx <= r->nlog && r->log[idx].term != et) {
                    nl_log(n, NL_L_DIST, 1, m->pkt_id, T_WARN "raft: %s %d..%d" T_RESET " — %s",
                           _("discarding uncommitted entries"), idx, r->nlog,
                           _("they conflict with the leader's log"));
                    nl_count(n, "raft.truncated", r->nlog - idx + 1);
                    r->nlog = idx - 1;
                }
                if (idx > r->nlog && idx <= RAFT_LOG) {
                    r->nlog = idx;
                    r->log[idx].term = et;
                    snprintf(r->log[idx].cmd, RAFT_CMD, "%.*s", cl, (const char *)b + off + 5);
                }
                off += 5 + cl;
            }
            if (cnt)
                nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s AppendEntries prev=%d/%d entries=%d: "
                       T_OK "%s" T_RESET " (%s %d)", r->pname[from], prev, pterm, cnt, _("appended"), _("log length"),
                       r->nlog);
            hint = idx;
            if (lcommit > r->commit)
                r->commit = lcommit < idx ? lcommit : idx;
            apply_entries(r);
        }
        on = head(out, r, M_AER, r->term);
        out[on] = (uint8_t)ok;
        nl_put32(out + on + 1, (uint32_t)hint);
        send_to(r, from, out, on + 5);
        break;
    }
    case M_AER: {
        int ok, idx;

        if (r->role != R_LEADER || term != r->term || m->len < 11)
            return;
        ok = b[6];
        idx = (int)nl_get32(b + 7);
        if (ok) {
            if (idx > r->match[from]) {
                r->match[from] = idx;
                nl_log(n, NL_L_DIST, 3, m->pkt_id, T_CHAN "raft" T_RESET " ← %s match=%d", r->pname[from], idx);
            }
            r->next[from] = r->match[from] + 1;
            advance_commit(r);
            if (r->next[from] <= r->nlog)
                send_append(r, from);           /* more to send: keep the catch-up going */
        } else {
            if (idx < 1) idx = 1;
            if (idx >= r->next[from]) idx = r->next[from] > 1 ? r->next[from] - 1 : 1;
            nl_log(n, NL_L_DIST, 2, m->pkt_id, T_CHAN "raft" T_RESET " ← %s %s: nextIndex %d → %d", r->pname[from],
                   _("rejected (log mismatch)"), r->next[from], idx);
            r->next[from] = idx;
            send_append(r, from);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- commands ---- */

static int parse_range(const char *s, nl_time *lo, nl_time *hi)
{
    char a[32], unit[8] = "ms";
    const char *dash = strchr(s, '-'), *u;

    if (!dash)
        return nl_parse_time(s, lo) && (*hi = *lo * 2, 1);
    for (u = dash + 1; *u && (*u == '.' || (*u >= '0' && *u <= '9')); u++)
        ;
    if (*u)
        snprintf(unit, sizeof(unit), "%s", u);
    snprintf(a, sizeof(a), "%.*s%s", (int)(dash - s), s, unit);
    return nl_parse_time(a, lo) && nl_parse_time(dash + 1, hi) && *hi >= *lo;
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

static int raft_start(nl_node *n, const nl_args *a)
{
    const char *peers = nl_arg_str(a, "peers", NULL);
    char list[320];
    char *tok, *save = NULL;
    raft *r;
    int i;

    if (!peers) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("the member list is missing (peers=n1,n2,...)"));
        return -1;
    }
    r = calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->n = n;
    r->self = -1;
    snprintf(list, sizeof(list), "%s", peers);
    for (tok = strtok_r(list, ",", &save); tok && r->np < RAFT_MAXN; tok = strtok_r(NULL, ",", &save))
        snprintf(r->pname[r->np++], sizeof(r->pname[0]), "%s", tok);
    qsort(r->pname, (size_t)r->np, sizeof(r->pname[0]), cmp_name);
    for (i = 0; i < r->np; i++) {
        if (!nl_resolve(r->pname[i], &r->pip[i])) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s '%s'", _("unknown member"), r->pname[i]);
            free(r);
            return -1;
        }
        if (strcmp(r->pname[i], n->name) == 0)
            r->self = i;
    }
    if (r->self < 0) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("this node is not in the member list"));
        free(r);
        return -1;
    }
    r->el_min = NL_MS(1000);
    r->el_max = NL_MS(2000);
    if (nl_arg_str(a, "election", NULL) && !parse_range(nl_arg_str(a, "election", ""), &r->el_min, &r->el_max)) {
        nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("bad election range (example: election=1000-2000ms)"));
        free(r);
        return -1;
    }
    r->hb = nl_arg_time(a, "heartbeat", NL_MS(300));
    r->prevote = (int)nl_arg_int(a, "prevote", 0);
    r->voted_for = r->leader = -1;
    nl_mod_set(n, &raft_mod, r);
    nl_udp_bind(n, RAFT_PORT, raft_rx, r);
    nl_log(n, NL_L_DIST, 2, 0, T_CHAN "raft" T_RESET " %s: %d %s, %s %s–%s, heartbeat %s", _("follower, term 0"),
           r->np, _("members"), _("election timeout"), nl_time_str(r->el_min), nl_time_str(r->el_max),
           nl_time_str(r->hb));
    reset_election(r);
    counters(r);
    return 0;
}

static int raft_run(nl_node *n, const nl_args *a)
{
    raft *r = nl_mod_get(n, &raft_mod);
    const char *sub = nl_arg_pos(a, 0);

    if (!r)
        return raft_start(n, a);
    if (sub && strcmp(sub, "put") == 0) {
        char cmd[RAFT_CMD];
        int i, kv = -1;

        for (i = 0; i < a->argc; i++)
            if (strchr(a->argv[i], '='))
                kv = i;
        if (kv < 0 || strlen(a->argv[kv]) > RAFT_CMD - 6) {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("usage: raft put KEY=VALUE"));
            return -1;
        }
        snprintf(cmd, sizeof(cmd), "put %s", a->argv[kv]);
        if (r->role == R_LEADER) {
            int idx = append_local(r, cmd);
            if (idx < 0) {
                nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("log full"));
                return -1;
            }
            nl_log(n, NL_L_DIST, 1, 0, T_INFO "raft: %s" T_RESET " [%s] → %s %d, %s", _("client command"), cmd,
                   _("appended at index"), idx, _("replicating"));
            counters(r);
            if (r->np == 1)
                advance_commit(r);
            nl_cancel(&r->hb_timer);
            on_heartbeat(r);
        } else if (r->leader >= 0) {
            uint8_t b[8 + RAFT_CMD];
            int on = head(b, r, M_CLIENT, r->term), cl = (int)strlen(cmd);

            b[on] = (uint8_t)cl;
            memcpy(b + on + 1, cmd, (size_t)cl);
            nl_log(n, NL_L_DIST, 1, 0, T_INFO "raft: %s" T_RESET " [%s] → %s %s", _("client command"), cmd,
                   _("not the leader: forwarded to"), r->pname[r->leader]);
            send_to(r, r->leader, b, on + 1 + cl);
        } else {
            nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s" T_RESET " [%s] — %s", _("client command rejected"), cmd,
                   _("no leader known (election in progress or no majority reachable)"));
            nl_count(n, "raft.rejected", 1);
            return 0;
        }
        return 0;
    }
    if (sub && strcmp(sub, "get") == 0) {
        const char *k = nl_arg_pos(a, 1), *v = k ? kv_get(r, k) : NULL;

        nl_log(n, NL_L_DIST, 1, 0, T_INFO "raft: get %s" T_RESET " = %s  " T_DIM "(%s %d%s)", k ? k : "?",
               v ? v : _("(not set)"), _("local state, applied index"), r->applied,
               r->role == R_LEADER ? "" : _(", may be stale: not the leader"));
        return 0;
    }
    nl_log(n, NL_L_DIST, 1, 0, T_ERR "raft: %s", _("usage: raft put KEY=VALUE | raft get KEY"));
    return -1;
}

static void raft_crash(nl_node *n)
{
    raft *r = nl_mod_get(n, &raft_mod);

    /* term, votedFor and the log are on stable storage; the rest is lost */
    r->el_timer = r->hb_timer = 0;
    r->role = R_FOLLOWER;
    r->leader = -1;
    r->commit = r->applied = 0;
    r->nkv = 0;
    r->votes = 0;
    r->pre = 0;
    counters(r);
}

static void raft_recover(nl_node *n)
{
    raft *r = nl_mod_get(n, &raft_mod);

    nl_log(n, NL_L_DIST, 1, 0, T_INFO "raft: %s" T_RESET " term=%d, %s=%d — %s", _("restarted from stable storage:"),
           r->term, _("log length"), r->nlog, _("follower again, commit index unknown until the leader tells"));
    r->last_heard = nl_now();
    reset_election(r);
    counters(r);
}

static void raft_stop(nl_node *n)
{
    raft *r = nl_mod_get(n, &raft_mod);

    if (!r)
        return;
    nl_cancel(&r->el_timer);
    nl_cancel(&r->hb_timer);
    nl_udp_unbind(n, RAFT_PORT);
    nl_mod_set(n, &raft_mod, NULL);
    free(r);
}

static int raft_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    raft *r = nl_mod_get(n, &raft_mod);
    tstyle txt = TS(C_TEXT, C_DEFAULT, 0);
    int row = 0, i, cx, first;

    tui_print(x, y + row++, w, txt, "%s %s " T_RESET " " T_DIM "term" T_RESET " " T_BOLD "%d" T_RESET "  " T_DIM "%s"
              T_RESET " %s  " T_DIM "%s" T_RESET " %s",
              r->role == R_LEADER ? T_CHIP_ACK : r->role == R_CANDIDATE ? T_CHIP_WARN : T_CHIP_OFF,
              role_name(r->role), r->term, _("voted for"), r->voted_for >= 0 ? r->pname[r->voted_for] : "—",
              _("leader"), r->leader >= 0 ? r->pname[r->leader] : "?");
    if (row < h && r->role != R_LEADER && n->up && r->el_deadline > r->el_start) {
        double fr = (double)(nl_now() - r->el_start) / (double)(r->el_deadline - r->el_start);
        int lab = tui_print(x, y + row, w, TS(C_DIM, C_DEFAULT, 0), "%s ", _("election timer"));
        tui_bar(x + lab, y + row, w - lab - 10, fr, fr < 0.6 ? C_OK : fr < 0.85 ? C_WARN : C_ERR);
        tui_print(x + w - 9, y + row, 9, TS(C_DIM, C_DEFAULT, 0), "%s", nl_time_str(r->el_deadline - r->el_start));
        row++;
    }
    if (row < h)
        tui_print(x, y + row++, w, txt, T_DIM "log" T_RESET " %d  " T_DIM "commit" T_RESET " %d  " T_DIM "applied"
                  T_RESET " %d", r->nlog, r->commit, r->applied);
    /* the log, most recent entries that fit: [index:term command] */
    if (row < h) {
        int cell = detail >= 2 ? 14 : 7, fit = w / cell;

        first = r->nlog - fit + 1;
        if (first < 1)
            first = 1;
        cx = x;
        for (i = first; i <= r->nlog; i++) {
            tstyle st = i <= r->applied ? TS(151, 22, 0) : i <= r->commit ? TS(16, C_ACK, 0) : TS(231, C_DATA_BG, 0);
            char cellbuf[40];

            if (detail >= 2)
                snprintf(cellbuf, sizeof(cellbuf), "%d:%d %s", i, r->log[i].term,
                         r->log[i].cmd[0] ? r->log[i].cmd + 4 : "nop");
            else
                snprintf(cellbuf, sizeof(cellbuf), "%d:%d", i, r->log[i].term);
            tui_fill(cx, y + row, cell - 1, 1, st);
            tui_print(cx, y + row, cell - 1, st, "%s", cellbuf);
            cx += cell;
        }
        if (!r->nlog)
            tui_print(x, y + row, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("(empty log)"));
        row++;
        if (row < h)
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("index:term · green = applied, blue = not committed"));
    }
    if (r->role == R_LEADER)
        for (i = 0; i < r->np && row < h; i++) {
            if (i == r->self)
                continue;
            tui_print(x, y + row++, w, txt, "  %-8s " T_DIM "next" T_RESET " %-3d " T_DIM "match" T_RESET " %-3d %s",
                      r->pname[i], r->next[i], r->match[i],
                      r->match[i] >= r->nlog ? T_OK "✓" : T_WARN "…");
        }
    for (i = 0; i < r->nkv && row < h; i++)
        tui_print(x, y + row++, w, txt, T_DIM "kv" T_RESET "  %s = " T_BOLD "%s", r->key[i], r->val[i]);
    return row;
}

/* ---- dissector ---- */

static void dis_raft(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const char *const names[] = { "?", "RequestVote", "RequestVoteReply", "PreVote", "PreVoteReply",
                                         "AppendEntries", "AppendEntriesReply", "ClientForward" };
    int type, term;

    if (len < 6)
        return;
    type = d[0];
    term = (int)nl_get32(d + 2);
    nl_dis_field(o, "Raft", "Type", off, 1, "%d (%s)", type, type >= 1 && type <= 7 ? names[type] : "?");
    nl_dis_field(o, "Raft", "From (member index)", off + 1, 1, "%d", d[1]);
    nl_dis_field(o, "Raft", "Term", off + 2, 4, "%d", term);
    switch (type) {
    case M_RV:
    case M_PV:
        if (len < 14)
            break;
        nl_dis_field(o, "Raft", "Last log index", off + 6, 4, "%u", nl_get32(d + 6));
        nl_dis_field(o, "Raft", "Last log term", off + 10, 4, "%u", nl_get32(d + 10));
        nl_dis_summary(o, "RAFT", 209, "%s term=%d lastLog=%u/%u", names[type], term, nl_get32(d + 6),
                       nl_get32(d + 10));
        return;
    case M_RVR:
    case M_PVR:
        if (len < 7)
            break;
        nl_dis_field(o, "Raft", "Vote granted", off + 6, 1, "%s", d[6] ? "yes" : "no");
        nl_dis_summary(o, "RAFT", 209, "%s term=%d %s", names[type], term, d[6] ? "granted" : "refused");
        return;
    case M_AE: {
        int cnt, p = 19, k;

        if (len < 19)
            break;
        cnt = d[18];
        nl_dis_field(o, "Raft", "Prev log index", off + 6, 4, "%u", nl_get32(d + 6));
        nl_dis_field(o, "Raft", "Prev log term", off + 10, 4, "%u", nl_get32(d + 10));
        nl_dis_field(o, "Raft", "Leader commit", off + 14, 4, "%u", nl_get32(d + 14));
        nl_dis_field(o, "Raft", "Entries", off + 18, 1, "%d%s", cnt, cnt ? "" : " (heartbeat)");
        for (k = 0; k < cnt && p + 5 <= len && p + 5 + d[p + 4] <= len; k++) {
            nl_dis_field(o, "Raft", "Entry", off + p, 5 + d[p + 4], "index %u term %u [%.*s]",
                         nl_get32(d + 6) + 1 + (unsigned)k, nl_get32(d + p), d[p + 4],
                         d[p + 4] ? (const char *)d + p + 5 : "no-op");
            p += 5 + d[p + 4];
        }
        nl_dis_summary(o, "RAFT", 209, "%s term=%d prev=%u/%u entries=%d commit=%u",
                       cnt ? "AppendEntries" : "Heartbeat", term, nl_get32(d + 6), nl_get32(d + 10), cnt,
                       nl_get32(d + 14));
        return;
    }
    case M_AER:
        if (len < 11)
            break;
        nl_dis_field(o, "Raft", "Success", off + 6, 1, "%s", d[6] ? "yes" : "no");
        nl_dis_field(o, "Raft", d[6] ? "Match index" : "Retry from", off + 7, 4, "%u", nl_get32(d + 7));
        nl_dis_summary(o, "RAFT", 209, "AppendEntriesReply term=%d %s %s=%u", term, d[6] ? "success" : "MISMATCH",
                       d[6] ? "match" : "retry", nl_get32(d + 7));
        return;
    case M_CLIENT:
        if (len < 7 || 7 + d[6] > len)
            break;
        nl_dis_field(o, "Raft", "Command", off + 6, 1 + d[6], "%.*s", d[6], (const char *)d + 7);
        nl_dis_summary(o, "RAFT", 209, "ClientForward [%.*s]", d[6], (const char *)d + 7);
        return;
    default:
        break;
    }
    nl_dis_summary(o, "RAFT", 209, "%s term=%d", type >= 1 && type <= 7 ? names[type] : "?", term);
}

static const nl_module raft_mod = {
    .name = "raft", .title = N_("Raft — consensus: one replicated log, one leader at a time"), .layer = NL_L_DIST,
    .usage = N_("run NODE raft peers=n1,n2,n3,... [election=1000-2000ms] [heartbeat=300ms] [prevote=1]\n"
                "  Start it on every member with the same peers list (an odd number is best).\n"
                "run NODE raft put KEY=VALUE   submit a command (forwarded to the leader)\n"
                "run NODE raft get KEY         read the local copy of the state machine\n"
                "  Crash and restart members with `node N down|up`, partition with `link A B down`."),
    .theory = N_("Consensus lets a group of machines behave like one reliable machine: they agree on a "
                 "single sequence of commands (the replicated log) and each applies it, in order, to its "
                 "own copy of a state machine. As long as a majority is alive and can communicate, the "
                 "service keeps working and never returns contradictory answers; when no majority is "
                 "reachable it stops making progress rather than risk an inconsistency. Raft obtains this "
                 "with a strong leader: one member at a time accepts commands and tells the others what "
                 "to store.\n\n"
                 "Time is divided into terms, numbered consecutively; every message carries the sender's "
                 "term and whoever sees a higher one adopts it and becomes a follower. A follower that "
                 "hears nothing from a leader for a randomized election timeout becomes a candidate: it "
                 "increments the term, votes for itself and asks the others (RequestVote). Each member "
                 "grants at most one vote per term, and only to a candidate whose log is at least as "
                 "up-to-date as its own (higher last term, or same term and at least as long). A majority "
                 "of votes makes a leader; randomized timeouts make split votes rare and short-lived. "
                 "Because any two majorities intersect, at most one leader exists per term, and the vote "
                 "restriction guarantees it already holds every committed entry (leader completeness).\n\n"
                 "The leader appends client commands to its log and replicates them with AppendEntries, "
                 "which also serves as heartbeat. Each request names the index and term of the entry that "
                 "precedes the new ones; a follower accepts only if its log contains that entry, otherwise "
                 "it refuses and the leader retries from further back. By induction, two logs that agree "
                 "on one (index, term) pair agree on everything before it (log matching). A follower whose "
                 "log diverges — because it once received entries from a leader that failed before "
                 "committing them — has the conflicting suffix overwritten.\n\n"
                 "An entry is committed once the leader of the current term has stored it on a majority; "
                 "committed entries are never lost and are applied in order by everybody. A subtle rule: "
                 "a leader never commits entries of earlier terms by counting replicas, only indirectly by "
                 "committing an entry of its own term after them (hence the no-op appended at the start of "
                 "each term). Without this rule an entry present on a majority could still be overwritten.\n\n"
                 "Term, vote and log are written to stable storage before answering, so a member that "
                 "crashes and restarts cannot vote twice in a term or forget an entry it acknowledged; "
                 "commit index and state machine are volatile and are rebuilt. A member isolated by a "
                 "partition keeps timing out and inflating its term; when the partition heals it forces a "
                 "needless election. The pre-vote extension avoids it: before incrementing the term a "
                 "would-be candidate asks whether it could win, and members that still hear a leader say "
                 "no. Real systems add log compaction (snapshots), membership changes through joint or "
                 "single-server reconfiguration, and linearizable reads (read index, leases); etcd, Consul "
                 "and CockroachDB are built on Raft, while Paxos and its Multi-Paxos variants solve the "
                 "same problem with a less prescriptive structure."),
    .run = raft_run, .stop = raft_stop, .crash = raft_crash, .recover = raft_recover, .panel = raft_panel,
};
NL_MODULE(raft_mod)

NL_INIT(raft_dis)
{
    nl_dissector_udp(RAFT_PORT, dis_raft);
}
