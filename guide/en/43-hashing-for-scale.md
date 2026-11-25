<!-- level: 4 -->
<!-- labs: 450-consistent-hashing -->
# Hashing for scale

Large services place work by hashing: a packet's five-tuple chooses a server, a key chooses a cache node or a storage shard. Hashing needs no shared state, so any number of balancers or clients can make the same decision independently. The difficulty is that the set of servers changes all the time, through failures, deployments and autoscaling, and a naive hash reshuffles nearly everything whenever it does. **How do we map keys to a changing set of servers so that few keys move, load stays even, and the mapping is cheap to compute and to store?**

These three goals, minimal disruption, balance and cost, pull against each other, and no scheme wins on all of them. Consistent hashing gives near-minimal disruption but uneven load unless each server is replicated many times on the ring. Rendezvous hashing is simple and exactly minimal but costs a pass over all servers per lookup. Maglev gets constant-time lookups and very even load by giving up a little disruption. The power of two choices balances far better than any single hash, but needs load information and, by itself, has no affinity.

The question is still open because the requirements keep moving: connection affinity across stateless balancers, bounded load under skewed keys, and heterogeneous or weighted servers. The lab measures disruption directly, by counting the client flows whose server changes.

<!-- detail: 2 -->
## The schemes

**Mod-N.** server = h(key) mod N. Zero memory and perfect expected balance. When N becomes N+1 a key stays put only if h mod N equals h mod (N+1), which happens with probability 1/(N+1): going from 3 to 4 servers moves three quarters of the keys.

**Consistent hashing** (Karger et al. 1997). Servers and keys are hashed onto a circle and each key belongs to the next server clockwise. Adding a server moves only the keys of the arc it takes over; removing one moves only its own keys. With one point per server the arcs are very unequal (the largest is about log N times the mean), so each server is placed at v virtual nodes; the relative spread of the load falls roughly as 1/sqrt(v). Memory is O(N·v) and a lookup is a binary search.

**Rendezvous, or highest-random-weight hashing** (Thaler and Ravishankar 1998). Compute h(key, server) for every server and choose the maximum. Disruption is exactly minimal: when a server leaves, only its keys move, each to its own second choice; when one joins, it takes exactly the keys for which it is now the maximum. No table is needed and weights are easy, but a lookup costs O(N).

**Maglev** (Eisenbud et al. 2016). Each server has a pseudo-random permutation of the M slots of a lookup table (M prime, much larger than N); the servers take turns claiming their next preferred free slot until the table is full. A lookup is one array access, table[h(key) mod M]. Each server owns almost exactly M/N slots, so balance is better than a ring of practical size. When the pool changes the table is rebuilt and a small fraction of slots beyond the minimum changes owner; that fraction shrinks as M grows, which is why production uses M = 65537 or larger.

**Power of two choices** (Azar et al.; Mitzenmacher). Sample two servers at random and send the request to the less loaded. With n items on n servers the maximum load drops from about log n / log log n to about log log n: an exponential improvement for one extra probe. It is a policy for new work, not a mapping: the decision depends on the current load, so two balancers will not agree, and a table of existing flows is required.

## Connection affinity

A layer-4 balancer must send every packet of a TCP connection to the same server. A per-flow table gives this trivially, but the table is local: with ECMP in front of several balancers, a packet may reach a balancer that has never seen the flow, and a restarted balancer has forgotten everything. The production answer is both: a consistent mapping so that independent balancers usually agree, plus a flow table to protect established connections from the residual remapping when the pool changes. Stateless designs go further and let the servers themselves redirect the stray packets of old flows (Beamer, Faild).

<!-- detail: 3 -->
## Open problems

**Bounded load with minimal movement.** Under skewed key popularity a perfectly even split of the key space is not an even split of the load. Consistent hashing with bounded loads (Mirrokni, Thorup and Zadimoghaddam) caps every server at (1+ε) times the mean and bounds the number of moves per update, at the cost of forwarding chains; how to combine it with per-connection affinity and with weights is still engineering by case.

**Affinity without state.** Flow tables cost memory and are a denial-of-service target (a SYN flood fills them). How far one can go with no per-flow state, under continuous pool churn and long-lived connections, remains an active question; QUIC connection migration adds a new twist, because the five-tuple is no longer the connection.

**What to measure.** Disruption can be counted over keys, over flows alive at the moment of the change, or over bytes; these give different rankings when flow sizes are heavy-tailed.

## The lab's model versus reality

The balancer of this emulator (see `./netlab help lb`) hashes a UDP five-tuple and the `reqgen` client cycles over at most **40 flows**, which live for the whole run. That is a tiny sample: with four servers each holds about ten flows, so a difference of three or four flows between schemes is within sampling noise, and the "balance" figures are dominated by where forty particular ports happen to land. Worse, the hash is deterministic and the flows are fixed, so the seed does not change the result: you have one sample, not a distribution. To obtain others you must change the flow population (the `flows` parameter) or write a scenario with other addresses.

The ring uses 100 virtual nodes per server. The Maglev table has **251 slots**, against 65537 in the paper, so both its balance and its disruption are coarser than the published figures. Rendezvous hashing, jump hashing and bounded loads are not implemented. With `affinity=0`, round robin, least connections and `p2c` schedule every packet afresh and the reported "moved" count is meaningless for them; they only make sense with the flow table. Requests are single UDP datagrams, so "a broken connection" is inferred from a change of answering server, not observed as a TCP reset. Finally there is one balancer: the multi-balancer disagreement that motivates the whole design is emulated only by turning the flow table off or by restarting the node.

