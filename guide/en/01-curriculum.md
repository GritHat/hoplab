<!-- level: 0 -->
<!-- labs: 100-two-hosts, 101-ping, 120-tcp-handshake, 200-static-routing, 220-tcp-loss, 320-tcp-congestion, 380-raft-election, 420-tcp-fairness -->
# The curriculum

The labs of hoplab form a study programme in four tracks of growing difficulty. Each track is a level in the catalog and in this guide, and inside a track the labs go from the bottom of the protocol stack upwards.

- **Foundations** is the first contact. No prerequisites: if you know what an IP address looks like, you can start. Frames, addresses, ping, a TCP connection, a web page, a name lookup, and the first ideas of the link layer.
- **Core** is the material of an undergraduate networking course: routing tables and routing protocols, TCP reliability and flow control, DNS and DHCP in depth, NAT, switching, VLANs, spanning tree, medium access.
- **Advanced** is graduate material: congestion control, queue management, inter-domain routing, consensus and distributed algorithms.
- **Frontier** collects labs with a research flavour, where the interesting part is what goes wrong: fairness between congestion controls and BBR, BGP security, DNS poisoning, consistent hashing, hidden terminals, network partitions.

You do not have to finish a track before entering the next. A good way to study is to follow one subject up through the tracks: TCP from the handshake to fairness, or routing from a static table to BGP hijacks.

## How to study with a lab

A lab is an experiment, and it teaches the most when you treat it like one.

- **Predict.** Read the description of the lab, then pause at time zero and say what you expect: which frame leaves first, who answers, how long it takes.
- **Run.** Let the lab play and follow the commentary at the bottom of the map.
- **Inspect.** Pause where something surprised you. Open the packet, read the routing table, look at the graph.
- **Change one parameter.** One at a time: double the loss, shrink the queue, cut a link. Predict again before you run.
- **Explain.** Say in your own words why the result changed. If you cannot, read the topic one depth further and repeat.

<!-- detail: 2 -->
The tools for each step: `space` and `.` to pause and advance one event at a time; `p` then Enter to dissect a frame; `s` for the sequence diagram; `n` to select a node and the arrow keys to read its tabs; `g` for the graphs; `:` for commands such as `link r1 r2 down` or `h1 ping h2`; and the parameter form of the catalog (or `name=value` on the command line) to run the same lab under different conditions.

<!-- detail: 1 -->
## Reading depths

Every topic of this guide, and the guide inside each lab, can be read at three depths. Press `1`, `2` or `3`.

- **1, essentials**: a short, complete read. What the thing is and what to watch in the lab.
- **2, standard**: the working knowledge. How the mechanism operates, the fields and timers that matter, what to try.
- **3, in depth**: the fine points. Corner cases, design trade-offs, what the real protocols do differently.

The same three levels control a running lab: at level 1 the log shows only the outcome of each action, at 2 the protocol messages, at 3 every frame. Start each lab at 1, and go deeper when you want to know how.

## Foundations

Take these in order; each uses what the previous one showed.

- `100-two-hosts` Two hosts and a cable: frames, addresses and ARP
- `101-ping` Two networks and a router: ping, traceroute, fragmentation
- `102-link-properties` What a link does to packets: delay, bandwidth, loss, queue
- `103-ipv6-first` IPv6, first contact: link-local addresses and Neighbor Discovery
- `120-tcp-handshake` TCP, segment by segment: handshake, data, teardown
- `121-http-page` Loading a web page: HTTP over TCP
- `140-dns-first` A first name lookup
- `141-dhcp-join` Joining a network with DHCP
- `160-error-detection` Detecting bit errors: nothing, a parity bit, a CRC
- `161-error-correction` Correcting instead of discarding: repetition and Hamming codes
- `162-aloha` Sharing one channel: pure ALOHA and slotted ALOHA
- `170-switch-learning` Learning switches: flood, learn, forward

<!-- detail: 2 -->
The first seven labs are the journey of a web request seen from the bottom up: a frame on a cable, a packet through a router, what the link does to it, a TCP connection, a page, the name lookup and the address assignment that come before everything else. The last four open the link layer: how errors are caught or repaired, how stations share a channel, how a switch learns where everybody is. If you are short of time, do 100, 101, 120 and 140.

<!-- detail: 1 -->
## Core

