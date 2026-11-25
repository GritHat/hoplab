/*
 * dns.c - The Domain Name System with the RFC 1035 wire format over UDP 53:
 *         authoritative servers with zones and delegations, a recursive
 *         resolver with a TTL cache, the `dig` client, and a forger for the
 *         cache-poisoning lab.
 *
 * Simplifications: class IN only; types A, NS, CNAME, MX, TXT, SOA; no TCP
 * fallback, no EDNS, no DNSSEC; a delegation needs glue (the resolver does
 * not resolve name-server names on its own); the SOA record is synthesized.
 */
#include "nl_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DNS_PORT   53
#define DNS_COLOR  141
#define NAME_MAX_  72

enum { T_A = 1, T_NS = 2, T_CNAME = 5, T_SOA = 6, T_MX = 15, T_TXT = 16 };
enum { RC_OK = 0, RC_SERVFAIL = 2, RC_NXDOMAIN = 3, RC_REFUSED = 5 };
enum { F_QR = 0x8000, F_AA = 0x0400, F_RD = 0x0100, F_RA = 0x0080 };
enum { SEC_AN = 0, SEC_NS = 1, SEC_AR = 2 };

typedef struct {
    char     name[NAME_MAX_];           /* owner, lower case, no trailing dot; "" = root */
    uint16_t type;
    uint32_t ttl;
    nl_ip    ip;                        /* A: address; SOA: minimum (negative TTL) */
    uint16_t pref;                      /* MX */
    char     text[NAME_MAX_];           /* NS/CNAME/MX target, TXT text, SOA mname */
    nl_time  expire;                    /* cache: absolute expiry (0 = free slot) */
    int      neg;                       /* cache: 1 = no data of this type, 2 = name does not exist */
    int      sec;                       /* parsed message: section */
    int      off, wlen;                 /* parsed message: position in the message */
} dns_rr;

typedef struct {
    uint16_t id, flags;
    int      nq, cnt[3];
    char     qname[NAME_MAX_];
    uint16_t qtype;
    int      qlen;                      /* bytes of the question section */
    dns_rr   rr[24];
    int      nrr;
} dns_msg;

/* ------------------------------------------------------------------ */
/* Names and types                                                    */
/* ------------------------------------------------------------------ */

static const char *type_name(int t)
{
    switch (t) {
    case T_A: return "A";
    case T_NS: return "NS";
    case T_CNAME: return "CNAME";
    case T_SOA: return "SOA";
    case T_MX: return "MX";
    case T_TXT: return "TXT";
    default: return "?";
    }
}

static int type_parse(const char *s)
{
    static const int types[] = { T_A, T_NS, T_CNAME, T_SOA, T_MX, T_TXT };
    size_t i;

    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (strcasecmp(s, type_name(types[i])) == 0)
            return types[i];
    return 0;
}

/* Lower case, no trailing dot */
static void name_norm(const char *in, char *out)
{
    size_t k = 0;

    for (; *in && k + 1 < NAME_MAX_; in++)
        out[k++] = (char)tolower((unsigned char)*in);
    while (k && out[k - 1] == '.')
        k--;
    out[k] = '\0';
}

/* Name written in a zone command: "@", relative, or absolute with a dot */
static void name_fq(const char *in, const char *origin, char *out)
{
    char tmp[NAME_MAX_];
    size_t len = strlen(in);

    if (strcmp(in, "@") == 0) {
        snprintf(out, NAME_MAX_, "%s", origin);
    } else if ((len && in[len - 1] == '.') || !*origin) {
        name_norm(in, out);
    } else {
        name_norm(in, tmp);
        snprintf(out, NAME_MAX_, "%.*s.%s", (int)(NAME_MAX_ - 2 - strlen(origin)), tmp, origin);
    }
}

static int name_in(const char *name, const char *zone)
{
    size_t nl = strlen(name), zl = strlen(zone);

    if (!zl)
        return 1;
    if (nl < zl || strcmp(name + nl - zl, zone) != 0)
        return 0;
    return nl == zl || name[nl - zl - 1] == '.';
}

static const char *name_parent(const char *name)
{
    const char *dot = strchr(name, '.');
    return dot ? dot + 1 : "";
}

static const char *name_show(const char *name) { return *name ? name : "."; }

