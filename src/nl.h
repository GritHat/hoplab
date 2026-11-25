/*
 * nl.h - hoplab core API.
 *
 * hoplab is a userland network emulator for study: nodes (hosts, routers,
 * switches) exchange real byte-level frames (Ethernet / ARP / IPv4 / ICMP /
 * UDP / TCP ...) over emulated links with bandwidth, delay, loss, corruption
 * and queues. Everything runs on virtual time driven by an event queue.
 *
 * The same node code runs in three ways:
 *   - all nodes in one process (hoplab run): links are in-memory;
 *   - one or more nodes per process (hoplab node): links towards nodes hosted
 *     elsewhere become UDP tunnels, on this machine or across machines;
 *   - no node at all (hoplab monitor): the process only displays what the
 *     node processes report.
 *
 * Protocols and applications are "modules" (nl_module) that register
 * themselves at program start with NL_MODULE(); they use the hooks declared
 * here (timers, UDP/TCP/IP/Ethernet handlers, routes, packet hooks, queue
 * disciplines, link codecs, dissectors) and never need to edit core files.
 *
 * Conventions: IPv4 addresses (nl_ip) are in host byte order; times
 * (nl_time) are virtual microseconds; user-visible strings go through _()
 * for translation and may contain the T_* colour markup of tui.h.
 */
#ifndef NL_H
#define NL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "i18n.h"
#include "tui.h"

/* ------------------------------------------------------------------ */
/* Limits                                                             */
/* ------------------------------------------------------------------ */

#define NL_MAX_NODES   64
#define NL_MAX_IF      16
#define NL_MAX_LINKS   160
#define NL_MAX_MODS    12          /* modules active on one node */
#define NL_MAX_ROUTES  256
#define NL_FRAME_MAX   1700        /* bytes of a frame, headers included */
#define NL_ETH_HLEN    14
#define NL_IP_HLEN     20
#define NL_UDP_HLEN    8
#define NL_IP6_HLEN    40
#define NL_MAX_IP6     4           /* IPv6 addresses per interface (the first is link-local) */
#define NL_MAX_ROUTES6 64
#define NL_DEFAULT_MTU 1500
#define NL_MAX_ARGS    40

typedef long long nl_time;                     /* virtual microseconds */
#define NL_US(x)   ((nl_time)(x))
#define NL_MS(x)   ((nl_time)(x) * 1000)
#define NL_SEC(x)  ((nl_time)(x) * 1000000)

typedef uint32_t nl_ip;                        /* host byte order */
#define NL_IP(a, b, c, d) (((nl_ip)(a) << 24) | ((nl_ip)(b) << 16) | ((nl_ip)(c) << 8) | (nl_ip)(d))
#define NL_IP_BROADCAST 0xFFFFFFFFu

/* IPv6 address, in network byte order */
typedef struct nl_ip6 { uint8_t b[16]; } nl_ip6;

typedef struct nl_node   nl_node;
typedef struct nl_link   nl_link;
typedef struct nl_module nl_module;

/* Layers, used to classify modules, log lines and study topics */
enum {
    NL_L_PHY = 1, NL_L_LINK = 2, NL_L_NET = 3, NL_L_TRANSPORT = 4,
    NL_L_APP = 7, NL_L_DIST = 8, NL_L_SIM = 9
};
const char *nl_layer_name(int layer);          /* translated */
short       nl_layer_color(int layer);

/* ------------------------------------------------------------------ */
/* Time, events, random numbers                                       */
/* ------------------------------------------------------------------ */

nl_time nl_now(void);

/* Timers. owner may be NULL; a timer owned by a node that is down (crashed)
 * is discarded instead of firing. The handle is 0 when not armed. */
typedef uint64_t nl_ev;
nl_ev nl_after(nl_node *owner, nl_time delay, void (*fn)(void *ctx), void *ctx);
void  nl_cancel(nl_ev *ev);                    /* safe on 0 and on fired timers; zeroes *ev */

void     nl_srand(uint64_t seed);
double   nl_rand01(void);                      /* uniform in [0, 1) */
unsigned nl_rand(unsigned n);                  /* uniform in [0, n) */
double   nl_rand_exp(double mean);             /* exponential */

/* ------------------------------------------------------------------ */
/* Arguments: "word word key=value key=value"                         */
/* ------------------------------------------------------------------ */

typedef struct nl_args {
    int   argc;
    char *argv[NL_MAX_ARGS];                   /* every token, in order */
    char  buf[1024];
} nl_args;

void        nl_args_parse(nl_args *a, const char *line);
const char *nl_arg_pos(const nl_args *a, int i);              /* i-th token without '=', or NULL */
const char *nl_arg_str(const nl_args *a, const char *key, const char *def);
long        nl_arg_int(const nl_args *a, const char *key, long def);
double      nl_arg_dbl(const nl_args *a, const char *key, double def);
nl_time     nl_arg_time(const nl_args *a, const char *key, nl_time def);  /* 5ms 2s 100us 1.5s */
double      nl_arg_rate(const nl_args *a, const char *key, double def);   /* 10M 56k 1G (bit/s) */
int         nl_arg_has(const nl_args *a, const char *key);

