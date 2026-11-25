<!-- level: 2 -->
<!-- labs: 250-nat-basics, 251-port-forwarding, 252-load-balancer -->
# NAT and load balancing

Routers are supposed to forward packets without touching their addresses. Two very common devices break that rule on purpose, in opposite directions, with the same tool: a table that remembers how each flow was rewritten.

**NAT** hides many hosts behind one address. IPv4 addresses are scarce, so homes and companies number their hosts from **private** ranges that anyone may reuse and that the Internet does not route. A packet from 192.168.1.10 can reach a server, but the answer has nowhere to go. The NAT router at the border rewrites the *source* of each outgoing packet to its own public address and to a port number it chooses, and records the pair. When the answer arrives on that port it rewrites the *destination* back. Since entries are created only by outgoing packets, nobody outside can start a conversation with a host inside, unless a static **port forwarding** rule is configured.

A **load balancer** hides many servers behind one address. Clients talk to a **virtual IP**; the balancer chooses a real server for each new flow, rewrites the *destination*, and remembers the choice so that the whole flow reaches the same server. It also probes the servers with **health checks** and stops choosing those that do not answer.

<!-- detail: 2 -->
## Private addresses and NAPT

RFC 1918 reserves 10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16. The form of NAT in every home router is **NAPT** (network address and port translation, RFC 3022). For an outgoing packet the router:

- looks up (protocol, inside address, inside port) in the translation table, and if there is no entry allocates a free public port and creates one;
- replaces source address and port, and updates the IP checksum and the TCP/UDP checksum (which also covers the addresses, through the pseudo-header);
- restarts the idle timer of the entry.

For an incoming packet it looks up (protocol, public port): if found it rewrites the destination and forwards inside; if not, the packet is dropped.

The table of lab 250 after h1 and h2 each send a UDP datagram from source port 49152:

```
proto  inside               public               remote
icmp   192.168.1.10:2       203.0.113.2:61001    198.51.100.10
udp    192.168.1.10:49152   203.0.113.2:61002    198.51.100.10:7
udp    192.168.1.11:49152   203.0.113.2:61003    198.51.100.10:7
```

The two hosts chose the same port; the NAT tells them apart by giving them different public ports. ICMP echo has no ports: its identifier field plays that role. The server only ever sees 203.0.113.2.

ICMP *errors* need more work: a "time exceeded" from a router outside quotes the header of the offending packet, which is the translated one. The NAT must find the mapping from the quoted header, and rewrite both the outer destination and the quote. That is why traceroute works through r1 in the lab.

## Port forwarding and hairpinning