static const char *rr_str(const dns_rr *r)
{
    static char bufs[4][160];
    static int k;
    char *b = bufs[k = (k + 1) % 4];

    switch (r->type) {
    case T_A:     snprintf(b, 160, "A %s", nl_ip_str(r->ip)); break;
    case T_MX:    snprintf(b, 160, "MX %u %s", r->pref, r->text); break;
    case T_TXT:   snprintf(b, 160, "TXT \"%s\"", r->text); break;
    case T_SOA:   snprintf(b, 160, "SOA %s", r->text); break;
    default:      snprintf(b, 160, "%s %s", type_name(r->type), name_show(r->text)); break;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* Wire format                                                        */
/* ------------------------------------------------------------------ */

static int get_name(const uint8_t *m, int len, int *pos, char *out)
{
    int p = *pos, jumped = 0, hops = 0, i;
    size_t k = 0;

    for (;;) {
        int c;

        if (p >= len)
            return 0;
        c = m[p];
        if ((c & 0xC0) == 0xC0) {
            if (p + 1 >= len || ++hops > 16)
                return 0;
            if (!jumped)
                *pos = p + 2;
            jumped = 1;
            p = ((c & 0x3f) << 8) | m[p + 1];
            continue;
        }
        if (c == 0) {
            if (!jumped)
                *pos = p + 1;
            break;
        }
        if (c > 63 || p + 1 + c > len)
            return 0;
        if (k && k + 1 < NAME_MAX_)
            out[k++] = '.';
        for (i = 0; i < c; i++)
            if (k + 1 < NAME_MAX_)
                out[k++] = (char)tolower(m[p + 1 + i]);
        p += 1 + c;
    }
    out[k] = '\0';
    return 1;
}

static int msg_parse(const uint8_t *m, int len, dns_msg *o)
{
    int pos = 12, i, s, total;

    memset(o, 0, sizeof(*o));
    if (len < 12)
        return 0;
    o->id = nl_get16(m);
    o->flags = nl_get16(m + 2);
    o->nq = nl_get16(m + 4);
    for (s = 0; s < 3; s++)
        o->cnt[s] = nl_get16(m + 6 + 2 * s);
    for (i = 0; i < o->nq; i++) {
        char name[NAME_MAX_];

        if (!get_name(m, len, &pos, name) || pos + 4 > len)
            return 0;
        if (i == 0) {
            strcpy(o->qname, name);
            o->qtype = nl_get16(m + pos);
        }
        pos += 4;
    }
    o->qlen = pos - 12;
    total = o->cnt[0] + o->cnt[1] + o->cnt[2];
    for (i = 0; i < total; i++) {
        dns_rr r;
        int rdlen, rd, p2;

        memset(&r, 0, sizeof(r));
        r.off = pos;
        if (!get_name(m, len, &pos, r.name) || pos + 10 > len)
            return 0;
        r.type = nl_get16(m + pos);
        r.ttl = nl_get32(m + pos + 4);
        rdlen = nl_get16(m + pos + 8);
        rd = pos + 10;
        if (rd + rdlen > len)
            return 0;
        pos = rd + rdlen;
        r.wlen = pos - r.off;
        r.sec = i < o->cnt[0] ? SEC_AN : i < o->cnt[0] + o->cnt[1] ? SEC_NS : SEC_AR;
        p2 = rd;
        switch (r.type) {
        case T_A:
            if (rdlen != 4)
                return 0;
            r.ip = nl_get32(m + rd);
            break;
        case T_NS: case T_CNAME:
            if (!get_name(m, len, &p2, r.text))
                return 0;
            break;
        case T_MX:
            if (rdlen < 3)
                return 0;
            r.pref = nl_get16(m + rd);
            p2 = rd + 2;
            if (!get_name(m, len, &p2, r.text))
                return 0;
            break;
        case T_TXT:
            if (rdlen >= 1)
                snprintf(r.text, sizeof(r.text), "%.*s", m[rd] < rdlen ? m[rd] : rdlen - 1, (const char *)m + rd + 1);
            break;
        case T_SOA: {
            char rname[NAME_MAX_];
            if (!get_name(m, len, &p2, r.text) || !get_name(m, len, &p2, rname) || p2 + 20 > len)
                return 0;
            r.ip = nl_get32(m + p2 + 16);
            break;
        }
        default:
            break;
        }
        if (o->nrr < (int)(sizeof(o->rr) / sizeof(o->rr[0])))
            o->rr[o->nrr++] = r;
    }
    return 1;
}

typedef struct {
    uint8_t b[1232];
    int     len;
    struct { char name[NAME_MAX_]; int off; } ct[20];   /* compression targets */
    int     nct;
    int     cnt[3];
} dns_buf;

static void buf_start(dns_buf *o, uint16_t id, uint16_t flags, const char *qname, uint16_t qtype);

static void put_name(dns_buf *o, const char *name)
{
    const char *s = name;
    int i;

    for (;;) {
        const char *dot;
        size_t ll;

        if (!*s) {
            o->b[o->len++] = 0;
            return;
        }
        for (i = 0; i < o->nct; i++)
            if (strcmp(o->ct[i].name, s) == 0) {
                nl_put16(o->b + o->len, (uint16_t)(0xC000 | o->ct[i].off));
                o->len += 2;
                return;
            }
        if (o->nct < (int)(sizeof(o->ct) / sizeof(o->ct[0]))) {
            snprintf(o->ct[o->nct].name, NAME_MAX_, "%s", s);
            o->ct[o->nct++].off = o->len;
        }
        dot = strchr(s, '.');
        ll = dot ? (size_t)(dot - s) : strlen(s);
        if (ll > 63)
            ll = 63;
        o->b[o->len++] = (uint8_t)ll;
        memcpy(o->b + o->len, s, ll);
        o->len += (int)ll;
        s = dot ? dot + 1 : s + strlen(s);
    }
}

static void buf_start(dns_buf *o, uint16_t id, uint16_t flags, const char *qname, uint16_t qtype)
{
    memset(o, 0, sizeof(*o));
    nl_put16(o->b, id);
    nl_put16(o->b + 2, flags);
    nl_put16(o->b + 4, 1);
    o->len = 12;
    put_name(o, qname);
    nl_put16(o->b + o->len, qtype);
    nl_put16(o->b + o->len + 2, 1);
    o->len += 4;
}

/* Sections must be filled in order: answers, authority, additional */
static void put_rr(dns_buf *o, int sec, const dns_rr *r, uint32_t ttl)
{
    int lenpos, start;

    if (o->len > (int)sizeof(o->b) - 300)
        return;
    put_name(o, r->name);
    nl_put16(o->b + o->len, r->type);
    nl_put16(o->b + o->len + 2, 1);
    nl_put32(o->b + o->len + 4, ttl);
    lenpos = o->len + 8;
    o->len += 10;
    start = o->len;
    switch (r->type) {
    case T_A:
        nl_put32(o->b + o->len, r->ip);
        o->len += 4;
        break;
    case T_NS: case T_CNAME:
        put_name(o, r->text);
        break;
    case T_MX:
        nl_put16(o->b + o->len, r->pref);
        o->len += 2;
        put_name(o, r->text);
        break;
    case T_TXT: {
        size_t tl = strlen(r->text);
        o->b[o->len++] = (uint8_t)tl;
        memcpy(o->b + o->len, r->text, tl);
        o->len += (int)tl;
        break;
    }
    case T_SOA: {
        char rname[NAME_MAX_ + 16];
        snprintf(rname, sizeof(rname), "hostmaster%s%s", *r->name ? "." : "", r->name);
        rname[NAME_MAX_ - 1] = '\0';
        put_name(o, r->text);
        put_name(o, rname);
        nl_put32(o->b + o->len, 1);             /* serial */
        nl_put32(o->b + o->len + 4, 3600);      /* refresh */
        nl_put32(o->b + o->len + 8, 600);       /* retry */
        nl_put32(o->b + o->len + 12, 86400);    /* expire */
        nl_put32(o->b + o->len + 16, r->ip);    /* minimum = negative TTL */
        o->len += 20;
        break;
    }
    default:
        break;
    }
    nl_put16(o->b + lenpos, (uint16_t)(o->len - start));
    o->cnt[sec]++;
    nl_put16(o->b + 6 + 2 * sec, (uint16_t)o->cnt[sec]);
}

static void buf_rcode(dns_buf *o, int rcode, int flags_on, int flags_off)
{
    uint16_t f = nl_get16(o->b + 2);
    f = (uint16_t)(((f | flags_on) & ~flags_off & ~0xF) | rcode);
    nl_put16(o->b + 2, f);
}

/* ------------------------------------------------------------------ */
/* Server state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char     origin[NAME_MAX_];
    uint32_t ttl, negttl;
    dns_rr   rr[64];
    int      nrr;
} dns_zone;

struct dns_state;

typedef struct {
    struct dns_state *st;
    int      used;
    char     qname[NAME_MAX_], cur[NAME_MAX_], zone[NAME_MAX_];
    uint16_t qtype, client_id, client_flags, qid;
    nl_ip    client, server;
    uint16_t client_port, sport;
    int      tries, steps, upstream;
    dns_rr   ans[10];
    int      nans;
    nl_time  t0;
    nl_ev    timer;
} dns_job;

#define CACHE_MAX 160
#define JOB_MAX   12

typedef struct dns_state {
    nl_node  *n;
    dns_zone *zone[4];
    int       nzone;
    int       resolver, randomize;
    nl_ip     root;
    uint16_t  next_id;
    nl_time   timeout;
    dns_rr    cache[CACHE_MAX];
    dns_job   job[JOB_MAX];
    unsigned  queries, hits, upstream, mismatched, referrals, nx;
} dns_state;

static const nl_module dns_mod;

static void dns_reply(dns_state *st, const dns_buf *o, nl_ip dst, uint16_t dport)
{
    nl_udp_send(st->n, DNS_PORT, dst, dport, o->b, o->len);
}

/* ---- authoritative ---- */

static dns_zone *zone_for(dns_state *st, const char *name)
{
    dns_zone *best = NULL;
    int i;

    for (i = 0; i < st->nzone; i++)
        if (name_in(name, st->zone[i]->origin) && (!best || strlen(st->zone[i]->origin) > strlen(best->origin)))
            best = st->zone[i];
    return best;
}

static int zone_has_name(const dns_zone *z, const char *name)
{
    int i;

    for (i = 0; i < z->nrr; i++)
        if (name_in(z->rr[i].name, name))       /* also empty non-terminals */
            return 1;
    return strcmp(name, z->origin) == 0;
}

static void zone_soa(const dns_zone *z, dns_rr *soa)
{
    int i;

    memset(soa, 0, sizeof(*soa));
    strcpy(soa->name, z->origin);
    soa->type = T_SOA;
    soa->ip = z->negttl;
    snprintf(soa->text, sizeof(soa->text), "ns%s%.60s", *z->origin ? "." : "", z->origin);
    for (i = 0; i < z->nrr; i++)
        if (z->rr[i].type == T_NS && strcmp(z->rr[i].name, z->origin) == 0) {
            strcpy(soa->text, z->rr[i].text);
            break;
        }
}

static void add_glue(dns_state *st, dns_buf *o, const char *target)
{
    dns_zone *z = zone_for(st, target);
    int i, k;

    /* glue may live in any of our zones (the parent holds it for the child) */
    for (k = 0; k < st->nzone; k++) {
        z = st->zone[k];
        for (i = 0; i < z->nrr; i++)
            if (z->rr[i].type == T_A && strcmp(z->rr[i].name, target) == 0)
                put_rr(o, SEC_AR, &z->rr[i], z->rr[i].ttl);
    }
}

/* Returns a short description of what was answered (for the log) */
static const char *auth_answer(dns_state *st, const dns_msg *q, dns_buf *o)
{
    static char what[160];
    dns_zone *z = zone_for(st, q->qname);
    char name[NAME_MAX_], targets[6][NAME_MAX_];
    const dns_rr *deleg[6];
    dns_rr soa;
    int ndeleg = 0, ntargets = 0, nans = 0, chain, i, rcode = RC_OK, aa = 1, nodata = 0;

    buf_start(o, q->id, (uint16_t)(F_QR | (q->flags & F_RD)), q->qname, q->qtype);
    if (!z) {
        buf_rcode(o, RC_REFUSED, 0, 0);
        snprintf(what, sizeof(what), "REFUSED (%s)", _("not my zone"));
        return what;
    }
    strcpy(name, q->qname);
    what[0] = '\0';
    for (chain = 0; chain < 8; chain++) {
        const char *c, *cut = NULL;
        const dns_rr *cname = NULL;
        int found = 0;

        /* a delegation between the zone apex and the name? */
        for (c = name; strcmp(c, z->origin) != 0; c = name_parent(c)) {
            for (i = 0; i < z->nrr; i++)
                if (z->rr[i].type == T_NS && strcmp(z->rr[i].name, c) == 0)
                    cut = c;
            if (!*c)
                break;
        }
        if (cut) {
            for (i = 0; i < z->nrr && ndeleg < 6; i++)
                if (z->rr[i].type == T_NS && strcmp(z->rr[i].name, cut) == 0)
                    deleg[ndeleg++] = &z->rr[i];
            aa = 0;
            break;
        }
        for (i = 0; i < z->nrr; i++) {
            const dns_rr *r = &z->rr[i];
            if (strcmp(r->name, name) != 0)
                continue;
            if (r->type == q->qtype) {
                put_rr(o, SEC_AN, r, r->ttl);
                nans++;
                found = 1;
                if (!what[0])
                    snprintf(what, sizeof(what), "%s", rr_str(r));
                if ((r->type == T_MX || r->type == T_NS) && ntargets < 6)
                    strcpy(targets[ntargets++], r->text);
            } else if (r->type == T_CNAME) {
                cname = r;
            }
        }
        if (found)
            break;
        if (cname) {
            put_rr(o, SEC_AN, cname, cname->ttl);
            nans++;
            if (!what[0])
                snprintf(what, sizeof(what), "%s", rr_str(cname));
            strcpy(name, cname->text);
            if (!name_in(name, z->origin))
                break;
            continue;
        }
        if (zone_has_name(z, name))
            nodata = 1;
        else if (!nans)
            rcode = RC_NXDOMAIN;
        else
            nodata = 1;
        break;
    }
    if (ndeleg) {
        for (i = 0; i < ndeleg; i++)
            put_rr(o, SEC_NS, deleg[i], deleg[i]->ttl);
        for (i = 0; i < ndeleg; i++)
            add_glue(st, o, deleg[i]->text);
        st->referrals++;
        snprintf(what, sizeof(what), "%s %s → %s", _("referral:"), name_show(deleg[0]->name), deleg[0]->text);
    } else if (rcode == RC_NXDOMAIN || (nodata && !nans)) {
        zone_soa(z, &soa);
        put_rr(o, SEC_NS, &soa, z->negttl);
        if (rcode == RC_NXDOMAIN) {
            st->nx++;
            snprintf(what, sizeof(what), "NXDOMAIN (%s)", _("no such name"));
        } else {
            snprintf(what, sizeof(what), "NODATA (%s)", _("the name exists, but not with this type"));
        }
    } else {
        for (i = 0; i < ntargets; i++)
            add_glue(st, o, targets[i]);
    }
    buf_rcode(o, rcode, aa ? F_AA : 0, 0);
    return what;
}

/* ---- cache ---- */

static void cache_put(dns_state *st, const dns_rr *r, uint32_t ttl, int neg)
{
    dns_rr *slot = NULL;
    nl_time now = nl_now();
    int i;

    if (!ttl)
        return;
    for (i = 0; i < CACHE_MAX; i++) {
        dns_rr *c = &st->cache[i];
        if (c->expire && c->expire <= now)
            c->expire = 0;
        if (c->expire && c->type == r->type && c->neg == neg && strcmp(c->name, r->name) == 0 &&
            (neg || (c->ip == r->ip && strcmp(c->text, r->text) == 0))) {
            slot = c;
            break;
        }
        if (!slot || (!c->expire && slot->expire) || (c->expire && slot->expire && c->expire < slot->expire))
            slot = c;
    }
    *slot = *r;
    slot->neg = neg;
    slot->ttl = ttl;
    slot->expire = now + NL_SEC(ttl);
}

static dns_rr *cache_next(dns_state *st, const char *name, int type, int *iter)
{
    nl_time now = nl_now();

    for (; *iter < CACHE_MAX; (*iter)++) {
        dns_rr *c = &st->cache[*iter];
        if (c->expire > now && strcmp(c->name, name) == 0 &&
            (c->neg == 2 || (c->type == type)) ) {
            (*iter)++;
            return c;
        }
    }
    return NULL;
}

static uint32_t cache_ttl(const dns_rr *c)
{
    nl_time left = c->expire - nl_now();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
}

/* ---- resolver ---- */

static void job_step(dns_job *j);

static void job_free(dns_job *j)
{
    nl_cancel(&j->timer);
    if (j->sport && j->sport != DNS_PORT)
        nl_udp_unbind(j->st->n, j->sport);
    j->used = 0;
    j->sport = 0;
}

static void job_finish(dns_job *j, int rcode)
{
    dns_state *st = j->st;
    dns_buf o;
    double ms = (nl_now() - j->t0) / 1000.0;
    int i;

    buf_start(&o, j->client_id, (uint16_t)(F_QR | F_RA | (j->client_flags & F_RD)), j->qname, j->qtype);
    for (i = 0; i < j->nans; i++)
        put_rr(&o, SEC_AN, &j->ans[i], j->ans[i].ttl);
    buf_rcode(&o, rcode, 0, 0);
    if (!j->upstream) {
        st->hits++;
        nl_count(st->n, "dns.cache_hits", 1);
    }
    nl_sample(st->n, "resolution (ms)", ms);
    if (rcode == RC_OK && j->nans)
        nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s → " T_OK "%s" T_RESET T_DIM "  (%d %s, %.1f ms%s%s)",
               type_name(j->qtype), name_show(j->qname), rr_str(&j->ans[j->nans - 1]), j->upstream,
               _("upstream queries"), ms, j->upstream ? "" : ", ", j->upstream ? "" : _("from the cache"));
    else
        nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s → " T_WARN "%s" T_RESET T_DIM "  (%d %s%s%s)",
               type_name(j->qtype), name_show(j->qname),
               rcode == RC_NXDOMAIN ? "NXDOMAIN" : rcode == RC_SERVFAIL ? "SERVFAIL" : "NODATA", j->upstream,
               _("upstream queries"), j->upstream ? "" : ", ", j->upstream ? "" : _("from the cache"));
    if (rcode == RC_SERVFAIL)
        nl_count(st->n, "dns.servfail", 1);
    dns_reply(st, &o, j->client, j->client_port);
    job_free(j);
}