int         nl_parse_time(const char *s, nl_time *out);       /* 1 = ok */
int         nl_parse_rate(const char *s, double *out);
int         nl_parse_ip(const char *s, nl_ip *out);           /* dotted quad only */
int         nl_parse_prefix(const char *s, nl_ip *ip, int *plen);   /* a.b.c.d/len */
/* Dotted quad, node name ("h2": its first address) or NODE:IFACE */
int         nl_resolve(const char *s, nl_ip *out);

/* Formatting helpers: each returns one of a few rotating static buffers */
const char *nl_ip_str(nl_ip ip);
const char *nl_mac_str(const uint8_t mac[6]);
const char *nl_time_str(nl_time t);            /* "12.345 ms", "3.2 s" */
const char *nl_rate_str(double bps);
nl_ip       nl_mask(int plen);

/* ------------------------------------------------------------------ */
/* Packets (frames)                                                   */
/* ------------------------------------------------------------------ */

typedef struct nl_pkt {
    uint32_t id;               /* trace identity, kept hop by hop */
    uint16_t len;              /* bytes used in data (from the Ethernet header) */
    uint8_t  hops;
    uint8_t  flags;
    nl_time  born;
    uint8_t  data[NL_FRAME_MAX];
} nl_pkt;

nl_pkt  *nl_pkt_new(uint32_t id);              /* id 0 = allocate a new identity */
nl_pkt  *nl_pkt_clone(const nl_pkt *p);
void     nl_pkt_free(nl_pkt *p);
uint32_t nl_pkt_newid(void);

/* Big-endian accessors */
static inline uint16_t nl_get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t nl_get32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void nl_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void nl_put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Internet checksum; init lets you chain buffers (pass 0 to start). The
 * result is already complemented. */
uint16_t nl_cksum(const void *data, int len, uint32_t init);
/* Checksum of a transport segment with the IPv4 pseudo-header */
uint16_t nl_l4_cksum(nl_ip src, nl_ip dst, uint8_t proto, const void *seg, int len);

/* ------------------------------------------------------------------ */
/* Links                                                              */
/* ------------------------------------------------------------------ */

typedef struct nl_linkcfg {
    double  bw;                /* bit/s; 0 = unlimited */
    nl_time delay, jitter;     /* propagation delay and its random variation */
    double  loss;              /* probability that a frame is lost */
    double  burst;             /* mean loss-burst length (Gilbert-Elliott); 0 = independent */
    double  ber;               /* bit error rate */
    double  corrupt;           /* probability that a frame gets one flipped bit */
    double  dup;               /* probability of duplication */
    double  reorder;           /* probability of extra delay (reordering) */
    int     qcap;              /* queue capacity in packets */
    int     mtu;
    char    qdisc[16];         /* queue discipline: droptail (default), or a registered one */
    char    codec[16];         /* link code: fcs (default), none, or a registered one */
    char    qargs[96];         /* extra key=value for the queue discipline */
} nl_linkcfg;

enum { NL_FATE_OK, NL_FATE_LOST, NL_FATE_CORRUPT, NL_FATE_QDROP, NL_FATE_DOWN, NL_FATE_FIXED, NL_FATE_FCS };
const char *nl_fate_name(int fate);

typedef struct nl_qitem { nl_pkt *p; nl_time t_in; } nl_qitem;

typedef struct nl_linkdir {    /* one direction of a link: queue + transmitter */
    nl_qitem *q;               /* ring of qalloc items */
    int       qalloc, qhead, qlen, qbytes;
    nl_time   busy_until;      /* transmitter busy until */
    int       sending;
    void     *qstate;          /* queue discipline state */
    int       ge_bad;          /* Gilbert-Elliott state */
    uint64_t  tx, tx_bytes, lost, corrupted, qdrops, marked, fixed, undetected, fcs_drops;
} nl_linkdir;

struct nl_link {
    int        idx;
    nl_node   *n[2];           /* the two ends */
    int        ifx[2];         /* interface index on each end */
    int        up;
    nl_linkcfg cfg;
    nl_linkdir d[2];           /* d[0]: n[0] -> n[1] */
};

nl_link *nl_link_find(const nl_node *a, const nl_node *b);
void     nl_link_set_up(nl_link *l, int up);
/* Ask the transmitter of a direction to look at its queue again: for queue
 * disciplines whose dequeue() returned NULL while packets were waiting
 * (shapers), called from a timer when the next packet may go. */
void     nl_link_kick(nl_link *l, int dir);

/* Queue disciplines (AQM / scheduling). Register with nl_qdisc_register()
 * from a constructor; selected per link with qdisc=NAME. The helpers below
 * operate on the FIFO ring inside nl_linkdir. */