## Primary literature

- D. Karger, E. Lehman, T. Leighton, R. Panigrahy, M. Levine, D. Lewin, "Consistent Hashing and Random Trees: Distributed Caching Protocols for Relieving Hot Spots on the World Wide Web", ACM STOC, 1997.
- D. Thaler, C. Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", IEEE/ACM Transactions on Networking, 1998.
- Y. Azar, A. Broder, A. Karlin, E. Upfal, "Balanced Allocations", SIAM Journal on Computing, 1999.
- M. Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", IEEE Transactions on Parallel and Distributed Systems, 2001.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-value Store", ACM SOSP, 2007.
- P. Patel et al., "Ananta: Cloud Scale Load Balancing", ACM SIGCOMM, 2013.
- J. Lamping, E. Veach, "A Fast, Minimal Memory, Consistent Hash Algorithm", arXiv, 2014.
- D. Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer", USENIX NSDI, 2016.
- R. Miao, H. Zeng, C. Kim, J. Lee, M. Yu, "SilkRoad: Making Stateful Layer-4 Load Balancing Fast and Cheap Using Switching ASICs", ACM SIGCOMM, 2017.
- V. Mirrokni, M. Thorup, M. Zadimoghaddam, "Consistent Hashing with Bounded Loads", ACM-SIAM SODA, 2018.
- V. Olteanu, A. Agache, A. Voinescu, C. Raiciu, "Stateless Datacenter Load-balancing with Beamer", USENIX NSDI, 2018.
- J. T. Araújo, L. Saino, L. Buytenhek, R. Landa, "Balancing on the Edge: Transport Affinity without Network State", USENIX NSDI, 2018.
- RFC 2992, "Analysis of an Equal-Cost Multi-Path Algorithm" (modulo-N, hash-threshold and highest random weight compared).

<!-- detail: 1 -->
## In the lab

Run `./netlab run 450-consistent-hashing`. Client c1 sends 920 requests at 40 per second over 40 flows to the balancer `lb`, which starts with s1, s2, s3; s4 is added at 8 s and s1 removed at 16 s. Parameters: `algo` (consistent, hash, maglev; the module also accepts rr, wrr, leastconn, p2c), `affinity` (0 = stateless), `flows`.

What to measure: `reqgen.moved` on c1 (flows whose answering server changed, end to end), `lb.remapped` on the balancer, the two log lines printed by `lb` at each pool change ("N/40 flows lost their server, M/40 more now hash to a different server"), and the final `reqgen` line with the share of each server. The ideal for 40 flows is about 10 moves when s4 joins plus the flows of s1 when it leaves. Headless:

```
./netlab run 450-consistent-hashing algo=maglev --headless --detail 1
```

**Experiment 1 — disruption.** Hypothesis: mod-N moves about 1 − 1/(N+1) of the flows per change; ring and Maglev stay near the minimum, Maglev slightly above. Manipulation: `algo=hash`, `algo=consistent`, `algo=maglev`. Metric: `reqgen.moved` out of 80 possible flow-moves, and how many of the flows that move on the addition go to s4 (any other move is unnecessary). Expected: about 60 for mod-N (more moves than flows: most of them move twice), about 20 for the other two; the lab's check `disruption` states both bounds. Note that the ring moved 14 flows, not 10, on the addition: decide whether this is sampling noise or an oversized arc before blaming the scheme.

**Experiment 2 — balance at small scale.** Hypothesis: Maglev balances better than a 100-virtual-node ring. Manipulation: `algo=consistent` and `algo=maglev`, each with `flows=10`, `20`, `40`. Metric: maximum over mean of the per-server request shares in the final `reqgen` line, separately for each pool phase if you add `expect sN whoami.served >= 0` lines to a copy. Expected: no clear winner; with so few flows the imbalance is large for both and the hypothesis cannot be confirmed. State what sample size would be needed, then see topic 47.

**Experiment 3 — what the flow table hides.** Hypothesis: a flow table masks the disruption of a poor mapping until the table is lost. Manipulation: `algo=hash affinity=1`; then the same with the balancer restarted in mid-run by typing `node lb down` and `node lb up` at about 12 s; repeat with `algo=consistent affinity=1`. Metric: `reqgen.moved`, `reqgen.backends_seen`, `lb.remapped`. Expected: without the restart only the flows of the removed server move (about 11) while `lb.remapped` records the roughly 70 that the hash alone would have moved, and s4 receives nothing because no new flow ever starts and mod-N sends every orphan of s1 to the same survivor (`backends_seen` is 3); after a restart mod-N loses almost every flow and the ring loses no more than it did without a table.

## Questions for discussion

- Rendezvous hashing is exactly minimal and needs no table. Why did Google build Maglev instead? Put numbers on lookup cost at N = 1000.
- With heavy-tailed flow sizes, should disruption be measured in flows, in bytes or in user-visible failures? Design the measurement.
- A flow table and a consistent hash protect against different events. Enumerate the events and say which mechanism covers each; which event is covered by neither?
- How many flows would experiment 2 need to detect a 5% difference in peak-to-mean load with reasonable confidence? What does that imply about the emulator's limits?
- QUIC connections survive a change of client address. What should a stateless balancer hash on, and what new attack surface does that create?