static void job_append(dns_job *j, const dns_rr *r, uint32_t ttl)
{
    if (j->nans < (int)(sizeof(j->ans) / sizeof(j->ans[0]))) {
        j->ans[j->nans] = *r;
        j->ans[j->nans++].ttl = ttl;
    }
}

static void job_send(dns_job *j);

static void job_timeout(void *ctx)
{
    dns_job *j = ctx;

    j->timer = 0;
    if (++j->tries >= 3) {
        nl_log(j->st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " " T_ERR "%s %s", _("no answer from"),
               nl_ip_str(j->server));
        job_finish(j, RC_SERVFAIL);
        return;
    }
    nl_log(j->st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s, %s", _("timeout waiting for"),
           nl_ip_str(j->server), _("asking again"));
    job_send(j);
}

static void resolver_rx(nl_node *n, void *ctx, const nl_udpmsg *m);

static void job_send(dns_job *j)
{
    dns_state *st = j->st;
    dns_buf o;
    int k;

    if (j->sport && j->sport != DNS_PORT)
        nl_udp_unbind(st->n, j->sport);
    if (st->randomize) {
        /* unpredictable source port and identifier: what an off-path forger must guess */
        do {
            j->sport = (uint16_t)(49152 + nl_rand(16384));
            for (k = 0; k < JOB_MAX; k++)
                if (&st->job[k] != j && st->job[k].used && st->job[k].sport == j->sport)
                    break;
        } while (k < JOB_MAX);
        nl_udp_bind(st->n, j->sport, resolver_rx, st);
        j->qid = (uint16_t)nl_rand(65536);
    } else {
        j->sport = DNS_PORT;
        j->qid = ++st->next_id;
    }
    buf_start(&o, j->qid, 0, j->cur, j->qtype);
    j->upstream++;
    st->upstream++;
    nl_count(st->n, "dns.upstream_queries", 1);
    nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s %s? → %s " T_DIM "(%s %s, id %u, port %u)", _("ask"),
           type_name(j->qtype), name_show(j->cur), nl_ip_str(j->server), _("server for"), name_show(j->zone),
           j->qid, j->sport);
    nl_udp_send(st->n, j->sport, j->server, DNS_PORT, o.b, o.len);
    nl_cancel(&j->timer);
    j->timer = nl_after(st->n, st->timeout, job_timeout, j);
}