enum { NL_Q_ACCEPT = 0, NL_Q_DROP = 1 };
typedef struct nl_qdisc {
    const char *name;
    const char *title;
    void *(*create)(nl_link *l, int dir, const nl_args *a);
    /* Decide what to do with p before it is queued; may ECN-mark it with
     * nl_ecn_mark(). The core still drops when the ring is full. */
    int   (*enqueue)(void *st, nl_link *l, int dir, nl_pkt *p);
    /* Pick the next packet to transmit (NULL = queue empty). May drop
     * packets itself with nl_q_drop(). NULL callback = plain FIFO. */
    nl_pkt *(*dequeue)(void *st, nl_link *l, int dir);
    void  (*panel)(void *st, nl_link *l, int dir, int x, int y, int w, int h);
} nl_qdisc;
void    nl_qdisc_register(const nl_qdisc *q);
nl_pkt *nl_q_pop(nl_link *l, int dir, nl_time *sojourn);      /* head of the FIFO, or NULL */
const nl_qitem *nl_q_peek(const nl_link *l, int dir, int i);  /* i-th from the head */
void    nl_q_drop(nl_link *l, int dir, nl_pkt *p, const char *why);   /* count, log, free */
int     nl_ecn_capable(const nl_pkt *p);       /* IPv4 packet with ECT(0)/ECT(1) */
int     nl_ecn_mark(nl_pkt *p);                /* set CE (fixes the IP checksum); 1 = marked */

/* Link codes (error detection / correction). encode() adds redundancy,
 * the channel flips bits of the encoded frame, decode() recovers the frame
 * or reports failure. Selected per link with codec=NAME. */
typedef struct nl_codec {
    const char *name;
    const char *title;
    int (*encode)(const uint8_t *in, int n, uint8_t *out, int cap);          /* bytes written, <0 error */
    /* returns the decoded length, or -1 when an error was detected but not
     * corrected; *corrected = number of corrected symbols/bits */
    int (*decode)(const uint8_t *in, int n, uint8_t *out, int cap, int *corrected);
} nl_codec;
void            nl_codec_register(const nl_codec *c);
const nl_codec *nl_codec_find(const char *name);
const nl_codec *nl_codec_at(int i);            /* enumerate; NULL at the end */

/* ------------------------------------------------------------------ */
/* Nodes and interfaces                                               */
/* ------------------------------------------------------------------ */

enum { NL_HOST, NL_ROUTER, NL_SWITCH };

typedef struct nl_iface {
    char     name[8];          /* eth0, eth1, ... */
    uint8_t  mac[6];
    nl_ip    ip;               /* 0 = none */
    int      plen;
    nl_link *link;
    int      end;              /* which end of the link (0/1) */
    int      up;               /* administrative state */
    int      blocked;          /* bridge port not forwarding (set by STP) */
    int      vlan;             /* access VLAN (0 = untagged world), -1 = 802.1Q trunk */
    uint64_t rx, tx, rx_bytes, tx_bytes;
    struct {
        nl_ip6  a;
        uint8_t plen;
        uint8_t state;         /* NL_A6_TENTATIVE while duplicate address detection runs */
        uint8_t autoconf;      /* formed from a router advertisement (SLAAC) */
    } ip6[NL_MAX_IP6];
    int      nip6;
} nl_iface;

enum { NL_A6_TENTATIVE = 0, NL_A6_OK = 1, NL_A6_DUPLICATE = 2 };

enum { NL_RT_CONNECTED = 0, NL_RT_STATIC = 1, NL_RT_RA = 4, NL_RT_BGP = 20, NL_RT_OSPF = 110, NL_RT_RIP = 120, NL_RT_DHCP = 5 };

typedef struct nl_route {
    nl_ip dst;
    int   plen;
    nl_ip gw;                  /* 0 = directly connected */
    int   ifx;
    int   metric;
    int   proto;               /* NL_RT_*: also the administrative distance */
    nl_time since;
} nl_route;

typedef struct nl_route6 {
    nl_ip6  dst;
    int     plen;
    nl_ip6  gw;                /* all zero = directly connected (on-link) */
    int     ifx;
    int     metric;
    int     proto;             /* NL_RT_*: NL_RT_RA for routes learned from router advertisements */
    nl_time since, expire;     /* expire 0 = never */
} nl_route6;

/* IPv6 neighbor cache entry (RFC 4861); the DELAY state is folded into PROBE */
enum { NL_NBR_FREE = 0, NL_NBR_INCOMPLETE, NL_NBR_REACHABLE, NL_NBR_STALE, NL_NBR_PROBE };
typedef struct nl_nbr {
    nl_ip6  ip;
    uint8_t mac[6];
    int     state, ifx, router, tries, nwait;
    nl_time confirmed, next;   /* last reachability confirmation; next retransmission */
    void   *wait[3];           /* packets waiting for the address (private) */
} nl_nbr;

typedef struct nl_arp {
    nl_ip   ip;
    uint8_t mac[6];
    int     state;             /* 0 free, 1 resolving, 2 valid */
    nl_time expire;
    nl_pkt *wait[4];           /* packets waiting for the answer */
    int     nwait, tries, ifx;
    nl_ev   timer;
} nl_arp;

typedef struct nl_fdb {        /* bridge forwarding table entry */
    uint8_t mac[6];
    int     port, vlan, used;
    nl_time seen;
} nl_fdb;

typedef struct nl_counter { char name[28]; double v; } nl_counter;

#define NL_SERIES_LEN 600
typedef struct nl_series {     /* time series for the graph view */
    char    name[24];
    int     n, head;
    nl_time t[NL_SERIES_LEN];
    float   v[NL_SERIES_LEN];
} nl_series;

