<!-- level: 2 -->
<!-- labs: 230-rip-convergence, 231-rip-count-to-infinity -->
# Distance-vector routing

Static routes do not scale and do not heal. A routing protocol lets routers build their tables by talking to each other. The **distance-vector** approach is the simplest: each router tells its neighbours, periodically, the list of networks it can reach and at what distance. Nobody ever sees the topology; a router knows only "network X is 3 hops away in the direction of neighbour B".

The rule for using what you hear is one line. If neighbour B says "I reach X at distance d", then I reach X at distance d plus the cost of my link to B. Among all neighbours I keep the smallest offer and remember who made it: that neighbour is my next hop. Repeated by everybody, this is the distributed **Bellman-Ford** algorithm, and it converges to the shortest paths.

Good news spreads fast: a new network is known n hops away after n exchanges. Bad news is the weak point. When a network disappears, a router can be told about a route that actually passed through itself, believe it, and advertise it back. The two routers then raise the distance step by step until it reaches a value agreed to mean infinity. This is **count to infinity**; RIP, the classic distance-vector protocol, sets infinity to 16 and adds three remedies: split horizon, poisoned reverse and triggered updates.

<!-- detail: 2 -->
## Bellman-Ford

Let c(x,v) be the cost of the link from x to neighbour v and D_v(y) the distance from v to y as advertised by v. Then:

```
D_x(y) = min over neighbours v of [ c(x,v) + D_v(y) ]
```

Each router stores its own distance vector and next hops, recomputes when a neighbour's vector or a link cost changes, and advertises its vector when it has changed (and periodically anyway).

## RIP in detail

RIP version 2 (RFC 2453) runs over UDP port 520 and sends responses to the multicast address 224.0.0.9. A message has a 4-byte header (command: 1 = request, 2 = response; version; two zero bytes) followed by up to 25 route entries of 20 bytes each:

- address family (2 bytes) and route tag (2 bytes)
- IP address (4), subnet mask (4), next hop (4)
- metric (4): 1 to 15, and 16 = unreachable

The cost of every link is 1, so the metric is a hop count and no path may exceed 15 hops. Three timers per RFC: **update** every 30 s (with a small random jitter), **timeout** 180 s (a route not refreshed is marked unreachable), **garbage collection** 120 s (it is then advertised with metric 16 before being deleted). In netlab the update period is a parameter, timeout is 6 updates and garbage collection 4.

On receiving an entry with metric m from neighbour N, the router computes m' = min(m + 1, 16) and: installs the route if the destination is new and m' < 16; replaces its route if m' is smaller; and, crucially, **accepts m' even if larger when N is the current next hop**, because the neighbour it relies on knows best.

## Count to infinity, with numbers

Lab 231 is a line `h1 - r1 - r2 - r3 - h3`, updates every 2 s, no protections. (netlab displays the stored metric with 0 for attached networks; on the wire the advertised value is one more, as in the RFC.) Before the failure r2 reaches h3's network at 1 and r1 at 2.

- 12.0 s: link r2-r3 fails. r2 drops its route.
- 12.5 s: r1's periodic update still says "I reach it" (stored metric 2): r2 installs it via r1 with metric 3. r1 points to r2 and r2 points to r1: a loop.
- 13.5 s: r2's update carries its new, worse route; r1, whose next hop is r2, must accept: 2 → 4.
- 14.5 s: r2 goes 3 → 5; then r1 4 → 6, and so on, one step per second.
- 25.5 s: r1 reaches 16; 26.5 s: r2 too. The route is finally gone.

For thirteen seconds packets to h3 bounce between r1 and r2 until their TTL expires. With the real 30-second timer the same count would last several minutes.

## The remedies

- **Split horizon**: never advertise a route on the interface through which it was learned. r1 says nothing about h3's network to r2, so r2 has nobody to believe.
- **Poisoned reverse**: advertise it there, but with metric 16. Stronger, because it actively breaks a loop instead of waiting for a timeout, at the price of bigger messages.
- **Triggered updates**: when a metric changes, send the changed entries immediately instead of waiting for the timer. Bad news then travels in milliseconds.

<!-- detail: 3 -->
## In depth

**What split horizon cannot do.** It only removes loops between two neighbours. With three routers in a triangle, A can still learn from C a route that C learned from B that B learned from A. Counting to infinity then happens around the loop; triggered updates make it fast, and the small value of infinity makes it short, but it is not prevented. Whether a loop forms at all is a race between the bad news and a stale periodic update, which is why lab 231 fixes the phase of the timers to reproduce it reliably.