- `200-static-routing` Static routing: routing tables, next hops and a missing route
- `201-mtu-fragmentation` MTU, fragmentation and path MTU discovery
- `202-routing-loop` A routing loop, and the TTL that saves the network
- `203-vlans` VLANs and trunks: several networks on the same switches
- `204-ipv6-slaac` IPv6 autoconfiguration: router advertisements and SLAAC
- `205-ipv6-pmtu` IPv6 never fragments on the way: Packet Too Big and path MTU discovery
- `206-dual-stack` Dual stack: IPv4 and IPv6 side by side on the same cables
- `207-tcp-over-ipv6` TCP and HTTP over IPv6, and how TCP finds the path MTU
- `220-tcp-loss` TCP on a lossy path: timeout and fast retransmit
- `221-tcp-flow-control` TCP flow control: a fast sender and a slow reader
- `230-rip-convergence` RIP: routers discover the network, then route around a failure
- `231-rip-count-to-infinity` RIP: count to infinity, and how split horizon prevents it
- `232-ospf-costs` OSPF: one map for everybody, and costs that steer the traffic
- `233-ripng-convergence` RIPng: the same algorithm for IPv6, side by side with RIP
- `234-ospfv3` OSPFv3: link state for IPv6, where topology and prefixes part ways
- `240-dns-hierarchy` The DNS hierarchy and the cache
- `241-dhcp-leases` DHCP leases: renewal, expiry and pool exhaustion
- `242-dns-aaaa` Names in a dual-stack world: A, AAAA and DNS over IPv6
- `243-dhcpv6` IPv6 autoconfiguration: SLAAC, stateless DHCPv6, stateful DHCPv6
- `250-nat-basics` NAT: why private addresses need a translator
- `251-port-forwarding` Port forwarding: publishing a server that sits behind a NAT
- `252-load-balancer` Load balancer: one address, three servers, and one of them fails
- `260-coding-bench` The coding bench: codes at work, bit by bit
- `261-csma-cd` Listen before talking: CSMA and Ethernet's CSMA/CD
- `270-broadcast-storm` A loop of switches without spanning tree: the broadcast storm
- `271-spanning-tree` Spanning tree: a loop made safe, and repaired when a link fails
- `272-priority-queueing` Priority queueing: voice and bulk traffic on a slow link
- `290-lamport-clocks` Lamport clocks: putting events in order without a shared clock

<!-- detail: 2 -->
A suggested order for a course: the network layer first (200, 201, 202, then the routing protocols 230, 231, 232), then transport (220, 221), then the services everybody uses (240, 241, 250, 251, 252), and finally the link layer (203, 260, 261, 270, 271, 272). Lab 290 is a first taste of distributed algorithms and prepares the Advanced track. Do 200 before any routing protocol: you appreciate RIP only after configuring routes by hand. Do 270 before 271: the storm is the reason spanning tree exists.

<!-- detail: 1 -->
## Advanced

- `320-tcp-congestion` Congestion control on a bottleneck: the sawtooth
- `321-tcp-sack` Many losses in one window: NewReno against SACK
- `330-ospf-vs-rip` Convergence after a failure: link state against distance vector
- `331-bgp-basics` BGP: autonomous systems, AS paths and withdrawals
- `332-bgp-ipv6` BGP for two protocols: IPv6 prefixes in a multiprotocol session
- `333-bgp-ipv4-over-ipv6` IPv4 islands over an IPv6-only core: BGP with IPv6 next hops
- `340-dns-ttl` CNAME chains, negative caching and the TTL trade-off
- `350-nat-hole-punching` NAT behaviours and UDP hole punching
- `351-lb-algorithms` Load balancing algorithms compared
- `360-rs-vs-hamming` Reed-Solomon against Hamming: random errors, bursts and interleaving
- `361-mac-curves` Throughput against load: the S(G) curves of every access protocol
- `370-stp-tuning` Engineering a spanning tree: priorities, port costs, timers and a root failure
- `371-bufferbloat-codel` Bufferbloat: a big buffer against CoDel, side by side
- `372-red-ecn` RED: early random drops, the average queue and ECN marks
- `373-fair-queueing` Fair queueing: a small flow protected from a flood
- `374-token-bucket` Token bucket shaping: a rate limit that tolerates bursts
- `380-raft-election` Raft: electing a leader, and replacing it when it crashes
- `381-raft-replication` Raft: replicating the log, committing, and catching up after a crash
- `382-gossip-rumor` Gossip: how fast does a rumor reach everybody?
- `390-causal-broadcast` Vector clocks and causal broadcast: the answer that overtakes the question
- `391-two-phase-commit` Two-phase commit: all or nothing across three databases
- `392-commit-blocking` The blocking problem: the coordinator crashes after PREPARE (2PC vs 3PC)
- `393-chord-ring` Chord: ten nodes form a ring and share a hash table
- `394-crdt-counter` A replicated counter that cannot lose increments (CRDT)