struct nl_node {
    int      idx;
    char     name[16];
    int      kind;             /* NL_HOST / NL_ROUTER / NL_SWITCH */
    char     role[12];         /* free label shown on the map: "DNS", "NAT", "LB", ... */
    int      local;            /* simulated by this process */
    int      up;               /* 0 = crashed */
    int      forward;          /* forwards IP packets */
    double   x, y;             /* map position, 0..100; <0 = automatic */
    int      placed;

    nl_iface ifc[NL_MAX_IF];
    int      nif;

    nl_route rt[NL_MAX_ROUTES];
    int      nrt;
    nl_arp   arp[32];
    nl_fdb   fdb[64];
    nl_time  fdb_age;

    struct { const nl_module *m; void *st; } mod[NL_MAX_MODS];
    int      nmod;

    nl_counter cnt[48];
    int        ncnt;
    nl_series *ser[8];
    int        nser;

    char     under_host[64];   /* where the node runs in distributed mode */
    int      under_port;
    int      sock;             /* tunnel socket when local, else -1 */
    void    *stack;            /* core private (sockets, handlers, hooks) */

    nl_route6 rt6[NL_MAX_ROUTES6];
    int       nrt6;
    nl_nbr    nbr[32];
    void     *stack6;          /* core private (IPv6) */
};

nl_node *nl_node_find(const char *name);
nl_node *nl_node_at(int idx);
int      nl_node_count(void);
nl_link *nl_link_at(int idx);
int      nl_link_count(void);
int      nl_if_find(const nl_node *n, const char *name);          /* index or -1 */
int      nl_if_usable(const nl_node *n, int ifx);                 /* admin up, link up, node up */
void     nl_if_set_addr(nl_node *n, int ifx, nl_ip ip, int plen); /* also the connected route */
nl_node *nl_if_peer(const nl_node *n, int ifx, int *peer_ifx);    /* other end of the link */
nl_ip    nl_node_ip(const nl_node *n);                            /* first configured address */
nl_node *nl_node_by_ip(nl_ip ip);                                 /* any node owning this address */
void     nl_node_set_up(nl_node *n, int up);                      /* crash / recover */

/* Counters ("ping.reply") and time series ("cwnd"): shown in the UI and
 * usable in scenario `expect` lines. */
void       nl_count(nl_node *n, const char *name, double delta);
void       nl_count_set(nl_node *n, const char *name, double v);
double     nl_count_get(const nl_node *n, const char *name);
void       nl_sample(nl_node *n, const char *series, double v);

/* ------------------------------------------------------------------ */
/* Log                                                                */
/* ------------------------------------------------------------------ */

/* level 1 = overview (what a beginner should see), 2 = protocol messages,
 * 3 = every frame and internal step. pkt_id links the line to a packet
 * (0 = none). n may be NULL. */
void nl_log(nl_node *n, int layer, int level, uint32_t pkt_id, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));

/* ------------------------------------------------------------------ */
/* Layer 2                                                            */
/* ------------------------------------------------------------------ */

#define NL_ETH_IP    0x0800
#define NL_ETH_ARP   0x0806
#define NL_ETH_VLAN  0x8100
extern const uint8_t NL_MAC_BROADCAST[6];

typedef struct nl_ethmeta {
    const uint8_t *dst, *src;
    uint16_t       type;
    const uint8_t *payload;
    int            len, in_if;
    const nl_pkt  *pkt;
} nl_ethmeta;
typedef void (*nl_eth_fn)(nl_node *n, void *ctx, const nl_ethmeta *m);

/* Handler for an EtherType on a node (also on switches, before bridging:
 * used for BPDUs and the like). */
void     nl_eth_bind(nl_node *n, uint16_t ethertype, nl_eth_fn fn, void *ctx);
void     nl_eth_unbind(nl_node *n, uint16_t ethertype);
/* Send a frame out of an interface; returns the packet id */
uint32_t nl_eth_send(nl_node *n, int ifx, const uint8_t dst[6], uint16_t ethertype,
                     const void *payload, int len, uint32_t pkt_id);
void     nl_fdb_flush(nl_node *n);

/* ------------------------------------------------------------------ */
/* Layer 3                                                            */
/* ------------------------------------------------------------------ */

#define NL_PROTO_ICMP 1
#define NL_PROTO_TCP  6
#define NL_PROTO_UDP  17
#define NL_PROTO_OSPF 89

typedef struct nl_ipmeta {     /* a received IPv4 packet, parsed */
    nl_ip    src, dst;
    uint8_t  proto, ttl, tos;
    uint16_t id;
    int      in_if;
    uint32_t pkt_id;
    const uint8_t *l4;         /* transport header */
    int      l4len;
    const nl_pkt *pkt;
} nl_ipmeta;
typedef void (*nl_ip_fn)(nl_node *n, void *ctx, const nl_ipmeta *m);

typedef struct nl_iptx {       /* how to send an IPv4 packet; zero = defaults */
    nl_ip    src, dst;         /* src 0 = address of the outgoing interface */
    uint8_t  proto, ttl, tos;  /* ttl 0 = 64 */
    int      ifx;              /* -1 or 0-with-use_if=0: route lookup; else force interface */
    int      use_if;           /* 1 = send out of ifx without routing (broadcast, multicast, link-local) */
    int      df;               /* don't fragment */
    uint32_t pkt_id;           /* 0 = new identity */
} nl_iptx;

