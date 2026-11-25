# Changelog

## 1.0.0 — 2026-11-25

First release of hoplab, a network laboratory in the terminal.

### What it contains

- **Emulator**: hosts, routers and switches exchanging real, byte-exact frames
  over links with bandwidth, delay, jitter, loss, bit errors, queues and MTU,
  on virtual time (pause, step, speed, reproducible from a seed).
- **Protocols and tools** (30+ modules, one C file each):
  - link: ARP, learning switches, 802.1Q VLANs, spanning tree and rapid
    spanning tree, link codes from parity to Reed–Solomon, medium access
    (ALOHA, CSMA, CSMA/CD, CSMA/CA)
  - network: IPv4 and IPv6 (Neighbor Discovery, SLAAC, ICMPv6, path MTU),
    static routing, RIP and RIPng, OSPF and OSPFv3, BGP over TCP with
    multiprotocol extensions and IPv4 over IPv6 next hops, NAT, queue
    disciplines (RED, CoDel, PIE, fair queueing, priority, token bucket)
  - transport: UDP; TCP with SACK, delayed ACKs, path MTU discovery and
    Tahoe / Reno / NewReno / CUBIC / BBR v1, over both address families;
    a layer-4 load balancer
  - application: DNS (A and AAAA, authoritative and recursive), DHCP and
    DHCPv6, HTTP/1.1, traffic generators
  - distributed systems: Raft, gossip and SWIM, logical clocks and ordered
    broadcast, two- and three-phase commit, Chord, CRDTs
- **Interface**: network map with packets in flight, node inspector, event
  log, packet dissection down to the hex dump, sequence diagram, graphs,
  command line, three levels of detail.
- **78 labs** in four levels, each with parameters, a narrated timeline and
  checked expectations; a wizard to build topologies; a text scenario language.
- **Three ways to run**: one window, tiled windows as separate processes
  joined by UDP tunnels, or several machines.
- **Study guide** inside the program: 41 topics in five tracks, three reading
  depths, labs started with one key.
- **English and Italian** throughout (PO catalogs, labs, guide, README).
- **The classic demos** the project started from: stop-and-wait, Go-Back-N
  and Selective Repeat over real UDP sockets.
- **Regression suite** (`make regress`): every lab with five seeds, more than
  700 parameter variants, sanitizers, the multi-process mode, the classic
  demos, translation and guide consistency.

### Known limits

- Linux only; needs a 256-colour UTF-8 terminal.
- Not exercised: the kitty layout of `hoplab windows` (tmux is), and runs
  across physically separate machines (several processes on one machine are).
- A teaching emulator, not a performance simulator: at most 64 nodes and 160
  links per lab. Some protocols are deliberately reduced, and each module's
  help says how: for example BGP is eBGP with one router per AS, OSPF has one
  area and no designated router, TCP has no timestamps, DHCPv6 has no relay
  or prefix delegation.
