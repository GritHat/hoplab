<!-- level: 1 -->
<!-- labs: 101-ping -->
# IP addresses and routers

A local network reaches as far as its cables and switches. The Internet is millions of such networks joined together, and the device that joins two networks is a **router**. The protocol that lets a packet cross them all is **IP**, the Internet Protocol.

Every interface has an **IP address**, and addresses are organised like telephone numbers: the first part (the **prefix**) identifies the network, as an area code identifies a city, and the rest identifies one machine inside it. This is what makes a worldwide network possible: a router far away does not need to know every machine, only in which direction each network lies.

A host follows one simple rule. Is the destination in my own network? Then I deliver directly. If not, I hand the packet to my **default gateway**, the router of my network, and let it worry. Each router repeats the same reasoning with its **routing table**, a list of "for this network, go that way", and passes the packet one step further. This is **forwarding**, hop by hop, like a letter passing from one sorting centre to the next.

Two everyday tools let you see all this: **ping** asks "are you there, and how far?", **traceroute** asks "which routers are on the way?".

<!-- detail: 2 -->

## How it works

An IPv4 address is 32 bits, written as four decimal numbers: `10.0.1.10`. A **prefix** such as `10.0.1.0/24` means "the first 24 bits are the network": all addresses from `10.0.1.0` to `10.0.1.255`. The same information can be written as the **mask** `255.255.255.0`.

In lab 101, h1 is `10.0.1.10/24` and h2 is `10.0.2.10/24`. The first 24 bits differ (`10.0.1` against `10.0.2`), so they are on different networks. The router r1 has one interface on each: `10.0.1.1` and `10.0.2.1`.

**What h1 does** to send to `10.0.2.10`:

- It compares the destination with its own prefix: not my network.
- It takes the **default route** (written `0.0.0.0/0`, "everything else") which points to the gateway `10.0.1.1`.
- It uses ARP to find the MAC address **of the gateway** and sends the frame to it. The IP destination inside is still `10.0.2.10`.

**What r1 does**: it looks up the destination in its routing table, choosing the most specific matching prefix; finds that `10.0.2.0/24` is directly connected; decrements the **TTL**; uses ARP for h2 and sends a new frame.

**TTL** (time to live) is a counter in the IP header. The sender sets it (64 here) and every router subtracts one. At zero the packet is discarded. This guarantees that a packet caught in a loop does not circulate forever. In lab 101 the reply from h2 arrives with `ttl=63`: one router was crossed.

**ICMP** is the protocol IP uses for its own messages. `ping` sends an ICMP **echo request** and the destination answers with an **echo reply**; the time between the two is the round-trip time. When a router discards a packet it sends back an ICMP error: **time exceeded** (TTL reached zero) or **destination unreachable** (no route).

**traceroute** turns the TTL into a tool. It sends a probe with TTL 1: the first router discards it and reveals its address in the "time exceeded" message. Then TTL 2, 3, … until the destination itself answers.

<!-- detail: 3 -->

## Going deeper

**Longest prefix match.** A routing table may hold several entries that match one destination, for example `10.0.0.0/8`, `10.0.2.0/24` and the default `0.0.0.0/0`. The router always picks the longest prefix, the most specific. This lets one line summarise thousands of networks while exceptions stay possible. Who fills the tables? By hand in small networks (lab 200), by routing protocols elsewhere (RIP, OSPF, BGP: labs 230, 232, 331).

**IP promises little.** It is **best effort** and **connectionless**: each packet is handled alone, may be lost, and two packets may take different paths. A router keeps no memory of conversations. Reliability is added by the hosts (topic 16).

**The IP header** is 20 bytes. Besides addresses and TTL it carries the total length, a protocol number that says what is inside (1 ICMP, 6 TCP, 17 UDP), a header checksum that every router must recompute because the TTL changes, and the fields for **fragmentation**. A packet larger than the MTU of the next link is cut into fragments that are reassembled only at the destination (the end of lab 101, and lab 201).

**Limits of the tools.** Ping measures the round trip, not each direction. Traceroute shows the routers of the outward path only, each identified by the interface the probe entered; paths can change between probes; many routers limit or refuse ICMP answers, so a missing reply does not prove a failure. Real traceroute on Unix sends UDP probes to unlikely ports, while here and on Windows it uses ICMP echo.

**Not enough addresses.** 32 bits give about 4 billion addresses, fewer than the devices in use. Two remedies coexist: private addresses reused behind **NAT** (lab 250), and **IPv6** with 128-bit addresses. Prefixes of any length (CIDR) replaced the old fixed classes A, B and C in 1993.

To read more: Kurose and Ross, chapter 4, section "The Internet Protocol (IP): IPv4, Addressing, IPv6, and More"; Tanenbaum, chapter 5, "The Network Layer in the Internet"; RFC 791 (IP), RFC 792 (ICMP), RFC 4632 (CIDR).

<!-- detail: 1 -->

## In the lab

**Lab 101-ping**: h1, the router r1 and h2, on two networks.

- Start it with `./netlab run 101-ping`.
- Select h1 with `n` and open its Routes tab: one line for its own network, one default route via `10.0.1.1`. Select r1: one line for each of the two networks it is connected to.
- 0 s, ping. In the log (`l`) the first exchange shows two ARP questions: h1 asks for the gateway, r1 asks for h2. Nobody asks h1 for the MAC of h2: why? The replies show `ttl=63`.
- Open the Sequence view (`s`) to see each packet stop at r1 and leave again.
- 5 s, traceroute. Watch `IP TTL expired` on r1, the `time exceeded` message, and the two lines of the result: hop 1 is `10.0.1.1`, hop 2 is h2.
- 8 s, a 3000-byte datagram is fragmented into three packets (see "Going deeper").

**Change this and predict.** Type `:` to open the command line.

- `h1 ping h2 ttl=1`: who will answer, and with which message?
- `h1 ping 10.0.9.9`: no network has this address. Which node will notice, and what will it tell h1?
- `link r1 h2 down` and then `h1 ping h2`: h1 still has its default route. What does r1 answer now? Bring the link back with `link r1 h2 up`.
- Run `./netlab run 101-ping loss=0.3`: the loss acts on the link h1–r1 in both directions. What fraction of pings should succeed?

## Check yourself

- Are `10.0.1.200` and `10.0.2.3` in the same /24 network? And in the same /16?
- h1 sends a packet to h2 through r1. Which destination MAC address and which destination IP address does the frame carry on the first link?
- Why does a packet arrive with a TTL lower than the one it was sent with, and what is the TTL for?
- How does traceroute discover a router that is not the destination of any of its packets?
- A host has the right address but no default gateway. What can it reach, and what not?