void     nl_ip_bind(nl_node *n, uint8_t proto, nl_ip_fn fn, void *ctx);
uint32_t nl_ip_send(nl_node *n, const nl_iptx *tx, const void *l4, int len);    /* 0 = not sent */
int      nl_ip_is_local(const nl_node *n, nl_ip ip);
int      nl_ip_mtu(const nl_node *n, nl_ip dst);          /* MTU of the outgoing interface */

/* Routing table. A route is identified by (dst, plen, proto). Lookup is
 * longest prefix first, then lowest administrative distance, then metric;
 * routes through unusable interfaces are skipped. */
void            nl_route_set(nl_node *n, nl_ip dst, int plen, nl_ip gw, int ifx, int metric, int proto);
void            nl_route_del(nl_node *n, nl_ip dst, int plen, int proto);
void            nl_route_flush(nl_node *n, int proto);
const nl_route *nl_route_lookup(const nl_node *n, nl_ip dst);
const char     *nl_route_proto_name(int proto);

/* Packet hooks (NAT, load balancers, firewalls). The callback may rewrite
 * the packet in place with nl_ipv_commit(). in_if/out_if are -1 when not
 * applicable. Return NL_HOOK_OK to continue, NL_HOOK_DROP to discard
 * (the hook then owns nothing: the core frees the packet). */
enum { NL_HOOK_PRE = 0, NL_HOOK_POST = 1, NL_HOOK_FWD = 2 };
enum { NL_HOOK_OK = 0, NL_HOOK_DROP = 1 };
typedef int (*nl_hook_fn)(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if);
void nl_hook_add(nl_node *n, int point, nl_hook_fn fn, void *ctx);
void nl_hook_del(nl_node *n, int point, nl_hook_fn fn, void *ctx);

/* Editable view of the IPv4 packet inside a frame */
typedef struct nl_ipview {
    nl_ip    src, dst;
    uint8_t  proto, ttl, tos;
    uint16_t sport, dport;     /* TCP/UDP ports; for ICMP echo both = identifier */
    uint8_t  icmp_type, icmp_code;
    uint8_t  tcp_flags;
    int      has_ports;        /* 1 for TCP, UDP, ICMP echo (first fragment only) */
    int      ihl, l4off, l4len;/* offsets from the start of the frame */
    int      frag;             /* non-first fragment */
} nl_ipview;
int  nl_ipv_parse(const nl_pkt *p, nl_ipview *v);          /* 1 = IPv4 frame understood */
void nl_ipv_commit(nl_pkt *p, const nl_ipview *v);         /* write src/dst/ports/ttl/tos back, fix checksums */

/* ICMP */
#define NL_ICMP_ECHO_REPLY   0
#define NL_ICMP_UNREACH      3
#define NL_ICMP_REDIRECT     5
#define NL_ICMP_ECHO         8
#define NL_ICMP_TIME_EXCEEDED 11
typedef struct nl_icmpmeta {
    nl_ip    from;             /* who sent the ICMP message */
    uint8_t  type, code;
    uint16_t id, seq;          /* echo / echo reply */
    const uint8_t *data;       /* echo payload, or the quoted original IP packet for errors */
    int      len;
    nl_ip    orig_src, orig_dst;   /* errors: fields of the quoted packet */
    uint8_t  orig_proto;
    uint16_t orig_sport, orig_dport, orig_id;
    uint8_t  ttl;
    uint32_t pkt_id;
    int      mtu;              /* fragmentation needed: next-hop MTU */
} nl_icmpmeta;
typedef void (*nl_icmp_fn)(nl_node *n, void *ctx, const nl_icmpmeta *m);
void     nl_icmp_watch(nl_node *n, nl_icmp_fn fn, void *ctx);     /* every ICMP message received */
void     nl_icmp_unwatch(nl_node *n, nl_icmp_fn fn, void *ctx);
uint32_t nl_icmp_echo(nl_node *n, nl_ip dst, uint16_t id, uint16_t seq, int ttl, int size, int df);

/* ------------------------------------------------------------------ */
/* Layer 4: UDP                                                       */
/* ------------------------------------------------------------------ */

typedef struct nl_udpmsg {
    nl_ip    src, dst;
    uint16_t sport, dport;
    const uint8_t *data;
    int      len, in_if;
    uint8_t  ttl;
    uint32_t pkt_id;
} nl_udpmsg;
typedef void (*nl_udp_fn)(nl_node *n, void *ctx, const nl_udpmsg *m);

int      nl_udp_bind(nl_node *n, uint16_t port, nl_udp_fn fn, void *ctx);   /* port 0 = ephemeral; returns the port, 0 on error */
void     nl_udp_unbind(nl_node *n, uint16_t port);
uint32_t nl_udp_send(nl_node *n, uint16_t sport, nl_ip dst, uint16_t dport, const void *data, int len);
/* Full control: tx->proto is ignored */
uint32_t nl_udp_send_ex(nl_node *n, const nl_iptx *tx, uint16_t sport, uint16_t dport,
                        const void *data, int len);

