# The scenario language

A netlab scenario (a *lab*) is a plain text file, by convention `NAME.nl`. It
describes a topology, its addressing, what runs on the nodes and when, the
parameters the user may change, the commentary shown while it runs and the
conditions that must hold at the end.

For a step-by-step introduction read the guide topic *Writing your own labs*
(`netlab guide writing`). This document is the reference; it follows
`src/scenario.c` and `src/link.c`.

- [Syntax](#syntax)
- [Summary of directives](#summary-of-directives)
- [Metadata](#metadata): `title`, `about`, `level`, `layer`, `topic`
- [Parameters](#parameters): `param`, `param+`, `$name`
- [Language variants](#language-variants): `[lang]`
- [Topology](#topology): `node`, `link`, `vlan`
- [Link parameters](#link-parameters)
- [Units](#units)
- [Addressing and routing](#addressing-and-routing): `ip`, `gw`, `route`, `auto`
- [Running things](#running-things): `run`, `at`, `stop`, `node up|down`, `set`, `log`
- [Commentary](#commentary): `note`
- [Run control](#run-control): `duration`, `seed`
- [Expectations](#expectations): `expect`
- [Distributed runs](#distributed-runs): `place`, `monitor`
- [Runtime-only commands](#runtime-only-commands)
- [Order of evaluation](#order-of-evaluation)
- [Limits](#limits)
- [Where scenarios are found](#where-scenarios-are-found)

## Syntax

- One directive per line: a keyword followed by arguments separated by spaces
  or tabs. Leading whitespace is ignored; trailing spaces are removed.
- Empty lines and lines whose first non-blank character is `#` are ignored.
  There are no end-of-line comments.
- An argument is either a plain word (*positional*) or `key=value`. Positional
  arguments and `key=value` pairs may be mixed in any order; a positional
  argument is found by its position among the positional ones.
- Double quotes group words: `"two words"` is one argument, and
  `msg="hello world"` is one `key=value` pair. There is no escape character.
- A line has at most 40 arguments and about 1000 characters. Directives that
  take free text (`title`, `about`, `note`, the label of `param`, `log`) use
  the rest of the line verbatim, so their text is not limited to 40 words.
- Keywords are case sensitive and lower case.
- A line with an error is reported as `FILE:LINE: message` on standard error;
  loading continues to report every error, then the program exits with
  status 2.

## Summary of directives

| Directive | Where | Purpose |
|---|---|---|
| `title TEXT` | file | name shown in the catalog and in the top bar |
| `about TEXT` | file | description (repeatable) |
| `level N` | file | track: 1 Foundations, 2 Core, 3 Advanced, 4 Frontier |
| `layer N` | file | protocol layer: 1, 2, 3, 4, 7 (application), 8 (distributed) |
| `topic WORD` | file | keyword for the catalog search |
| `param NAME DEFAULT [LABEL]` | file | declare a parameter |
| `param+ NAME DEFAULT [LABEL]` | file | declare an advanced parameter |
| `node NAME KIND [x= y=] [role=]` | file | create a node |
| `link A[:IF] B[:IF] [key=value ...]` | file | create a link |
| `link A B up\|down` / `link A B key=value ...` | file, `at`, console | change an existing link |
| `vlan SWITCH:PORT N\|trunk` | file | VLAN membership of a switch port |
| `ip NODE[:IF] A.B.C.D/LEN` | file, `at`, console | set an interface address |
| `gw NODE GATEWAY` | file, `at`, console | set the default route |
| `route NODE [add] PREFIX via GATEWAY [metric=N]` | file, `at`, console | add a static route |
| `route NODE [add] PREFIX dev IF [metric=N]` | file, `at`, console | add an on-link static route |
| `route NODE del PREFIX` | file, `at`, console | remove a static route |
| `auto addr\|routes\|all` | file | automatic addressing and routing |
| `run NODE MODULE [args]` | file, `at`, console | start a module or send it a command |
| `NODE MODULE [args]` | file, `at`, console | short for `run NODE MODULE ...` |
| `stop NODE MODULE` | file, `at`, console | stop a module |
| `at TIME COMMAND` | file, console | run a command later |
| `node NAME up\|down` | `at`, console | crash or recover a node |
| `set speed\|detail\|baseport VALUE` | file, `at`, console | change a setting |
| `log TEXT` | file, `at`, console | write a line in the event log |
| `help [MODULE]` | console | list modules, or print the usage of one in the log |
| `note TIME TEXT` | file | commentary shown when the lab reaches TIME |
| `duration TIME` | file | length of a headless run (default `30s`) |
| `seed N` | file | random seed, unless `--seed` is given |
| `expect NODE COUNTER OP VALUE` | file | condition checked at the end |
| `place NODE HOST[:PORT]` | file | where a node runs in a distributed lab |
| `monitor HOST[:PORT]` | file | where the monitor of a distributed lab listens |

*file* = a line of the scenario; *`at`* = as the command of an `at` line;
*console* = typed after `:` in a running lab.

## Metadata

```
title  Two networks and a router: ping, traceroute, fragmentation
level  1
layer  3
topic  ip
about  h1 and h2 live on different networks, joined by the router r1.
about  Things to try: select r1 and read its routing table.
```

| Directive | Meaning | Default |
|---|---|---|
| `title TEXT` | One line. | the file name |
| `about TEXT` | Description shown in the catalog and in the Guide view of the lab. Consecutive `about` lines are joined with a space into one paragraph; an `about` line with no text starts a new paragraph. The text is rendered with the guide's Markdown subset (`**bold**`, `` `code` ``). At most about 4000 characters. | empty |
| `level N` | Track in the catalog: 1 Foundations, 2 Core, 3 Advanced, 4 Frontier. Anything else is listed as *Other* by `netlab list`. Files in the user directory are always listed under *My scenarios*. | 0 |
| `layer N` | Layer used by the catalog filter and shown as `L3` next to the title: 1 physical, 2 link, 3 network, 4 transport, 7 application, 8 distributed. | 0 |
| `topic WORD` | Extra keyword matched by the catalog search (which also matches the title and the file name). | empty |

## Parameters

```
param  cc reno Congestion control (tahoe, reno, newreno, cubic, bbr)
param  bw 2M Bottleneck rate
param+ delay 20ms One-way delay of the bottleneck

link r1 r2 delay=$delay bw=$bw
at 1s run h1 tcp send h2 bytes=5M cc=$cc
```

`param NAME DEFAULT [LABEL]` declares a parameter. `NAME` is made of letters,
digits and `_` (at most 23 characters); `DEFAULT` is a single argument (quote
it if it contains spaces; at most 63 characters); `LABEL` is the rest of the
line and is what the launch form shows (default: the name).

`param+` is the same but marks the parameter **advanced**: the launch form
shows it only when *Settings* is switched to *advanced*.

### `$name` substitution

Before a line is interpreted, every `$name` and `${name}` whose name is a
parameter known *at that point of the file* is replaced by the parameter's
value. Consequences:

- A `param` line must precede the lines that use the parameter.
- Substitution is textual and happens everywhere on the line, including
  `about` and `note` text and comments. `$` followed by a name that is not a
  parameter is left untouched.
- Use `${name}` when the reference is followed by a letter, digit or `_`.

### Setting values

- Command line: `netlab run FILE name=value ...` (also `windows`, and `node`
  after the node list). Command-line values win over the defaults. A
  `name=value` for a name the file never declares is still substituted.
- Menu: the launch form has one field per parameter and passes every value
  as `name=value`.

A scenario may have at most 32 parameters.

## Language variants

`title`, `about`, `note` and `param` accept a two-letter language code in
square brackets:

```
title      Static routing
title[it]  Instradamento statico
about[it]  Tre router in fila, configurati a mano.
note 6s    The administrator adds the missing route on r2.
note[it] 6s L'amministratore aggiunge la rotta mancante su r2.
param      loss 0 Packet loss on the h1 link (0..1)
param[it]  loss Perdita di pacchetti sul collegamento di h1 (0..1)
```

- A `[xx]` line is used only when netlab runs in language `xx` (see
  `--lang`, `NETLAB_LANG`); otherwise it is skipped.
- `title[xx]` replaces the title.
- `about[xx]` lines replace *all* the plain `about` lines, wherever they are
  in the file.
- `note[xx] TIME text` replaces the plain note that has the same `TIME`, in
  either order. A plain note with no translated counterpart is still shown.
- `param[xx] NAME LABEL` replaces only the label. It takes no default value
  and must come after the plain `param NAME ...` line.

## Topology

### `node`

```
node NAME [host|router|switch] [x=N y=N] [role=TEXT]

node h1 host
node r1 router x=36 y=50
node ns host role=DNS
node s1 switch
```

| Part | Meaning | Default |
|---|---|---|
| `NAME` | Starts with a letter, at most 12 characters, unique. Avoid names that are also directives. | required |
| kind | `host`: end system, does not forward. `router`: forwards IPv4 packets between its interfaces. `switch`: learning bridge, no IP addresses. | `host` |
| `x=`, `y=` | Position on the map, 0..100 each, origin at the top left. Both must be given. Nodes without a position are placed by a force-directed layout; if some nodes are placed by hand, those stay fixed and the others are arranged around them. | automatic |
| `role=` | Short label (at most 11 characters) drawn next to the node: `DNS`, `NAT`, `LB`... | none |

Nodes are numbered in the order of their `node` lines, from 0. The index
determines MAC addresses (`02:00:4e:4c:<index+1>:<interface>`) and the
default tunnel port in distributed mode.

### `link`

```
link A[:IF] B[:IF] [key=value ...]

link h1 r1
link r1 r2 bw=2M delay=20ms queue=25 loss=1%
link a1 s1:p0 delay=2ms
link r1:eth1 r2:eth0 bw=1M qdisc=codel target=5ms
```

Creates a full-duplex point-to-point link between two different nodes. Each
direction has its own queue and transmitter with the same parameters (see
[Link parameters](#link-parameters)).

Interfaces are named `eth0`, `eth1`, ... on hosts and routers and `p0`, `p1`,
... on switches. Without `:IF` the link takes the first interface of the node
that is not connected yet, creating a new one if necessary; so the n-th link
of a node normally lands on interface n-1. With `:IF` the named interface is
used (and created, together with any lower-numbered ones that do not exist
yet); it must not be connected already.

If a link between `A` and `B` already exists and no new interface is named,
the line does not create a second link: it changes the existing one, exactly
as at run time.

### Changing a link

```
link A B down
link A B up
link A B key=value ...

at 5s link r1 r2 down
at 9s link r1 r2 bw=500k loss=0.02
```

Usable in the file (after the link was created), in `at` lines and on the
console. `down` discards the frames waiting in both queues; frames already on
the wire are dropped on arrival. Routes through a link that is down are
skipped by the forwarding lookup. Changing parameters keeps the queued
packets (those beyond a smaller `queue` are discarded) and re-creates the
state of the queue discipline. On the console the new settings are echoed in
the log.

### `vlan`

```
vlan SWITCH:PORT N
vlan SWITCH:PORT trunk

vlan s1:p0 10
vlan s1:p2 trunk
```

Makes a switch port an access port of VLAN `N`, or an 802.1Q trunk that
carries every VLAN tagged. A port with no `vlan` line is in VLAN 0, the
untagged default. The link that owns the port must already exist. File only.

## Link parameters

| Key | Meaning | Default | Range / notes |
|---|---|---|---|
| `bw` (alias `rate`) | Bit rate. Serialisation time of a frame is `(length + 4) × 8 / bw`; the 4 bytes are the frame check sequence. | `10M` | `0` = infinitely fast |
| `delay` | One-way propagation delay. | `10ms` | |
| `jitter` | Extra delay drawn uniformly in `[0, jitter)` for every frame. Frames may overtake each other. | `0` | |
| `loss` | Probability that a frame is lost on the wire. | `0` | `0.05` or `5%` |
| `burst` | Mean length, in frames, of a loss burst. With a value above 1 losses follow a Gilbert-Elliott model: every frame is lost while in the bad state, and the long-run loss rate is still `loss`. | `0` (independent losses) | plain number |
| `ber` | Bit error rate: every bit of the frame is flipped independently with this probability. | `0` | plain number, e.g. `1e-5` |
| `corrupt` | Probability that a frame gets exactly one flipped bit. | `0` | `0.1` or `10%` |
| `dup` | Probability that a frame is delivered twice. | `0` | `0.01` or `1%` |
| `reorder` | Probability that a frame is held back (by one more `delay` plus three serialisation times) so that later frames arrive first. | `0` | `0.1` or `10%` |
| `queue` | Capacity of each output queue, in packets. | `50` | 1..10000 |
| `mtu` | Largest IP packet the link carries. | `1500` | 68..1636 |
| `qdisc` | Queue discipline, see below. | `droptail` | |
| `codec` (alias `fec`) | Link code, see below. | `fcs` | |
| anything else | Passed as `key=value` to the queue discipline. | | at most about 95 characters in total |

Because unknown keys are handed to the queue discipline, a misspelt parameter
(`dealy=5ms`) is **not** reported as an error: it is silently ignored unless
the discipline understands it.

What happens to a frame, in order: it is queued (or dropped by the queue
discipline or because the queue is full); when the transmitter is free it is
dequeued; the loss model decides whether it is lost; otherwise bit errors are
applied and the link code decides whether the frame is delivered intact,
repaired, delivered damaged or discarded; it arrives after serialisation +
propagation (+ jitter, + the reordering penalty).

### Queue disciplines (`qdisc=`)

| Name | Behaviour | Extra keys (defaults) |
|---|---|---|
| `droptail` | Arrivals are dropped when the queue holds `queue` packets. | |
| `fifo` | Drop-tail that also records queue length and waiting time for the graphs. | |
| `red` | Random early drops (or ECN marks) as the average queue grows. | `min=N` `max=N` (packets), `maxp=0.1`, `wq=0.02`, `gentle=1`, `ecn=0` |
| `codel` | Keeps the time packets spend in the queue near a target. | `target=5ms`, `interval=100ms`, `ecn=0` |
| `pie` | Drop probability steered by a controller on the queueing delay. | `target=15ms`, `tupdate=15ms`, `ecn=0` |
| `fq` | One queue per flow (IP 5-tuple), served by deficit round robin. | `quantum=1514` (bytes per round) |
| `prio` | Strict priority, three bands chosen by the DSCP: band 0 for DSCP ≥ 40 and ARP, band 1 for other non-zero DSCP, band 2 best effort. | |
| `tbf` | Token bucket shaper; the link `bw` still applies underneath. | `shape=RATE`, `bucket=BYTES` |

`netlab modules` lists the disciplines compiled in and `netlab help qdisc`
gives their parameters and counters.

```
link r1 r2 bw=1M delay=5ms qdisc=red queue=100 min=10 max=40 ecn=1
link r1 r2 bw=10M qdisc=tbf shape=1M bucket=20000
```

### Link codes (`codec=`)

| Name | Behaviour |
|---|---|
| `fcs` | Frame check sequence: a damaged frame is detected and discarded (default). |
| `none` | No check: damaged frames reach the upper layers. |
| `parity` | One even-parity bit per frame: detects an odd number of flipped bits. |
| `checksum` | 16-bit Internet checksum trailer. |
| `crc8`, `crc16`, `crc32` | CRC trailers: detect every burst up to 8, 16, 32 bits. |
| `rep3` | Three copies and a majority vote: corrects, at three times the airtime. |
| `hamming74` | Hamming(7,4): corrects one flipped bit in every 7. |
| `hamming84` | Hamming(8,4) SECDED: corrects one bit per byte, detects two. |
| `hamming74i` | Hamming(7,4) with a block interleaver: also survives bursts. |
| `rs` | Reed-Solomon (255,223): corrects 16 bytes per block. |

With a code other than `fcs` and `none` the frame is really encoded, the bit
errors are applied to the encoded bits, and the frame is decoded at the far
end; the encoded length is what occupies the link.

## Units

**Time** (`delay`, `jitter`, `at`, `note`, `duration`, `--until`, module
arguments such as `interval=`):

| Suffix | Unit | Example |
|---|---|---|
| `us` | microseconds | `500us` |
| `ms` or none | milliseconds | `20ms`, `20` |
| `s` | seconds | `1.5s` |
| `m`, `min` | minutes | `2m` |

A bare number is milliseconds. Decimals are allowed; the resolution of
virtual time is one microsecond.

**Rate** (`bw`, `shape=`, module arguments such as `rate=`), in bits per
second:

| Suffix | Multiplier | Example |
|---|---|---|
| none or `b` | 1 | `9600` |
| `k`, `K` | 10^3 | `56k` |
| `m`, `M` | 10^6 | `10M` |
| `g`, `G` | 10^9 | `1G` |

**Probabilities** (`loss`, `corrupt`, `dup`, `reorder`) are a fraction
(`0.05`) or a percentage (`5%`). `burst` and `ber` are plain numbers.

**Sizes** given to modules (`bytes=5M`, `size=3000`) are defined by each
module: see `netlab help MODULE`.

## Addressing and routing

### `ip`

```
ip NODE[:IF] A.B.C.D/LEN

ip h1 10.0.1.10/24
ip r1:eth1 10.0.12.1/24
```

Sets the IPv4 address and prefix length of an interface and installs the
connected route. Without `:IF` the first interface of the node that has no
address is used. Without `/LEN` the prefix length is 32. The interface must
exist: put `ip` lines after the `link` lines.

### `gw`

```
gw NODE GATEWAY

gw h1 10.0.1.1
gw h1 r1
```

Sets the default route (`0.0.0.0/0`, static, metric 1). `GATEWAY` is an
address, or the name of a node that has an address on a subnet of `NODE`
(that address is used). The gateway must be on a subnet of the node, so the
addresses must already be set: `gw` by name cannot be combined with
`auto addr`, which assigns addresses only after the whole file is read (and
sets the default gateways of hosts itself).

### `route`

```
route NODE [add] PREFIX via GATEWAY [metric=N]
route NODE [add] PREFIX dev IF [metric=N]
route NODE del PREFIX

route r1 10.0.4.0/24 via 10.0.12.2
route r1 10.0.4.0/24 via r2 metric=5
route r2 10.0.9.0/24 dev eth1
at 6s route r2 add 10.0.4.0/24 via 10.0.23.3
at 9s route r3 del 10.0.1.0/24
```

Adds or removes a **static** route. `GATEWAY` is an address, a node name or
`NODE:IF`, and must be on a subnet of the node; the outgoing interface is
derived from it. `dev IF` installs an on-link route with no gateway. `metric`
defaults to 1. `del` removes only the static route for that exact prefix;
routes learned by routing protocols are not touched.

Forwarding uses the longest matching prefix, then the lowest administrative
distance (connected 0, static 1, DHCP 5, BGP 20, OSPF 110, RIP 120), then the
lowest metric. Routes through an interface that is down are skipped.

### `auto`

```
auto addr
auto routes
auto all        (also: auto)
```

Requests automatic configuration, applied once after the whole file has been
read. The position of the line in the file does not matter.

**`auto addr`** assigns addresses per *broadcast domain*. A domain is a set of
interfaces that can reach each other at layer 2: the two ends of a link, plus
everything joined through switch ports that are in the same VLAN.

- Domains are visited in the order of the `link` lines that first touch
  them. The n-th domain that receives addresses gets `10.0.n.0/24`, starting
  from `10.0.1.0/24`.
- Inside a domain, interfaces of **routers** get `.1`, `.2`, ... and
  interfaces of **hosts** get `.10`, `.11`, ..., both in the order of the
  `node` lines. Switch ports get nothing.
- A domain in which any interface already has an address (from an `ip` line)
  is left completely alone. So is a domain with fewer than two non-switch
  interfaces.
- Then every **host** that has no default route gets one, towards the first
  router (in `node` order) that has an address in one of the host's domains.
  A host with no router on its subnets gets none.
- Trunk ports do not join domains for this purpose: a VLAN that spans
  several switches over a trunk is seen as separate domains and would get one
  subnet per switch. Address such networks by hand.

For example, with

```
node h1 host
node r1 router
node h2 host
link h1 r1
link r1 h2
auto all
```

the result is h1 `10.0.1.10/24`, r1:eth0 `10.0.1.1/24`, r1:eth1
`10.0.2.1/24`, h2 `10.0.2.10/24`, default gateway `10.0.1.1` on h1 and
`10.0.2.1` on h2.

**`auto routes`** installs, on every host and router, a static route to every
subnet in the topology that it is not directly connected to, along a path
with the fewest router hops (computed with global knowledge; only routers
forward). The metric is the hop count. A route that already exists for the
same prefix with an equal or lower metric is kept. It uses whatever
addresses exist, automatic or written by hand.

Typical combinations:

| Lines | Use |
|---|---|
| `auto all` | Addressing and routing are not the subject of the lab. |
| `auto addr` + `run rN rip` / `ospf` | A routing protocol must discover the routes. |
| `ip ...` + `auto routes` | You want specific addresses but no routing work. |
| `ip ...` + `gw ...` + `route ...` | Static routing is the subject. |

### IPv6

A lab is IPv4-only until its scenario asks for IPv6; then every interface of
every host and router also gets a link-local address (`fe80::/64` plus an
identifier derived from the MAC address), checks its addresses with duplicate
address detection for one second, and speaks Neighbor Discovery instead of
ARP. Existing IPv4 labs are not affected in any way. IPv6 is switched on by
any of:

| Directive | Effect |
|---|---|
| `ip NODE[:IF] 2001:db8:1::10/64` | An IPv6 address on an interface (up to 3 global addresses each, besides the link-local one). Without `:IF`, the first interface that has no global IPv6 address. |
| `auto addr6` | Like `auto addr`: every subnet without a global IPv6 address gets `2001:db8:N::/64` (N = 1, 2, ... in the order of the `link` lines); routers `::1`, `::2`, ..., hosts `::10`, `::11`, ... (hexadecimal); every host gets a default route through the first router of its subnet. |
| `auto routes6` | Like `auto routes`: static shortest-path IPv6 routes on every node. |
| `auto all6` | Both. Combine with `auto all` for a dual-stack lab. |
| `ipv6 on` | Only the link-local addresses: hosts then configure themselves from router advertisements (SLAAC). |

`gw` and `route` accept IPv6 too, and choose the table from the form of the
address:

```
ip r1:eth0 2001:db8:a::1/64
gw h1 2001:db8:a::1
route r1 2001:db8:b::/64 via 2001:db8:12::2
route r1 2001:db8:c::/64 dev eth1
route r1 del 2001:db8:b::/64
```

A gateway must be a global address on a subnet of the node (or a node name:
`gw h1 r1` sets the default route of both protocols when both nodes have
addresses of both). With `auto addr6` the addresses do not exist yet while the
file is read, so `route ... via NAME` cannot be used together with it: write
the address.

Routers advertise their prefixes with the `ra` module
(`run r1 ra [interval=10s] [lifetime=1800s]`); hosts listen without any
command.

The tools choose the protocol from the destination: an IPv6 literal, or
`v6=1` with a node name (`run h1 ping h2 v6=1`), uses IPv6; a node name
without `v6=1` uses IPv4 when the node has an IPv4 address and IPv6 otherwise.
A node name stands for its first global IPv6 address, or its link-local one
when it has no other.

Counters worth using in `expect`: `ip6.forwarded`, `ip6.too_big`,
`ip6.hlim_expired`, `ip6.noroute`, `ip6.fragmented`, `ip6.reassembled`,
`ip6.pmtu_updates`, `nd.ns_sent`, `nd.failed`, `nd.dad_failed`,
`slaac.addresses`, `ra.sent`, `icmp6.echo_served`, `icmp6.errors_sent`.

Limits: no MLD, no DHCPv6, extension headers other than Fragment are skipped,
and the routing protocols (RIP, OSPF, BGP), DNS and DHCP still run over IPv4
only (UDP, TCP and HTTP run over both).
In a distributed run a node name resolves only to the addresses written in
the file (or assigned by `auto addr6`), not to those formed by SLAAC in
another process.

## Running things

### `run`

```
run NODE MODULE [args ...]
NODE MODULE [args ...]

run h2 echo
run h1 ping h2 count=3
run r1 rip update=5s
h1 tcp send h2 bytes=5M cc=cubic
```

Starts a module on a node, or sends a command to a module that is already
running there (what the arguments mean is up to the module: see
`netlab modules` and `netlab help MODULE`). The short form without `run`
works whenever the first word is a node name.

In a file, a `run` line is not executed while the file is read: it is
scheduled for time 0, after automatic addressing, in file order. An error in
a deferred command (unknown module, bad arguments) is therefore reported in
the event log at run time, not as a load error. Note that the short form
*is* executed immediately while the file is read, before `auto` is applied;
prefer `run` in files.

Destinations accepted by most modules are an IPv4 address, a node name (its
first address) or `NODE:IF`.

Running a module on a node that is down is an error. In a distributed run a
`run` line for a node hosted by another process is silently skipped.

### `at`

```
at TIME COMMAND ...

at 5s run h1 traceroute h2
at 5s link r1 r2 down
at 7.5s node r2 down
at 12s set detail 3
```

Runs `COMMAND` at a later time. `COMMAND` is anything valid at run time:
`run`, the short form, `stop`, `link` (state or parameters), `node NAME
up|down`, `ip`, `gw`, `route`, `set`, `log`, even another `at`.

- In a file, `TIME` is absolute virtual time from the start of the lab.
- On the console, `TIME` is relative to now.

Errors in the command are reported in the event log when it runs.

### `stop`

```
stop NODE MODULE
at 20s stop r2 rip
```

Stops a module on a node and frees its state. Nothing happens if it is not
running.

### `node NAME up|down`

```
at 7s node r2 down
at 15s node r2 up
```

Crashes or recovers a node. A crashed node receives nothing, its timers are
discarded, and every module on it is told to drop its volatile state; on
recovery modules are told to restart. Because a `node` line in a file
*creates* a node, this form can appear in a file only after `at`.

### `set`

```
set speed 0.05
set detail 3
set baseport 30000
```

| Setting | Meaning |
|---|---|
| `speed X` | Time scale: virtual seconds per real second. Overridden by `--speed`. Setting it in the file disables the automatic slow-motion choice of the interface. |
| `detail N` | Detail level 1..3. Overridden by `--detail`. |
| `baseport N` | First UDP port of a distributed run (default 47000). Ignored when `--baseport` is given. |

### `log`

```
at 10s log --- second phase ---
```

Writes the rest of the line in the event log at detail level 1.

## Commentary

```
note TIME TEXT

note 0   ping: an ICMP echo request crosses the router and the reply comes back.
note 5s  traceroute: TTL 1 dies at r1.
```

When virtual time reaches `TIME` the text is written in the log and shown at
the bottom of the map until the next note; the Guide view lists all notes as
a timeline. Notes are displayed in the order they appear in the file, so
write them in increasing time order. A second note with the same `TIME`
replaces the first. Notes are shown by the interface only (not in headless
output); the automatic skipping of idle time stops at each note. At most 48
notes, about 400 characters each.

## Run control

| Directive | Meaning | Default |
|---|---|---|
| `duration TIME` | How long a headless run lasts (`--until` overrides it). The interface ignores it and runs until the user quits. | `30s` |
| `seed N` | Seed of the random generator, used unless `--seed N` is given. Decimal or `0x` hexadecimal; 0 means "not set". Without any seed each run picks a new one (printed in the first line of headless output). | none |

## Expectations

```
expect NODE COUNTER OP VALUE

expect h1 ping.reply == 3
expect h1 tcp.goodput_kbps >= 800
expect a1 arp.failed == 1
```

`OP` is one of `==`, `!=`, `>=`, `<=`, `>`, `<`; `VALUE` is a number. Spaces
around the operator are required.

**Where counters come from.** Every node has a set of named numeric counters
maintained by the core and by the modules running on it with `nl_count()` and
`nl_count_set()`. Names are conventionally `module.what`: `ping.reply`,
`ping.error`, `traceroute.hops`, `udp.reply`, `tcp.sink_bytes`,
`tcp.send_done`, and from the stack itself `ip.forwarded`, `ip.noroute`,
`ip.ttl_expired`, `ip.reassembled`, `arp.failed`, `icmp.echo_served`,
`udp.no_port`, `bridge.flood`... To discover them, run the lab and open the
*Node* tab of the node at detail 2 or 3: the *Counters* section lists every
counter with its current value. A counter that was never touched reads 0.
Time series (`nl_sample`) are not counters and cannot be tested.

**When they are checked.**

- In a headless run (`--headless`, piped output, `netlab check`), when
  virtual time reaches `duration`. Each expectation is printed as
  `ok`/`FAIL` with the value found, then `PASS` or `FAIL` and the title; the
  exit status is 1 if any failed. `--quiet` prints only the final line.
- In the interface, when the user quits: the list is printed for
  information, whatever time the lab had reached.
- `netlab check [FILE ...]` runs each scenario (the whole catalog by
  default) with `--quiet --seed 1` and prints a summary.
- In a distributed run each process checks only the expectations about the
  nodes it hosts.

An expectation that names an unknown node, or uses an unknown operator,
fails. At most 32 expectations; later ones are ignored.

### Expectations that depend on parameters: `check`

A lab with parameters has more than one right outcome: with `qdisc=prio` the
voice flow loses nothing, with `qdisc=droptail` it must lose a lot. An
`expect` line compares one counter with a constant and cannot say that. The
`check` module can: it evaluates an expression over the counters of any node
at a chosen moment and stores the result in a counter of its own, which an
ordinary `expect` then tests.

```
at 14900ms run r1 check overload  "$bulk * 1.046 + 82k > $bw"
at 14900ms run r1 check protected "(is_$qdisc != is_prio && is_$qdisc != is_fq) || v1:cbr.sent - v2:sink.received <= 3"
expect r1 check.protected == 1
```

```
run NODE check NAME "EXPRESSION"        ->  counter check.NAME on NODE
```

| In an expression | Meaning |
|---|---|
| `h2:sink.lost` | the counter `sink.lost` of node `h2`, now (0 if it does not exist yet) |
| `42`, `0.3`, `1e-3`, `50%` | numbers |
| `500k`, `2M`, `1G` | numbers with a multiplier |
| `20ms`, `3s`, `100us` | times, in milliseconds (`3s` is 3000) |
| `+ - * /`, `< <= > >= == !=`, `&& \|\| !`, `( )` | the usual operators; a comparison is worth 1 or 0, so `2 + ($ttl < 6)` is legal |
| `min(a,b) max(a,b) pow(a,b) abs(a) exp(a) log2(a) sqrt(a)` | functions |
| `is_$algo == is_rr` | bare words are text and can only be compared with `==` and `!=`; prefix the parameter so that the word starts with a letter |

A result of 1 or 0 is logged as a passed or failed check (level 1); any other
value is a measurement, kept for a later check: take a snapshot with
`at 5s run r1 check before "h2:sink.received"` and use `r1:check.before`
afterwards. Parameters are substituted before the expression is read, as
everywhere else. Run the check on a node that is alive at that moment (a
crashed node runs nothing).

Write the expectations so that they hold for every value a student may
reasonably give to the parameters: `tests/run-variants.sh` runs the lines of
`tests/variants/*.txt` (`PASS  scenario  key=value ...`) with five seeds and
reports any that disagrees; `tests/try.sh` and `tests/counters.sh` help while
designing them.

An expectation can be limited to one configuration of the lab with a trailing
`if A == B` or `if A != B` (a plain text comparison, normally of a parameter
with a value), so that every documented setting of a parameter has its own
checked outcome:

```
param  policy 1 Apply business relations
expect hc1 ping.reply == 3 if $policy == 1
expect hc1 ping.reply == 6 if $policy == 0
```

## Distributed runs

```
place NODE HOST[:PORT]
monitor HOST[:PORT]

place h1 192.168.1.20
place r1 192.168.1.21:47010
monitor 192.168.1.20
```

`place` says on which machine a node runs when the lab is split across
processes with `netlab node`; `HOST` is an address or a host name. A node
with no `place` line is assumed at `127.0.0.1`. The default `PORT` is
*baseport + index of the node* (47000 for the first `node` line, 47001 for
the second, ...). `place` lines must come after the `node` lines they refer
to.

`monitor` says where the monitor (or hub) listens; node processes report
their log and captures there and wait for it before starting their clocks.
The default port is *baseport − 1* (46999). `--monitor HOST[:PORT]` on the
command line takes precedence.

Both directives are ignored by `netlab run`. See [distributed.md](distributed.md).

## Runtime-only commands

One command is accepted on the console of a running lab (after `:`) and
inside `at`, but not as a plain line of a file:

| Command | Meaning |
|---|---|
| `node NAME up\|down` | crash / recover (in a file, `node` creates a node) |

These are meant for the console (`help` is also accepted elsewhere, but its
output goes to the event log):

| Command | Meaning |
|---|---|
| `help` | list commands and modules in the log |
| `help MODULE` | print the usage of a module in the log |
| `pause`, `resume` | stop and restart virtual time (console only) |
| `q`, `quit` | leave the lab (console only) |

Conversely, these directives are valid only while the file is read and are
rejected on the console as unknown commands: `title`, `about`, `level`,
`layer`, `topic`, `param`, `param+`, `note`, `duration`, `seed`, `auto`,
`vlan`, `place`, `monitor`, `expect`, and `node NAME KIND`. `link` cannot
create a link at run time, only change one.

## Order of evaluation

1. `name=value` pairs from the command line are recorded as parameter values.
2. The file is read top to bottom. Each line is substituted, then executed:
   metadata, `node`, `link`, `vlan`, `ip`, `gw`, `route`, `set`, `place`,
   `monitor`, `expect`, `note` take effect immediately; `run` and `at` are
   scheduled.
3. After the last line: broadcast domains are computed, `auto addr` and then
   `auto routes` are applied, nodes without a position are laid out, and
   tunnel ports are assigned.
4. The random generator is seeded (`--seed`, else `seed`, else the clock).
5. `--detail`, `--speed` and `--until` override what the file set.
6. Time starts. At time 0 the deferred `run` and `at 0` commands execute in
   file order; then events run in time order.

## Limits

| What | Limit |
|---|---|
| nodes | 64 |
| links | 160 |
| interfaces per node | 16 |
| modules active on one node | 12 |
| routes per node | 256 |
| counters per node | 48 |
| time series per node | 8 |
| parameters | 32 |
| notes | 48 |
| expectations | 32 |
| arguments per line | 40 |
| frame size | 1700 bytes |

## Where scenarios are found

`netlab run NAME` (and `windows`, `node`, `monitor`, `plan`, `check`)
resolves `NAME` in this order:

1. a path to an existing file;
2. `scenarios/NAME` in the netlab home;
3. `scenarios/NAME.nl`;
4. a catalog file whose name ends in `-NAME.nl` (`ping` finds `101-ping.nl`).

The netlab home is `$NETLAB_HOME` if set, else the directory of the
executable if it contains `scenarios/`, else `../share/netlab` relative to
it. The menu's catalog also lists `~/.local/share/netlab/scenarios/*.nl`
(where the wizard and the distributed set-up write their files) under *My
scenarios*; those are run by path.