/* Consult the cache, then pick the closest known name server and ask it */
static void job_step(dns_job *j)
{
    dns_state *st = j->st;

    for (;;) {
        dns_rr *c;
        int it = 0, got = 0;
        const char *z;

        if (++j->steps > 24) {
            job_finish(j, RC_SERVFAIL);
            return;
        }
        while ((c = cache_next(st, j->cur, j->qtype, &it)) != NULL) {
            if (c->neg) {
                job_finish(j, c->neg == 2 && !j->nans ? RC_NXDOMAIN : RC_OK);
                return;
            }
            job_append(j, c, cache_ttl(c));
            got = 1;
        }
        if (got) {
            job_finish(j, RC_OK);
            return;
        }
        it = 0;
        if (j->qtype != T_CNAME && (c = cache_next(st, j->cur, T_CNAME, &it)) != NULL && !c->neg) {
            job_append(j, c, cache_ttl(c));
            strcpy(j->cur, c->text);
            continue;
        }
        /* closest enclosing zone whose server address we know */
        j->server = 0;
        for (z = j->cur; !j->server; z = name_parent(z)) {
            int it2 = 0;
            dns_rr *ns;

            while (!j->server && (ns = cache_next(st, z, T_NS, &it2)) != NULL) {
                int it3 = 0;
                dns_rr *a = ns->neg ? NULL : cache_next(st, ns->text, T_A, &it3);
                if (a && !a->neg) {
                    j->server = a->ip;
                    strcpy(j->zone, z);
                }
            }
            if (!*z)
                break;
        }
        if (!j->server) {
            j->server = st->root;
            j->zone[0] = '\0';
        }
        j->tries = 0;
        job_send(j);
        return;
    }
}

static void job_response(dns_job *j, const dns_msg *r)
{
    dns_state *st = j->st;
    int i, chain, referral = 0, rcode = r->flags & 0xF;
    uint32_t negttl = 30;

    nl_cancel(&j->timer);
    /* cache what the server is entitled to say: records inside its zone */
    for (i = 0; i < r->nrr; i++) {
        const dns_rr *x = &r->rr[i];
        if (x->type == T_SOA) {
            negttl = x->ip < x->ttl ? x->ip : x->ttl;
            continue;
        }
        if (!name_in(x->name, j->zone)) {
            nl_log(st->n, NL_L_APP, 3, 0, T_FIN "DNS" T_RESET " %s %s (%s %s)", _("ignored out-of-bailiwick record"),
                   name_show(x->name), _("server for"), name_show(j->zone));
            continue;
        }
        if (x->type == T_A || x->type == T_NS || x->type == T_CNAME || x->type == T_MX || x->type == T_TXT)
            cache_put(st, x, x->ttl, 0);
        if (x->sec == SEC_NS && x->type == T_NS && strlen(x->name) > strlen(j->zone) && name_in(j->cur, x->name))
            referral = 1;
    }
    if (rcode == RC_NXDOMAIN) {
        dns_rr neg;
        memset(&neg, 0, sizeof(neg));
        strcpy(neg.name, j->cur);
        neg.type = j->qtype;
        cache_put(st, &neg, negttl, 2);
        nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s: %s NXDOMAIN " T_DIM "(%s %u s)", nl_ip_str(j->server),
               name_show(j->cur), _("negative answer cached for"), negttl);
        job_finish(j, j->nans ? RC_OK : RC_NXDOMAIN);
        return;
    }
    if (rcode != RC_OK) {
        job_finish(j, RC_SERVFAIL);
        return;
    }
    /* answers in the message, following CNAMEs inside it */
    for (chain = 0; chain < 8; chain++) {
        int got = 0, moved = 0;

        for (i = 0; i < r->nrr; i++)
            if (r->rr[i].sec == SEC_AN && r->rr[i].type == j->qtype && strcmp(r->rr[i].name, j->cur) == 0) {
                job_append(j, &r->rr[i], r->rr[i].ttl);
                got = 1;
            }
        if (got) {
            nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s: %s %s", nl_ip_str(j->server), _("answer"),
                   rr_str(&j->ans[j->nans - 1]));
            job_finish(j, RC_OK);
            return;
        }
        for (i = 0; i < r->nrr && !moved; i++)
            if (r->rr[i].sec == SEC_AN && r->rr[i].type == T_CNAME && strcmp(r->rr[i].name, j->cur) == 0) {
                job_append(j, &r->rr[i], r->rr[i].ttl);
                nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s: %s %s → %s", nl_ip_str(j->server),
                       _("alias (CNAME)"), name_show(j->cur), r->rr[i].text);
                strcpy(j->cur, r->rr[i].text);
                moved = 1;
            }
        if (!moved)
            break;
        referral = 2;                   /* keep resolving the new name */
    }
    if (referral == 1) {
        for (i = 0; i < r->nrr; i++)
            if (r->rr[i].sec == SEC_NS && r->rr[i].type == T_NS) {
                nl_log(st->n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s: %s %s → %s", nl_ip_str(j->server),
                       _("referral:"), name_show(r->rr[i].name), r->rr[i].text);
                break;
            }
    }
    if (referral) {
        job_step(j);
        return;
    }
    /* no error, no data */
    {
        dns_rr neg;
        memset(&neg, 0, sizeof(neg));
        strcpy(neg.name, j->cur);
        neg.type = j->qtype;
        cache_put(st, &neg, negttl, 1);
        job_finish(j, RC_OK);
    }
}

static void resolver_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    dns_state *st = ctx;
    dns_msg *r = malloc(sizeof(*r));
    int i;

    if (!r)
        return;
    if (msg_parse(m->data, m->len, r) && (r->flags & F_QR)) {
        for (i = 0; i < JOB_MAX; i++) {
            dns_job *j = &st->job[i];
            if (j->used && j->timer && j->sport == m->dport && j->qid == r->id && j->server == m->src &&
                m->sport == DNS_PORT && strcmp(r->qname, j->cur) == 0 && r->qtype == j->qtype) {
                job_response(j, r);
                free(r);
                return;
            }
        }
        st->mismatched++;
        nl_count(n, "dns.mismatched", 1);
        nl_log(n, NL_L_APP, 3, m->pkt_id, T_FIN "DNS" T_RESET " %s %s (id %u, port %u)",
               _("response matching no pending query, dropped: from"), nl_ip_str(m->src), r->id, m->dport);
    }
    free(r);
}

static void resolver_query(dns_state *st, const dns_msg *q, const nl_udpmsg *m)
{
    dns_job *j = NULL;
    int i;

    for (i = 0; i < JOB_MAX && !j; i++)
        if (!st->job[i].used)
            j = &st->job[i];
    if (!j) {
        dns_buf o;
        buf_start(&o, q->id, (uint16_t)(F_QR | F_RA | (q->flags & F_RD)), q->qname, q->qtype);
        buf_rcode(&o, RC_SERVFAIL, 0, 0);
        dns_reply(st, &o, m->src, m->sport);
        return;
    }
    memset(j, 0, sizeof(*j));
    j->st = st;
    j->used = 1;
    strcpy(j->qname, q->qname);
    strcpy(j->cur, q->qname);
    j->qtype = q->qtype;
    j->client = m->src;
    j->client_port = m->sport;
    j->client_id = q->id;
    j->client_flags = q->flags;
    j->t0 = nl_now();
    nl_log(st->n, NL_L_APP, 2, m->pkt_id, T_FIN "DNS" T_RESET " %s %s %s? %s %s", _("recursive query"),
           type_name(q->qtype), name_show(q->qname), _("from"), nl_ip_str(m->src));
    job_step(j);
}

static void dns_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    dns_state *st = ctx;
    dns_msg *q;
    dns_buf o;
    const char *what;

    if (m->len >= 4 && (nl_get16(m->data + 2) & F_QR)) {
        if (st->resolver)
            resolver_rx(n, st, m);      /* fixed source port 53: answers come back here */
        return;
    }
    q = malloc(sizeof(*q));
    if (!q)
        return;
    if (!msg_parse(m->data, m->len, q) || !q->nq) {
        free(q);
        return;
    }
    st->queries++;
    nl_count(n, "dns.queries", 1);
    if (st->resolver && ((q->flags & F_RD) || !zone_for(st, q->qname))) {
        resolver_query(st, q, m);
    } else {
        what = auth_answer(st, q, &o);
        nl_log(n, NL_L_APP, 2, m->pkt_id, T_FIN "DNS" T_RESET " %s %s? %s %s → %s", type_name(q->qtype),
               name_show(q->qname), _("from"), nl_ip_str(m->src), what);
        dns_reply(st, &o, m->src, m->sport);
    }
    free(q);
}