/* ------------------------------------------------------------------ */
/* IPv6 (ip6.c)                                                       */
/* ------------------------------------------------------------------ */

/* A lab uses IPv6 when its scenario says so (`ipv6 on`, an IPv6 `ip` line,
 * `auto addr6`): every interface of hosts and routers then gets a
 * link-local address and speaks Neighbor Discovery. Labs that do not ask
 * stay IPv4-only, frame for frame. */
#define NL_ETH_IP6     0x86DD
#define NL_PROTO_ICMP6 58
#define NL_PROTO_FRAG6 44
#define NL_IP6_MIN_MTU 1280

int         nl_ip6_enabled(void);
int         nl_parse_ip6(const char *s, nl_ip6 *out);                   /* 1 = ok; accepts :: compression */
int         nl_parse_prefix6(const char *s, nl_ip6 *ip, int *plen);     /* addr/len; no /len = 128 */
const char *nl_ip6_str(const nl_ip6 *ip);                               /* canonical text (RFC 5952); rotating buffers */
int         nl_ip6_eq(const nl_ip6 *a, const nl_ip6 *b);
int         nl_ip6_zero(const nl_ip6 *a);                               /* the unspecified address :: */
int         nl_ip6_linklocal(const nl_ip6 *a);                          /* fe80::/10 */
int         nl_ip6_multicast(const nl_ip6 *a);                          /* ff00::/8 */
int         nl_ip6_match(const nl_ip6 *a, const nl_ip6 *prefix, int plen);
/* IPv6 literal, node name (its first global address, else link-local) or NODE:IF */
int         nl_resolve6(const char *s, nl_ip6 *out);

/* Add an address to an interface: installs the connected route and starts
 * duplicate address detection. Returns 0, or -1 when the interface is full. */
int   nl_if_add_addr6(nl_node *n, int ifx, const nl_ip6 *ip, int plen);
/* Remove an address (not the link-local one) and its connected route: for
 * addresses with a lifetime, such as DHCPv6 leases */
void  nl_if_del_addr6(nl_node *n, int ifx, const nl_ip6 *ip);
/* Receive packets sent to a multicast group (ff02::1:2 for DHCPv6 servers, ...) */
void  nl_ip6_join(nl_node *n, const nl_ip6 *group);
void  nl_ip6_leave(nl_node *n, const nl_ip6 *group);
/* Called on a host for every Router Advertisement accepted on an interface,
 * with its M (managed: addresses from DHCPv6) and O (other configuration
 * from DHCPv6) flags */
typedef void (*nl_ra_fn)(nl_node *n, void *ctx, int ifx, const nl_ip6 *router, int managed, int other);
void  nl_ra_watch(nl_node *n, nl_ra_fn fn, void *ctx);
void  nl_ra_unwatch(nl_node *n, nl_ra_fn fn, void *ctx);
int   nl_ip6_is_local(const nl_node *n, const nl_ip6 *ip);
nl_node *nl_node_by_ip6(const nl_ip6 *ip);
/* Source address the node would use towards dst (RFC 6724 in miniature:
 * link-local for link-local, else a global address of the outgoing
 * interface). 0 = none usable yet. Needed before computing a checksum. */
int   nl_ip6_src(const nl_node *n, const nl_ip6 *dst, nl_ip6 *src);
/* Largest packet the node may send towards dst: the link MTU, lowered by
 * the Packet Too Big messages received (path MTU discovery) */
int   nl_ip6_mtu(const nl_node *n, const nl_ip6 *dst);

void             nl_route6_set(nl_node *n, const nl_ip6 *dst, int plen, const nl_ip6 *gw, int ifx, int metric,
                               int proto);
void             nl_route6_del(nl_node *n, const nl_ip6 *dst, int plen, int proto);
void             nl_route6_flush(nl_node *n, int proto);
const nl_route6 *nl_route6_lookup(const nl_node *n, const nl_ip6 *dst);

typedef struct nl_ip6meta {    /* a received IPv6 packet, parsed (extension headers skipped, reassembled) */
    nl_ip6   src, dst;
    uint8_t  proto, hlim, tclass;      /* proto = the upper-layer next header */
    int      in_if;
    uint32_t pkt_id;
    const uint8_t *l4;
    int      l4len;
    const nl_pkt *pkt;
} nl_ip6meta;
typedef void (*nl_ip6_fn)(nl_node *n, void *ctx, const nl_ip6meta *m);

typedef struct nl_ip6tx {      /* zero = defaults */
    nl_ip6   src, dst;         /* src all zero = chosen as by nl_ip6_src() */
    uint8_t  proto, hlim, tclass;      /* hlim 0 = 64 */
    int      ifx, use_if;      /* use_if = 1: send out of ifx (needed for link-local and multicast) */
    uint32_t pkt_id;
} nl_ip6tx;

void     nl_ip6_bind(nl_node *n, uint8_t proto, nl_ip6_fn fn, void *ctx);
/* Sends l4 as the payload; fragments AT THE SOURCE when it exceeds the path
 * MTU (routers never fragment IPv6). 0 = not sent. */
