/*
 * core.c - hoplab kernel: virtual time and events, random numbers, argument
 *          parsing, packets, counters, log and capture rings, registries,
 *          message catalogs (i18n).
 */
#include "nl_int.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

nl_world W;
const uint8_t NL_MAC_BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_time  t;
    uint64_t id;
    nl_node *owner;
    void   (*fn)(void *);
    void    *ctx;
} event;

static event   *g_heap;
static int      g_nheap, g_heap_cap;
static uint64_t g_ev_id;

static int ev_less(const event *a, const event *b)
{
    return a->t < b->t || (a->t == b->t && a->id < b->id);
}

nl_time nl_now(void) { return W.now; }

nl_ev nl_after(nl_node *owner, nl_time delay, void (*fn)(void *), void *ctx)
{
    event e;
    int i;

    if (g_nheap == g_heap_cap) {
        int ncap = g_heap_cap ? g_heap_cap * 2 : 1024;
        event *nh = realloc(g_heap, (size_t)ncap * sizeof(*nh));
        if (!nh)
            return 0;
        g_heap = nh;
        g_heap_cap = ncap;
    }
    e.t = W.now + (delay < 0 ? 0 : delay);
    e.id = ++g_ev_id;
    e.owner = owner;
    e.fn = fn;
    e.ctx = ctx;
    i = g_nheap++;
    while (i > 0 && ev_less(&e, &g_heap[(i - 1) / 2])) {
        g_heap[i] = g_heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    g_heap[i] = e;
    return e.id;
}

void nl_cancel(nl_ev *ev)
{
    int i;

    if (!ev || !*ev)
        return;
    for (i = 0; i < g_nheap; i++)
        if (g_heap[i].id == *ev) {
            g_heap[i].fn = NULL;
            break;
        }
    *ev = 0;
}

static event heap_pop(void)
{
    event top = g_heap[0], last = g_heap[--g_nheap];
    int i = 0;

    for (;;) {
        int c = 2 * i + 1;
        if (c >= g_nheap)
            break;
        if (c + 1 < g_nheap && ev_less(&g_heap[c + 1], &g_heap[c]))
            c++;
        if (!ev_less(&g_heap[c], &last))
            break;
        g_heap[i] = g_heap[c];
        i = c;
    }
    if (g_nheap > 0)
        g_heap[i] = last;
    return top;
}

nl_time nl_next_event(void)
{
    while (g_nheap > 0 && !g_heap[0].fn)
        heap_pop();
    return g_nheap > 0 ? g_heap[0].t : -1;
}

int nl_step(nl_time until)
{
    int count = 0;

    while (g_nheap > 0 && g_heap[0].t <= until) {
        event e = heap_pop();
        if (!e.fn)
            continue;
        if (e.t > W.now)
            W.now = e.t;
        if (e.owner && !e.owner->up)
            continue;
        e.fn(e.ctx);
        count++;
    }
    if (until > W.now)
        W.now = until;
    return count;
}

/* ------------------------------------------------------------------ */
/* Random numbers (xorshift64*)                                       */
/* ------------------------------------------------------------------ */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

void nl_srand(uint64_t seed)
{
    g_rng = seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    if (!g_rng)
        g_rng = 1;
}

static uint64_t rng_next(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}

double   nl_rand01(void)        { return (double)(rng_next() >> 11) / 9007199254740992.0; }
unsigned nl_rand(unsigned n)    { return n ? (unsigned)(rng_next() >> 33) % n : 0; }
double   nl_rand_exp(double mean) { return -mean * log(1.0 - nl_rand01()); }

/* ------------------------------------------------------------------ */
/* Arguments and parsing                                              */
/* ------------------------------------------------------------------ */

void nl_args_parse(nl_args *a, const char *line)
{
    char *s;

    a->argc = 0;
    snprintf(a->buf, sizeof(a->buf), "%s", line ? line : "");
    s = a->buf;
    while (*s && a->argc < NL_MAX_ARGS) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
            s++;
        if (!*s)
            break;
        if (*s == '"') {                         /* "quoted token" */
            a->argv[a->argc++] = ++s;
            while (*s && *s != '"')
                s++;
        } else {
            a->argv[a->argc++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
                if (*s == '"') {                 /* key="quoted value" */
                    memmove(s, s + 1, strlen(s));
                    while (*s && *s != '"')
                        s++;
                    if (*s == '"')
                        memmove(s, s + 1, strlen(s));
                    continue;
                }
                s++;
            }
        }
        if (*s)
            *s++ = '\0';
    }
}

