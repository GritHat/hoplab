<!-- level: 2 -->
<!-- labs: 240-dns-hierarchy, 241-dhcp-leases -->
# DNS and DHCP in depth

Two services make a network usable by people, and both are databases consulted over UDP.

The **DNS** maps names to data, above all to addresses. No single server could hold or answer for all names, so the name space is a tree cut into **zones**, each managed by its owner and served by its own **authoritative** servers. A zone hands a subtree over to someone else by **delegation**: it publishes which servers are responsible. The root knows who serves `com`; the `com` servers know who serves `example.com`. To resolve a name you start at the root and follow the delegations down. Hosts do not do this themselves: they ask a **recursive resolver**, which walks the tree for them and **caches** every answer for the time the owner of the data allowed, the **TTL**. Caching is what makes the system fast and keeps the root from being overwhelmed.

**DHCP** gives a host its configuration when it joins a network: address, mask, default gateway, DNS resolver. The address is not given but **leased** for a limited time. The client must renew it periodically; an address whose lease expires goes back to the pool. Leases let a small pool serve many transient hosts, and when the pool is empty a new host simply gets nothing.

<!-- detail: 2 -->
## DNS messages and records

Queries and responses share one format, normally in a UDP datagram to port 53. A 12-byte header holds a 16-bit **identifier** (copied into the response), flags, and four counters for the sections that follow: question, answer, authority, additional. Important flags: QR (response), AA (authoritative answer), RD (recursion desired), RA (recursion available), TC (truncated), and the 4-bit response code (0 = no error, 3 = NXDOMAIN).

A resource record is (name, type, class, TTL, data). Common types:

- **A** / **AAAA**: IPv4 / IPv6 address of the name
- **NS**: a name server of the zone; **SOA**: zone parameters
- **CNAME**: the name is an alias of another name
- **MX**: mail exchanger with a preference; **TXT**: free text; **PTR**: reverse mapping

## Iterative and recursive resolution

The stub resolver of a host sends one query with RD = 1 to its resolver: a **recursive** query, "give me the final answer". The resolver then sends **iterative** queries: each server answers with what it knows, which is usually a **referral**: no answer, NS records in the authority section, and the addresses of those servers (**glue**) in the additional section.

Lab 240, resolving `www.example.com`:

- resolver → root: referral "`com` is served by a.gtld" plus glue
- resolver → com server: referral "`example.com` is served by ns.example.com" plus glue
- resolver → authoritative server: answer `A 10.9.9.9`, TTL 60, AA set

Three upstream queries and about 240 ms. The same question three seconds later is answered from the cache in 4 ms with zero upstream queries, and the TTL shown is 58 s: a cache hands out the *remaining* lifetime. The NS records were cached too, with their TTL of 3600 s, so a later query for another name in `example.com` needs only one upstream query. Negative answers are cached as well, for a time taken from the zone's SOA record (30 s in the lab).

## The DHCP exchange

DHCP runs over UDP, server port 67 and client port 68. The client has no address, so it sends from 0.0.0.0 to 255.255.255.255. Messages are BOOTP frames (operation, transaction identifier **xid**, client address, **yiaddr** = "your address", client hardware address) followed by options; option 53 is the message type.

- **DISCOVER** (client, broadcast): is there a server?
- **OFFER** (server): proposes yiaddr with mask (option 1), router (3), DNS (6), lease time (51), server identifier (54)
- **REQUEST** (client, broadcast): "I accept the offer of server S"; broadcast so that other servers withdraw theirs
- **ACK** (server): the lease starts. NAK if the address is no longer available.

## Lease timers

With a lease of L seconds the client sets **T1 = L/2** and **T2 = 7L/8**. At T1 it sends a REQUEST by unicast to its own server (*renewing*); an ACK restarts all timers. If nothing arrives by T2 it broadcasts the REQUEST to any server (*rebinding*). At L it must stop using the address and start again from DISCOVER. RELEASE gives the address back early.

In lab 241 L = 10 s: h1 is bound at 1.01 s and renews at 6.01 s, 11.02 s, and so on, every 5 s. The pool has two addresses: h1 and h2 take them, and h3's DISCOVER is answered by nothing at all (`pool exhausted, no offer`); it retries every 2 s. If the server dies after a renewal at time t, the clients try at t + 5 s (T1), broadcast at t + 8 s (T2, netlab rounds down to whole seconds) and lose the address at t + 10 s.

<!-- detail: 3 -->
## In depth: DNS

**Why UDP, and its consequences.** One datagram each way keeps queries cheap, but a resolver accepts any response that matches source address, port, identifier and question. An off-path attacker who guesses them can inject a false record into the cache (Kaminsky, 2008); resolvers therefore randomise both the identifier and the source port (RFC 5452). DNSSEC (RFC 4033 to 4035) signs records so that answers can be verified. Responses over 512 bytes need EDNS (RFC 6891) or a retry over TCP (RFC 7766).

