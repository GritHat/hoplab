<!-- level: 2 -->
<!-- labs: 200-static-routing, 202-routing-loop -->
# Forwarding and static routes

A router does not know paths. It knows, for each destination prefix, which neighbour to hand the packet to: the **next hop**. A packet crosses the Internet because every router on the way independently makes one such local decision. Nobody checks that the decisions fit together.

The decision is a lookup in the **routing table**. Each entry holds a prefix (network address and length), a next hop and an output interface. Several entries may match a destination; the router takes the one with the **longest prefix**, because the longest prefix is the most specific piece of knowledge it has.

Two consequences follow, and they explain most routing faults. First, a path works only if every router on it has a matching entry, and the answer needs a path too: routes for the return direction are separate entries on separate routers. Second, if two routers point at each other for the same prefix the packet circulates. Forwarding has no memory and cannot notice; the **TTL** field in the IP header, decremented at every hop, is what eventually kills the packet.

With static routing an administrator writes every entry by hand. It is simple and predictable, and it does not react to failures.

<!-- detail: 2 -->
## The forwarding algorithm

For every IP packet that is not addressed to the router itself:

- Verify the header checksum, then decrement the TTL. If it reaches 0, discard the packet and send ICMP **time exceeded** (type 11, code 0) to the source.
- Look up the destination address: among all entries whose prefix matches, choose the longest. If none matches, discard and send ICMP **destination unreachable, network unreachable** (type 3, code 0).
- If the entry has a gateway, the next hop is that gateway; if the network is directly attached, the next hop is the destination itself.
- Resolve the MAC address of the next hop with ARP and send the frame. The IP addresses in the packet do not change; only the Ethernet addresses, the TTL and the header checksum do.

A host does the same with a table of two entries: its own subnet (directly attached) and a default route `0.0.0.0/0` towards its gateway. The default route matches everything with length 0, so it wins only when nothing else matches.

## Longest prefix match, with numbers

Suppose a table contains:

```
10.0.0.0/8      via 10.0.12.2
10.0.4.0/24     via 10.0.23.3
10.0.4.10/32    via 10.0.12.1
0.0.0.0/0       via 192.0.2.1
```

A packet for 10.0.4.10 matches all four entries; the /32 wins. A packet for 10.0.4.77 matches three; the /24 wins. A packet for 10.7.1.1 matches the /8 and the default; the /8 wins. A packet for 8.8.8.8 matches only the default. To test a match, AND the destination with the mask: 10.0.4.77 AND 255.255.255.0 = 10.0.4.0, equal to the prefix, so it matches.

## Next hop is not a path

In lab 200 three routers are in a row: `h1 - r1 - r2 - r3 - h2`. r1 has a route for 10.0.4.0/24 via r2, but at the start r2 has none. r1 forwards happily: it cannot know that the next hop is ignorant. The packet dies at r2, which reports network unreachable. Once the route is added, the reply needs r3 and r2 to know 10.0.1.0/24. A missing return route gives a subtle symptom: the request arrives, the reply is dropped far from the sender, and the ICMP error goes to the *replier*, so the sender sees only silence.

## Loops and TTL

In lab 202, r1 says "10.9.0.0/16 via r2" and r2 says "10.9.0.0/16 via r1". A ping sent with TTL 16 is decremented to 15 by r1, to 14 by r2, and so on: it crosses the r1-r2 link 15 times before r2 brings the TTL to 0. One packet has consumed fifteen transmissions; with the usual initial TTL of 64 it would be 63. traceroute, which sends probes with TTL 1, 2, 3, ..., shows the same two addresses alternating: the signature of a loop.

<!-- detail: 3 -->
## In depth

**Where entries come from.** A real table merges connected networks, static routes and routes learned by routing protocols. When two sources offer the same prefix, the router prefers the more trustworthy source (administrative distance on Cisco, preference on Juniper), and only then compares metrics. Longest prefix match is applied first and across all sources: a /24 learned from RIP beats a static /16. Classless prefixes and this rule were introduced with CIDR (RFC 1519, now RFC 4632); router behaviour is specified in RFC 1812 and the packet format in RFC 791, ICMP in RFC 792.