static int is_kv(const char *tok)
{
    const char *eq = strchr(tok, '=');
    const char *p;

    if (!eq || eq == tok)
        return 0;
    for (p = tok; p < eq; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
    return 1;
}

const char *nl_arg_pos(const nl_args *a, int i)
{
    int k;

    for (k = 0; k < a->argc; k++)
        if (!is_kv(a->argv[k]) && i-- == 0)
            return a->argv[k];
    return NULL;
}

const char *nl_arg_str(const nl_args *a, const char *key, const char *def)
{
    size_t kl = strlen(key);
    int k;

    for (k = 0; k < a->argc; k++)
        if (strncmp(a->argv[k], key, kl) == 0 && a->argv[k][kl] == '=')
            return a->argv[k] + kl + 1;
    return def;
}

int nl_arg_has(const nl_args *a, const char *key)
{
    int k;

    if (nl_arg_str(a, key, NULL))
        return 1;
    for (k = 0; k < a->argc; k++)
        if (strcmp(a->argv[k], key) == 0)
            return 1;
    return 0;
}

long nl_arg_int(const nl_args *a, const char *key, long def)
{
    const char *s = nl_arg_str(a, key, NULL);
    return s && *s ? strtol(s, NULL, 0) : def;
}

double nl_arg_dbl(const nl_args *a, const char *key, double def)
{
    const char *s = nl_arg_str(a, key, NULL);
    char *end;
    double v;

    if (!s || !*s)
        return def;
    v = strtod(s, &end);
    if (*end == '%')
        v /= 100.0;
    return v;
}

int nl_parse_time(const char *s, nl_time *out)
{
    char *end;
    double v = strtod(s, &end);

    if (end == s)
        return 0;
    if (strcmp(end, "us") == 0)                       ;
    else if (strcmp(end, "ms") == 0 || !*end)         v *= 1e3;    /* bare numbers are ms */
    else if (strcmp(end, "s") == 0)                   v *= 1e6;
    else if (strcmp(end, "m") == 0 || strcmp(end, "min") == 0) v *= 60e6;
    else return 0;
    *out = (nl_time)(v + 0.5);
    return 1;
}

int nl_parse_rate(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);

    if (end == s)
        return 0;
    switch (*end) {
    case 'k': case 'K': v *= 1e3; break;
    case 'm': case 'M': v *= 1e6; break;
    case 'g': case 'G': v *= 1e9; break;
    case '\0': case 'b': break;
    default: return 0;
    }
    *out = v;
    return 1;
}

nl_time nl_arg_time(const nl_args *a, const char *key, nl_time def)
{
    const char *s = nl_arg_str(a, key, NULL);
    nl_time t;
    return s && nl_parse_time(s, &t) ? t : def;
}

double nl_arg_rate(const nl_args *a, const char *key, double def)
{
    const char *s = nl_arg_str(a, key, NULL);
    double r;
    return s && nl_parse_rate(s, &r) ? r : def;
}

int nl_parse_ip(const char *s, nl_ip *out)
{
    unsigned a, b, c, d;
    char tail;

    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    *out = NL_IP(a, b, c, d);
    return 1;
}

int nl_parse_prefix(const char *s, nl_ip *ip, int *plen)
{
    char buf[40];
    char *slash;

    snprintf(buf, sizeof(buf), "%s", s);
    slash = strchr(buf, '/');
    if (slash) {
        *slash = '\0';
        *plen = atoi(slash + 1);
        if (*plen < 0 || *plen > 32)
            return 0;
    } else {
        *plen = 32;
    }
    if (strcmp(buf, "default") == 0) {
        *ip = 0;
        *plen = 0;
        return 1;
    }
    return nl_parse_ip(buf, ip);
}

int nl_resolve(const char *s, nl_ip *out)
{
    char buf[40];
    char *colon;
    nl_node *n;

    if (!s || !*s)
        return 0;
    if (nl_parse_ip(s, out))
        return 1;
    snprintf(buf, sizeof(buf), "%s", s);
    colon = strchr(buf, ':');
    if (colon)
        *colon = '\0';
    n = nl_node_find(buf);
    if (!n)
        return 0;
    if (colon) {
        int i = nl_if_find(n, colon + 1);
        if (i < 0 || !n->ifc[i].ip)
            return 0;
        *out = n->ifc[i].ip;
        return 1;
    }
    *out = nl_node_ip(n);
    return *out != 0;
}