uint32_t nl_ip6_send(nl_node *n, const nl_ip6tx *tx, const void *l4, int len);
/* Upper-layer checksum with the IPv6 pseudo-header (RFC 8200 section 8.1) */
uint16_t nl_l4_cksum6(const nl_ip6 *src, const nl_ip6 *dst, uint8_t proto, const void *seg, int len);

/* Packet hooks on IPv6 packets: same callback and points as nl_hook_add */
void nl_hook6_add(nl_node *n, int point, nl_hook_fn fn, void *ctx);
void nl_hook6_del(nl_node *n, int point, nl_hook_fn fn, void *ctx);

/* ICMPv6 (RFC 4443) */
#define NL_ICMP6_UNREACH       1
#define NL_ICMP6_TOO_BIG       2
#define NL_ICMP6_TIME_EXCEEDED 3
#define NL_ICMP6_ECHO          128
#define NL_ICMP6_ECHO_REPLY    129
typedef struct nl_icmp6meta {
    nl_ip6   from;
    uint8_t  type, code, hlim;
    uint16_t id, seq;          /* echo */
    const uint8_t *data;       /* echo payload, or the quoted packet for errors */
    int      len;
    nl_ip6   orig_src, orig_dst;       /* errors: the quoted packet */
    uint8_t  orig_proto;
    uint16_t orig_sport, orig_dport, orig_id, orig_seq;
    int      mtu;              /* packet too big */
    uint32_t pkt_id;
} nl_icmp6meta;
typedef void (*nl_icmp6_fn)(nl_node *n, void *ctx, const nl_icmp6meta *m);
void     nl_icmp6_watch(nl_node *n, nl_icmp6_fn fn, void *ctx);
void     nl_icmp6_unwatch(nl_node *n, nl_icmp6_fn fn, void *ctx);
uint32_t nl_icmp6_echo(nl_node *n, const nl_ip6 *dst, uint16_t id, uint16_t seq, int hlim, int size);

/* UDP over IPv6: its own port space, checksum mandatory */
typedef struct nl_udp6msg {
    nl_ip6   src, dst;
    uint16_t sport, dport;
    const uint8_t *data;
    int      len, in_if;
    uint8_t  hlim;
    uint32_t pkt_id;
} nl_udp6msg;
typedef void (*nl_udp6_fn)(nl_node *n, void *ctx, const nl_udp6msg *m);
int      nl_udp6_bind(nl_node *n, uint16_t port, nl_udp6_fn fn, void *ctx);     /* port 0 = ephemeral */
void     nl_udp6_unbind(nl_node *n, uint16_t port);
uint32_t nl_udp6_send(nl_node *n, uint16_t sport, const nl_ip6 *dst, uint16_t dport, const void *data, int len);
uint32_t nl_udp6_send_ex(nl_node *n, const nl_ip6tx *tx, uint16_t sport, uint16_t dport, const void *data,
                         int len);

/* ------------------------------------------------------------------ */
/* Layer 4: TCP (implemented by tcp.c)                                */
/* ------------------------------------------------------------------ */

typedef struct nl_tcp nl_tcp;                  /* one connection */
enum { NL_TCP_CLOSED_FIN = 0, NL_TCP_CLOSED_RST = 1, NL_TCP_CLOSED_TIMEOUT = 2, NL_TCP_CLOSED_REFUSED = 3 };
typedef struct nl_tcp_ops {
    void (*on_connect)(nl_tcp *c, void *ctx);                     /* handshake done (active or passive side) */
    void (*on_data)(nl_tcp *c, void *ctx, const uint8_t *d, int len);
    void (*on_sent)(nl_tcp *c, void *ctx, int acked);             /* bytes newly acknowledged */
    void (*on_eof)(nl_tcp *c, void *ctx);                         /* the peer sent FIN */
    void (*on_close)(nl_tcp *c, void *ctx, int reason);           /* connection gone; c is invalid afterwards */
    /* listeners only: return the ctx for the new connection */
    void *(*on_accept)(nl_tcp *c, void *listen_ctx);
} nl_tcp_ops;
/* opts: "cc=reno|tahoe|newreno|cubic|bbr mss=N rwnd=N ecn=1 sack=1 ..." or NULL */
nl_tcp  *nl_tcp_connect(nl_node *n, nl_ip dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx, const char *opts);
int      nl_tcp_listen(nl_node *n, uint16_t port, const nl_tcp_ops *ops, void *listen_ctx, const char *opts);
void     nl_tcp_unlisten(nl_node *n, uint16_t port);
int      nl_tcp_write(nl_tcp *c, const void *data, int len);      /* bytes accepted into the send buffer */
int      nl_tcp_writable(const nl_tcp *c);                        /* free space in the send buffer */
void     nl_tcp_close(nl_tcp *c);                                 /* orderly: FIN after pending data */
void     nl_tcp_abort(nl_tcp *c);                                 /* RST */
void     nl_tcp_peer(const nl_tcp *c, nl_ip *ip, uint16_t *port);
void     nl_tcp_local(const nl_tcp *c, nl_ip *ip, uint16_t *port);
nl_node *nl_tcp_node(const nl_tcp *c);
/* TCP over IPv6. Connections of the two families are the same objects and
 * use the same callbacks; a listener accepts both. nl_tcp_family() tells
 * which one a connection is (4 or 6): use nl_tcp_peer / nl_tcp_local for
 * IPv4 connections and the *6 variants for IPv6 ones (the others give 0 or
 * the unspecified address). Over IPv6 the MSS leaves room for the 40-byte
 * header (1440 on a 1500-byte MTU) and follows ICMPv6 Packet Too Big; over
 * IPv4 segments carry DF and follow "fragmentation needed" unless the
 * connection was opened with pmtud=0. */
