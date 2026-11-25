<!-- level: 2 -->
<!-- labs: 232-ospf-costs -->
# Link-state routing

A distance-vector router believes what its neighbours say about distant networks: hearsay. A **link-state** router asks every router for facts only: "who are your neighbours, and what does each link cost?". These facts are spread unchanged to the whole network, so every router ends up with the same complete **map**. Each router then computes the shortest paths on the map by itself, with Dijkstra's algorithm, and installs the first hop of each path in its routing table.

The protocol has three jobs. **Hello** packets discover neighbours and detect when they disappear. Each router describes its own links in a **link-state advertisement** (LSA), and **flooding** delivers every LSA reliably to every router. The collection of LSAs is the **link-state database**: the map.

The benefits follow from having the map. A change is flooded at once and everybody recomputes from facts, so there is no count to infinity and convergence takes a fraction of a second. Costs can be any positive number, so the administrator can make traffic prefer fast links. The price is more memory, more computation and a more complex protocol. OSPF and IS-IS, the protocols that run inside almost every large network, are link-state.

<!-- detail: 2 -->
## Neighbours: Hello

OSPF (RFC 2328) runs directly over IP, protocol number 89, and sends Hellos to the multicast address 224.0.0.5 every *hello interval* (10 s by default on LANs). A Hello carries the sender's router identifier, the intervals, and the list of neighbours recently heard. Seeing *your own* identifier in a neighbour's Hello proves the link works in both directions: the state goes from **Init** to **2-Way**. A neighbour silent for the *dead interval* (4 hellos, 40 s) is declared down. Both intervals must match or no adjacency forms.

Real OSPF then synchronises databases through the states ExStart, Exchange and Loading (database description, link-state request, link-state update packets) and reaches **Full**. hoplab skips the description exchange: a new adjacency goes straight to Full and each side sends its whole database.

## LSAs and flooding

A router-LSA lists the links of its originator: for each, the neighbour (or attached network) and the cost. Its header identifies it by advertising router and carries a **sequence number** and an age. Flooding rule, on receiving an LSA from a neighbour:

- If it is **newer** (higher sequence number) than the stored copy, or unknown: store it, acknowledge it, send it to all other neighbours, and schedule a shortest-path computation.
- If it is the same: just acknowledge (it is a duplicate arriving by another path).
- If it is older: reply with the newer copy.

Each LSA sent is retransmitted until acknowledged, so flooding is reliable hop by hop. When a link or cost changes, the router issues a new LSA with the sequence number increased by one.

## Dijkstra on the lab's map

In lab 232 the cost of an interface is 100 Mbit/s divided by its bandwidth: 1 for the 100 Mbit/s links r1-r2 and r2-r4, 10 for the 10 Mbit/s links r1-r3 and r3-r4. Dijkstra from r1 keeps a set of *finished* nodes and tentative distances for the others, and repeatedly finishes the closest tentative node:

```
step  finished          tentative distances
 0    r1 (0)            r2 = 1 via r2,  r3 = 10 via r3
 1    + r2 (1)          r4 = 1 + 1 = 2 via r2,  r3 = 10
 2    + r4 (2)          r3 = min(10, 2 + 10) = 10
 3    + r3 (10)         done
```

h4's network hangs off r4 at cost 1, so r1 reaches it at cost 3 with next hop r2; the lower path would cost 10 + 10 + 1 = 21. The log shows exactly these metrics.

At 20 s the cost of r1's interface towards r2 becomes 50. r1 floods a new LSA; 200 ms later every router has run SPF again. For r1 the upper path now costs 52, so it uses r3 (21). But a cost belongs to an *outgoing interface*: r2's cost towards r1 is still 1, so r4 keeps answering through r2. Forward and return paths differ.

## Compared with distance vector

- **Information**: full topology, flooded, against neighbours' distances only.
- **Bad news**: as fast as good news; no count to infinity.
- **Messages**: small and event-driven, against the whole table periodically.
- **Cost**: database memory and O((N + E) log N) computation per change.
- **Loops**: only transient, while routers have not all recomputed.

<!-- detail: 3 -->
## In depth