static char *rot_buf(void)
{
    static char bufs[12][48];
    static int k;

    k = (k + 1) % 12;
    return bufs[k];
}

const char *nl_ip_str(nl_ip ip)
{
    char *b = rot_buf();
    snprintf(b, 48, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

const char *nl_mac_str(const uint8_t m[6])
{
    char *b = rot_buf();
    snprintf(b, 48, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

const char *nl_time_str(nl_time t)
{
    char *b = rot_buf();
    double a = t < 0 ? -(double)t : (double)t;

    if (a < 1000)          snprintf(b, 48, "%lld µs", t);
    else if (a < 1e6)      snprintf(b, 48, "%.4g ms", t / 1e3);
    else                   snprintf(b, 48, "%.4g s", t / 1e6);
    return b;
}

const char *nl_rate_str(double bps)
{
    char *b = rot_buf();

    if (bps <= 0)          snprintf(b, 48, "∞");
    else if (bps < 1e3)    snprintf(b, 48, "%.0f bit/s", bps);
    else if (bps < 1e6)    snprintf(b, 48, "%.4g kbit/s", bps / 1e3);
    else if (bps < 1e9)    snprintf(b, 48, "%.4g Mbit/s", bps / 1e6);
    else                   snprintf(b, 48, "%.4g Gbit/s", bps / 1e9);
    return b;
}

nl_ip nl_mask(int plen)
{
    return plen <= 0 ? 0 : plen >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - plen)) - 1);
}

const char *nl_layer_name(int layer)
{
    switch (layer) {
    case NL_L_PHY:       return _("Physical");
    case NL_L_LINK:      return _("Link");
    case NL_L_NET:       return _("Network");
    case NL_L_TRANSPORT: return _("Transport");
    case NL_L_APP:       return _("Application");
    case NL_L_DIST:      return _("Distributed");
    default:             return _("Simulator");
    }
}

short nl_layer_color(int layer)
{
    switch (layer) {
    case NL_L_PHY:       return 180;
    case NL_L_LINK:      return 221;
    case NL_L_NET:       return 114;
    case NL_L_TRANSPORT: return 75;
    case NL_L_APP:       return 177;
    case NL_L_DIST:      return 209;
    default:             return C_DIM;
    }
}

const char *nl_fate_name(int fate)
{
    switch (fate) {
    case NL_FATE_LOST:    return _("lost");
    case NL_FATE_CORRUPT: return _("corrupted");
    case NL_FATE_QDROP:   return _("queue drop");
    case NL_FATE_DOWN:    return _("link down");
    case NL_FATE_FIXED:   return _("corrected");
    case NL_FATE_FCS:     return _("bad FCS");
    default:              return _("delivered");
    }
}

/* ------------------------------------------------------------------ */
/* Packets and checksums                                              */
/* ------------------------------------------------------------------ */

static uint32_t g_pkt_id;

uint32_t nl_pkt_newid(void)
{
    /* node processes of a distributed run must not collide: the high bits
     * come from the process id */
    if (!g_pkt_id)
        g_pkt_id = W.mode == NL_MODE_RUN ? 0 : ((uint32_t)getpid() & 0xFFF) << 20;
    return ++g_pkt_id;
}

nl_pkt *nl_pkt_new(uint32_t id)
{
    nl_pkt *p = malloc(sizeof(*p));

    if (!p) {
        fprintf(stderr, "hoplab: out of memory\n");
        exit(EXIT_FAILURE);
    }
    memset(p, 0, offsetof(nl_pkt, data));
    p->id = id ? id : nl_pkt_newid();
    p->born = W.now;
    return p;
}

nl_pkt *nl_pkt_clone(const nl_pkt *src)
{
    nl_pkt *p = malloc(sizeof(*p));

    if (!p) {
        fprintf(stderr, "hoplab: out of memory\n");
        exit(EXIT_FAILURE);
    }
    memcpy(p, src, offsetof(nl_pkt, data) + src->len);
    return p;
}

void nl_pkt_free(nl_pkt *p) { free(p); }

uint16_t nl_cksum(const void *data, int len, uint32_t init)
{
    const uint8_t *d = data;
    uint32_t sum = init;
    int i;

    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(d[i] << 8 | d[i + 1]);
    if (i < len)
        sum += (uint32_t)(d[i] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t nl_l4_cksum(nl_ip src, nl_ip dst, uint8_t proto, const void *seg, int len)
{
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + (uint32_t)len;
    return nl_cksum(seg, len, sum);
}

/* ------------------------------------------------------------------ */
/* World, nodes                                                       */
/* ------------------------------------------------------------------ */

void nl_world_init(void)
{
    int i;

    if (!W.caps)
        W.caps = calloc(NL_CAP_RING, sizeof(nl_cap));
    if (!W.logs)
        W.logs = calloc(NL_LOG_RING, sizeof(nl_logrec));
    if (!W.caps || !W.logs) {
        fprintf(stderr, "hoplab: out of memory\n");
        exit(EXIT_FAILURE);
    }
    W.speed = 1.0;
    W.detail = 2;
    W.mon_sock = W.mon_listen = -1;
    W.baseport = 47000;
    W.duration = NL_SEC(30);
    W.cap_seq = W.log_seq = 1;
    for (i = 0; i < NL_MAX_NODES; i++)
        W.nodes[i].sock = -1;
}

nl_node *nl_node_add(const char *name, int kind)
{
    nl_node *n;

    if (W.nnodes >= NL_MAX_NODES || nl_node_find(name))
        return NULL;
    n = &W.nodes[W.nnodes];
    memset(n, 0, sizeof(*n));
    n->idx = W.nnodes++;
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->kind = kind;
    n->forward = kind == NL_ROUTER;
    n->up = 1;
    n->local = 1;
    n->x = n->y = -1;
    n->sock = -1;
    nl_stack_init(n);
    return n;
}

int nl_if_add(nl_node *n)
{
    nl_iface *f;

    if (n->nif >= NL_MAX_IF)
        return -1;
    f = &n->ifc[n->nif];
    memset(f, 0, sizeof(*f));
    snprintf(f->name, sizeof(f->name), "%s%d", n->kind == NL_SWITCH ? "p" : "eth", n->nif);
    f->mac[0] = 0x02;                   /* locally administered */
    f->mac[1] = 0x00;
    f->mac[2] = 0x4e;
    f->mac[3] = 0x4c;
    f->mac[4] = (uint8_t)(n->idx + 1);
    f->mac[5] = (uint8_t)n->nif;
    f->up = 1;
    return n->nif++;
}

nl_node *nl_node_find(const char *name)
{
    int i;

    for (i = 0; i < W.nnodes; i++)
        if (strcmp(W.nodes[i].name, name) == 0)
            return &W.nodes[i];
    return NULL;
}

nl_node *nl_node_at(int idx)  { return idx >= 0 && idx < W.nnodes ? &W.nodes[idx] : NULL; }
int      nl_node_count(void)  { return W.nnodes; }
nl_link *nl_link_at(int idx)  { return idx >= 0 && idx < W.nlinks ? &W.links[idx] : NULL; }
int      nl_link_count(void)  { return W.nlinks; }

int nl_if_find(const nl_node *n, const char *name)
{
    int i;

    for (i = 0; i < n->nif; i++)
        if (strcmp(n->ifc[i].name, name) == 0)
            return i;
    return -1;
}

int nl_if_usable(const nl_node *n, int ifx)
{
    return ifx >= 0 && ifx < n->nif && n->up && n->ifc[ifx].up && n->ifc[ifx].link && n->ifc[ifx].link->up;
}

nl_node *nl_if_peer(const nl_node *n, int ifx, int *peer_ifx)
{
    const nl_iface *f;

    if (ifx < 0 || ifx >= n->nif || !n->ifc[ifx].link)
        return NULL;
    f = &n->ifc[ifx];
    if (peer_ifx)
        *peer_ifx = f->link->ifx[!f->end];
    return f->link->n[!f->end];
}

nl_ip nl_node_ip(const nl_node *n)
{
    int i;

    for (i = 0; i < n->nif; i++)
        if (n->ifc[i].ip)
            return n->ifc[i].ip;
    return 0;
}

nl_node *nl_node_by_ip(nl_ip ip)
{
    int i, k;

    if (!ip)
        return NULL;
    for (i = 0; i < W.nnodes; i++)
        for (k = 0; k < W.nodes[i].nif; k++)
            if (W.nodes[i].ifc[k].ip == ip)
                return &W.nodes[i];
    return NULL;
}

void nl_node_set_up(nl_node *n, int up)
{
    int i;

    if (n->up == !!up)
        return;
    if (!up) {
        nl_log(n, NL_L_SIM, 1, 0, T_ERR "✗ %s", _("node crashed"));
        for (i = 0; i < n->nmod; i++)
            if (n->mod[i].m->crash)
                n->mod[i].m->crash(n);
        n->up = 0;
        nl_stack_node_down(n);
    } else {
        n->up = 1;
        nl_log(n, NL_L_SIM, 1, 0, T_OK "● %s", _("node recovered"));
        nl_ip6_node_up(n);
        for (i = 0; i < n->nmod; i++)
            if (n->mod[i].m->recover)
                n->mod[i].m->recover(n);
    }
}

/* ------------------------------------------------------------------ */
/* Counters and series                                                */
/* ------------------------------------------------------------------ */

static nl_counter *counter(nl_node *n, const char *name, int create)
{
    int i;

    for (i = 0; i < n->ncnt; i++)
        if (strcmp(n->cnt[i].name, name) == 0)
            return &n->cnt[i];
    if (!create || n->ncnt >= (int)(sizeof(n->cnt) / sizeof(n->cnt[0])))
        return NULL;
    snprintf(n->cnt[n->ncnt].name, sizeof(n->cnt[0].name), "%s", name);
    n->cnt[n->ncnt].v = 0;
    return &n->cnt[n->ncnt++];
}

void nl_count(nl_node *n, const char *name, double delta)
{
    nl_counter *c = n ? counter(n, name, 1) : NULL;
    if (c)
        c->v += delta;
}

void nl_count_set(nl_node *n, const char *name, double v)
{
    nl_counter *c = n ? counter(n, name, 1) : NULL;
    if (c)
        c->v = v;
}

double nl_count_get(const nl_node *n, const char *name)
{
    const nl_counter *c = counter((nl_node *)n, name, 0);
    return c ? c->v : 0.0;
}

const nl_series *nl_series_find(const nl_node *n, const char *name)
{
    int i;

    for (i = 0; i < n->nser; i++)
        if (strcmp(n->ser[i]->name, name) == 0)
            return n->ser[i];
    return NULL;
}

void nl_sample(nl_node *n, const char *name, double v)
{
    nl_series *s = (nl_series *)nl_series_find(n, name);
    int pos;

    if (!s) {
        if (n->nser >= (int)(sizeof(n->ser) / sizeof(n->ser[0])))
            return;
        s = calloc(1, sizeof(*s));
        if (!s)
            return;
        snprintf(s->name, sizeof(s->name), "%s", name);
        n->ser[n->nser++] = s;
    }
    pos = (s->head + s->n) % NL_SERIES_LEN;
    if (s->n == NL_SERIES_LEN)
        s->head = (s->head + 1) % NL_SERIES_LEN;
    else
        s->n++;
    s->t[pos] = W.now;
    s->v[pos] = (float)v;
}

/* ------------------------------------------------------------------ */
/* Log and captures                                                   */
/* ------------------------------------------------------------------ */

void nl_strip_markup(const char *in, char *out, size_t n)
{
    size_t k = 0;

    for (; *in && k + 1 < n; in++) {
        if (*in == '\001') {
            if (!in[1])
                break;
            in++;
            continue;
        }
        out[k++] = *in;
    }
    out[k] = '\0';
}

void nl_log(nl_node *n, int layer, int level, uint32_t pkt_id, const char *fmt, ...)
{
    nl_logrec *r = &W.logs[W.log_seq % NL_LOG_RING];
    va_list ap;

    r->seq = W.log_seq++;
    r->t = W.now;
    r->node = n ? (short)n->idx : -1;
    r->layer = (uint8_t)layer;
    r->level = (uint8_t)level;
    r->pkt_id = pkt_id;
    va_start(ap, fmt);
    vsnprintf(r->text, sizeof(r->text), fmt, ap);
    va_end(ap);

    if (W.headless && !W.quiet && level <= W.detail) {
        char plain[sizeof(r->text)];
        nl_strip_markup(r->text, plain, sizeof(plain));
        printf("%10.3f ms  %-8s L%d  %s\n", r->t / 1000.0, n ? n->name : "-", layer, plain);
    }
    /* simulator-level lines (no node) are produced identically by every process */
    if (n && W.mon_sock >= 0 && W.mode == NL_MODE_NODE)
        nl_mon_send_log(r);
}

const nl_logrec *nl_log_get(uint64_t seq)
{
    const nl_logrec *r = &W.logs[seq % NL_LOG_RING];
    return seq && r->seq == seq ? r : NULL;
}

nl_cap *nl_cap_new(void)
{
    nl_cap *c = &W.caps[W.cap_seq % NL_CAP_RING];

    memset(c, 0, offsetof(nl_cap, pkt));
    c->seq = W.cap_seq++;
    return c;
}

const nl_cap *nl_cap_get(uint64_t seq)
{
    const nl_cap *c = &W.caps[seq % NL_CAP_RING];
    return seq && c->seq == seq ? c : NULL;
}

/* ------------------------------------------------------------------ */
/* Registries                                                         */
/* ------------------------------------------------------------------ */

#define REG_MAX 64
static const nl_module *g_mods[REG_MAX];
static const nl_qdisc  *g_qdiscs[REG_MAX];
static const nl_codec  *g_codecs[REG_MAX];
static int g_nmods, g_nqdiscs, g_ncodecs;

void nl_module_register(const nl_module *m)
{
    int i = g_nmods;

    if (g_nmods >= REG_MAX)
        return;
    /* keep them sorted by layer, then name: constructors run in link order */
    while (i > 0 && (g_mods[i - 1]->layer > m->layer ||
                     (g_mods[i - 1]->layer == m->layer && strcmp(g_mods[i - 1]->name, m->name) > 0))) {
        g_mods[i] = g_mods[i - 1];
        i--;
    }
    g_mods[i] = m;
    g_nmods++;
}

const nl_module *nl_module_find(const char *name)
{
    int i;

    for (i = 0; i < g_nmods; i++)
        if (strcmp(g_mods[i]->name, name) == 0)
            return g_mods[i];
    return NULL;
}

const nl_module *nl_module_at(int i) { return i >= 0 && i < g_nmods ? g_mods[i] : NULL; }

void *nl_mod_get(const nl_node *n, const nl_module *m)
{
    int i;

    for (i = 0; i < n->nmod; i++)
        if (n->mod[i].m == m)
            return n->mod[i].st;
    return NULL;
}

void nl_mod_set(nl_node *n, const nl_module *m, void *st)
{
    int i;

    for (i = 0; i < n->nmod; i++)
        if (n->mod[i].m == m) {
            if (st) {
                n->mod[i].st = st;
            } else {
                memmove(&n->mod[i], &n->mod[i + 1], (size_t)(n->nmod - i - 1) * sizeof(n->mod[0]));
                n->nmod--;
            }
            return;
        }
    if (st && n->nmod < NL_MAX_MODS) {
        n->mod[n->nmod].m = m;
        n->mod[n->nmod].st = st;
        n->nmod++;
    }
}

void nl_qdisc_register(const nl_qdisc *q)
{
    if (g_nqdiscs < REG_MAX)
        g_qdiscs[g_nqdiscs++] = q;
}

const nl_qdisc *nl_qdisc_find(const char *name)
{
    int i;

    for (i = 0; i < g_nqdiscs; i++)
        if (strcmp(g_qdiscs[i]->name, name) == 0)
            return g_qdiscs[i];
    return NULL;
}

const nl_qdisc *nl_qdisc_at(int i) { return i >= 0 && i < g_nqdiscs ? g_qdiscs[i] : NULL; }

void nl_codec_register(const nl_codec *c)
{
    if (g_ncodecs < REG_MAX)
        g_codecs[g_ncodecs++] = c;
}

const nl_codec *nl_codec_find(const char *name)
{
    int i;

    for (i = 0; i < g_ncodecs; i++)
        if (strcmp(g_codecs[i]->name, name) == 0)
            return g_codecs[i];
    return NULL;
}

const nl_codec *nl_codec_at(int i) { return i >= 0 && i < g_ncodecs ? g_codecs[i] : NULL; }