/* ---- configuration ---- */

static dns_zone *zone_get(dns_state *st, const char *origin_in)
{
    char origin[NAME_MAX_];
    dns_zone *z;
    int i;

    name_norm(origin_in, origin);
    for (i = 0; i < st->nzone; i++)
        if (strcmp(st->zone[i]->origin, origin) == 0)
            return st->zone[i];
    if (st->nzone >= 4 || (z = calloc(1, sizeof(*z))) == NULL)
        return NULL;
    strcpy(z->origin, origin);
    z->ttl = 60;
    z->negttl = 30;
    st->zone[st->nzone++] = z;
    return z;
}

static uint32_t parse_secs(const char *s) { return (uint32_t)strtoul(s, NULL, 10); }

static int zone_add(dns_state *st, dns_zone *z, const char *key, const char *val, uint32_t ttl)
{
    char owner[NAME_MAX_], rest[160];
    char *colon, *colon2;
    dns_rr *r;

    snprintf(rest, sizeof(rest), "%s", val);
    colon = strchr(rest, ':');
    if (!colon || z->nrr >= (int)(sizeof(z->rr) / sizeof(z->rr[0])))
        return -1;
    *colon++ = '\0';
    name_fq(rest, z->origin, owner);
    r = &z->rr[z->nrr];
    memset(r, 0, sizeof(*r));
    strcpy(r->name, owner);
    r->ttl = ttl;
    if (!strcmp(key, "a") || !strcmp(key, "glue")) {
        r->type = T_A;
        if (!nl_resolve(colon, &r->ip)) {
            nl_log(st->n, NL_L_APP, 1, 0, T_ERR "dns: %s '%s'", _("unknown address"), colon);
            return -1;
        }
    } else if (!strcmp(key, "ns")) {
        r->type = T_NS;
        name_fq(colon, z->origin, r->text);
    } else if (!strcmp(key, "cname")) {
        r->type = T_CNAME;
        name_fq(colon, z->origin, r->text);
    } else if (!strcmp(key, "mx")) {
        r->type = T_MX;
        r->pref = 10;
        colon2 = strchr(colon, ':');
        if (colon2) {
            *colon2 = '\0';
            r->pref = (uint16_t)atoi(colon2 + 1);
        }
        name_fq(colon, z->origin, r->text);
    } else if (!strcmp(key, "txt")) {
        r->type = T_TXT;
        snprintf(r->text, sizeof(r->text), "%s", colon);
    } else {
        return -1;
    }
    z->nrr++;
    return 0;
}

static int dns_run(nl_node *n, const nl_args *a)
{
    dns_state *st = nl_mod_get(n, &dns_mod);
    const char *role = nl_arg_pos(a, 0);
    dns_zone *z = NULL;
    uint32_t ttl = 0;
    int k;

    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->n = n;
        st->randomize = 1;
        st->timeout = NL_MS(800);
        nl_mod_set(n, &dns_mod, st);
        nl_udp_bind(n, DNS_PORT, dns_rx, st);
    }
    if (role && strcmp(role, "flush") == 0) {
        memset(st->cache, 0, sizeof(st->cache));
        nl_log(n, NL_L_APP, 1, 0, T_FIN "DNS" T_RESET " %s", _("cache emptied"));
        return 0;
    }
    if (role && strcmp(role, "resolver") == 0) {
        const char *root = nl_arg_str(a, "root", NULL);

        st->resolver = 1;
        if (!root || !nl_resolve(root, &st->root)) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "dns: %s", _("resolver needs root=ADDRESS (a root name server)"));
            return -1;
        }
        st->randomize = (int)nl_arg_int(a, "randomize", st->randomize);
        st->timeout = nl_arg_time(a, "timeout", st->timeout);
        nl_log(n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s%s", _("recursive resolver ready, root server"),
               nl_ip_str(st->root), st->randomize ? "" : _(" — fixed source port, sequential ids"));
    }
    if (nl_arg_has(a, "ttl"))
        ttl = parse_secs(nl_arg_str(a, "ttl", "60"));
    for (k = 0; k < a->argc; k++) {
        const char *tok = a->argv[k], *eq = strchr(tok, '=');
        char key[16];

        if (!eq)
            continue;
        snprintf(key, sizeof(key), "%.*s", (int)(eq - tok), tok);
        if (!strcmp(key, "zone")) {
            z = zone_get(st, eq + 1);
            if (!z) {
                nl_log(n, NL_L_APP, 1, 0, T_ERR "dns: %s", _("too many zones"));
                return -1;
            }
            if (ttl)
                z->ttl = ttl;
            if (z->negttl > z->ttl)
                z->negttl = z->ttl;
            if (nl_arg_has(a, "negttl"))
                z->negttl = parse_secs(nl_arg_str(a, "negttl", "30"));
        } else if (!strcmp(key, "a") || !strcmp(key, "glue") || !strcmp(key, "ns") || !strcmp(key, "cname") ||
                   !strcmp(key, "mx") || !strcmp(key, "txt")) {
            if (!z && st->nzone)
                z = st->zone[st->nzone - 1];
            if (!z || zone_add(st, z, key, eq + 1, ttl ? ttl : z->ttl) != 0) {
                nl_log(n, NL_L_APP, 1, 0, T_ERR "dns: %s '%s'", _("bad record (give zone=NAME first, then key=owner:value)"),
                       tok);
                return -1;
            }
        }
    }
    if (z)
        nl_log(n, NL_L_APP, 2, 0, T_FIN "DNS" T_RESET " %s %s (%d %s)", _("authoritative for zone"),
               name_show(z->origin), z->nrr, _("records"));
    return 0;
}

static void dns_stop(nl_node *n)
{
    dns_state *st = nl_mod_get(n, &dns_mod);
    int i;

    if (!st)
        return;
    for (i = 0; i < JOB_MAX; i++)
        if (st->job[i].used)
            job_free(&st->job[i]);
    for (i = 0; i < st->nzone; i++)
        free(st->zone[i]);
    nl_udp_unbind(n, DNS_PORT);
    nl_mod_set(n, &dns_mod, NULL);
    free(st);
}

/* A crash loses the cache and the queries in progress; zones are on disk */
static void dns_crash(nl_node *n)
{
    dns_state *st = nl_mod_get(n, &dns_mod);
    int i;

    for (i = 0; i < JOB_MAX; i++)
        if (st->job[i].used)
            job_free(&st->job[i]);
    memset(st->cache, 0, sizeof(st->cache));
}

static int dns_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    dns_state *st = nl_mod_get(n, &dns_mod);
    nl_time now = nl_now();
    int row = 0, i, k;

    row += nl_ui_kv(x, y + row, w, _("queries received"), "%u", st->queries);
    if (st->resolver) {
        row += nl_ui_kv(x, y + row, w, _("root server"), "%s", nl_ip_str(st->root));
        row += nl_ui_kv(x, y + row, w, _("answered from cache"), "%u", st->hits);
        row += nl_ui_kv(x, y + row, w, _("upstream queries"), "%u", st->upstream);
        row += nl_ui_kv(x, y + row, w, _("source port / id"), "%s",
                        st->randomize ? _("random") : _("fixed 53 / sequential"));
        if (detail >= 2)
            row += nl_ui_kv(x, y + row, w, _("unmatched responses"), "%u", st->mismatched);
        if (row < h)
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("cache (remaining TTL)"));
        for (i = 0; i < CACHE_MAX && row < h; i++) {
            const dns_rr *c = &st->cache[i];
            if (c->expire <= now)
                continue;
            if (c->neg)
                tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_TIME "%4us" T_RESET " %-22s " T_WARN "%s",
                          cache_ttl(c), name_show(c->name), c->neg == 2 ? "NXDOMAIN" : "NODATA");
            else
                tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_TIME "%4us" T_RESET " %-22s %s", cache_ttl(c),
                          name_show(c->name), rr_str(c));
        }
    } else {
        row += nl_ui_kv(x, y + row, w, _("referrals / NXDOMAIN"), "%u / %u", st->referrals, st->nx);
    }
    for (k = 0; k < st->nzone && row < h; k++) {
        const dns_zone *z = st->zone[k];
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_DIM "%s" T_RESET " " T_BOLD "%s" T_RESET T_DIM "  TTL %u s",
                  _("zone"), name_show(z->origin), z->ttl);
        for (i = 0; i < z->nrr && row < h; i++)
            tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "  %-22s %s", name_show(z->rr[i].name),
                      rr_str(&z->rr[i]));
    }
    return row;
}

