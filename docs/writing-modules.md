# Writing a module

Everything a node can run in netlab is a **module**: `ping`, `tcp`, `rip`,
`dns`, `raft`... A module is one C file in `src/` that registers itself when
the program starts and talks to the emulated network only through the API in
[`src/nl.h`](../src/nl.h). Adding a protocol or a tool never requires editing
a core file, the Makefile or a table of modules.

This document explains the API by task. `src/apps.c` (ping, traceroute, echo,
udp, cbr, sink) is the best set of small real examples; `src/rip.c` is a
compact routing protocol; `src/qdisc.c` and `src/fec.c` show queue
disciplines and link codes.

- [A complete example](#a-complete-example)
- [Building and trying it](#building-and-trying-it)
- [The module structure](#the-module-structure)
- [Arguments](#arguments)
- [Per-node state](#per-node-state)
- [Time and timers](#time-and-timers)
- [Crash and recovery](#crash-and-recovery)
- [Sending and receiving](#sending-and-receiving): UDP, TCP, IP, ICMP, Ethernet
- [Routes](#routes)
- [Packet hooks](#packet-hooks)
- [Queue disciplines](#queue-disciplines)
- [Link codes](#link-codes)
- [Dissectors](#dissectors)
- [Logging](#logging)
- [Counters and time series](#counters-and-time-series)
- [The panel](#the-panel)
- [Translations](#translations)
- [Adding a scenario](#adding-a-scenario)
- [Adding a guide topic](#adding-a-guide-topic)
- [Checklist](#checklist)

## Conventions

- **Time** is `nl_time`, virtual microseconds. Use `NL_US()`, `NL_MS()`,
  `NL_SEC()`. Never read the real clock, sleep or block: a module only reacts
  to callbacks, and time advances between them.
- **IPv4 addresses** are `nl_ip` in host byte order (`NL_IP(10, 0, 1, 1)`).
  Bytes on the wire are big-endian: use `nl_get16/32`, `nl_put16/32`.
- **Randomness** comes from `nl_rand01()`, `nl_rand(n)` and
  `nl_rand_exp(mean)` only, so that a run with the same `--seed` repeats.
- **One process may simulate many nodes.** Keep no per-node data in global
  or `static` variables: everything hangs off the node (see
  [Per-node state](#per-node-state)).
- **User-visible strings** are English wrapped in `_()` (or `N_()` in
  initialisers), and may contain the `T_*` colour markup of `tui.h`.
- Include `nl.h`. `nl_int.h` holds the internals shared by the core files
  and is not meant for modules.

## A complete example

`beacon` is a heartbeat: a node sends a small UDP datagram to a peer at a
fixed interval, and every node running the module counts the beacons it
hears. Save it as `src/beacon.c`.

```c
/* beacon.c - a heartbeat: periodic UDP datagrams towards a peer */
#include "nl.h"

#include <stdio.h>
#include <stdlib.h>

#define BEACON_PORT 4242

typedef struct {
    nl_node *n;
    nl_ip    peer;                      /* 0 = only listen */
    nl_time  interval;
    unsigned sent, heard;
    nl_ev    timer;
} beacon_state;

static const nl_module beacon_mod;

static void beacon_tick(void *ctx)
{
    beacon_state *b = ctx;
    char msg[32];
    int len = snprintf(msg, sizeof(msg), "beacon %u", ++b->sent);

    nl_udp_send(b->n, BEACON_PORT, b->peer, BEACON_PORT, msg, len);
    nl_count(b->n, "beacon.sent", 1);
    b->timer = nl_after(b->n, b->interval, beacon_tick, b);
}

static void beacon_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    beacon_state *b = ctx;

    b->heard++;
    nl_count(n, "beacon.heard", 1);
    nl_log(n, NL_L_APP, 2, m->pkt_id, T_OK "beacon" T_RESET " %s %s: %.*s", _("from"), nl_ip_str(m->src),
           m->len, (const char *)m->data);
}

static int beacon_run(nl_node *n, const nl_args *a)
{
    beacon_state *b = nl_mod_get(n, &beacon_mod);
    const char *peer = nl_arg_pos(a, 0);

    if (!b) {                           /* first run on this node */
        if (!(b = calloc(1, sizeof(*b))))
            return -1;
        b->n = n;
        nl_mod_set(n, &beacon_mod, b);
        nl_udp_bind(n, BEACON_PORT, beacon_rx, b);
    }
    nl_cancel(&b->timer);
    b->peer = 0;
    b->interval = nl_arg_time(a, "interval", NL_SEC(1));
    if (peer && !nl_resolve(peer, &b->peer)) {
        nl_log(n, NL_L_APP, 1, 0, T_ERR "beacon: %s '%s'", _("unknown destination"), peer);
        return -1;
    }
    if (b->peer)
        beacon_tick(b);
    return 0;
}

static void beacon_stop(nl_node *n)
{
    beacon_state *b = nl_mod_get(n, &beacon_mod);

    if (!b)
        return;
    nl_cancel(&b->timer);
    nl_udp_unbind(n, BEACON_PORT);
    nl_mod_set(n, &beacon_mod, NULL);
    free(b);
}

static void beacon_recover(nl_node *n)  /* the timer died with the node */
{
    beacon_state *b = nl_mod_get(n, &beacon_mod);

    b->timer = 0;
    if (b->peer)
        beacon_tick(b);
}

static int beacon_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    beacon_state *b = nl_mod_get(n, &beacon_mod);

    (void)h; (void)detail;
    nl_ui_kv(x, y, w, _("peer"), "%s", b->peer ? nl_ip_str(b->peer) : "—");
    nl_ui_kv(x, y + 1, w, _("sent / heard"), "%u / %u", b->sent, b->heard);
    return 2;
}

static const nl_module beacon_mod = {
    .name = "beacon", .title = N_("Heartbeat: periodic UDP beacons towards a peer"), .layer = NL_L_APP,
    .usage = N_("run NODE beacon [PEER] [interval=1s]\n"
                "  Without PEER the node only listens. Counters: beacon.sent beacon.heard"),
    .theory = N_("A heartbeat is the simplest failure detector: silence for longer than a few "
                 "intervals means that the peer, or the path to it, is gone."),
    .run = beacon_run, .stop = beacon_stop, .recover = beacon_recover, .panel = beacon_panel,
};
NL_MODULE(beacon_mod)
```

What each part does:

- `beacon_run` is called for `run NODE beacon ...`. The first time it
  allocates the state, attaches it to the node with `nl_mod_set()` and binds
  the UDP port; called again on the same node, it only changes the
  configuration.
- `beacon_tick` is a timer callback that re-arms itself. The timer is owned
  by the node (`nl_after(b->n, ...)`), so it disappears if the node crashes.
- `beacon_rx` is the UDP handler. It logs at level 2 (a protocol message)
  and links the line to the packet with `m->pkt_id`.
- `beacon_recover` restarts the timer when the node comes back.
- `beacon_panel` draws the tab that appears in the node inspector.
- `NL_MODULE(beacon_mod)` registers the module before `main()` runs.

## Building and trying it

`make` compiles every `.c` file in `src/` into `netlab`; there is nothing to
add to the Makefile. While developing, `tools/build-mod.sh` builds a private
headless binary from the core plus the files you name:

```
tools/build-mod.sh beacon src/beacon.c      # -> ./netlab-beacon
./netlab-beacon help beacon
./netlab-beacon run my-test.nl --detail 2 --seed 1
```

A scenario to exercise the example:

```
node a host
node b host
link a b delay=5ms
auto all
run b beacon
run a beacon b interval=500ms
at 2.2s node a down
at 4.2s node a up
duration 8s
expect b beacon.heard >= 4
```

In the interface, try it without a scenario of its own: open any lab, press
`:` and type `h2 beacon`, then `h1 beacon h2`.

## The module structure

```c
struct nl_module {
    const char *name;      /* used in `run NODE name ...` */
    const char *title;     /* N_() marked: one line */
    int         layer;     /* NL_L_LINK, NL_L_NET, NL_L_TRANSPORT, NL_L_APP, NL_L_DIST */
    const char *usage;     /* N_() marked: syntax and parameters, one per line */
    const char *theory;    /* N_() marked: what it is and how it works; may be NULL */
    int  (*run)(nl_node *n, const nl_args *a);
    void (*stop)(nl_node *n);
    void (*crash)(nl_node *n);
    void (*recover)(nl_node *n);
    int  (*panel)(nl_node *n, int x, int y, int w, int h, int detail);
};
```

| Member | When it is called | Contract |
|---|---|---|
| `run` | `run NODE name args` in a scenario or on the console (also the short form `NODE name args`). | Start the module on the node or, if it is already running there, execute a command. `a` holds only the arguments after the module name. Return 0, or -1 after logging the reason. Required: a module without `run` cannot be started. |
| `stop` | `stop NODE name`, and only if the module has state on the node. | Cancel timers, unbind ports, remove hooks and routes, `nl_mod_set(n, &mod, NULL)`, free. May be NULL. |
| `crash` | The node goes down (`node N down`, key `k`), before the node is marked down. Only for modules with state on the node. | Forget volatile state (tables, connections), flush the routes you installed. May be NULL. |
| `recover` | The node comes back up. | Re-arm timers and restart the protocol from cold. May be NULL. |
| `panel` | Each frame, when the module's tab of the node inspector is visible and the node is simulated by this process. | Draw inside the rectangle `(x, y, w, h)`; `detail` is 1..3. Return the rows used. May be NULL: then the module has no tab. |

Where the texts appear: `title` in `netlab modules`, the Modules screen and
above the panel; `usage` in `netlab help NAME`, in `help NAME` on the
console (one log line per line of text) and, from detail 2, in the Guide
view of a lab; `theory` in `netlab help NAME`, the Modules screen and the
Guide view of every lab where the module runs on the selected node.

`layer` groups the module in listings; the list is kept sorted by layer and
then name.

Registration:

```c
static const nl_module beacon_mod = { .name = "beacon", ... };
NL_MODULE(beacon_mod)
```

`NL_MODULE(m)` expands to a constructor function that calls
`nl_module_register(&m)`. A file may define several modules (`apps.c` has
six). For any other start-up work (registering dissectors, queue
disciplines, link codes) write a constructor with `NL_INIT(name) { ... }`;
`name` only has to be unique in the program.

A forward declaration `static const nl_module beacon_mod;` at the top of the
file lets the functions refer to the module before its definition.

## Arguments

`run` receives the words after the module name, already split:

```c
typedef struct nl_args { int argc; char *argv[NL_MAX_ARGS]; char buf[1024]; } nl_args;
```

| Function | Returns |
|---|---|
| `nl_arg_pos(a, i)` | the i-th token that is not `key=value` (from 0), or NULL |
| `nl_arg_str(a, "key", def)` | the value of `key=value`, or `def` |
| `nl_arg_int(a, "key", def)` | integer |
| `nl_arg_dbl(a, "key", def)` | number; a trailing `%` divides by 100 |
| `nl_arg_time(a, "key", def)` | `nl_time` from `5ms`, `2s`, `100us`, `1.5s`; a bare number is milliseconds |
| `nl_arg_rate(a, "key", def)` | bits per second from `56k`, `10M`, `1G` |
| `nl_arg_has(a, "key")` | 1 if `key=value` or the bare word `key` is present |
| `nl_resolve(s, &ip)` | 1 if `s` is a dotted quad, a node name (its first address) or `NODE:IF` |

By convention the first positional argument of a multi-purpose module is a
sub-command (`tcp send`, `tcp sink`, `dns zone`, `qdisc show`), and
everything optional is `key=value` with a default documented in `usage`.

## Per-node state

```c
void *nl_mod_get(const nl_node *n, const nl_module *m);   /* state or NULL */
void  nl_mod_set(nl_node *n, const nl_module *m, void *st);   /* NULL removes */
```

A module is *running* on a node exactly when it has a non-NULL state there.
That is what makes its tab appear, what makes `stop`, `crash` and `recover`
be called, and what lists its theory in the Guide view. Allocate the state
in `run` the first time, keep a back pointer to the node in it (timer
callbacks receive only your `ctx`), and free it in `stop`. A node can have at
most `NL_MAX_MODS` (12) modules running.

Useful accessors: `nl_node_find(name)`, `nl_node_at(idx)`, `nl_node_count()`,
`nl_node_ip(n)` (first configured address), `nl_node_by_ip(ip)`,
`nl_if_find(n, "eth1")`, `nl_if_usable(n, ifx)`, `nl_if_peer(n, ifx, &pifx)`,
`nl_if_set_addr(n, ifx, ip, plen)`, and the public fields of `nl_node`
(`name`, `kind`, `ifc[]`, `nif`, `rt[]`, `nrt`, `up`, `local`).

A protocol module must only use what a real node could know: its own
interfaces, tables and the packets it receives. Looking at another node's
structures is acceptable for presentation only (for example printing the
name of the node that owns an address, as traceroute does).

## Time and timers

```c
nl_time nl_now(void);
nl_ev   nl_after(nl_node *owner, nl_time delay, void (*fn)(void *ctx), void *ctx);
void    nl_cancel(nl_ev *ev);
```

`nl_after` schedules `fn(ctx)` after `delay` and returns a non-zero handle
(0 only when memory is exhausted).
Timers are one-shot: a periodic timer re-arms itself in its callback.
`nl_cancel(&ev)` is safe on 0 and on a timer that has already fired, and
zeroes the handle. Keep the handle in your state, set it to 0 at the start
of the callback, and cancel it in `stop`.

Pass the node as `owner`: a timer owned by a node that is down is discarded
instead of firing. Pass NULL only for events that do not belong to a node.

Give periodic protocol timers a little random jitter
(`interval + nl_rand(...)`) as real protocols do; otherwise every node of a
symmetric topology fires in the same microsecond.

## Crash and recovery

`node N down` models a power failure:

- `crash` of every module on the node is called;
- the node stops receiving frames, its ARP cache and bridge table are
  cleared, and its pending timers will never fire;
- the module state pointer, counters, time series, bound ports and hooks
  are **kept**: it is the module's job to make the state look like a freshly
  booted node.

So in `crash` forget what a reboot would lose (neighbour tables, learned
routes with `nl_route_flush(n, NL_RT_xxx)`, connections, volatile protocol
state), and keep what would be on disk (a Raft log, a DHCP lease database,
the configuration given to `run`). In `recover`, zero the timer handles and
start the timers again. A module that has timers but no `recover` simply
stays silent after a crash.

## Sending and receiving

### UDP

```c
int      nl_udp_bind(nl_node *n, uint16_t port, nl_udp_fn fn, void *ctx);
void     nl_udp_unbind(nl_node *n, uint16_t port);
uint32_t nl_udp_send(nl_node *n, uint16_t sport, nl_ip dst, uint16_t dport, const void *data, int len);
uint32_t nl_udp_send_ex(nl_node *n, const nl_iptx *tx, uint16_t sport, uint16_t dport, const void *data, int len);

typedef void (*nl_udp_fn)(nl_node *n, void *ctx, const nl_udpmsg *m);
```

`nl_udp_bind` with port 0 picks an ephemeral port; it returns the port, or 0
on error. The handler receives an `nl_udpmsg` with `src`, `dst`, `sport`,
`dport`, `data`, `len`, the input interface `in_if`, the `ttl` and the
`pkt_id`; the data is valid only during the call. Datagrams larger than the
MTU are fragmented and reassembled by the stack. `nl_udp_send_ex` gives full
control through an `nl_iptx` (source address, TTL, TOS, forced interface for
broadcasts, don't-fragment). The send functions return the packet id, or 0
if the datagram could not be sent.

### TCP

TCP is implemented by `src/tcp.c`; applications use callbacks:

```c
typedef struct nl_tcp_ops {
    void  (*on_connect)(nl_tcp *c, void *ctx);
    void  (*on_data)(nl_tcp *c, void *ctx, const uint8_t *d, int len);
    void  (*on_sent)(nl_tcp *c, void *ctx, int acked);
    void  (*on_eof)(nl_tcp *c, void *ctx);
    void  (*on_close)(nl_tcp *c, void *ctx, int reason);
    void *(*on_accept)(nl_tcp *c, void *listen_ctx);     /* listeners: ctx of the new connection */
} nl_tcp_ops;

nl_tcp *nl_tcp_connect(nl_node *n, nl_ip dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx, const char *opts);
int     nl_tcp_listen(nl_node *n, uint16_t port, const nl_tcp_ops *ops, void *listen_ctx, const char *opts);
void    nl_tcp_unlisten(nl_node *n, uint16_t port);
int     nl_tcp_write(nl_tcp *c, const void *data, int len);   /* bytes accepted */
int     nl_tcp_writable(const nl_tcp *c);
void    nl_tcp_close(nl_tcp *c);                              /* FIN after pending data */
void    nl_tcp_abort(nl_tcp *c);                              /* RST */
```

- `nl_tcp_write` may accept fewer bytes than asked when the send buffer is
  full: keep the rest and continue from `on_sent`.
- After `on_close` the connection pointer is invalid; `reason` is one of
  `NL_TCP_CLOSED_FIN`, `_RST`, `_TIMEOUT`, `_REFUSED`.
- `opts` is a string of TCP options (`"cc=cubic mss=1000 ecn=1"`) or NULL;
  modules usually pass through a user argument, as `http` does with `tcp=`.
- `nl_tcp_peer`, `nl_tcp_local` and `nl_tcp_node` describe a connection.

`src/http.c` is a complete client and server on this interface.

### Raw IP and ICMP

```c
void     nl_ip_bind(nl_node *n, uint8_t proto, nl_ip_fn fn, void *ctx);
uint32_t nl_ip_send(nl_node *n, const nl_iptx *tx, const void *l4, int len);
void     nl_icmp_watch(nl_node *n, nl_icmp_fn fn, void *ctx);
void     nl_icmp_unwatch(nl_node *n, nl_icmp_fn fn, void *ctx);
uint32_t nl_icmp_echo(nl_node *n, nl_ip dst, uint16_t id, uint16_t seq, int ttl, int size, int df);
```

`nl_ip_bind` registers the handler of an IP protocol number (OSPF uses 89).
The handler gets an `nl_ipmeta` with the addresses, `ttl`, `tos`, the input
interface and a pointer to the transport payload. To send, fill an `nl_iptx`:
zeroed fields mean defaults (source address of the outgoing interface, TTL
64, route lookup); set `use_if = 1` and `ifx` to send out of a specific
interface without routing, which is how link-local multicast and broadcast
are sent. `nl_cksum()` and `nl_l4_cksum()` compute Internet checksums.

`nl_icmp_watch` delivers every ICMP message the node receives, already
parsed (`nl_icmpmeta`): echo replies with their id and sequence number, and
errors with the fields of the quoted original packet. ping and traceroute
are built on it.

### Ethernet

```c
void     nl_eth_bind(nl_node *n, uint16_t ethertype, nl_eth_fn fn, void *ctx);
uint32_t nl_eth_send(nl_node *n, int ifx, const uint8_t dst[6], uint16_t ethertype,
                     const void *payload, int len, uint32_t pkt_id);
```

For protocols that live directly on layer 2. On a switch the handler runs
before bridging, which is how spanning tree receives BPDUs (`src/stp.c`). A
bridge module can block a port by setting `n->ifc[i].blocked` and should call
`nl_fdb_flush(n)` when the topology changes.

## Routes

```c
void            nl_route_set(nl_node *n, nl_ip dst, int plen, nl_ip gw, int ifx, int metric, int proto);
void            nl_route_del(nl_node *n, nl_ip dst, int plen, int proto);
void            nl_route_flush(nl_node *n, int proto);
const nl_route *nl_route_lookup(const nl_node *n, nl_ip dst);
```

A route is identified by `(dst, plen, proto)`: setting it again replaces it.
`proto` says who owns the route and is also its administrative distance:
`NL_RT_CONNECTED` 0, `NL_RT_STATIC` 1, `NL_RT_DHCP` 5, `NL_RT_BGP` 20,
`NL_RT_OSPF` 110, `NL_RT_RIP` 120. Lookup prefers the longest prefix, then
the lowest distance, then the lowest metric, and skips routes through
unusable interfaces. `gw` 0 means directly connected.

A routing protocol installs its results with its own `proto`, removes them
with `nl_route_del`, and calls `nl_route_flush(n, proto)` in `stop` and
`crash`. A new protocol needs a new `NL_RT_*` value in `nl.h` and a name in
`nl_route_proto_name()` (in `src/stack.c`): the one case where a core file
is touched. The Routes tab highlights routes younger than three seconds.

## Packet hooks

For modules that inspect, rewrite or drop packets in transit (NAT, load
balancers, firewalls):

```c
enum { NL_HOOK_PRE, NL_HOOK_POST, NL_HOOK_FWD };
enum { NL_HOOK_OK, NL_HOOK_DROP };
typedef int (*nl_hook_fn)(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if);
void nl_hook_add(nl_node *n, int point, nl_hook_fn fn, void *ctx);
void nl_hook_del(nl_node *n, int point, nl_hook_fn fn, void *ctx);
```

| Point | Called for | `in_if` / `out_if` |
|---|---|---|
| `NL_HOOK_PRE` | every valid IPv4 packet received, before the node decides whether it is for itself or must be forwarded | input / -1 |
| `NL_HOOK_FWD` | packets being forwarded, after the route lookup | input / output |
| `NL_HOOK_POST` | every IPv4 packet about to leave an interface, forwarded or locally generated (each fragment separately) | input or -1 / output |

Return `NL_HOOK_DROP` to discard the packet; the core frees it. To rewrite
a packet, parse it into an editable view, change the fields and commit:

```c
static int my_hook(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    nl_ipview v;

    if (!nl_ipv_parse(p, &v) || !v.has_ports)
        return NL_HOOK_OK;
    if (v.proto == NL_PROTO_UDP && v.dport == 8080) {
        v.dst = NL_IP(10, 0, 2, 10);
        v.dport = 80;
        nl_ipv_commit(p, &v);           /* writes the fields back and fixes the checksums */
    }
    return NL_HOOK_OK;
}
```

`src/nat.c` and `src/lb.c` are the full-size examples. Remove the hooks in
`stop`.

## Queue disciplines

A queue discipline decides what happens when packets arrive at an output
interface faster than the link can send them. It is not a module: it is an
`nl_qdisc` registered from a constructor and selected per link with
`qdisc=NAME`.

```c
typedef struct nl_qdisc {
    const char *name, *title;
    void   *(*create)(nl_link *l, int dir, const nl_args *a);
    int     (*enqueue)(void *st, nl_link *l, int dir, nl_pkt *p);    /* NL_Q_ACCEPT or NL_Q_DROP */
    nl_pkt *(*dequeue)(void *st, nl_link *l, int dir);
    void    (*panel)(void *st, nl_link *l, int dir, int x, int y, int w, int h);
} nl_qdisc;

NL_INIT(my_qdisc) { nl_qdisc_register(&q_mine); }
```

- `create` is called once per direction of the link, and again whenever the
  link is reconfigured. It returns the state (allocated with
  `malloc`/`calloc`; the core frees it with `free`). `a` holds the link
  parameters the core did not recognise, so `link a b qdisc=mine limit=5ms`
  reaches `create` as `limit=5ms`.
- `enqueue` sees each arriving packet before it is stored in the FIFO ring
  of the direction (`l->d[dir]`: `qlen`, `qbytes`, `qalloc`). Return
  `NL_Q_DROP` to refuse it (the core counts, logs and frees it), or mark it
  with `nl_ecn_mark(p)` and accept. The core still drops when the ring is
  full. NULL = accept everything.
- `dequeue` picks the next packet to transmit. `nl_q_pop(l, dir, &sojourn)`
  takes the head of the ring and tells how long it waited;
  `nl_q_peek(l, dir, i)` looks without removing; `nl_q_drop(l, dir, p, why)`
  discards a packet with a log line and a capture. NULL = plain FIFO.
- A shaper whose `dequeue` returns NULL while packets are waiting must
  arrange to be asked again: arm a timer and call `nl_link_kick(l, dir)`.
- `panel` draws two rows in the Links tab of the transmitting node.

Record measurements with `nl_sample()` on the transmitting node
(`l->n[dir]`), throttled to a few samples per second of virtual time.

## Link codes

A link code adds redundancy to each frame so that bit errors (`ber=`,
`corrupt=`) can be detected or corrected; it is selected with `codec=NAME`.

```c
typedef struct nl_codec {
    const char *name, *title;
    int (*encode)(const uint8_t *in, int n, uint8_t *out, int cap);              /* bytes written, <0 error */
    int (*decode)(const uint8_t *in, int n, uint8_t *out, int cap, int *corrected);
} nl_codec;

NL_INIT(my_codec) { nl_codec_register(&c_mine); }
```

The link encodes the frame, flips bits of the *encoded* bytes, and decodes.
`decode` returns the decoded length, or -1 when it detected an error it could
not correct (the frame is discarded, like a bad FCS). If it returns data that
differs from the original, the frame is delivered damaged and counted as an
undetected error; if the data is intact although bits were flipped, it is
counted as corrected. The encoded length is what occupies the link, so a
rate-1/3 code triples the transmission time. Both functions are pure: no
state, no node.

## Dissectors

The Packets view, the labels on the map and the sequence diagram are
produced by dissectors. The core dissects Ethernet, ARP, IPv4, ICMP, UDP and
TCP; a module adds the dissector of its own protocol:

```c
static void beacon_dissect(nl_dissect *o, const uint8_t *d, int len, int off)
{
    nl_dis_field(o, "Beacon", "Text", off, len, "%.*s", len, (const char *)d);
    nl_dis_summary(o, "BEACON", C_OK, "%.*s", len, (const char *)d);
}

NL_INIT(beacon_dissector)
{
    nl_dissector_udp(4242, beacon_dissect);
}
```

| Registration | Matches |
|---|---|
| `nl_dissector_udp(port, fn)` | UDP datagrams from or to the port |
| `nl_dissector_tcp(port, fn)` | TCP payload from or to the port |
| `nl_dissector_ip(proto, fn)` | an IP protocol number |
| `nl_dissector_eth(ethertype, fn)` | an EtherType |

`d` points to the payload and `off` is its offset inside the frame.

- `nl_dis_field(o, layer, name, off, len, fmt, ...)` adds a line to the
  field tree. `off` and `len` are the bytes of the frame the field covers:
  the hex dump highlights them when the field is selected (`len` 0 = none).
  Fields with the same `layer` string are grouped under one heading.
- `nl_dis_summary(o, proto, color, fmt, ...)` sets the short protocol label
  (at most 9 characters; it is what travels on the map), its colour and the
  one-line summary. The innermost protocol wins, so call it last.

A dissector receives bytes from the wire, possibly damaged or truncated:
check `len` before every read. It must not depend on module state: the
monitor of a distributed run dissects frames of nodes it does not simulate.

## Logging

```c
void nl_log(nl_node *n, int layer, int level, uint32_t pkt_id, const char *fmt, ...);
```

| Level | Use it for | Example |
|---|---|---|
| 1 | The outcome a beginner should see. A lab watched at detail 1 must still tell its story. | `ping h2: reply from 10.0.2.10 seq=1` |
| 2 | Protocol messages sent and received, state changes, drops. | `ARP who has 10.0.1.1?` |
| 3 | Every frame and internal step. | timer ticks, table scans |

- `n` is the node the line belongs to (NULL for none); the log can be
  filtered by node.
- `layer` is one of `NL_L_PHY`, `NL_L_LINK`, `NL_L_NET`, `NL_L_TRANSPORT`,
  `NL_L_APP`, `NL_L_DIST`, `NL_L_SIM`; it is shown as `L2`, `L3`... and
  coloured.
- `pkt_id` links the line to a packet (0 = none): pass the `pkt_id` of the
  message you are handling or the value returned by a send function.
- A line is at most about 250 bytes. Colour it with the markup of `tui.h`:
  `T_OK`, `T_ERR`, `T_WARN`, `T_INFO`, `T_DIM`, `T_BOLD`, ... and `T_RESET`.
  Headless output strips the markup.

Write lines that read well alone: start with the protocol name, say what
happened and to whom. Protocol jargon (`[SYN, ACK]`, `who has`) stays in
English; ordinary words go through `_()`.

## Counters and time series

```c
void   nl_count(nl_node *n, const char *name, double delta);
void   nl_count_set(nl_node *n, const char *name, double v);
double nl_count_get(const nl_node *n, const char *name);
void   nl_sample(nl_node *n, const char *series, double v);
const nl_series *nl_series_find(const nl_node *n, const char *name);
```

**Counters** are named numbers attached to a node, created on first use.
They are listed in the Node tab (detail 2 and above) and are what scenario
`expect` lines test, so they are the test interface of your module: count
every outcome a lab might want to assert (`beacon.heard`, `tcp.send_done`).
Name them `module.what`, at most 27 characters. A node holds at most 48;
document them in `usage`.

**Time series** feed the Graphs view and `nl_ui_graph()`. `nl_sample`
appends a point at the current time; a series keeps the last 600 points and
a node at most 8 series. The name (at most 23 characters) is the title of
the graph: include the unit, as in `"ping rtt (ms)"`. Sample on change or at
a fixed virtual-time period, not on every packet.

## The panel

`panel(n, x, y, w, h, detail)` draws the module's tab. It is called at
every screen refresh, so it must only read state. The rectangle starts at
column `x`, row `y`, and is `w` columns by `h` rows; never draw outside it.

| Helper | Draws |
|---|---|
| `nl_ui_kv(x, y, w, label, fmt, ...)` | a dimmed label and a value on one row; returns 1 |
| `nl_ui_wrap(x, y, w, maxh, style, text)` | word-wrapped text; returns the rows used |
| `nl_ui_graph(x, y, w, h, series, color, title)` | a bar graph of a time series with scale and last value (needs `h` ≥ 3) |
| `tui_print(x, y, maxw, TS(fg, bg, attr), fmt, ...)` | one clipped line with `T_*` markup |
| `tui_bar(x, y, w, fraction, color)` | a horizontal gauge |

Colours are the `C_*` constants of `tui.h` (`C_TEXT`, `C_DIM`, `C_OK`,
`C_ERR`, `C_WARN`, `C_INFO`, `C_DATA`, `C_ACK`); `TS(C_TEXT, C_DEFAULT, 0)`
is ordinary text. Use `detail` to show more at levels 2 and 3. The panel is
not called for a node simulated by another process.

## Translations

netlab is written in English and translated at run time from gettext PO
files; see [`locale/README.md`](../locale/README.md).

1. Wrap user-visible strings in `_("...")`. In static initialisers (the
   `title`, `usage` and `theory` of the module, tables of labels) use
   `N_("...")`: the core translates them when it displays them.
2. Keep colour markup outside the translated string:
   `T_ERR "%s"` with `_("no reply")` as the argument.
3. Run `tools/extract-strings.py` to rebuild `locale/netlab.pot`.
4. Add the translations in a catalog named after the source file:
   `src/beacon.c` → `locale/it/beacon.po`. Every `.po` file in the language
   directory is loaded; when a `msgid` appears in several, the first one read
   wins, so translate short common words (`from`, `sent`, `lost`) the same
   way everywhere.
5. `tools/extract-strings.py --check locale/it src/beacon.c` lists the
   strings of your file that are still untranslated and the stale entries.

A string with no translation is shown in English, so a module works before
it is translated.

## Adding a scenario

A module is not finished until a lab shows it. Create
`scenarios/NNN-name.nl` following [scenario-format.md](scenario-format.md):

- The number places the lab in the catalog: first digit the track (1
  Foundations ... 4 Frontier, matching `level`), then the subject group of
  the neighbouring labs.
- Give it `title`, `level`, `layer`, `topic`, an `about` that says what to
  watch and what to try, `note` lines that narrate the run, and `param`
  lines for the values worth changing.
- End with `expect` lines on your counters and a `duration`. Then
  `netlab check NNN-name` must print `PASS`, and `netlab check` (the whole
  catalog, seed 1) must still report 0 failures.
- Add `title[it]`, `about[it]`, `note[it]` and `param[it]` lines if you
  maintain the Italian translation.

## Adding a guide topic

The study guide is a set of Markdown files in `guide/en/` (and translations
with the same file name in `guide/<lang>/`). A topic file starts with:

```
<!-- level: 2 -->
<!-- labs: 250-nat-basics, 251-port-forwarding -->
# Title of the topic
```

`level` is 0 for *Getting started* and 1..4 for the tracks; topics are
listed by level and then by file name. `labs` names up to 8 catalog labs
(without `.nl`) that the reader can start from the topic with the keys `a`,
`b`, `c`...

The body is read at three depths. Text after a line `<!-- detail: 2 -->` is
shown only at depths 2 and 3, after `<!-- detail: 3 -->` only at depth 3,
and `<!-- detail: 1 -->` returns to text that is always shown. Write depth 1
so that it is a complete short read on its own.

The renderer understands a small subset of Markdown: `#`, `##`, `###`
headings, paragraphs separated by blank lines, `- ` bullets (one source line
each, no nesting), `**bold**`, `` `code` `` and fenced code blocks. No
tables, images or numbered lists. Check the result with `netlab guide NAME`.

## Checklist

- [ ] One file `src/NAME.c` that includes `nl.h`; no per-node globals.
- [ ] `NL_MODULE()` with `name`, `title`, `layer`, `usage` (every argument
      and counter) and `theory`.
- [ ] `run` validates its arguments and logs the reason before returning -1.
- [ ] `stop` cancels timers, unbinds, removes hooks and routes, frees.
- [ ] `crash` / `recover` if the module has timers or volatile state.
- [ ] Log lines at the right levels; the story is readable at detail 1.
- [ ] Counters for everything a scenario may want to `expect`.
- [ ] A `panel`, and a dissector if the protocol has its own messages.
- [ ] Only `nl_rand*` for randomness; the same seed gives the same run.
- [ ] Strings in `_()` / `N_()`; `tools/extract-strings.py --check` is clean.
- [ ] A scenario with expectations; `netlab check` passes.
- [ ] A guide topic, or a mention in an existing one.