**Hold-down.** Some implementations (IGRP, Cisco's RIP) refuse any new route for a destination for some time after it was lost, to let the bad news reach everyone. It prevents loops but delays legitimate alternatives.

**Synchronisation.** Routers that start together, or that reset their timers on a triggered update, drift into sending their periodic updates at the same instant, causing periodic bursts and loss. Floyd and Jacobson analysed this in "The Synchronization of Periodic Routing Messages" (1994); the RFC's random jitter is the cure. Triggered updates are themselves rate-limited (1 to 5 s) to avoid storms.

**Limits of RIP.** Hop count ignores bandwidth; 15 hops limits the diameter; convergence after a failure with no alternative path relies on the 180 s timeout if the failure is not detected locally; every router sends its whole table every 30 s. RIPv1 (RFC 1058) carried no masks and was classful; RIPv2 adds masks, next hop, multicast and authentication (RFC 4822); RIPng (RFC 2080) is the IPv6 version.

**Successors.** EIGRP keeps distance vectors but guarantees loop freedom with the DUAL algorithm (Garcia-Luna-Aceves, 1993): a neighbour may be used only if its own distance is smaller than ours ever was. BGP is a *path-vector* protocol: it advertises the whole list of autonomous systems, so a router rejects any route that contains itself. Babel (RFC 8966) is a modern distance-vector protocol for wireless meshes, with sequence numbers to avoid loops.

**Origins.** Bellman (1958) and Ford and Fulkerson (1962) gave the algorithm; the original ARPANET routing of 1969 was distance-vector with delay as the metric, and its slow reaction to bad news was the reason for the move to link state in 1979.

**What netlab simplifies.** Real message format; no authentication and no route tags.

<!-- detail: 1 -->
## In the lab

**Lab 230: `./netlab run 230-rip-convergence`**

- Select r1 (`n`) and open the Routes tab and the `rip` tab: within the first tenth of a second neighbours' networks arrive at metric 1, then h3's network (10.0.6.0/24) at metric 2 via r2.
- In the packets view (`p`) press Enter on a RIP response: read the header and the 20-byte entries. Compare a periodic update with a triggered one ("only the routes that changed").
- At 16 s traceroute shows r1, r2, r3. At 20 s the link r1-r2 fails: metric 16 in the log, triggered updates, and at 45 s traceroute goes through r4.
- Predict, then try: `./netlab run 230-rip-convergence triggered=0`. When does r3 learn that r1's network is gone through r2, and when does r1 get a new route to h3's network? Compare the timestamps with the default run.
- Predict, then try: `./netlab run 230-rip-convergence update=10s`. Which events are delayed and which are not? Then type `:link r1 r2 up` after 45 s and watch the shorter path return.

**Lab 231: `./netlab run 231-rip-count-to-infinity`**

- Set detail 2 and follow the log from 12 s: the metric of 10.0.4.0/24 climbs alternately on r1 and r2. Keep the Routes tab of r1 open to see next hop and metric change.
- At 17 s the ping from h1 reports `time to live exceeded` from 10.0.2.2: open the packets view to see the echo request bouncing.
- Predict, then try: `./netlab run 231-rip-count-to-infinity split=1`. What does r1 advertise to r2 at 12.5 s? What does the ping report, and who sends the error? (The lab's checks know the setting: with any protection on they verify that nobody counts and no packet loops.) Then try `triggered=1` alone: why is that already enough in this small network, and why would you not rely on it?
- Predict, then try: `split=1 poison=1` and open a RIP response from r1 to r2: which entries have metric 16? Then `split=0 triggered=1`: does the loop still form? Why is this a race and not a guarantee?

## Check yourself

- State the Bellman-Ford equation and apply it: router A has neighbours B (link cost 1) and C (link cost 4). B advertises distance 5 to network N, C advertises 1. What does A store? What if B later advertises 7?
- Describe the RIPv2 route entry field by field. Why can a response carry at most 25 routes?
- Three routers in a line A-B-C; C's network fails. Show, update by update, how count to infinity develops without split horizon, and show why it does not happen with it.
- Explain the difference between split horizon and poisoned reverse, and give a topology in which neither prevents count to infinity.
- Why must a router accept a worse metric from its current next hop, although it ignores worse offers from other neighbours?