static const nl_module dns_mod = {
    .name = "dns", .title = N_("DNS server: authoritative zones and recursive resolver"), .layer = NL_L_APP,
    .usage = N_("run NODE dns zone=NAME [ttl=60] [negttl=30] RECORD...   authoritative server\n"
                "  records, as key=owner:value (owner @ = the zone itself, names are relative\n"
                "  to the zone unless they end with a dot, addresses may be node names):\n"
                "    a=www:10.0.3.10  cname=web:www  mx=@:mail[:10]  txt=@:hello\n"
                "    ns=@:ns1  ns=sub:ns.sub  glue=ns.sub:10.0.4.2   (ns below the apex = delegation)\n"
                "  the root zone is zone=. ; repeat the command to add records or zones.\n"
                "run NODE dns resolver root=ADDR [randomize=1] [timeout=800ms]   recursive resolver\n"
                "  randomize=0: fixed source port 53 and sequential ids (for the poisoning lab)\n"
                "run NODE dns flush     empty the resolver cache"),
    .theory = N_("The Domain Name System is a distributed database that maps names to records "
                 "(A: an IPv4 address, NS: the name servers of a zone, CNAME: an alias, MX: the mail "
                 "exchanger, TXT: free text). The name space is a tree cut into zones; each zone is "
                 "served by authoritative servers, and a zone delegates a subtree by publishing NS "
                 "records for it, plus the addresses of those servers (glue). Nobody knows "
                 "everything: the root servers only know who serves com, org, it...; those know who "
                 "serves example.com; and so on.\n\n"
                 "A host does not walk this tree itself. Its stub resolver sends one query with the "
                 "Recursion Desired bit to a recursive resolver, which iterates: it asks a root "
                 "server, gets a referral (no answer, NS records in the authority section, glue in "
                 "the additional section), asks the server it was referred to, and repeats until an "
                 "authoritative answer arrives, following CNAME aliases on the way. Every record "
                 "carries a TTL: the resolver caches it for that long, so the next query for the "
                 "same name, or for another name in the same zone, skips part or all of the walk. "
                 "Negative answers (NXDOMAIN, or a name without that record type) are cached too, for "
                 "the time given by the SOA record of the zone. Short TTLs let a change propagate "
                 "quickly, long TTLs save queries and hide server outages: that is the trade-off.\n\n"
                 "Messages have a 12-byte header (identifier, flags, four counters) followed by the "
                 "question and three record sections; repeated names are compressed with pointers. "
                 "DNS normally runs on UDP, so a resolver accepts a response when it comes from the "
                 "address it asked, to the port it used, with the identifier and question it sent: "
                 "an attacker who can guess those values can forge an answer and poison the cache, "
                 "which is why resolvers randomize both the identifier and the source port. Here: "
                 "class IN only, no TCP fallback, EDNS or DNSSEC, and delegations need glue."),
    .run = dns_run, .stop = dns_stop, .crash = dns_crash, .panel = dns_panel,
};
NL_MODULE(dns_mod)

/* ------------------------------------------------------------------ */
/* dig: the client                                                    */
/* ------------------------------------------------------------------ */

static nl_ip g_default_server[NL_MAX_NODES];

/* Used by DHCP (option 6) to configure the resolver of a host */
void nl_dns_set_default(nl_node *n, nl_ip server);
void nl_dns_set_default(nl_node *n, nl_ip server)
{
    g_default_server[n->idx] = server;
}

struct dig_state;

typedef struct {
    struct dig_state *st;
    int      used, trace, tries, hops;
    char     name[NAME_MAX_], cur[NAME_MAX_];
    uint16_t qtype, id, sport;
    nl_ip    server, root;
    nl_time  t0;
    nl_ev    timer;
} dig_q;

typedef struct dig_state {
    nl_node *n;
    dig_q    q[6];
    uint16_t next_id;
    char     hist[8][120];
    int      nhist;
} dig_state;

static const nl_module dig_mod;

static void dig_note(dig_state *st, const char *fmt, ...)
{
    va_list ap;
    char line[120];

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (st->nhist == 8) {
        memmove(st->hist[0], st->hist[1], sizeof(st->hist[0]) * 7);
        st->nhist--;
    }
    snprintf(st->hist[st->nhist++], sizeof(st->hist[0]), "%s", line);
}

static void dig_end(dig_q *q)
{
    nl_cancel(&q->timer);
    nl_udp_unbind(q->st->n, q->sport);
    q->used = 0;
}

static void dig_send(dig_q *q);

static void dig_timeout(void *ctx)
{
    dig_q *q = ctx;

    q->timer = 0;
    if (++q->tries >= 3) {
        nl_log(q->st->n, NL_L_APP, 1, 0, T_ERR "dig %s: %s %s", name_show(q->name), _("no answer from server"),
               nl_ip_str(q->server));
        nl_count(q->st->n, "dig.timeout", 1);
        dig_note(q->st, "%s %s: %s", type_name(q->qtype), name_show(q->name), _("timeout"));
        dig_end(q);
        return;
    }
    dig_send(q);
}

static void dig_send(dig_q *q)
{
    dns_buf o;

    buf_start(&o, q->id, q->trace ? 0 : F_RD, q->cur, q->qtype);
    nl_udp_send(q->st->n, q->sport, q->server, DNS_PORT, o.b, o.len);
    nl_cancel(&q->timer);
    q->timer = nl_after(q->st->n, NL_SEC(3), dig_timeout, q);
}

static void dig_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    dig_state *st = ctx;
    dns_msg *r = malloc(sizeof(*r));
    dig_q *q = NULL;
    const dns_rr *final = NULL;
    double ms;
    int i, rcode;

    if (!r)
        return;
    if (msg_parse(m->data, m->len, r) && (r->flags & F_QR))
        for (i = 0; i < 6 && !q; i++)
            if (st->q[i].used && st->q[i].sport == m->dport && st->q[i].id == r->id)
                q = &st->q[i];
    if (!q) {
        free(r);
        return;
    }
    rcode = r->flags & 0xF;
    ms = (nl_now() - q->t0) / 1000.0;
    for (i = 0; i < r->nrr; i++)
        if (r->rr[i].sec == SEC_AN) {
            nl_log(n, NL_L_APP, q->trace ? 1 : 2, m->pkt_id, "dig %s:   %-20s %5u s  %s", name_show(q->name),
                   name_show(r->rr[i].name), r->rr[i].ttl, rr_str(&r->rr[i]));
            if (r->rr[i].type == q->qtype)
                final = &r->rr[i];
        }
    if (q->trace && !final && rcode == RC_OK) {
        const dns_rr *ns = NULL, *cname = NULL;
        nl_ip next = 0;
        int k;

        for (i = 0; i < r->nrr; i++) {
            if (r->rr[i].sec == SEC_AN && r->rr[i].type == T_CNAME && strcmp(r->rr[i].name, q->cur) == 0)
                cname = &r->rr[i];
            if (r->rr[i].sec == SEC_NS && r->rr[i].type == T_NS && !next) {
                for (k = 0; k < r->nrr; k++)
                    if (r->rr[k].sec == SEC_AR && r->rr[k].type == T_A && strcmp(r->rr[k].name, r->rr[i].text) == 0) {
                        ns = &r->rr[i];
                        next = r->rr[k].ip;
                    }
            }
        }
        if ((cname || next) && ++q->hops < 12) {
            if (cname) {
                strcpy(q->cur, cname->text);
                next = q->root;
                nl_log(n, NL_L_APP, 1, m->pkt_id, "dig %s: " T_INFO "%s" T_RESET " %s %s", name_show(q->name),
                       nl_ip_str(m->src), _("says it is an alias: start again from the root for"), q->cur);
            } else {
                nl_log(n, NL_L_APP, 1, m->pkt_id, "dig %s: " T_INFO "%s" T_RESET " %s %s → %s (%s)",
                       name_show(q->name), nl_ip_str(m->src), _("refers to the servers of"), name_show(ns->name),
                       ns->text, nl_ip_str(next));
            }
            q->server = next;
            q->id = ++st->next_id;
            q->tries = 0;
            dig_send(q);
            free(r);
            return;
        }
    }
    if (final) {
        nl_count(n, "dig.answers", 1);
        if (final->type == T_A)
            nl_count_set(n, "dig.last_a", (double)final->ip);
        nl_sample(n, "dig time (ms)", ms);
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "dig %s: %s" T_RESET "  " T_DIM "(TTL %u s, %s %s, %.1f ms%s)",
               name_show(q->name), rr_str(final), final->ttl, _("server"), nl_ip_str(m->src), ms,
               (r->flags & F_AA) ? _(", authoritative") : "");
        dig_note(st, "%s %s → %s (%.1f ms)", type_name(q->qtype), name_show(q->name), rr_str(final), ms);
    } else if (rcode == RC_NXDOMAIN) {
        nl_count(n, "dig.nxdomain", 1);
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_WARN "dig %s: NXDOMAIN" T_RESET " — %s " T_DIM "(%.1f ms)",
               name_show(q->name), _("the name does not exist"), ms);
        dig_note(st, "%s %s → NXDOMAIN (%.1f ms)", type_name(q->qtype), name_show(q->name), ms);
    } else if (rcode != RC_OK) {
        nl_count(n, "dig.failed", 1);
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_ERR "dig %s: %s" T_RESET " — %s", name_show(q->name),
               rcode == RC_REFUSED ? "REFUSED" : "SERVFAIL", _("the server could not answer"));
        dig_note(st, "%s %s → %s", type_name(q->qtype), name_show(q->name), _("failed"));
    } else {
        nl_count(n, "dig.nodata", 1);
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_WARN "dig %s:" T_RESET " %s %s " T_DIM "(%.1f ms)", name_show(q->name),
               _("the name exists but has no record of type"), type_name(q->qtype), ms);
        dig_note(st, "%s %s → NODATA", type_name(q->qtype), name_show(q->name));
    }
    dig_end(q);
    free(r);
}

