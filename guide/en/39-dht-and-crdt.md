<!-- level: 3 -->
<!-- labs: 393-chord-ring, 394-crdt-counter -->
# Distributed hash tables and CRDTs: scaling without coordination

Consensus and atomic commit buy strong guarantees with coordination: quorums, leaders, rounds of messages. Two families of techniques go the other way and ask how far one can get with none. They answer two different questions.

**Where is the data?** A hash table spread over thousands of machines that join and leave needs a rule mapping each key to a machine. A central directory is a bottleneck and a single point of failure; `hash(key) mod N` remaps almost every key whenever `N` changes. **Consistent hashing** places nodes and keys on the same circle of identifiers and gives each key to the first node clockwise: a join or a leave moves only the keys of one arc. A **distributed hash table** such as Chord adds routing: each node knows only `O(log N)` others, chosen at exponentially growing distances, and any key is found in `O(log N)` hops. The hard part is churn: the ring must stay correct while nodes come and go, with no one in charge.

**What is the value?** If replicas accept updates independently (during a partition, or just for speed), they diverge, and reconciling arbitrary divergent states is an application problem that is often solved by losing data. A **conflict-free replicated data type** is designed so that this cannot happen: its states form a lattice and merging is a least upper bound, so replicas that have seen the same updates hold the same value, whatever the order, duplication or delay of the messages.

<!-- detail: 2 -->
## Algorithms and models

**Consistent hashing.** Identifiers are `m`-bit numbers on a circle modulo `2^m`; nodes and keys are hashed onto it; `successor(k)` is the first node whose identifier is equal to or follows `k`. With `N` nodes and `K` keys, a node holds `K/N` keys on average, and a join or leave moves `O(K/N)` keys. With one point per node the arcs are uneven (the longest is about `log N` times the mean); `v` virtual nodes per machine smooth the load.

**Chord.** Node `n` keeps its successor, its predecessor, a list of `r` successors and a finger table:

```
finger[i] = successor(n + 2^i)          i = 0 .. m-1

n.find_successor(id):
    if id in (n, successor]:  return successor
    n' = closest_preceding_node(id)     # furthest finger inside (n, id)
    return n'.find_successor(id)
```

Each hop at least halves the remaining clockwise distance to `id`, so a lookup takes `O(log N)` hops with high probability, about `(1/2)*log2 N` on average, with `O(log N)` state per node.

**Stabilisation.** Correctness needs only successor pointers; fingers are an optimisation. Joins and failures are absorbed by periodic tasks:

```
n.join(n0):        predecessor = nil;  successor = n0.find_successor(n)
n.stabilize():     x = successor.predecessor
                   if x in (n, successor):  successor = x
                   successor.notify(n)
n.notify(n'):      if predecessor is nil or n' in (predecessor, n):
                       predecessor = n'
n.fix_fingers():   refresh one finger[i] = find_successor(n + 2^i)
```

If the successor fails, the node takes the next live entry of its successor list; with `r = O(log N)` the ring survives the failure of half the nodes with high probability. Every pair exists in `r` copies, on its owner and on the owner's next `r - 1` successors, which are exactly the nodes that will inherit the arc; when its neighbourhood changes a node copies the keys it owns again, restoring the `r` copies.

**Worked example (lab 393, 8 bits).** The ten nodes hash to 0 (n7), 1 (n1), 129 (n3), 141 (n5), 145 (n6), 172 (n8), 198 (n10), 208 (n4), 239 (n9), 247 (n2). The key `apple` hashes to 248: its owner is `successor(248)` = n7 at 0, across the wrap. From n3 (129) the fingers point at the successors of 130, 131, 133, 137, 145, 161, 193 and 1; the furthest one before 248 is n10 (198). n10 forwards to n2 (247), whose successor n7 covers 248: two hops. Note the arc (1, 129]: half the ring belongs to n3, which is why `cherry` (90) and `plum` (28) both land there.

**State-based CRDTs.** A set of states `S` with a merge `⊔` that is

```
commutative:  a ⊔ b = b ⊔ a
associative:  (a ⊔ b) ⊔ c = a ⊔ (b ⊔ c)
idempotent:   a ⊔ a = a
```

