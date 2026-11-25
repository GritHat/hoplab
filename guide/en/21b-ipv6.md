<!-- level: 2 -->
<!-- labs: 103-ipv6-first, 204-ipv6-slaac, 205-ipv6-pmtu, 206-dual-stack, 207-tcp-over-ipv6 -->
# IPv6: addresses, Neighbor Discovery and autoconfiguration

IPv4 has 32-bit addresses: about four billion, fewer than the devices that want one. **IPv6** is the same idea of a network layer — packets with a source and a destination address, forwarded hop by hop by longest prefix match — with **128-bit addresses** and a header redesigned with thirty years of hindsight. It does not replace IPv4 on a given day: the two run side by side on the same cables (**dual stack**), told apart by the type field of the Ethernet frame.

Three things feel different the moment you look at a lab. An interface has **several addresses**, and one of them, the **link-local** address starting with `fe80::`, exists before anybody configures anything. There is **no ARP and no broadcast**: finding a neighbor's MAC address, finding the routers, checking that an address is free are all done by one protocol, **Neighbor Discovery**, with ICMPv6 messages sent to multicast groups. And a host can **configure itself** from what the router announces, with no DHCP server.

One more rule surprises people who know IPv4: **routers never fragment** an IPv6 packet. If it does not fit the next link they drop it and tell the source, which must send smaller packets.

<!-- detail: 2 -->
## Writing addresses

An address is 128 bits written as eight groups of four hexadecimal digits: `2001:0db8:0000:0000:0000:0000:0000:0010`. Leading zeros of a group are dropped, and **one** run of all-zero groups is replaced by `::`, giving `2001:db8::10`. A prefix is written as in IPv4: `2001:db8:1::/64`. Almost every subnet is a **/64**: 64 bits of network prefix, 64 bits of **interface identifier**.

Kinds of address you meet in the labs:

- **link-local** `fe80::/10`: valid only on one link, never forwarded by a router; every interface has one
- **global unicast** `2000::/3`: the routable addresses; the labs use the documentation prefix `2001:db8::/32`
- **multicast** `ff00::/8`: `ff02::1` is all nodes on the link, `ff02::2` all routers, `ff02::1:ffXX:XXXX` the *solicited-node* group of the addresses ending in XX:XXXX
- **unspecified** `::`, the source of a node that does not have an address yet

netlab builds the interface identifier from the MAC address (modified EUI-64): the MAC `02:00:4e:4c:01:00` gives `fe80::4eff:fe4c:100` — `ff:fe` is inserted in the middle and one bit of the first byte is flipped.

## The header

The IPv6 header is always **40 bytes** and has eight fields: version, traffic class, flow label, payload length, **next header**, **hop limit**, source, destination. Compared with IPv4:

- no header checksum: links and transports already have theirs, and routers no longer recompute one at every hop
- no fragmentation fields and no options: they moved to **extension headers**, chained by the next header field (an IPv6 packet carrying a fragment of a UDP datagram has next header 44, Fragment, whose own next header is 17, UDP)
- TTL is called hop limit, which is what it always was

The price of removing the header checksum is that the **UDP checksum becomes mandatory**, and that every transport checksum covers a pseudo-header with the two 128-bit addresses.

## Neighbor Discovery

Five ICMPv6 messages (types 133–137; netlab implements the first four) do the work of ARP, router discovery and more. All are sent with hop limit 255 and discarded if they arrive with less, so they cannot come from another link.

- **Neighbor Solicitation / Advertisement.** To resolve `2001:db8:1::1` a node sends an NS to the solicited-node group `ff02::1:ff00:1`, carried in a frame for the multicast MAC `33:33:ff:00:00:01`. Only the nodes whose addresses end in those 24 bits are interested, instead of everybody as with an ARP broadcast. The owner answers with a unicast NA carrying its MAC. Entries of the **neighbor cache** have a state: *incomplete* while asking, *reachable* for about 30 seconds after a confirmation, then *stale*; a stale entry is still used, but triggers a unicast probe.
- **Duplicate address detection (DAD).** Before using any address, a node sends an NS *for its own address* from the source `::`. If someone answers, the address is taken. Only after a second of silence does the address stop being *tentative*.
- **Router Solicitation / Advertisement.** A host asks `ff02::2` whether there is a router; routers answer, and repeat periodically, with an RA carrying the prefixes of the link and how long the router may be used as default gateway.

## Autoconfiguration (SLAAC)

A host needs four steps and no server. It forms its link-local address and checks it with DAD. It sends a Router Solicitation. From each prefix of the Router Advertisement flagged *autonomous* it forms prefix + interface identifier, and checks that too. It installs a default route through the link-local address of the router. In netlab a router advertises with `run r1 ra`; the default route shows up in the Routes tab with source `ra`.

## Fragmentation and path MTU

Every IPv6 link must carry packets of **1280 bytes**. A router that cannot forward a packet because it is larger than the next link sends ICMPv6 **Packet Too Big** with that link's MTU and drops it. The source records the value for that destination (**path MTU discovery**) and, for a datagram that still does not fit, fragments it itself with the Fragment header: offset in units of 8 bytes, an M flag for "more fragments", a 32-bit identification. Reassembly happens only at the destination, as in IPv4.

<!-- detail: 3 -->
## In depth

