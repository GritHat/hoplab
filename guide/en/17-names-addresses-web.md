<!-- level: 1 -->
<!-- labs: 140-dns-first, 141-dhcp-join, 121-http-page -->
# Names, addresses and the web

You open your laptop in a library, type the name of a site, and a page appears. In that second or two, three services worked for you, each built on the layers below.

First your laptop needed an address. It arrived knowing nothing about this network, so it asked aloud: "Is there anyone who can give me an address?". A server answered with everything needed: an IP address, the prefix, the default gateway, the address of a name server. This is **DHCP**. It is like arriving at a hotel: the reception gives you a room number and tells you where things are, and the room is yours only for the length of your stay.

Then the name had to become a number, because packets carry addresses, not names. Your laptop asked a **DNS** server: "what is the address of www.example.com?". DNS is the telephone directory of the Internet.

Finally the browser opened a TCP connection to that address and asked for the page with **HTTP**: "GET this page", and the server answered "200 OK, here it is". The page mentions images and other objects, so the browser asks for each of them too.

Three questions, three answers: who am I here, where is the server, give me the page.

<!-- detail: 2 -->

## How it works

**DHCP: four messages**, easy to remember as D-O-R-A.

- **DISCOVER**: the client, which has no address, sends a broadcast: "any DHCP server here?".
- **OFFER**: a server proposes an address, in lab 141 `10.0.1.100`.
- **REQUEST**: the client asks for exactly that address (still in broadcast, so that other servers know which offer was chosen).
- **ACK**: the server confirms, with the mask, the default gateway and the **lease** time (60 s in the lab) after which the address must be renewed.

Only now can the host configure its interface and its default route. DHCP travels over UDP, ports 67 (server) and 68 (client).

**DNS: one question, one answer**, normally over UDP port 53. The question holds a name and a **record type**; the answer holds records, each with a **TTL** that says for how many seconds it may be remembered.

- An **A** record maps a name to an IPv4 address: `www.example.com → 10.0.2.11` in lab 140.
- An **MX** record names the mail server of a domain.
- If the name does not exist the answer is the error **NXDOMAIN**.

**HTTP: text over TCP.** The client sends a request, the server a response:

```
GET /index.html HTTP/1.1          HTTP/1.1 200 OK
Host: www                         Content-Length: 138
                                  (empty line, then the 138 bytes)
```

The first line of the response carries a **status code**: 200 OK, 404 Not Found. The page of lab 121 embeds three objects, so the browser makes four requests in all.

**One connection or many?** With a **persistent connection** (HTTP/1.1) all four requests use the same TCP connection: one handshake. Opening a new connection for each object (as HTTP/1.0 did) costs one more handshake, that is one more round trip, per object. In lab 121 the round trip is about 68 ms, and the second way of loading takes about 3 x 68 = 204 ms longer.

<!-- detail: 3 -->

## Going deeper

**DNS is not one directory but a hierarchy.** No server knows every name. The name space is a tree: the root, the top-level domains (`com`, `it`), then the domains below them, each managed by its owner on **authoritative** servers. Your laptop talks to a **recursive resolver** (usually run by your provider), which walks down the tree on your behalf and **caches** every answer for its TTL. The cache is what makes DNS fast, and the TTL is the trade-off: long TTLs mean fewer queries but slow changes. Labs 240 and 340 show the hierarchy, the cache and the TTL at work. Lab 140 has a single authoritative server, the simplest case. Plain DNS has no authentication: a forged answer can poison a cache (lab 440), which is why DNSSEC and encrypted DNS exist.

**DHCP details.** Why a lease and not a permanent assignment? Because devices leave without saying goodbye, and addresses must return to the pool (lab 241 shows renewal, expiry and an exhausted pool). The broadcast does not cross routers, so large networks use relay agents to reach a central server. And DHCP trusts whoever answers first: a rogue server can hand out a false gateway.

**HTTP today.** HTTP/1.1 sends requests one after the other on a connection, so browsers open several connections in parallel. HTTP/2 multiplexes many requests on one connection; HTTP/3 runs over QUIC, on UDP, to avoid a lost packet blocking all the streams. Almost all traffic is **HTTPS**: a TLS handshake after the TCP one encrypts the conversation and proves the server's identity, at the cost of further round trips. Caches and content delivery networks bring copies of the objects closer to users. Because every step costs round trips, the delay of the path often matters more than its bandwidth for the loading time of a page.

**The whole story in order**: DHCP (once, when joining), ARP for the gateway, DNS query, TCP handshake, (TLS handshake,) HTTP request and response, more requests for the embedded objects.

To read more: Kurose and Ross, chapter 2 ("Application Layer": the Web and HTTP, DNS) and the section "A Day in the Life of a Web Page Request" at the end of chapter 6; RFC 1034 and RFC 1035 (DNS), RFC 2131 (DHCP), RFC 9110 and RFC 9112 (HTTP).

<!-- detail: 1 -->

## In the lab

**Lab 141-dhcp-join**: h1 arrives with no address; gw is router and DHCP server.

- Start it with `./netlab run 141-dhcp-join`. Select h1 (`n`): its interface has no address.
- At 1 s read the four messages in the log (`l`) and then the line `DHCP bound`. Open the Packets view (`p`) and check the destination MAC of the DISCOVER.
- Open the Routes tab of h1: a default route marked `dhcp` has appeared. In this lab the server announces no name server (the line says `dns —`).
- At 4 s h1 pings h2, which is on another network: it works only thanks to the gateway learned from DHCP.

**Lab 140-dns-first**: h1 asks the name server ns three questions.

- Start it with `./netlab run 140-dns-first`. At 1 s, 3 s and 5 s read the question and the answer in the log: an A record, an MX record, NXDOMAIN.
- Open the DNS query in the Packets view and find: UDP, port 53, the name, the type. In the answer find the TTL.
- Netlab's `ping` accepts addresses and node names, not DNS names: to use the answer type `:` and `h1 ping 10.0.2.11`.

**Lab 121-http-page**: h1 loads a page from www, twice.

- Start it with `./netlab run 121-http-page` and press `2`. At 1 s the page is loaded on one persistent connection, at 4 s with one connection per object.
- In the log count the `SYN` lines of each load, and read the two `complete` lines: about 422 ms and 559 ms. The first load also includes about 68 ms of ARP that the second does not pay: the true difference is three handshakes.
- In the Packets view find a frame with `GET` and one with `200 OK` and open them: the request is readable text.

**Change this and predict.**

- Lab 141: type `h1 dhcp release`, then `h1 ping h2`. What does the log say? Then `h1 dhcp client`.
- Lab 140: `h1 dig example.com type=TXT` and `h1 dig mail.example.com`. Which one fails, and with which answer?
- Lab 121: `./netlab run 121-http-page delay=100ms`. The round trip becomes about 208 ms: how much longer will the second load take than the first? Then try `objects=8`.
- Lab 121: type `h1 http get www/missing`. Which status code comes back?

## Check yourself

- Why must the DHCP DISCOVER be a broadcast?
- Name the four things a host typically learns from DHCP.
- What does the TTL of a DNS record tell the one who receives it?
- A page embeds 10 objects and the round trip is 100 ms. Roughly how much time do persistent connections save compared with one connection per object?
- Put in order: HTTP request, DNS query, TCP handshake, DHCP exchange, ARP for the gateway.