is a **join semilattice**; `a <= b` means `a ⊔ b = b`. Every update must be an inflation (`s <= update(s)`). Replicas send their state to others at any time; the receiver merges. **Strong eventual consistency**: any two replicas that have received the same set of updates are in the same state, with no rollback and no consensus.

```
G-Counter:   state P[1..N];   inc at replica i: P[i] += 1
             value = sum(P);  merge = element-wise max
PN-Counter:  two G-Counters P, N;  dec at i: N[i] += 1
             value = sum(P) - sum(N)
```

**Worked example (lab 394).** Concurrently n1 adds 2, n2 adds 1, n3 adds 3: `P` is `[2,0,0]`, `[0,1,0]`, `[0,0,3]`. When n3 merges n2's state it holds `[0,1,3]` = 4; after all merges `[2,1,3]` = 6 everywhere. A decrement at n2 gives `N = [0,1,0]` and the value 5. A single shared integer merged by maximum would give 3, merged by sum would count twice at every re-delivery.

<!-- detail: 3 -->
## In depth

**Chord: invariants and their fragility.** The ideal state is a single cycle of successor pointers in identifier order. Lookups are correct if successors are correct, and merely slow if fingers are stale. The original paper claimed that stabilisation maintains the ring under joins and failures; Zave's formal analysis (2012) showed that none of the claimed invariants holds for the published protocol, with counterexamples of a few nodes, and gave a corrected version later. In practice the failure modes are: lookups that return the wrong owner while a join is being absorbed (the data is then "not found" although it exists); loss of an arc when a node and its whole successor list fail together; rings that split after a partition and do not merge by themselves; high churn, where timeouts on dead fingers dominate latency. Open membership adds Sybil and eclipse attacks. Overlay hops ignore network distance, so `O(log N)` hops can mean seconds.

**What deployed systems do.** The DHT that survived in the open Internet is Kademlia (BitTorrent, IPFS, Ethereum discovery): XOR distance makes routing tables symmetric, so lookups refresh them for free, and lookups are iterative and parallel. Inside a data centre membership is small and trusted, so Dynamo, Cassandra and Riak keep the full ring at every node (a zero-hop DHT, spread by gossip) with virtual nodes and replication on the next `r` distinct machines.

**CRDTs: why convergence holds and what it costs.** The state of a replica is the join of the updates it has received; a join is independent of order and multiplicity, so equal sets of updates give equal states. The transport may lose, duplicate and reorder, as long as updates eventually get through. Operation-based CRDTs send operations instead of states and require concurrent operations to commute, but then need reliable, exactly-once, causally ordered delivery; delta CRDTs ship only the recent part of the state. The costs are metadata that grows with the number of replicas ever seen and, for sets and sequences, tombstones whose garbage collection needs coordination. The fundamental limit is expressiveness: a CRDT cannot enforce an invariant over replicas, such as a counter that never goes below zero or a unique username. The CALM theorem makes this precise: exactly the monotone programs have coordination-free, consistent implementations. And convergence is not intention: a last-writer-wins register converges by discarding a concurrent write.

**In hoplab.** `chord`: lookups are recursive, liveness is checked by pings at each stabilisation round, identifiers are a hash of the node name (with few bits two names collide: the collision is detected and one node moves to the next identifier), a restarted node has lost its state and joins as new, `succlist` defaults to 3. `crdt`: state-based; each replica sends its whole state to one random peer every `sync` and answers a peer whose state differs; `type` selects the data types a replica hosts, and `crdt.converged` tells when the last state heard from every peer equalled its own.

**Primary literature.**

