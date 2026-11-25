<!-- level: 2 -->
<!-- labs: 203-vlans, 270-broadcast-storm, 271-spanning-tree -->
# VLANs and spanning tree

A switched Ethernet network is one **broadcast domain**: a frame sent to the broadcast address, or to an address no switch has learned yet, reaches every port. Two things go wrong as the network grows.

First, everybody is in the same network. Departments that should be separated see each other's broadcasts, and one misbehaving host disturbs all. **VLANs** split one physical set of switches into several logical LANs. Each port belongs to a VLAN; a frame is delivered only to ports of the same VLAN; a link between switches (a **trunk**) carries several VLANs by adding a small tag to each frame. Going from one VLAN to another requires a router, exactly as between two separate LANs.

Second, a network without redundant links breaks at the first cable failure, but a network *with* redundant links contains loops, and Ethernet frames have no TTL. One broadcast in a loop is copied for ever: a **broadcast storm**. The **spanning tree protocol** (STP) lets the switches agree on a loop-free subset of the links, block the others, and unblock them when the tree breaks.

<!-- detail: 2 -->
## 802.1Q tagging

An access port belongs to one VLAN and exchanges ordinary untagged frames with a host that knows nothing about VLANs. On a trunk the switch inserts a 4-byte tag between the source MAC address and the EtherType:

- **TPID** (16 bits) = 0x8100: says "a tag follows".
- **PCP** (3 bits): priority class; **DEI** (1 bit): drop eligible.
- **VID** (12 bits): the VLAN identifier, 1 to 4094.

The maximum frame grows from 1518 to 1522 bytes. The receiving switch reads the VID, removes the tag and forwards only to ports of that VLAN. The MAC address table is kept per VLAN: an entry is (VLAN, MAC) → port.

In lab 203 hosts a1, a2 are in VLAN 10 and b1, b2 in VLAN 20, on two switches joined by a trunk. All four have addresses in 10.0.0.0/24. a1 pings a2 across the trunk; but when a1 pings b2 its ARP request is flooded only in VLAN 10, b2 never sees it, and ARP fails. Same IP subnet, same cables, yet no connectivity at layer 2.

## Why loops are fatal

In lab 270 three switches form a triangle with no STP. One ARP request from h1 is flooded by s1 to s2 and s3; each floods it to the other; both copies return to s1, which floods them again. Two copies circulate for ever, clockwise and anticlockwise. Each lap delivers a copy to every host, and each switch sees h1's address arrive on different ports, so its table keeps changing. In nine seconds each switch floods about 1200 frames, all born from one.

## The spanning tree algorithm

Each bridge has an identifier: a 16-bit **priority** (default 32768) followed by its MAC address. Bridges exchange configuration **BPDUs**, sent to the multicast address 01:80:C2:00:00:00, carrying: root identifier, root path cost, sender bridge identifier, sender port identifier, and the timers (message age, max age, hello time, forward delay). A BPDU is *better* than another if it has a lower root identifier; if equal, a lower cost; then a lower sender identifier; then a lower port identifier. Three choices follow:

- **Root bridge**: the bridge with the lowest identifier. Everybody starts by claiming to be root and gives up on hearing a better BPDU.
- **Root port**: on every other bridge, the port offering the cheapest path to the root (received cost plus the cost of the port).
- **Designated port**: on every link, the port of the bridge that offers the best path to the root. All ports of the root are designated.

Every other port is **blocked**: it listens to BPDUs but forwards no data and learns no addresses.

Worked example (lab 271, all links cost 100): s1 has the lowest MAC and is root. s2 and s3 each reach it directly: root ports with cost 100. On the link s2-s3 both advertise cost 100; the tie is broken by the sender identifier, s2 wins and keeps a designated port, s3 blocks its port. The triangle has become a tree.

## States and timers

A port that should forward goes through **blocking → listening → learning → forwarding**, staying one *forward delay* in listening (no data, no learning) and one in learning (learning, no data). The root sends a BPDU every *hello time*; stored information expires after *max age*. Standard values are 2 s, 20 s and 15 s; netlab uses 1 s, 6 s and 3 s.

When the link s1-s2 fails at 18 s, s2 loses its root port. s3 turns its blocked port into a designated one, which becomes forwarding after 2 × 3 s: the outage is two forward delays (30 s with standard timers). A failure that is not directly visible, such as a crashed root, is detected only when max age expires: up to 20 + 30 = 50 s.

<!-- detail: 3 -->
## In depth

**Topology changes.** After the tree changes, stations may be reached through different ports, and stale table entries would black-hole traffic for the 300 s of normal ageing. A bridge that detects a change sends a topology change notification towards the root; the root sets a flag in its BPDUs and all bridges shorten the ageing time to the forward delay. netlab simply flushes the tables, which is what RSTP does.