**Cache behaviour.** The TTL is a trade-off chosen by the zone owner: long values save queries and survive server outages, short ones make changes take effect quickly. A change is never instantaneous: old data lives in caches until it expires. Negative caching is defined in RFC 2308. Resolvers trust data according to where it came from (RFC 2181): glue is only good enough to continue the walk. A delegation pointing to a server that does not serve the zone is a *lame delegation*, a frequent cause of slow lookups.

**The real system.** There are 13 root server names, each an anycast group of hundreds of machines. Resolvers measure servers and prefer the fast ones, and query minimisation (RFC 9156) reveals to each server only the labels it needs. DNS over TLS (RFC 7858) and over HTTPS (RFC 8484) hide the queries between stub and resolver. Reference: RFC 1034 and RFC 1035 (Mockapetris, 1987).

**netlab simplifies:** class IN only; no TCP, EDNS or DNSSEC; delegations must have glue.

## In depth: DHCP

**Relays.** Broadcasts do not cross routers. A relay agent on each subnet forwards DISCOVERs by unicast to a central server and writes its own address in the *giaddr* field, which tells the server which pool to use. netlab's server serves only its own subnet.

**State.** A real server stores leases on disk so that a restart does not hand out addresses still in use, and many probe an address (ICMP echo) before offering it; clients check with ARP (RFC 5227) and answer DECLINE on a conflict. A client that reboots asks for its previous address directly (INIT-REBOOT). The netlab server keeps leases in memory only.

**Security.** DHCP has no authentication. A rogue server can answer faster than the real one and make itself gateway and DNS for the victims; a client forging many hardware addresses can exhaust the pool (starvation). Switches defend with DHCP snooping: server messages are accepted only from trusted ports.

**Lease length** is the analogue of the DNS TTL: short leases recycle addresses quickly in a café, at the cost of traffic and of dependence on the server; long leases suit stable networks. References: RFC 2131, RFC 2132 (options), RFC 951 (BOOTP), RFC 8415 (DHCPv6); IPv6 hosts can also configure themselves without a server (SLAAC, RFC 4862).

<!-- detail: 1 -->
## In the lab

**Lab 240: `./netlab run 240-dns-hierarchy`**

- At 1 s follow the log at detail 2: the resolver r asks root, com, auth in turn. On the map the queries leave r on three different links.
- In the packets view (`p`) press Enter on the root's response: answer count 0, NS in the authority section, glue in the additional section. Compare with the final answer from auth (AA flag, one answer).
- At 4 s the same query: `0 upstream queries, from the cache`, TTL 58 s. Select r and open its `dns` tab to see the cache.
- At 6 s the script runs `dig ... trace=1` from h1: this time h1 does the walk itself, without the resolver. The log shows it asking the root, being referred to the servers of com, then to the server of example.com, and getting the authoritative answer: the same three steps r performed at 1 s.
- Predict, then try: `:h1 dig example.com server=r`. How many upstream queries, and to whom? Then `:h1 dig mail.example.com server=r` twice: what is the answer, and what changes the second time?
- Predict, then try: `:r dns flush` and then `:h1 dig www.example.com server=r`. How many upstream queries now? Then `:h1 dig example.com type=NS server=r` and read the TTL.

**Lab 241: `./netlab run 241-dhcp-leases`**

- Follow DISCOVER, OFFER, REQUEST, ACK for h1 at 1 s; open each in the packets view and find xid, yiaddr and options 53, 51 and 54. Note the source and destination addresses of the DISCOVER.
- At 3 s h3 finds the pool exhausted. At 6 s h1 reaches T1 and renews with a unicast REQUEST. Open the `dhcp` tab of s to see the lease table.
- Predict, then try: `:h1 dhcp release`. What happens to h3, and within how many seconds?
- At 14 s the lab crashes the server for 26 seconds. Before it happens, write down when h1 will log T1, T2 and `lease expired`, then check. At 40 s the server returns with its lease table (it is kept on stable storage, like the lease file of a real server) and the clients, which have been sending DISCOVER all along, are served again: see who gets the two addresses this time, and explain why it need not be the same two hosts.
- Predict, then try: `./netlab run 241-dhcp-leases lease=60`. Does any client notice the outage?
- Try also `./netlab run 241-dhcp-leases lease=4`: at what times does h1 renew?

## Check yourself

- A resolver with an empty cache resolves `mail.cs.example.org`. List the queries it sends, the kind of response to each, and what is in its cache at the end.
- Distinguish recursive from iterative queries. Who sends which, and why do root servers refuse recursion?
- A site changes the address of its web server. Its A record had a TTL of 86400. What do users observe, for how long, and how should the administrator have prepared the change?
- A client obtains a lease of 8 hours at 09:00 and its server goes off at 12:00. At what times does the client send messages, of which type and to which address, and when does it lose the address?
- Why is the DHCP REQUEST broadcast even though the client already knows the server's address?