nl_tcp  *nl_tcp_connect6(nl_node *n, const nl_ip6 *dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx,
                         const char *opts);
int      nl_tcp_family(const nl_tcp *c);
void     nl_tcp_peer6(const nl_tcp *c, nl_ip6 *ip, uint16_t *port);
void     nl_tcp_local6(const nl_tcp *c, nl_ip6 *ip, uint16_t *port);

/* ------------------------------------------------------------------ */
/* Dissection (packet inspector)                                      */
/* ------------------------------------------------------------------ */

typedef struct nl_field {
    char  layer[12];           /* "Ethernet", "IPv4", "DNS", ... */
    char  name[28];
    char  value[100];
    short off, len;            /* bytes of the frame this field covers (len 0 = none) */
} nl_field;

typedef struct nl_dissect {
    char     proto[10];        /* short label of the innermost protocol: "ARP", "DNS" */
    short    color;            /* colour of the innermost protocol */
    char     summary[150];     /* one line, innermost protocol wins */
    char     addr[64];         /* "10.0.0.1 → 10.0.0.2" (or MACs) */
    int      nf;
    nl_field f[120];
} nl_dissect;

void nl_dissect_frame(const uint8_t *frame, int len, nl_dissect *o);
void nl_dis_field(nl_dissect *o, const char *layer, const char *name, int off, int len,
                  const char *fmt, ...) __attribute__((format(printf, 6, 7)));
void nl_dis_summary(nl_dissect *o, const char *proto, short color, const char *fmt, ...)
     __attribute__((format(printf, 4, 5)));

/* Application dissectors: d points at the payload, off is its offset in
 * the frame (for nl_dis_field). */
typedef void (*nl_dissect_fn)(nl_dissect *o, const uint8_t *d, int len, int off);
void nl_dissector_udp(uint16_t port, nl_dissect_fn fn);
void nl_dissector_tcp(uint16_t port, nl_dissect_fn fn);
void nl_dissector_ip(uint8_t proto, nl_dissect_fn fn);
void nl_dissector_eth(uint16_t ethertype, nl_dissect_fn fn);
/* For the TCP dissector: run the application dissector of either port */
void nl_dissect_tcp_payload(nl_dissect *o, uint16_t sport, uint16_t dport, const uint8_t *d, int len, int off);

/* ------------------------------------------------------------------ */
/* Modules                                                            */
/* ------------------------------------------------------------------ */

struct nl_module {
    const char *name;          /* used in `run NODE name ...` */
    const char *title;         /* N_() marked */
    int         layer;
    const char *usage;         /* N_() marked: syntax and parameters, one per line */
    const char *theory;        /* N_() marked: what the protocol is and how it works; may be NULL */
    /* `run NODE name args...`: start the module on the node or, when it is
     * already running, execute a command. Return 0, or -1 after logging
     * the reason. */
    int  (*run)(nl_node *n, const nl_args *a);
    void (*stop)(nl_node *n);                  /* free everything; may be NULL */
    void (*crash)(nl_node *n);                 /* node went down: lose volatile state; may be NULL */
    void (*recover)(nl_node *n);               /* node is back; may be NULL */
    /* Draw the module state for node n inside the rectangle; detail 1..3.
     * May be NULL. Return the number of rows used (for scrolling), or 0. */
    int  (*panel)(nl_node *n, int x, int y, int w, int h, int detail);
};

void             nl_module_register(const nl_module *m);
const nl_module *nl_module_find(const char *name);
const nl_module *nl_module_at(int i);          /* enumerate; NULL at the end */
void            *nl_mod_get(const nl_node *n, const nl_module *m);    /* per-node state or NULL */
void             nl_mod_set(nl_node *n, const nl_module *m, void *st);/* NULL removes */

#define NL_CONCAT_(a, b) a##b
#define NL_CONCAT(a, b)  NL_CONCAT_(a, b)
#define NL_INIT(name) static void __attribute__((constructor)) NL_CONCAT(nl_init_, name)(void)
#define NL_MODULE(m) NL_INIT(NL_CONCAT(m, __LINE__)) { nl_module_register(&m); }

/* Run a command line as typed in the console or written in a scenario
 * ("h1 ping h2", "link r1 r2 down", "set detail 2"). 0 = ok. */
int nl_exec(const char *line);

/* Panel drawing helpers (inside nl_module.panel) */
int  nl_ui_kv(int x, int y, int w, const char *label, const char *fmt, ...)
     __attribute__((format(printf, 5, 6)));           /* "label  value"; returns 1 (row used) */
void nl_ui_graph(int x, int y, int w, int h, const nl_series *s, short color, const char *title);
int  nl_ui_wrap(int x, int y, int w, int maxh, tstyle base, const char *text);   /* rows used */
const nl_series *nl_series_find(const nl_node *n, const char *name);

#endif