- D. Karger et al., "Consistent Hashing and Random Trees", STOC 1997.
- I. Stoica, R. Morris, D. Karger, M. F. Kaashoek, H. Balakrishnan, "Chord: A Scalable Peer-to-Peer Lookup Service for Internet Applications", SIGCOMM 2001.
- P. Maymounkov, D. Mazières, "Kademlia: A Peer-to-Peer Information System Based on the XOR Metric", 2002.
- S. Rhea, D. Geels, T. Roscoe, J. Kubiatowicz, "Handling Churn in a DHT", 2004.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-Value Store", SOSP 2007.
- P. Zave, "Using Lightweight Modeling to Understand Chord", 2012.
- D. Terry et al., "Managing Update Conflicts in Bayou, a Weakly Connected Replicated Storage System", SOSP 1995.
- M. Shapiro, N. Preguiça, C. Baquero, M. Zawirski, "Conflict-Free Replicated Data Types", SSS 2011.
- P. Almeida, A. Shoker, C. Baquero, "Delta State Replicated Data Types", 2018.
- J. Hellerstein, P. Alvaro, "Keeping CALM: When Distributed Consistency Is Easy", 2020.

<!-- detail: 1 -->
## In the lab

**393-chord-ring.** `./hoplab run 393-chord-ring`. n1 creates the ring and nine nodes join through it at once. Select a node and open its `chord` tab: identifier, successor, predecessor, fingers, stored keys. In the first half second every node has n1 as successor; watch the lines "new successor" and "new predecessor" as stabilisation (every 400 ms) sorts the ring. Before 12 s, draw the ring from the identifiers and compute by hand the owner of 248, 173, 90 and 28. At 12 s n3 stores four keys: the log shows each `find_successor(…) → forwarded to …`, the owner, the hop count, and three "stored a copy" lines on the owner's successors. At 15 s other nodes read them; `mango` is not found. The graph `lookup hops` and the counter `chord.hops` give the path lengths: compare with `(1/2)*log2 10`, about 1.7.

- In a running lab, before the ring is in order (pause at about 1 s), type `n3 chord put key=early value=1` and `n6 chord get key=early`. Prediction: the key is stored on a node that is not its final owner and an early read may return "not found", although no node failed; a few seconds later the same read succeeds, because stabilisation hands the key to its owner. Explain which Chord property was missing at the time of the put. (The scripted keys are stored at 12 s: keep `stabilize` at 1 s or below, or the ring is not ready by then.)
- `:` then `node n7 down`, wait two stabilisation rounds, then `n6 chord get key=apple`. Prediction: n2 logs "my successor is now n1" and the get is answered by n1 from its replica.
- `bits=5` or `bits=4`: a ring small enough to check by hand. Two names hash to the same identifier: read the "identifier … is also the identifier of" lines and the counter `chord.collisions`, and say what a real system does about collisions (160-bit identifiers make them negligible).
- `:` then `n4 chord lookup key=banana` from several nodes: record the hops and identify which finger was used at each step.

**394-crdt-counter.** `./hoplab run 394-crdt-counter`. Three replicas of a PN-Counter, gossiping the whole state to one random peer every 500 ms over links that lose 20% of the packets. Open the `crdt` tab of each replica: the state is a pair (increments, decrements) per replica; the value is the sum. At 2 s the three concurrent increments make the replicas disagree (2, 1, 3); follow the "merge with …: counter a → b" lines until all show 6, then the decrement at 6 s until all show 5. Count the merges needed (`crdt.merges`) and the state messages lost.

- `loss=0.6`: predict the outcome (the same value, later). `sync=2s`: convergence time scales with the period.
- Partition: `:` then `link n3 s1 down`; `n3 crdt inc by=10`; `n1 crdt dec by=4`. Read the three values, predict the final one, then `link n3 s1 up`.
- Thought experiment: with `type=gcounter` which of the lab's operations becomes impossible, and why is "merge = max of the values" wrong for a counter but right for each entry?

## Check yourself

- Prove that a Chord lookup takes O(log N) hops with high probability, and say what the lookup costs, and whether it is still correct, when all fingers are stale but successors are right.
- Trace stabilize and notify for a node joining between two adjacent nodes, and show a moment at which a lookup for a key in the new node's arc returns the wrong owner.
- Why does consistent hashing with one point per node give uneven load, and how do virtual nodes change the mean and the variance of the arc owned by a machine?
- Show that the G-Counter merge is a least upper bound, and that a counter state consisting of a single integer admits no merge that is both idempotent and correct under concurrent increments.
- Give an invariant that no CRDT can maintain without coordination and relate it to monotonicity. Why does a last-writer-wins register satisfy strong eventual consistency and still lose updates?