**Why the redesign matters to routers.** A fixed 40-byte header with no checksum and no fragmentation means the forwarding path is: decrement the hop limit, look up the destination, send. Everything optional is in extension headers that routers are supposed to ignore (only Hop-by-Hop is for them). In practice, packets with extension headers are often dropped in the Internet (RFC 7872 measured it), which is one reason transports avoid IPv6 fragmentation altogether and keep their segments under the path MTU.

**Path MTU discovery is fragile.** It depends on the Packet Too Big message arriving. Firewalls that drop all ICMPv6 create black holes: large packets vanish with no error. RFC 4890 lists the ICMPv6 messages that must never be filtered; RFC 8899 describes discovery that does not rely on ICMP, by probing with packets of growing size. Note also that the path MTU is learned **per direction**: in the lab the request of the second ping arrives, and it is the reply that dies.

**Privacy and the interface identifier.** An identifier derived from the MAC address is the same on every network the device visits, so it tracks the device. Real hosts now use temporary random identifiers (RFC 8981) or stable-but-opaque ones (RFC 7217). netlab keeps the EUI-64 form because it lets you recognise the node in an address.

**Security of Neighbor Discovery.** Like ARP, ND trusts the link: anybody can answer a solicitation, or advertise itself as a router. The hop limit 255 rule only proves the message comes from the same link. Switches defend with RA Guard (RFC 6105); the cryptographic answer, SEND (RFC 3971), is hardly deployed.

**What netlab leaves out.** There is no MLD: nodes do not announce the multicast groups they listen to, and the switches simply flood multicast, so the saving of solicited-node groups over broadcast is visible in who *processes* a solicitation, not in who receives it. There is no DHCPv6, no redirect message, no privacy address; extension headers other than Fragment are skipped without being acted upon; addresses learned by SLAAC do not expire; the neighbor cache has no DELAY state (a stale entry goes straight to a probe after five seconds). UDP, TCP and HTTP run over both families (lab `207-tcp-over-ipv6` shows TCP discovering the path MTU from Packet Too Big); the routing protocols of the lab (RIP, OSPF, BGP), DNS and DHCP run over IPv4 only: IPv6 routes are static or automatic, and there are no AAAA records.

**Transition.** Dual stack is the simple case: both protocols everywhere. Where that is impossible, tunnels carry one protocol inside the other, and translators (NAT64 with DNS64, RFC 6146/6147; 464XLAT, RFC 6877) let IPv6-only networks reach the IPv4 Internet. None of these is in the lab.

Primary sources: RFC 8200 (IPv6), RFC 4291 (addressing architecture), RFC 5952 (text representation), RFC 4443 (ICMPv6), RFC 4861 (Neighbor Discovery), RFC 4862 (SLAAC), RFC 8201 (path MTU discovery). Textbooks: Kurose and Ross, chapter 4, the section on IPv6; Peterson and Davie, section 4.1.

<!-- detail: 1 -->
## In the lab

Start with `103-ipv6-first`, at detail 2.

- In the first second nothing "useful" happens: every interface sends two Neighbor Solicitations from `::`, one for its link-local address and one for its global address. That is DAD. Select h1 (`n`), open the Node tab and watch the addresses lose the label "tentative".
- At 2 s the ping starts. In the Sequence view (`s`) find the solicitation "Who has 2001:db8:1::1?" and its advertisement: the same role ARP played in lab 101. Open the solicitation in the Packets view (`p`, Enter): destination `ff02::1:ff00:1`, Ethernet destination `33:33:ff:00:00:01`, hop limit 255.
- Open the ARP tab of h1: the IPv4 table is empty, the IPv6 neighbor cache has the router, *reachable*; wait and it becomes *stale*.
- Predict, then try: `:h1 ping fe80::4eff:fe4c:200` (the link-local address of r1) works; the link-local address of h2 does not. Why?

Then `204-ipv6-slaac`: nobody gave the hosts an address.

- Watch h1's Node and Routes tabs between 1 s and 2 s: router solicitation, advertisement, a new address marked SLAAC, a default route with source `ra`.
- Open the Router Advertisement in the Packets view and read the prefix option and the router lifetime.
- Type `:stop r1 ra`, then crash and restart h1 with `k` twice. Predict what h1 has after two seconds, and whether it can ping h2.

`205-ipv6-pmtu`: run it once, then run lab 201 and compare who cuts the packet. Change the parameter `size` to 1200: which messages disappear? In `206-dual-stack` compare the two echo requests field by field, then explain why removing an IPv4 route leaves IPv6 untouched. In `207-tcp-over-ipv6` open the Packets view and compare the MSS option of the two SYNs (1460 and 1440), then find the two ICMP errors that make www lower its MSS; run it again with `mtu=1500` and with `mtu=1400` and predict the MSS each time (MTU minus 40 over IPv4, minus 60 over IPv6).

## Check yourself

- Write `2001:0db8:0000:0000:0000:ff00:0042:8329` in its shortest form. Why may `::` appear only once?
- A host with MAC `02:00:4e:4c:03:00` receives the prefix `2001:db8:b::/64` in a Router Advertisement. Which address does it form, and what must happen before it uses it?
- Neighbor Solicitations are sent to a multicast group rather than broadcast. Which nodes on a link of a thousand hosts process a solicitation for one address, and how does the receiver's network card know?
- The middle link of a path has MTU 1280 and a host sends a 1500-byte packet. List the packets that travel until a 3000-byte UDP datagram is delivered, and say who fragments it.
- Why is the UDP checksum optional over IPv4 and mandatory over IPv6?
</content>