A static entry "public port 8080 → 192.168.1.10:7" never expires and accepts packets from anyone. In lab 251 the outside client is refused before the rule exists (the lab's NAT answers ICMP port unreachable) and served after. The reply leaves with source 203.0.113.2:8080: the client never learns the private address. An inside host that uses the public address reaches the server only if the NAT supports **hairpinning**: translating a packet that arrives from inside and sending it back inside.

## What NAT breaks

- **Inbound connections**: servers and peer-to-peer applications behind a NAT are unreachable without configuration or tricks.
- **Protocols that carry addresses in their payload** (FTP, SIP): the private address inside the data is useless outside, unless the NAT understands and rewrites the application protocol.
- **Idle connections**: the entry times out and the connection dies silently; applications send keepalives.
- **The layering itself**: a network-layer device now depends on transport ports, so new transport protocols cannot cross it, and IPsec authentication of the header fails.

## Load balancing at layer 4

The balancer keeps a pool of servers, a **flow table** (client address and port → server) and, for each server, a health state. For the first packet of a flow it selects a server; with **round robin** it simply takes the next in the list. It rewrites the destination from the virtual IP to the server; on the way back it rewrites the source to the virtual IP. Later packets follow the flow table: without it a TCP connection would be sent to a server that has never seen its handshake.

Health checks in lab 252 are ICMP echoes every second; a server is removed after 2 consecutive misses (`fall`). s2 crashes at 2.0 s: the checks at 2.0 and 3.0 s fail, and at 4.0 s it leaves the pool and its 10 flows are assigned again. In those two seconds the requests of the flows pinned to s2 are lost: about one third of 20 requests per second, 13 in the lab.

<!-- detail: 3 -->
## In depth: NAT

**Behaviours** (RFC 4787 for UDP, RFC 5382 for TCP, RFC 5508 for ICMP). *Mapping*: an endpoint-independent NAT uses the same public port for everything an inside socket sends; a symmetric one allocates a new port per destination. *Filtering*: who may send to a mapped port: anyone (full cone), only addresses already contacted, only address and port pairs already contacted (port-restricted, netlab's default), or only the mapping's destination. These properties decide whether two hosts behind NATs can connect directly by **hole punching**: both learn their public endpoints from a server (STUN, RFC 8489), send to each other at the same time, and each NAT takes the peer's packet for an answer. When this fails a relay is needed (TURN, RFC 8656); ICE (RFC 8445) tries all options.

**Limits.** One public address offers about 64000 ports per protocol, a ceiling on concurrent flows. Carrier-grade NAT shares one address among many customers (shared range 100.64.0.0/10, RFC 6598), adds a second translation, and makes it hard to attribute traffic to a subscriber. Typical timeouts: 30 s to a few minutes for UDP, hours for established TCP.

**Not a firewall.** NAT blocks unsolicited inbound traffic as a side effect, but offers no policy. Hosts can ask the NAT for a mapping with UPnP or PCP (RFC 6887). The architectural criticism is in RFC 2993; terminology in RFC 2663. IPv6 removes the need for NAT, not the need for filtering.

## In depth: load balancing

**Return path.** With destination rewriting only, the servers must send replies back through the balancer (in lab 252 it is their router). Alternatives: *full NAT* rewrites the source as well, so the servers lose the client address; *direct server return* delivers to servers that own the virtual IP on a loopback and answer the client directly, so the balancer handles only the small inbound packets.

**Choosing a server.** Round robin ignores load; least connections adapts to long flows; hashing the flow needs no table, so several balancers behind ECMP can choose alike, but plain modulo hashing remaps almost all flows when the pool changes. Consistent hashing (Karger et al., 1997) and Maglev hashing (Eisenbud et al., NSDI 2016) move only about 1/N of them. "The power of two choices" (Mitzenmacher) explains why picking the better of two random servers is nearly as good as knowing all loads.

**Health checks** are a failure detector with the usual trade-off: a short interval and small `fall` react fast but eject healthy servers on a single lost probe; real systems also require several successes (`rise`) before readmission, and drain connections before planned removals. An ICMP echo proves the machine is up, not that the service works: production checks open a TCP connection or fetch a URL.

**Layer 7.** A proxy that terminates TCP and TLS can route by URL or cookie, at a higher cost per connection.

**netlab simplifies:** the labs use UDP request/response flows, and a flow ends after `life` requests rather than with a FIN.

<!-- detail: 1 -->
## In the lab

**Lab 250: `./netlab run 250-nat-basics`**

- Before 4 s there is no NAT: the server answers, and isp logs `no route to 192.168.1.10`. Notice who receives the ICMP error.
- At 5 s open the packets view (`p`) and compare the same echo request on the link sw-r1 and on r1-isp: source address, identifier, checksums. Select r1 and open its `nat` tab.
- At 9 s read the two UDP mappings; at 12 s open the ICMP time exceeded from isp before and after r1.
- Predict, then try: `./netlab run 250-nat-basics preserve=1`. Which public ports do h1 and h2 get at 9 s?
- Predict, then try: `./netlab run 250-nat-basics timeout=500ms`. How many mappings does the three-packet ping at 5 s create, and why? Then `type=symmetric` and compare the port numbers.

**Lab 251: `./netlab run 251-port-forwarding`**

- At 1 s r1 logs `no mapping, nothing inside to deliver it to`; at 4 s the rule is added; at 5 s the datagram reaches web. In the packets view compare its destination before and after r1, and the source of the reply before and after r1.
- At 7 s h1 tries the public address from inside and fails.
- Predict, then try: `./netlab run 251-port-forwarding hairpin=1`. Open the packet web receives at 7 s: what is its source address, and why must the NAT rewrite it?

**Lab 252: `./netlab run 252-load-balancer`**

- Select lb and open its `lb` tab: pool, flow table, hits. In the log `new flow ... → s2 (rr)` cycles s2, s3, s1. Select c1 for the tally of who answered.
- At 2 s s2 crashes; at 4 s `health check failed, taken out of the pool` and `10/30 flows lost their server`; at 9 s s2 returns.
- Predict, then try: `./netlab run 252-load-balancer check=250ms`. When is s2 removed and how many requests are lost? Then `fall=1` with the default interval.
- Predict, then try: `./netlab run 252-load-balancer algo=leastconn`. After 9 s, how does s2's share of new flows compare with round robin, and why?

## Check yourself

- Two inside hosts open TCP connections from the same source port to the same web server. Write the translation table and the headers of one packet in each direction, on both sides of the NAT.
- Why must a NAT recompute the TCP checksum even though it does not touch the TCP payload?
- Explain why a host behind a NAPT cannot run a public server without extra configuration, and what a port forwarding rule changes.
- A layer-4 balancer uses round robin without a flow table. What goes wrong with TCP?
- Health checks run every 2 s with `fall` = 3 and a server receives 300 requests per second. Estimate the requests lost when it crashes. What is the risk of setting `fall` = 1?