**Rapid spanning tree** (802.1w, merged into 802.1D-2004) replaces the timers with a proposal/agreement handshake on point-to-point links, defines alternate and backup port roles so that a spare root port can take over at once, and treats ports towards hosts as edge ports. Convergence takes well under a second. netlab's `rapid=1` implements this: RST BPDUs with proposal and agreement flags, the sync of the bridge that agrees, alternate ports that become root ports at once, automatic edge ports (no BPDU heard for three hellos), and information dropped after three missing hellos instead of max age. Backup ports and 802.1w's own topology-change rules are not modelled. `portfast=1` makes host-facing ports edge ports from the start.

**Choosing the root.** With default priorities the root is the bridge with the lowest MAC address, usually the oldest one. Operators set the priority of the intended root (and of a backup) explicitly, and protect the design with root guard and BPDU guard on access ports. Port cost steers which links are used: classic values are 100 for 10 Mbit/s, 19 for 100 Mbit/s and 4 for 1 Gbit/s; netlab computes 10⁹ / bandwidth, which gives 100 for its default 10 Mbit/s links.

**Failure modes.** STP fails open: a port that stops *receiving* BPDUs (a unidirectional fibre, an overloaded CPU) starts forwarding and creates the very loop it should prevent. Loop guard and UDLD exist for this reason.

**VLANs and STP.** One tree for all VLANs wastes the blocked links; per-VLAN trees (PVST+) or Multiple Spanning Tree (802.1s) let different VLANs block different links. The trunk's **native VLAN** travels untagged, which enables the double-tagging VLAN-hopping attack; best practice is to tag everything. The 12-bit VID is too small for cloud data centres: 802.1ad stacks two tags, and VXLAN (RFC 7348) carries a 24-bit identifier over UDP.

**References.** Radia Perlman, "An Algorithm for Distributed Computation of a Spanning Tree in an Extended LAN" (1985); IEEE 802.1D and 802.1Q. Large data centres today prefer routed fabrics with ECMP, or TRILL / Shortest Path Bridging (802.1aq), which use all links.

<!-- detail: 1 -->
## In the lab

**Lab 203: `./netlab run 203-vlans`**

- In the packets view (`p`) select a frame on the s1-s2 link and press Enter: find the 802.1Q tag and its VID. Open the same frame on the a1-s1 link: no tag.
- Select s1 (`n`) and read the MAC table tab: the entries carry a VLAN column.
- At 6 s, a1 looks for b2: three ARP requests, then `ARP no answer`. Follow where the broadcast goes on the map.
- Predict, then try: `:b1 ping a2 count=1` and `:link s1 s2 down` followed by `:a1 ping a2 count=1`. Which fail, and at which layer?

**Lab 270: `./netlab run 270-broadcast-storm`**

- After 1 s the map never goes quiet. Select a switch and watch `bridge.flood` grow; at detail 3 the port of h1 in the table keeps flipping.
- Predict, then try: `:link s2 s3 down`. How quickly does the storm stop? Then `:link s2 s3 up`: does it restart by itself?
- Predict, then try: `./netlab run 270-broadcast-storm delay=1ms`. By what factor does the flood counter grow?

**Lab 271: `./netlab run 271-spanning-tree`**

- Select each switch and open its `stp` tab: role and state of every port. Follow blocking, listening (0 s), learning (3 s), forwarding (6 s). s3 blocks port p0.
- At 14 s h2 pings h1, then two steady streams start between them (50 datagrams per second each way). At 18 s the link s1-s2 fails. s3's p0 becomes designated; s2's new root port has cost 200. Forwarding resumes at 24 s: two forward delays. The lab counts the datagrams of h2 that never reached h1 (the last lines of the log, `check outage`): about 300, that is 6 s.
- Predict, then try: `./netlab run 271-spanning-tree rapid=1`. At detail 2 follow the handshake in the log: s3 proposes on p0, s2 answers with an agreement, s3 forwards. How many datagrams are lost now? (One or none: the outage is the time two BPDUs take to cross a link.) Open a BPDU in the Packets view and read the flags: role, proposal, agreement. Why did the host ports wait 3 s at the start, and what changes with `portfast=1`?
- Predict, then try: restart, and at about 22 s type `:node s1 down`. Who becomes root, and how long is the outage compared with the link failure? Relate it to `maxage` and `fwd`.

## Check yourself

- Draw an 802.1Q-tagged Ethernet frame and give the size and meaning of each field of the tag. Where is the tag added and where is it removed?
- Two hosts are in the same IP subnet but in different VLANs. Describe exactly what happens when one pings the other.
- Four bridges with identifiers B1 < B2 < B3 < B4 are connected in a ring B1-B2-B3-B4-B1 with equal costs. Which is the root, which are the root ports, and which port is blocked?
- Why does a port wait in listening and learning before forwarding? What could happen if it forwarded immediately?
- With the standard timers, how long can connectivity be lost after a failure the bridges cannot see directly, and why is that longer than after a link failure?