<!-- detail: 2 -->
Three paths cross this track. **Congestion and queues**: 320, then 371, 372, 373, 374; the sender's view first, then what the router can do. **Routing between and inside domains**: 330, 331, with 370 for the same questions at layer 2. **Distributed systems**: 390 for time and ordering, 380 and 381 for consensus, 391 and 392 for atomic commitment, 382 for epidemic protocols, 393 and 394 for structures that need no coordinator. The remaining labs (340, 350, 351, 360, 361) each deepen a Core topic and can be taken whenever you reach it.

<!-- detail: 1 -->
## Frontier

- `420-tcp-fairness` Two flows, one bottleneck: fairness of Reno, CUBIC and BBR
- `421-bbr-anatomy` Anatomy of BBR: a model of the path instead of a reaction to loss
- `430-bgp-policies` BGP policies: customers, peers, providers and valley-free paths
- `431-bgp-hijack` BGP prefix hijack, more-specific hijack and origin validation
- `432-bgp-ipv6-hijack` An IPv6-only Internet: BGP over IPv6, a sub-prefix hijack and origin validation
- `440-dns-poisoning` DNS cache poisoning and why port randomisation matters
- `450-consistent-hashing` Consistent hashing: what happens when the server pool changes
- `460-hidden-terminal` Wi-Fi's blind spot: hidden terminals, CSMA/CA and RTS/CTS
- `480-raft-partition` Raft under a network partition: no split brain
- `481-raft-split-votes` Raft: election timeouts, split votes and availability
- `482-swim-failure-detection` SWIM: detecting a crashed member without false alarms
- `483-gossip-partition` Gossip across a partition: anti-entropy brings everyone back in sync
- `490-chord-churn` Chord under churn: crashes and joins while lookups continue
- `491-crdt-partition` Partition and heal: OR-Set versus last-writer-wins

<!-- detail: 2 -->
Each Frontier lab continues an Advanced one: 420 follows 320; 430 and 431 follow 331; 440 follows 340; 450 follows 351; 460 follows 361; 480 and 481 follow 380 and 381; 482 and 483 follow 382; 490 follows 393; 491 follows 394. These labs have more parameters than the others and reward many runs: they are experiments to design, not demonstrations to watch.

<!-- detail: 1 -->
## By layer

The catalog can also be filtered by layer with `Tab`, which is useful when you study one subject at a time.

- **Physical and link**: 100, 102, 160, 161, 162, 170, 203, 260, 261, 270, 271, 360, 361, 370, 460
- **Network**: 101, 200, 201, 202, 230, 231, 232, 250, 251, 272, 330, 331, 350, 371, 372, 373, 374, 430, 431
- **Transport**: 120, 220, 221, 252, 320, 321, 351, 420, 421, 450
- **Application**: 121, 140, 141, 240, 241, 340, 440
- **Distributed**: 290, 380, 381, 382, 390, 391, 392, 393, 394, 480, 481, 482, 483, 490, 491

<!-- detail: 3 -->
The number of a lab says where it sits. The first digit is the track (1 Foundations, 2 Core, 3 Advanced, 4 Frontier). The second is the subject: 0 first steps, IP and forwarding, 2 TCP, 3 routing protocols, 4 DNS and DHCP, 5 NAT and load balancing, 6 coding and medium access, 7 switching and queues, 8 consensus and gossip, 9 other distributed algorithms. So 220, 320 and 420 are the TCP thread, and 380, 381, 480, 481 the Raft thread. Labs you write yourself, or build with the wizard, appear in the catalog in a fifth group, **My scenarios**.

Every lab is a plain text file in `scenarios/`. When a lab leaves you with a question it does not answer, copy the file, change it and run your copy: **Writing your own labs** shows how.
