<!-- level: 3 -->
<!-- labs: 360-rs-vs-hamming, 361-mac-curves, 370-stp-tuning, 340-dns-ttl -->
# Codes, shared channels, spanning trees and caches: four engineering trade-offs

This topic gathers four classic design problems below and beside the network layer. Each has a quantitative model, and in each the textbook answer becomes interesting only when a parameter is varied.

**Error-correcting codes.** Redundancy buys reliability, but which code wins depends on the statistics of the errors: independent bit flips or bursts. The key idea is that long codes over large symbols (Reed–Solomon) average the noise out, and that an interleaver turns a burst into isolated errors.

**Medium access.** Stations sharing a channel with no arbiter collide. The throughput `S` as a function of the offered load `G` rises, peaks and collapses; the whole family ALOHA, CSMA, CSMA/CD, CSMA/CA is a sequence of ways to shrink the time during which a transmission is vulnerable, governed by one parameter: the ratio `a` of propagation delay to frame time.

**Spanning trees.** Ethernet has no hop limit, so redundant links must be blocked by a distributed election. The protocol is simple; the engineering is choosing the root, the costs and the timers, where safety (no transient loop) is paid in recovery time.

**DNS caching.** A TTL is a promise not to ask again. Long TTLs save queries and latency and hide outages; short TTLs make changes visible quickly. Negative answers need a TTL too.

What makes all four hard is the same thing: a local, cheap decision (how many parity bits, when to transmit, which port to block, how long to remember) fixes a global behaviour that only a model or a measurement reveals.

<!-- detail: 2 -->
## Algorithms and models

**Block codes.** An `(n, k)` code with minimum distance `d` has rate `k/n` and corrects `t = floor((d - 1)/2)` errors per block.

```
Hamming(7,4):  d = 3,   t = 1 bit per 7-bit block,       rate 0.57
RS(255,223):   d = 33,  t = 16 bytes per 255-byte block, rate 0.87  (d = n-k+1)
```

On a channel with independent bit error rate `p`, a Hamming block fails when two or more of its 7 bits flip:

```
P(block fails) ~ C(7,2)*p^2 = 21*p^2
P(frame of L data bits fails) ~ (L/4)*21*p^2
```

**Worked example (lab 360).** `p` = 0.001, frames of about 4300 bits: 1084 blocks, so about `1084*21e-6` = 2.3% of the frames are damaged. For Reed–Solomon a byte is bad with probability about `8p` = 0.008, a block holds on average 2 bad bytes and fails only above 16: practically never. The code with less redundancy wins. A burst of `b` bits spoils at most `floor((b - 1)/8) + 2` bytes, so a 12-bit burst costs RS two or three symbols of its sixteen, while it puts several errors in one or two Hamming blocks. An **interleaver** of depth `D` transmits the codewords column by column: a burst no longer than `D` leaves at most one error in each.

**S(G) curves.** With Poisson attempts at rate `G` per frame time, infinitely many stations, and `a = propagation delay / frame time`:

```
pure ALOHA:            S = G*exp(-2G)          max 1/(2e) = 0.184 at G = 0.5
slotted ALOHA:         S = G*exp(-G)           max 1/e   = 0.368 at G = 1
non-persistent CSMA:   S = G*exp(-a*G) / ( G*(1 + 2a) + exp(-a*G) )
CSMA/CD (saturation):  S ~ 1 / (1 + 2e*a)
```

With `a` = 0.01 and `G` = 5.55 the CSMA formula gives `5.25/6.61` = 0.795, the value printed by the lab. 1-persistent CSMA does better at low load and peaks near 0.53 for the same `a`.

**Spanning tree.** Every BPDU carries a priority vector, compared lexicographically (lower is better):

```
( root bridge ID, root path cost, sender bridge ID, sender port ID )
bridge ID = priority (16 bits) . MAC address
```

The bridge with the lowest ID is the root; on every other bridge the port receiving the best vector is the root port; on each segment the bridge offering the best vector owns the designated port; all other ports block. A port moving to forwarding spends one forward delay in listening and one in learning. Recovery takes `2*fwd` after a failure detected locally, and up to `max_age + 2*fwd` when the information must expire (20 + 30 s with the standard timers 2/20/15 s).

**DNS caching.** For a name queried at a resolver as a Poisson process of rate `λ`, with TTL `T`:

```
upstream queries per second = λ/(1 + λ*T)        hit ratio = λ*T/(1 + λ*T)
worst-case staleness after a change = T
```

With `λ` = 1/s, `T` = 60 s gives a 98.4% hit ratio and `T` = 5 s gives 83%. A CNAME chain is cached link by link; the effective lifetime is the minimum along the chain. A negative answer (NXDOMAIN, or no data of that type) is cached for the minimum of the SOA record's TTL and its MINIMUM field (RFC 2308).

<!-- detail: 3 -->
## In depth

**Codes.** Shannon's theorem says a binary symmetric channel has capacity `1 - H(p)`: for `p` = 0.001 that is 0.989, so both codes of the lab waste rate, and short codes must: reliability near capacity needs long blocks. Reed–Solomon codes are maximum distance separable (they meet the Singleton bound) and their decoder either corrects or reports failure, whereas a Hamming decoder facing two errors confidently flips a third bit: the bench column "silently wrong". That is why links keep a CRC above the FEC. Interleaving costs delay and memory proportional to its depth and does nothing against random errors. Deployed systems concatenate (RS outside a convolutional code, from Voyager to DVB), or use LDPC and polar codes (Wi-Fi, 5G), with hybrid ARQ on top.

**Channels.** The curves assume infinite population and no memory; a real system is a feedback loop. ALOHA with a fixed retransmission probability is unstable: past the peak, backlog grows, `G` rises and `S` falls further. Backoff is the stabiliser, and binary exponential backoff is stable only for bounded populations. CSMA degrades as `a` grows (long cables, short frames, high bit rates), which is the reason for Ethernet's minimum frame size. For 802.11 the right model is Bianchi's Markov chain of the backoff counter, not `S(G)`.

**Spanning tree.** Safety invariant: a port forwards only after every bridge has had time (two forward delays) to learn of the new topology. The danger is not a lost link but a lost BPDU: a blocked port that stops hearing BPDUs (unidirectional link, overloaded CPU, timers too tight for the loss rate) believes it is designated and opens a loop. Hence the standard constraint `2*(fwd - 1) >= max_age >= 2*(hello + 1)`, and features such as loop guard, root guard and BPDU guard. Place the root (and a backup with the next priority) at the centre of the topology: the tree is not shortest-path between leaves. RSTP (802.1w) replaces timers with a proposal/agreement handshake; routed fabrics with ECMP, TRILL and SPB avoid blocking altogether.

**DNS.** Measurements (Jung et al.) show that, because name popularity is heavy-tailed, A-record TTLs of a few minutes cost little hit ratio; what matters is caching the NS records of the delegation chain. Short TTLs enable load balancing and failover but expose users to an authoritative outage; resolvers clamp extreme TTLs and may serve stale data (RFC 8767).

**In netlab.** Coding: frames are coded whole, the interleaver spans one frame, RS uses fixed (255,223) blocks. MAC: time advances in ticks of one propagation delay, every station is one tick from every other, ten stations with finite queues, so agreement with theory is in shape. STP: timers shortened (1/6/3 s); `rapid=1` only lets a new root port forward at once. DNS: no TCP, EDNS or DNSSEC.

**Primary literature.**

- C. Shannon, "A Mathematical Theory of Communication", 1948; R. Hamming, "Error Detecting and Error Correcting Codes", 1950; I. Reed, G. Solomon, "Polynomial Codes over Certain Finite Fields", 1960.
- N. Abramson, "The ALOHA System: Another Alternative for Computer Communications", 1970.
- L. Kleinrock, F. Tobagi, "Packet Switching in Radio Channels: Part I, Carrier Sense Multiple-Access Modes and Their Throughput-Delay Characteristics", 1975.
- R. Metcalfe, D. Boggs, "Ethernet: Distributed Packet Switching for Local Computer Networks", 1976.
- G. Bianchi, "Performance Analysis of the IEEE 802.11 Distributed Coordination Function", 2000.
- R. Perlman, "An Algorithm for Distributed Computation of a Spanning Tree in an Extended LAN", 1985; IEEE 802.1D.
- P. Mockapetris, RFC 1034 and RFC 1035, 1987; RFC 2308 (negative caching).
- J. Jung, E. Sit, H. Balakrishnan, R. Morris, "DNS Performance and the Effectiveness of Caching", 2002.