**Lookup speed.** A backbone table holds about a million IPv4 prefixes and a lookup must take nanoseconds. Software routers use compressed tries (Linux uses an LC-trie, the BSDs a radix tree); hardware routers use TCAMs, memories that compare the address with all prefixes in parallel. The control plane computes the routing table (RIB); the data plane forwards with a compact copy (FIB).

**Failures of static routing.** A static route stays in the table when the path behind the next hop breaks: traffic falls into a black hole. Operators use floating static routes (a backup with a worse preference), or tie a route to a liveness probe such as BFD (RFC 5880). A static route to a "null" interface is deliberate black-holing, used to drop attack traffic and to anchor aggregate prefixes so that they cannot loop.

**Loops in practice.** Permanent loops come from configuration mistakes, typically an aggregate route pointing towards a customer who has a default route pointing back. Transient micro-loops appear during the convergence of any routing protocol, when routers switch to new next hops at slightly different times. TTL bounds the damage per packet but not the load: a loop of two routers multiplies traffic for that prefix by up to the initial TTL.

**Asymmetry and filtering.** Forward and return paths are often different on the Internet. This breaks strict reverse-path filtering (RFC 3704), which drops packets arriving on an interface that would not be used to reach their source.

**What hoplab simplifies.** Routes have a single next hop (no equal-cost multipath) and the table is searched linearly. The `auto routes` option used by other labs computes shortest paths for you; labs 200 and 202 switch it off on purpose.

<!-- detail: 1 -->
## In the lab

**Lab 200: `./hoplab run 200-static-routing`**

- Press `n` to select r1, r2 and r3 in turn and read the **Routes** tab. Before 6 s, r2 has entries for 10.0.12.0/24, 10.0.23.0/24 and 10.0.1.0/24 only.
- In the log (`l`) the first ping fails with `from 10.0.12.2: network unreachable`: the address tells you which router gave up.
- At 6 s the route appears in the table of r2 and the second ping works with `ttl=61`: 64 minus three routers.
- At 11 s traceroute lists r1, r2, r3, h2. Open the packets view (`p`), press Enter on a probe and on the ICMP time exceeded that answers it.
- Predict, then try: `:route r3 del 10.0.1.0/24` and then `:h1 ping h2 count=2`. Does h2 receive the request? Who gets the ICMP error, and what does h1 print?
- Predict, then try: `:route r2 add 10.0.4.10/32 via 10.0.12.1` and ping again. The /24 is still there and correct. Why does the ping now end with "time to live exceeded"? Undo with `:route r2 del 10.0.4.10/32`.

**Lab 202: `./hoplab run 202-routing-loop`**

- Watch the map: one echo request bounces on the r1-r2 link. In the packets view the same packet appears again and again with a TTL one lower each time.
- The error comes from 10.0.12.2 (r2) after about 0.14 s; the traceroute at 4 s alternates r1, r2, r1, r2.
- Predict, then try: `./hoplab run 202-routing-loop ttl=7`. How many times is the link crossed, and which router sends the error now?
- Predict, then try: `:route r2 del 10.9.0.0/16`, then `:h1 ping 10.9.1.1 count=1`. Which message replaces "time exceeded", and who sends it?

## Check yourself

- A table holds 172.16.0.0/12 via A, 172.16.32.0/19 via B, 172.16.40.0/24 via C and a default via D. Give the next hop for 172.16.41.9, 172.16.40.200, 172.31.255.1 and 172.32.0.1, showing the matches.
- Explain why a router that has a correct route for the destination can still be the reason a ping fails. Which symptom distinguishes a missing forward route from a missing return route?
- Which fields of the IP header and of the Ethernet header change at each hop, and which never change between source and destination?
- Two routers loop a prefix. A host sends 100 packets per second to it with initial TTL 64. How many packets per second cross the link between the two routers, approximately?
- Why does traceroute work, and what do you see in its output when the path contains a loop?
