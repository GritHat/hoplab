<!-- level: 3 -->
<!-- labs: 330-ospf-vs-rip, 331-bgp-basics -->
# Routing convergence and interdomain routing

A routing protocol is a distributed algorithm that must reach a consistent set of forwarding tables, and reach it again after every change. Between a failure and the moment all routers agree, tables are mutually inconsistent: packets are dropped at a router that has no route (a black hole) or circulate between routers that each believe the other is closer (a transient loop). **Convergence time** is therefore the quantity that matters operationally, far more than the optimality of the paths.

It is hard because no router sees the network: each one acts on information that is partial, delayed and possibly already false. A link-state router floods facts (my links, their costs) and everyone computes on the same map; bad news travels as fast as good news. A distance-vector router exchanges conclusions (my distance to X), and a conclusion may silently depend on the router that receives it: hence slow recovery and counting to infinity.

Between autonomous systems the problem changes nature. There is no common metric, no AS will reveal its topology, and each wants to apply its own commercial policy. BGP answers with a **path vector**: every route carries the list of ASes it has crossed, which gives loop freedom without a global map and gives policy something to act on. The price is that convergence is slow, and not even guaranteed in general.

<!-- detail: 2 -->
## Algorithms and models

**Anatomy of convergence.** For any protocol:

```
T_conv = T_detect + T_propagate + T_compute + T_install
```

Detection is either physical (loss of carrier, milliseconds) or by timeout (OSPF dead interval = 4 hellos; BGP hold time = 3 keepalives; RIP route timeout = 6 updates).

**Link state.** Each router originates an LSA (its neighbours and costs) with a sequence number; flooding is reliable: a newer LSA is stored, acknowledged and forwarded on all other interfaces. After a hold-down that batches related changes, each router runs Dijkstra on the database, in `O((V + E) log V)`. Then `T_propagate` is a few link delays and `T_compute` is dominated by the SPF hold-down.

**Distance vector.** Each router keeps, per destination, a distance and a next hop, and applies Bellman-Ford to what the neighbours advertise:

```
D_x(d) = min over neighbours v of ( c(x,v) + D_v(d) )
```

RIP remembers only the best offer. When that route dies, an alternative that had been rejected is gone from memory and returns only with the neighbour's next periodic update: `T_propagate` is uniform in `(0, update)` per hop. Without split horizon a router may accept its own route back from a neighbour and count upwards to 16. Split horizon, poisoned reverse and triggered updates remove the two-node case only.

**Path vector.** A BGP route is a prefix plus attributes; AS_PATH is extended at each export, and a router discards a route whose AS_PATH contains its own AS. Among several routes for a prefix the decision process is a lexicographic comparison:

```
1. highest LOCAL_PREF            (policy: customer > peer > provider)
2. shortest AS_PATH
3. lowest ORIGIN
4. lowest MED                    (only among routes from the same AS)
5. eBGP-learned over iBGP-learned
6. lowest IGP cost to NEXT_HOP   (hot-potato routing)
7. lowest router ID              (tie-break)
```

Only the best route is exported, subject to export policy (routes from customers to everyone; routes from peers and providers only to customers). A route that disappears is announced by an explicit **withdrawal**.

**Path exploration.** After a withdrawal a router does not know that the destination is gone: it selects the next best of the routes it still holds, which may be stale and may pass through the very failure, and advertises it. Neighbours do the same. The system walks through progressively longer obsolete paths before all are eliminated; each step is delayed by the minimum route advertisement interval (MRAI, 30 s in practice). In a clique of `n` ASes the worst case explores a number of paths factorial in `n`.

**Worked example (lab 330).** 50 datagrams per second cross r1–r2–r3; the link r2–r3 fails at 20.000 s. With OSPF, r2 sees the carrier drop at 20.044, its LSA reaches every router by 20.056, SPF runs after the 200 ms hold-down and the detour is installed at 20.22: about 11 datagrams lost. With RIP, r1 hears "unreachable" within 0.2 s, but the detour depends on r4's next periodic update, uniform in 0–5 s: on average 2.5 s, about 125 datagrams, with a maximum near 250.

<!-- detail: 3 -->
## In depth

**Correctness.** Link state is correct once databases are equal: every router runs the same deterministic computation on the same graph, so next hops are consistent. During flooding they are not, and micro-loops are possible; remedies are ordered FIB updates and precomputed loop-free alternates (RFC 5286). Distance vector converges for static costs (asynchronous Bellman-Ford), but after a cost increase stale distances can be propagated around a cycle. DUAL (EIGRP) keeps the feasibility invariant "accept a neighbour only if its reported distance is smaller than my best distance ever" and is loop-free at every instant, at the cost of a diffusing computation.

**BGP need not converge.** Each AS ranks paths by private preferences; the resulting Stable Paths Problem may have no solution (the BAD GADGET oscillates for ever), several solutions, or solutions reached only by luck, and deciding which is NP-complete. Gao and Rexford showed that if every AS prefers customer routes and exports valley-free, with no cycle of provider relationships, a unique stable state exists and is always reached: economics, not the protocol, keeps the Internet stable. Path-vector loop detection prevents counting to infinity but not path exploration: Labovitz et al. measured minutes of convergence after a withdrawal.

**Trade-offs.** Short timers detect faster but turn congestion into false failures and flapping; SPF and MRAI delays damp the churn at the price of blackout time. Deployed networks detect with BFD (RFC 5880) in tens of milliseconds, throttle SPF with exponential back-off, and protect with fast reroute; BGP adds route-flap damping (RFC 2439), which can itself delay convergence, and route reflectors and iBGP inside each AS.