static int dig_run(nl_node *n, const nl_args *a)
{
    dig_state *st = nl_mod_get(n, &dig_mod);
    const char *name = nl_arg_pos(a, 0), *srv = nl_arg_str(a, "server", NULL), *root = nl_arg_str(a, "root", NULL);
    dig_q *q = NULL;
    nl_ip server = g_default_server[n->idx];
    int i, type;

    if (!st) {
        st = calloc(1, sizeof(*st));
        if (!st)
            return -1;
        st->n = n;
        st->next_id = (uint16_t)(n->idx * 512);
        nl_mod_set(n, &dig_mod, st);
    }
    if (srv) {
        if (!nl_resolve(srv, &server)) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "dig: %s '%s'", _("unknown server"), srv);
            return -1;
        }
        g_default_server[n->idx] = server;
    }
    if (!name)
        return srv ? 0 : -1;
    type = type_parse(nl_arg_str(a, "type", "A"));
    if (!type) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dig: %s", _("unknown record type (A, NS, CNAME, MX, TXT, SOA)"));
        return -1;
    }
    for (i = 0; i < 6 && !q; i++)
        if (!st->q[i].used)
            q = &st->q[i];
    if (!q)
        return -1;
    memset(q, 0, sizeof(*q));
    q->trace = (int)nl_arg_int(a, "trace", 0);
    if (q->trace && root && !nl_resolve(root, &server)) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dig: %s '%s'", _("unknown server"), root);
        return -1;
    }
    if (!server) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dig: %s", _("no name server configured (use server=ADDRESS, or DHCP)"));
        return -1;
    }
    q->st = st;
    q->used = 1;
    name_norm(name, q->name);
    strcpy(q->cur, q->name);
    q->qtype = (uint16_t)type;
    q->id = ++st->next_id;
    q->server = q->root = server;
    q->t0 = nl_now();
    q->sport = (uint16_t)nl_udp_bind(n, 0, dig_rx, st);
    nl_count(n, "dig.queries", 1);
    nl_log(n, NL_L_APP, 1, 0, T_INFO "dig %s" T_RESET " %s → %s %s%s", name_show(q->name), type_name(type), _("asking"),
           nl_ip_str(server), q->trace ? _(" and following the referrals myself") : "");
    dig_send(q);
    return 0;
}

static void dig_stop(nl_node *n)
{
    dig_state *st = nl_mod_get(n, &dig_mod);
    int i;

    if (!st)
        return;
    for (i = 0; i < 6; i++)
        if (st->q[i].used)
            dig_end(&st->q[i]);
    nl_mod_set(n, &dig_mod, NULL);
    free(st);
}

static void dig_crash(nl_node *n)
{
    dig_state *st = nl_mod_get(n, &dig_mod);
    int i;

    for (i = 0; i < 6; i++)
        if (st->q[i].used)
            dig_end(&st->q[i]);
}

static int dig_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    dig_state *st = nl_mod_get(n, &dig_mod);
    int row = 0, i;

    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("name server"), "%s",
                    g_default_server[n->idx] ? nl_ip_str(g_default_server[n->idx]) : "—");
    row += nl_ui_kv(x, y + row, w, _("answers / NXDOMAIN"), "%.0f / %.0f", nl_count_get(n, "dig.answers"),
                    nl_count_get(n, "dig.nxdomain"));
    for (i = st->nhist - 1; i >= 0 && row < h; i--)
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%s", st->hist[i]);
    if (h - row >= 5) {
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "dig time (ms)"), DNS_COLOR, "dig time (ms)");
        row = h;
    }
    return row;
}

static const nl_module dig_mod = {
    .name = "dig", .title = N_("dig — ask the DNS for a name"), .layer = NL_L_APP,
    .usage = N_("run NODE dig NAME [type=A|NS|CNAME|MX|TXT] [server=ADDR] [trace=1] [root=ADDR]\n"
                "  server= is remembered as the node's name server (DHCP sets it too).\n"
                "  trace=1: do not ask for recursion; start from the server (a root) and follow\n"
                "  every referral, showing each step.\n"
                "run NODE dig server=ADDR      only set the name server"),
    .theory = N_("dig is the stub resolver of a host: it sends a single query to the configured name "
                 "server with the Recursion Desired bit set and prints what comes back, with the TTL "
                 "of each record. With trace=1 it plays the part of the recursive resolver instead: "
                 "it asks a root server without recursion and follows the referrals one by one, so "
                 "you can see the delegation chain root → top-level domain → authoritative server."),
    .run = dig_run, .stop = dig_stop, .crash = dig_crash, .panel = dig_panel,
};
NL_MODULE(dig_mod)

/* ------------------------------------------------------------------ */
/* dnsspoof: an off-path forger for the cache-poisoning lab           */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_node *n;
    nl_ip    victim, as, fake;
    char     name[NAME_MAX_];
    int      guesses, sent, first_id, port, sport;
    uint32_t ttl;
    nl_time  gap;
    nl_ev    timer;
    unsigned attacks, wins;
} spoof_state;

static const nl_module spoof_mod;

static void spoof_tick(void *ctx)
{
    spoof_state *s = ctx;
    dns_buf o;
    dns_rr r;
    nl_iptx tx;
    uint16_t id, port;

    s->timer = 0;
    if (s->sent >= s->guesses)
        return;
    /* port known (the resolver always uses the same one): walk the ids;
     * port unknown: both must be guessed blindly */
    id = s->port ? (uint16_t)(s->first_id + s->sent) : (uint16_t)nl_rand(65536);
    port = s->port ? (uint16_t)s->port : (uint16_t)(49152 + nl_rand(16384));
    buf_start(&o, id, F_QR | F_AA, s->name, T_A);
    memset(&r, 0, sizeof(r));
    strcpy(r.name, s->name);
    r.type = T_A;
    r.ip = s->fake;
    put_rr(&o, SEC_AN, &r, s->ttl);
    memset(&tx, 0, sizeof(tx));
    tx.src = s->as;                     /* forged source: the authoritative server */
    tx.dst = s->victim;
    nl_udp_send_ex(s->n, &tx, DNS_PORT, port, o.b, o.len);
    s->sent++;
    nl_count(s->n, "spoof.forged", 1);
    s->timer = nl_after(s->n, s->gap, spoof_tick, s);
}