**Scaling.** Every change makes every router recompute, and the database grows with the network. OSPF divides a large domain into **areas**: routers know the full map of their area only, and area border routers summarise prefixes into the backbone (area 0), which behaves like distance vector between areas. On a broadcast LAN with n routers, n(n−1)/2 adjacencies would be wasteful: the routers elect a **designated router**, everyone synchronises with it only, and it originates a network-LSA representing the LAN. hoplab has one area and no designated router.

**Ageing and sequence numbers.** Each LSA has an age; the originator refreshes it every 30 minutes, and at 1 hour it is purged, so that the information of a dead router eventually disappears. Sequence numbers are linear 32-bit values starting at 0x80000001. The ARPANET used circular numbers, and in 1980 corrupted updates that each looked newer than the next kept flooding for ever and brought the network down (RFC 789, Rosen): the reason modern LSAs carry a checksum and an age. In hoplab LSAs do not age.

**Speed of convergence.** Detection is the slow part: with timers alone it takes the dead interval. Routers therefore react to loss of carrier, or run BFD (RFC 5880) with tens of milliseconds. SPF is delayed and throttled (200 ms in hoplab) so that a burst of LSAs causes one computation; this trades a little speed for stability. Between the first and the last router's update, micro-loops are possible; loop-free alternates (RFC 5286) precompute backup next hops.

**Costs.** The reference bandwidth of 100 Mbit/s makes every faster link cost 1, so operators raise it or set costs by hand. Making the cost follow the *load* was tried on the ARPANET and caused oscillations: traffic moved to the empty path, which then became the loaded one (Khanna and Zinky, "The Revised ARPANET Routing Metric", 1989). Equal-cost paths are used together (ECMP).

**Security and relatives.** A single router can lie about its links and attract traffic; OSPF supports cryptographic authentication of packets. IS-IS (ISO 10589, RFC 1195) is the same idea running directly over layer 2, preferred by many carriers. OSPFv3 (RFC 5340) carries IPv6.

**References.** Dijkstra, "A Note on Two Problems in Connexion with Graphs" (1959); McQuillan, Richer and Rosen, "The New Routing Algorithm for the ARPANET" (1980); RFC 2328.

**What hoplab simplifies.** Besides the points above, packet layouts are OSPF-like but not bit-exact.

<!-- detail: 1 -->
## In the lab

**Lab 232: `./hoplab run 232-ospf-costs`**

- Slow down (`-`) for the first 50 ms: Hello, `new neighbor, state Init`, `two-way, adjacency Full`, then LSAs `newer: stored and flooded on`. Note how each router re-originates its LSA (seq=1, 2, 3) as it finds neighbours.
- Select r1 and then r4 (`n`) and open the `ospf` tab: the two databases list the same four LSAs with the same sequence numbers. Open the Routes tab of r1: 10.0.6.0/24 via 10.0.2.2, metric 3.
- In the packets view (`p`) open a Hello (find the neighbour list) and a link-state update (find links and costs).
- At 14 s traceroute goes r1, r2, r4. At 20 s the cost changes: r1's route moves to 10.0.3.2 with metric 21. Check the Routes tab of r4: its route to 10.0.1.0/24 still goes via r2.
- At 32 s the link r1-r2 fails; both ends originate new LSAs and within about 0.2 s all routes use r3.
- Predict, then try: `./hoplab run 232-ospf-costs newcost=15`. Compute the cost of both paths first. Does r1 change next hop at 20 s? Which metric appears in its table?
- Predict, then try: in the default run, after 21 s type `:r1 ospf cost eth1 1` and watch the sequence number of r1's LSA and the route. Then, after 33 s, `:link r3 r4 down`: what do the tables of r1 and r4 contain now, and what does `:h1 ping h4 count=1` report?

## Check yourself

- Run Dijkstra by hand from node A on this graph and give the routing table of A: A-B 2, A-C 5, B-C 1, B-D 4, C-D 1, D-E 3.
- What are the three rules a router applies when it receives an LSA, and why does flooding terminate even though the topology has loops?
- How does a router know that a link is bidirectional before using it? What is the dead interval for?
- In lab 232, after the cost change, requests from h1 to h4 go through r3 but replies come back through r2. Explain why, and say whether this is an error.
- Compare link state and distance vector on: information exchanged, reaction to a link failure, computation and memory, and types of loops.