**In hoplab.** `ospf`: one area, no designated router, a new adjacency sends its whole database instead of the database-description exchange, LSAs do not age, packet formats are not bit-exact. `rip`: RIPv2 format on UDP 520, no authentication. `bgp`: sessions over TCP port 179 with the RFC 4271 messages and state machine, eBGP only, one router per AS, no MED, communities or aggregation; the decision process is LOCAL_PREF from the relation, AS_PATH length, router ID; MRAI defaults to 500 ms.

**Primary literature.**

- R. Bellman, "On a Routing Problem", 1958; E. W. Dijkstra, "A Note on Two Problems in Connexion with Graphs", 1959.
- J. McQuillan, I. Richer, E. Rosen, "The New Routing Algorithm for the ARPANET", 1980.
- J. J. Garcia-Luna-Aceves, "Loop-Free Routing Using Diffusing Computations", 1993.
- C. Labovitz, A. Ahuja, A. Bose, F. Jahanian, "Delayed Internet Routing Convergence", SIGCOMM 2000.
- L. Gao, J. Rexford, "Stable Internet Routing Without Global Coordination", 2001.
- T. Griffin, F. B. Shepherd, G. Wilfong, "The Stable Paths Problem and Interdomain Routing", 2002.
- RFC 2453 (RIPv2), RFC 2328 (OSPFv2), RFC 4271 (BGP-4).

<!-- detail: 1 -->
## In the lab

**330-ospf-vs-rip.** `./hoplab run 330-ospf-vs-rip`, then `./hoplab run 330-ospf-vs-rip proto=rip`. A 50 datagram/s stream runs from h1 to h3 from 12 s; at 20 s the link r2–r3 goes down. Select h3 and open its `sink` tab: the `lost` count divided by 50 is the blackout in seconds (the last lines of the log give the same number as `check lost`). In the log (`l`, detail `2`) follow, for OSPF, "interface down", "originating router-LSA", "stored and flooded on", "SPF #2"; for RIP, "is now unreachable", "triggered update", and then the wait. The traceroute at 58 s must show 5 hops.

- Repeat RIP with several seeds (`--seed 1`, `--seed 2`, …). Prediction: the loss is spread between a few and about 250 datagrams, because it depends on the phase of r4's timer; OSPF stays near 11. The lab's check `blackout` states the bound to expect for each setting: at most hello + spf for OSPF, between three and four hellos plus spf with `carrier=0`, up to about one update period for RIP.
- `proto=rip update=15s`: predict the mean and the maximum loss (375 and 750).
- `proto=ospf carrier=0`: detection now waits for the dead interval, 4 hellos of 1 s. Predict about 160 lost. Then `spf=2s`: about 100.
- In a running lab, `:` then `link r2 r3 up`, wait, and `link r2 r3 down` again: the experiment repeats with a different timer phase.

**331-bgp-basics.** `./hoplab run 331-bgp-basics`. Three ASes in a triangle. In the packets view (`p`) follow one session from the TCP handshake to port 179 through OPEN and KEEPALIVE, then open an UPDATE and read the AS_PATH. Select r1 and open its `bgp` tab: 10.3.0.0/24 is known with path `65003` and with `65002 65003`, and the log states the reason for each choice (for instance "only path", "lowest router id"). At 25 s the link r1–r3 fails: the session drops, r1 switches to the stored path via AS 65002, and the traceroute at 37 s shows 4 hops. At 40 s r3 withdraws 10.3.0.0/24 and the last ping is answered "network unreachable" by r1.

- Path exploration in miniature (do it before the link fails at 25 s). `:` then `r3 bgp announce 10.9.0.0/24`; wait a few seconds; `r3 bgp withdraw 10.9.0.0/24`. Prediction: r2 does not lose the route at once; for one MRAI it installs the obsolete path `65001 65003` learned from r1 and even offers `65002 65001 65003` to r3, which rejects it ("our own AS is in the path"); only after r1's withdrawal does the prefix disappear. The scripted withdrawal of 10.3.0.0/24 at 40 s shows no exploration: by then the triangle is broken and r2 has no alternative to fall back on.
- `prepend=2`: r3 exports `65003 65003 65003` towards r1 only (`prepend.r1=`), and plain `65003` towards r2. Predict r1's choice for 10.3.0.0/24 and the first traceroute. r1 now prefers `65002 65003` (two ASes against three) and the traceroute at 15 s shows 4 hops instead of 3: inbound traffic engineering. With `prepend=1` the two paths tie and the lower router id decides. Explain why prepending towards every neighbour would change nothing.
- `keepalive=1s`: the hold time becomes 3 s. Here the failure is detected by the interface going down, so the outage barely changes; say when the hold time would matter.

## Check yourself

- Decompose the convergence time of OSPF and of RIP after the failure of a link on the best path, and identify for each the dominant term and the parameter that controls it. Why is the RIP term a random variable?
- Give a three-router topology and a sequence of events in which split horizon with poisoned reverse does not prevent counting to infinity.
- Prove that the AS_PATH check makes BGP loop-free in the steady state, and explain why it does not bound the number of intermediate states after a withdrawal.
- Two routes to the same prefix differ as follows: A has LOCAL_PREF 100 and AS_PATH length 2; B has LOCAL_PREF 200 and AS_PATH length 5. Which is selected, and what does this imply for AS-path prepending as a traffic-engineering tool?
- State the Gao–Rexford conditions and explain informally why they exclude a BAD GADGET.