<!-- detail: 1 -->
## In the lab

**360-rs-vs-hamming.** `./netlab run 360-rs-vs-hamming`. Two links with BER 0.001: h1–h2 coded with Hamming(7,4), r1–r2 with Reed–Solomon. At 10 s compare the `sink` tabs of h2 and r2: of 200 datagrams, RS delivers all, Hamming loses a few (compute the expected 2.3% first; the log shows "NOT detected by the link: delivered damaged" followed by "UDP wrong checksum"). From 11 s the node lab prints three sweeps with errors in bursts of 12 bits: read "delivered", "repaired", "discarded", "silently wrong" for `hamming74`, `hamming74i` and `rs`. The bench keeps the average BER fixed, so longer bursts mean fewer frames hit: judge a code by the share of hit frames it repairs, not by "delivered" alone. Experiments: `burst=300` (about 38 bytes, more than the 16 that RS corrects: RS now repairs nothing and discards, while interleaved Hamming still repairs, because a 200-byte frame is 400 blocks and the burst puts at most one error in each); `burst=600` (longer than the interleaver depth: predict that no code repairs any more); in the lab, `:` then `lab coding sweep code=rs size=500 burst=200`, and `lab coding sweep code=rep3`. Note that RS discards where Hamming delivers wrong data.

**361-mac-curves.** `./netlab run 361-mac-curves`. The node bench runs seven sweeps; open its `mac` tab (bars, the yellow mark is the theory) or read the tables in the log: `load`, measured `G`, `S`, `theory`. Check by hand one row of ALOHA and one of non-persistent CSMA with the formulas above. Experiments: `prop=10ms` (so `a` = 0.1: predict that the ALOHA peaks do not move and every CSMA peak falls, roughly from 0.84 to 0.6 for non-persistent); `stations=3` and `stations=24` (distance from the infinite-population model); `:` then `bench mac sweep proto=csmap p=0.01`.

**370-stp-tuning.** `./netlab run 370-stp-tuning`. Four switches in a ring, s3 forced root by priority 4096. In the `stp` tab of each switch read role, state and cost of every port. Verify the costs: s1 reaches the root with cost 110 through s4 (100 + 10) and 200 through s2, so p1 is its root port and p0 is blocked. At 14 s the root crashes: measure the time until "learning → forwarding" on s1 (counter `stp.last_change_ms`). Expected about 10.8 s: the remaining age of the information plus two forward delays. Experiments:

- `hello=200ms maxage=1s fwd=500ms`: predict the outage (under 2 s). Then, in the lab, `:` and `link s1 s4 loss=0.3`: with what probability are five consecutive BPDUs lost, and what happens when they are?
- `:` then `s1 stp cost p1 500`: predict the new root port of s1 and which port of the ring blocks.
- `rapid=1`: predict the gain. Here there is none, because the port that must open after the crash is a designated port of the new root, not a root port.

**340-dns-ttl.** `./netlab run 340-dns-ttl`. In the log: the first `dig shop.example.com` costs one upstream query and about 68 ms and returns the chain shop → www → host → A; the second lookup of `ghost` is answered from the negative cache in 4 ms; `www` at 7 s is answered from the cache with the TTL decremented to 55. Experiments: `ttl=3` (the entry has expired at 7 s: one more upstream query); `negttl=1` (the second NXDOMAIN goes upstream); in the lab, `:` then `r dns flush` and `h1 dig www.example.com server=r`.

## Check yourself

- For a channel with independent errors at BER p, estimate the frame failure probability of Hamming(7,4) and of RS(255,223) for 1500-byte frames, and find roughly the p at which RS starts to fail. Why does RS discard where Hamming delivers wrong data?
- An interleaver of depth D precedes a single-error-correcting code of block length n. What burst lengths are fully corrected, and what are the costs?
- Derive S = G·e^(−2G) and its maximum. Explain why operating at G above the maximum is unstable without backoff.
- Using the non-persistent CSMA formula, show the limit of S for G → ∞ as a → 0, and explain physically why CSMA/CD is insensitive to overload while CSMA is not.
- A network uses hello 2 s, max age 20 s, forward delay 15 s. Give the worst-case recovery time after the failure of the root and after the failure of a link adjacent to a blocked port. Why is it dangerous to shorten forward delay alone?
