<!-- level: 2 -->
<!-- labs: 201-mtu-fragmentation -->
# MTU, fragmentation and path MTU discovery

Every link has a **maximum transmission unit** (MTU): the largest IP packet, header included, that one frame can carry. Ethernet's is 1500 bytes; tunnels and some access links have less. A sender only knows the MTU of its own link, yet the packet must cross every link of the path, and the smallest MTU on the way, the **path MTU**, decides what fits.

IPv4 offers two answers. A router that must send a packet larger than the next link's MTU can **fragment** it: cut the payload into pieces, give each piece a copy of the header, and send them as independent packets. Only the destination puts them back together. This always works, but it is fragile: if one fragment is lost the whole datagram is lost, and reassembly costs memory and time.

The alternative is to make the sender use the right size. The sender sets the **don't fragment** (DF) flag; a router that cannot forward the packet drops it and returns an ICMP error that states the MTU of the offending link. The sender shrinks its packets and tries again. This is **path MTU discovery**, and it is what TCP does today on practically every connection.

<!-- detail: 2 -->
## The header fields

Three fields of the IPv4 header exist only for fragmentation:

- **Identification** (16 bits): chosen by the sender, equal in all fragments of one datagram. Together with source, destination and protocol it tells the receiver which fragments belong together.
- **Flags** (3 bits): one reserved bit, **DF** (don't fragment) and **MF** (more fragments). MF is 1 in every fragment except the last.
- **Fragment offset** (13 bits): the position of this fragment's payload in the original payload, in units of 8 bytes. Hence every fragment except the last must carry a multiple of 8 bytes.

The total length field and the header checksum are recomputed for every fragment. A complete, unfragmented packet has MF = 0 and offset = 0.

## Fragmenting: a worked example

In lab 201 h1 pings h2 with 1400 bytes of data. ICMP adds 8 bytes of header, IP adds 20: the packet is 1428 bytes long with a payload of 1408. The middle link has MTU 600.

Each fragment may be at most 600 bytes, so at most 580 bytes of payload; rounded down to a multiple of 8, that is 576. The router r1 produces:

```
fragment   payload bytes   offset field   MF   total length
   1          0 - 575            0         1       596
   2        576 - 1151          72         1       596
   3       1152 - 1407         144         0       276
```

Check: 72 × 8 = 576, 144 × 8 = 1152, and 576 + 576 + 256 = 1408. The log says exactly `1428 B > MTU 600 → 3 fragments`, and later h2 reports `datagram reassembled (1408 B)`. Only the first fragment contains the ICMP header; in a UDP or TCP datagram only the first one contains the port numbers.

## Reassembly

The destination keeps a buffer for each (source, destination, protocol, identification). It places each fragment at offset × 8, learns the total length from the fragment with MF = 0, and delivers the datagram when no hole is left. A timer starts with the first fragment; if it expires (typically 15 to 60 s) everything is discarded. Fragments may arrive in any order, and a fragment may be fragmented again by a later router: the fields are designed so that this needs no special case.

Loss is amplified. If each packet is lost with probability p, a datagram in n fragments survives with probability (1 − p)^n. With p = 0.2 a small ping needs two packets to survive (request and reply): 0.8² = 64%. The fragmented ping of the lab needs six: 0.8⁶ = 26%.

## Path MTU discovery

With DF = 1 the router must not fragment. It discards the packet and sends ICMP **destination unreachable, fragmentation needed and DF set** (type 3, code 4), which carries the MTU of the next hop. The sender records that value as the path MTU for that destination and sends smaller packets; if a later link is smaller still, the process repeats. In the lab, r1 answers `fragmentation needed` with MTU 600; a ping of 500 data bytes (528 in total) then passes untouched.

For TCP the path MTU sets the segment size: MSS = path MTU − 40 without options.

<!-- detail: 3 -->
## In depth

**Why fragmentation is avoided.** Kent and Mogul's paper "Fragmentation Considered Harmful" (1987) listed the problems, and RFC 8900 ("IP Fragmentation Considered Fragile", 2020) confirms them: loss amplification; reassembly state that an attacker can exhaust; fragments after the first carry no ports, so firewalls, NATs and load balancers cannot classify them and often drop them; and the 16-bit identification wraps quickly at high rates, so that fragments of different datagrams can be mixed (RFC 4963, RFC 6864). Overlapping fragments were used to crash hosts and to evade inspection (the "teardrop" attacks).

**IPv6** removes router fragmentation altogether (RFC 8200): only the source may fragment, with an extension header, and every link must support 1280 bytes. In IPv4 every host must accept datagrams of 576 bytes and every link must carry 68.

**When discovery fails.** Classic PMTUD (RFC 1191; RFC 8201 for IPv6) depends on the ICMP error reaching the sender. Firewalls that drop all ICMP create a **black hole**: the handshake works because its packets are small, then the first full-sized segment disappears and the connection hangs. Remedies are MSS clamping (a router rewrites the MSS option in SYN segments, common on PPPoE and tunnels) and Packetization Layer PMTUD (RFC 4821, and RFC 8899 for datagram transports), in which the transport probes with larger packets and treats the loss of a probe as a size signal, without needing ICMP. Discovered values must be aged, because paths change; and an ICMP error must be validated against the quoted packet, or anyone could shrink the MTU of a connection.

**Tunnels** make the issue permanent: every encapsulation (VPN, VXLAN, GRE) subtracts its header from the MTU, so paths with an effective MTU of 1400 to 1480 are common.

**What netlab simplifies.** The lab shows the mechanism but not the automation: the third ping is simply scripted with a smaller size. The MTU of a link is a parameter (`mtu=`) and applies to both directions.

<!-- detail: 1 -->
## In the lab

**Lab 201: `./netlab run 201-mtu-fragmentation`**

- Phase one (0 s): in the log, r1 reports `IP fragmentation: 1428 B > MTU 600 → 3 fragments`, h2 reassembles, and the reply is fragmented by r2 in the other direction.
- Open the packets view (`p`), choose one of the three fragments on the r1-r2 link and press Enter. Read identification, flags and fragment offset, and compare the three fragments with the table above. Then open the same packet on the h1-r1 link: one piece, MF = 0.
- Phase two (4 s): the same ping with DF set. r1 drops it and sends `fragmentation needed`; open the ICMP message and find the next-hop MTU and the quoted header of the dropped packet.
- Phase three (8 s): 500 data bytes with DF pass without being touched.
- Predict, then try: `./netlab run 201-mtu-fragmentation mtu=1000`. How many fragments, with which offsets and lengths? Then `mtu=500`: what happens to the third phase, and why?
- Predict, then try: `./netlab run 201-mtu-fragmentation loss=0.2` several times (a different seed each run). Which of the three phases loses more pings? Compare with 0.8⁶ and 0.8².
- Find the limit by hand from the command line of the lab: `:h1 ping h2 count=1 size=572 df=1` and then `size=573`. Explain the boundary.

## Check yourself

- A datagram with 3000 bytes of payload and a 20-byte header crosses a link with MTU 1500 and then one with MTU 620. List the fragments that reach the destination: payload length, offset field and MF for each.
- Why is the fragment offset expressed in units of 8 bytes, and what constraint does this put on fragment sizes?
- Why are fragments reassembled only at the destination and not by the next router?
- Describe path MTU discovery step by step, and explain what a "PMTUD black hole" is and why the TCP handshake does not reveal it.
- A firewall wants to block UDP port 53. Why are fragments a problem for it?