static void spoof_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    spoof_state *s = ctx;
    dns_msg *r = malloc(sizeof(*r));
    int i, won = 0;

    if (!r)
        return;
    if (msg_parse(m->data, m->len, r))
        for (i = 0; i < r->nrr; i++)
            if (r->rr[i].sec == SEC_AN && r->rr[i].type == T_A && r->rr[i].ip == s->fake)
                won = 1;
    free(r);
    nl_cancel(&s->timer);
    if (won) {
        s->wins++;
        nl_count(n, "spoof.success", 1);
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_ERR "dnsspoof: %s" T_RESET " — %s %s → %s (%d %s)",
               _("cache POISONED"), _("the resolver now answers"), s->name, nl_ip_str(s->fake), s->sent,
               _("forged responses sent"));
    } else {
        nl_log(n, NL_L_APP, 1, m->pkt_id, T_OK "dnsspoof: %s" T_RESET " — %s (%d %s)", _("attack failed"),
               _("the genuine answer won the race"), s->sent, _("forged responses sent"));
    }
}

static int spoof_run(nl_node *n, const nl_args *a)
{
    spoof_state *s = nl_mod_get(n, &spoof_mod);
    const char *victim = nl_arg_str(a, "resolver", NULL), *as = nl_arg_str(a, "as", NULL);
    const char *name = nl_arg_str(a, "name", NULL), *fake = nl_arg_str(a, "fake", "6.6.6.6");
    dns_buf o;

    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        s->n = n;
        s->sport = nl_udp_bind(n, 0, spoof_rx, s);
        nl_mod_set(n, &spoof_mod, s);
    }
    if (!victim || !as || !name || !nl_resolve(victim, &s->victim) || !nl_resolve(as, &s->as) ||
        !nl_resolve(fake, &s->fake)) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "dnsspoof: %s", _("usage: dnsspoof resolver=ADDR as=ADDR name=NAME [fake=ADDR]"));
        return -1;
    }
    nl_cancel(&s->timer);
    name_norm(name, s->name);
    s->guesses = (int)nl_arg_int(a, "guesses", 64);
    s->first_id = (int)nl_arg_int(a, "firstid", 1);
    s->port = (int)nl_arg_int(a, "port", DNS_PORT);
    s->ttl = (uint32_t)nl_arg_int(a, "ttl", 3600);
    s->gap = nl_arg_time(a, "gap", NL_US(200));
    s->sent = 0;
    s->attacks++;
    nl_log(n, NL_L_APP, 1, 0, T_ERR "dnsspoof" T_RESET " %s %s: %s %s, %s %d %s %s %s",
           _("attack on resolver"), nl_ip_str(s->victim), _("trigger a lookup of"), s->name, _("then race"),
           s->guesses, _("forged answers pretending to come from"), nl_ip_str(s->as),
           s->port ? "" : _("(blind: random ports and ids)"));
    /* make the resolver ask, then flood it with forged answers */
    buf_start(&o, (uint16_t)(0x6600 + s->attacks), F_RD, s->name, T_A);
    nl_udp_send(n, (uint16_t)s->sport, s->victim, DNS_PORT, o.b, o.len);
    s->timer = nl_after(n, nl_arg_time(a, "lead", NL_US(100)), spoof_tick, s);
    return 0;
}

static void spoof_stop(nl_node *n)
{
    spoof_state *s = nl_mod_get(n, &spoof_mod);

    if (!s)
        return;
    nl_cancel(&s->timer);
    nl_udp_unbind(n, (uint16_t)s->sport);
    nl_mod_set(n, &spoof_mod, NULL);
    free(s);
}

static int spoof_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    spoof_state *s = nl_mod_get(n, &spoof_mod);
    int row = 0;

    (void)h;
    (void)detail;
    row += nl_ui_kv(x, y + row, w, _("target resolver"), "%s", nl_ip_str(s->victim));
    row += nl_ui_kv(x, y + row, w, _("impersonated server"), "%s", nl_ip_str(s->as));
    row += nl_ui_kv(x, y + row, w, _("forged record"), "%s A %s", s->name, nl_ip_str(s->fake));
    row += nl_ui_kv(x, y + row, w, _("attacks / successful"), "%u / %u", s->attacks, s->wins);
    row += nl_ui_kv(x, y + row, w, _("forged responses"), "%.0f", nl_count_get(n, "spoof.forged"));
    return row;
}

static const nl_module spoof_mod = {
    .name = "dnsspoof", .title = N_("DNS cache poisoning: an off-path forger of answers"), .layer = NL_L_APP,
    .usage = N_("run NODE dnsspoof resolver=ADDR as=ADDR name=NAME [fake=6.6.6.6] [guesses=64]\n"
                "        [port=53] [firstid=1] [ttl=3600] [gap=200us]\n"
                "  Asks the resolver for NAME, then sends `guesses` forged answers with the source\n"
                "  address of the authoritative server `as`. port=N: the resolver's known source\n"
                "  port, ids firstid, firstid+1, ...; port=0: unknown port, random ports and ids."),
    .theory = N_("A resolver trusts a UDP response if it comes from the address it asked, arrives on "
                 "the port the query left from, and repeats the 16-bit identifier and the question. "
                 "An attacker who is not on the path cannot see the query, but can send packets with "
                 "a forged source address. If the resolver always uses the same source port and "
                 "predictable identifiers, a handful of forged responses is enough: the first that "
                 "matches wins, the genuine answer arriving later is discarded, and the false record "
                 "stays in the cache for its whole TTL, served to every client. The attacker even "
                 "chooses the moment, by asking the resolver for the name himself. Randomizing the "
                 "identifier gives 16 bits of uncertainty; randomizing the source port as well "
                 "(the fix deployed after Kaminsky's 2008 attack) gives about 30, which makes a blind "
                 "race impractical. DNSSEC, not modelled here, removes the problem by signing records."),
    .run = spoof_run, .stop = spoof_stop, .panel = spoof_panel,
};
NL_MODULE(spoof_mod)

/* ------------------------------------------------------------------ */
/* Dissector                                                          */
/* ------------------------------------------------------------------ */


static void dis_dns(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const char *const secname[] = { "Answer", "Authority", "Additional" };
    static const char *const rcname[] = { "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN" };
    dns_msg *m = malloc(sizeof(*m));
    const dns_rr *first = NULL, *ns = NULL;
    int i, rcode, qr;

    if (!m)
        return;
    if (!msg_parse(d, len, m)) {
        nl_dis_summary(o, "DNS", DNS_COLOR, "malformed DNS message");
        free(m);
        return;
    }
    rcode = m->flags & 0xF;
    qr = (m->flags & F_QR) != 0;
    nl_dis_field(o, "DNS", "Transaction ID", off, 2, "0x%04x", m->id);
    nl_dis_field(o, "DNS", "Flags", off + 2, 2, "0x%04x  %s%s%s%s rcode=%s", m->flags,
                 qr ? "QR " : "", (m->flags & F_AA) ? "AA " : "", (m->flags & F_RD) ? "RD " : "",
                 (m->flags & F_RA) ? "RA " : "", rcode < 4 ? rcname[rcode] : "?");
    nl_dis_field(o, "DNS", "Questions", off + 4, 2, "%d", m->nq);
    nl_dis_field(o, "DNS", "Answer / Authority / Additional", off + 6, 6, "%d / %d / %d",
                 m->cnt[0], m->cnt[1], m->cnt[2]);
    if (m->nq)
        nl_dis_field(o, "DNS", "Query", off + 12, m->qlen, "%s %s", type_name(m->qtype), name_show(m->qname));
    for (i = 0; i < m->nrr; i++) {
        const dns_rr *r = &m->rr[i];
        nl_dis_field(o, "DNS", secname[r->sec], r->off, r->wlen, "%s %s  (TTL %u)", name_show(r->name),
                     rr_str(r), r->ttl);
        if (r->sec == SEC_AN && !first)
            first = r;
        if (r->sec == SEC_NS && r->type == T_NS && !ns)
            ns = r;
    }
    if (!qr) {
        nl_dis_summary(o, "DNS", DNS_COLOR, "Standard query %s %s", type_name(m->qtype), name_show(m->qname));
    } else if (rcode == RC_NXDOMAIN) {
        nl_dis_summary(o, "DNS", DNS_COLOR, "Response %s %s: NXDOMAIN", type_name(m->qtype), name_show(m->qname));
    } else if (first) {
        nl_dis_summary(o, "DNS", DNS_COLOR, "Response %s %s: %s", type_name(m->qtype), name_show(m->qname),
                       rr_str(first));
    } else if (ns) {
        nl_dis_summary(o, "DNS", DNS_COLOR, "Referral %s to %s", name_show(ns->name), ns->text);
    } else {
        nl_dis_summary(o, "DNS", DNS_COLOR, "Response %s %s: no data", type_name(m->qtype), name_show(m->qname));
    }
    free(m);
}

NL_INIT(dns)
{
    nl_dissector_udp(DNS_PORT, dis_dns);
}
