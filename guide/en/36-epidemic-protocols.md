<!-- level: 3 -->
<!-- labs: 382-gossip-rumor -->
# Epidemic protocols: rumor spreading and anti-entropy

How should one piece of information reach thousands of machines, some of which are crashed, slow or unreachable at any moment? A coordinator that sends to everybody is a bottleneck and a single point of failure; a spanning tree delivers with the minimum number of messages but loses a whole subtree when one inner node fails, and must be rebuilt. Both need accurate, global knowledge of who is alive, which is exactly what a large system does not have.

The **epidemic** (gossip) approach abandons structure. In every round each node contacts a few peers chosen uniformly at random and exchanges information with them. Nobody coordinates, nobody knows the topology, no message is essential. The difficulty moves from engineering to probability: nothing is guaranteed deterministically, so one must show that the outcome holds with high probability and quantify its cost.

The key fact is that an epidemic grows exponentially. While few nodes are informed, each contact almost surely reaches an uninformed node and the informed set multiplies every round; `N` nodes are covered in `O(log N)` rounds, with a constant load per node and per round. Lost messages and crashed nodes only reduce the growth rate. What is paid is redundancy: near the end most contacts are wasted on nodes that already know, and deciding when to stop is itself a trade-off between traffic and the risk of leaving somebody out.

<!-- detail: 2 -->
## Algorithms and models

**Three styles.** Let node `p` contact a random peer `q` in a round.

- **Push**: if `p` knows the rumor, it sends it to `q`.
- **Pull**: `p` asks `q` for news; `q` answers if it knows.
- **Push-pull**: both.

**Deterministic approximation.** Let `s_t` be the fraction of nodes still uninformed after round `t`, with fanout `f` and `N` large. A node stays uninformed under push if none of the `N*(1 - s_t)*f` pushes hits it, under pull if all its `f` contacts are uninformed:

```
push:       s(t+1) = s(t) * exp( -f*(1 - s(t)) )
pull:       s(t+1) = s(t) ^ (f + 1)
push-pull:  s(t+1) = s(t) ^ (f + 1) * exp( -f*(1 - s(t)) )
```

Push starts fast (the informed set roughly multiplies by `1 + f` while it is small) but ends slowly: when almost everyone knows, `s` shrinks only by `e^(-f)` per round, so the last nodes take a further `ln N / f` rounds. For `f` = 1 the classic result is

```
rounds(push) = log2 N + ln N + O(1)      with high probability
```

Pull is the opposite: slow to start, because the few informed nodes are rarely asked, but once `s < 1/2` the uninformed fraction is squared every round, so the tail takes only `O(log log N)` rounds. Push-pull has both good phases: `O(log N)` rounds and `O(N log log N)` messages in total, against `O(N log N)` for push alone.

**The SI model.** In continuous time, with `i` the informed fraction and contact rate `β`:

```
di/dt = β * i * (1 - i)          i(t) = 1 / (1 + (N - 1) * exp(-β*t))
```

The logistic curve: exponential growth, an inflection at `i = 1/2` reached at `t = ln(N - 1)/β`, then exponential approach to 1. Nodes never recover; the rumor is spread for ever (a **simple epidemic**).

**Rumor mongering (SIR).** To bound the traffic, an infective node eventually loses interest and becomes **removed**. Variants: stop with probability `1/k` after each useless contact (coin), or after `k` useless contacts (counter); decide on the peer's answer (feedback) or regardless (blind). Now some nodes may never hear the rumor. For feedback with a coin the residue `s` solves

```
s = exp( -(k + 1)*(1 - s) )        k = 1: s = 0.20     k = 2: s = 0.06
```

and counters do better than coins for the same traffic. The residue falls exponentially in `k`, the traffic grows linearly.

**Anti-entropy.** Periodically each node picks a random peer and the two reconcile their entire state (by exchanging digests, version vectors or Merkle trees to find the differences). It is a simple epidemic, so every update reaches every node with probability 1, including nodes that were down or partitioned; it is too expensive to run often. Practical systems combine fast rumor mongering for new updates with slow anti-entropy as a safety net.

**Worked example.** `N` = 12, as in the lab: `log2 12` = 3.6 and `ln 12` = 2.5. Push with fanout 2: from `s` = 11/12 the recurrence gives 0.78, 0.50, 0.18, 0.04, 0.005, so about 5 rounds until fewer than one node is missing. Push-pull with fanout 2: 0.65, 0.13, 0.0004: 2 to 3 rounds.

<!-- detail: 3 -->
## In depth

**Why it is robust.** The analysis uses only two facts: contacts are uniform and independent, and each succeeds with some probability. A loss rate `ε` or a fraction of crashed nodes multiplies the effective fanout by a constant; the exponents change, the logarithmic shape does not. There is no state to repair after a failure, because there is no structure. The same independence is the weakness: guarantees are probabilistic, delivery order is arbitrary, and the protocol cannot know that it has finished.

**Failure modes.** Rumor mongering can die out early (the residue). A partition longer than the infective period leaves one side permanently uninformed unless anti-entropy runs. Deleting an item is not the absence of an item: without a **death certificate** the deleted data is resurrected by a replica that still has it, and certificates must themselves be kept (and eventually dropped, with dormant copies). Uniform sampling requires knowing all members, which does not scale: peer-sampling services (SCAMP, Cyclon, HyParView) keep a small random partial view by gossiping the views themselves. On topologies that are not complete graphs the spreading time depends on the conductance of the graph and can be far from logarithmic; spatial gossip biases the choice towards near nodes to spare long links. Malicious nodes can bias or flood an epidemic; Byzantine-tolerant variants limit what a single node may inject.

**Trade-offs against structure.** A tree sends `N - 1` messages and gossip `O(N log N)`; hybrid protocols (Plumtree) gossip only message identifiers lazily and let a tree emerge from the first deliveries, falling back to gossip when it breaks.

**What deployed systems do.** Clearinghouse (Xerox) was the origin. Dynamo and Cassandra gossip membership and repair replicas by anti-entropy over Merkle trees, plus read repair and hinted handoff. SWIM and Serf piggyback membership updates on failure-detector probes. Bimodal multicast pairs unreliable broadcast with gossip repair. Blockchain networks flood transactions and blocks epidemically. Gossip also computes aggregates (averages, counts) by repeated pairwise averaging.

**In netlab.** The `gossip` module runs synchronous rounds of fixed length on a full membership list; `stop=k` is the feedback/counter rule; n1 reports the count of informed nodes at each round from global knowledge that a real node would not have. This lab has no anti-entropy (see `483-gossip-partition` in the Frontier track).

**Primary literature.**

- W. Kermack, A. McKendrick, "A Contribution to the Mathematical Theory of Epidemics", 1927.
- A. Frieze, G. Grimmett, "The Shortest-Path Problem for Graphs with Random Arc-Lengths", 1985.
- B. Pittel, "On Spreading a Rumor", 1987.
- A. Demers et al., "Epidemic Algorithms for Replicated Database Maintenance", PODC 1987.
- K. Birman et al., "Bimodal Multicast", 1999.
- R. Karp, C. Schindelhauer, S. Shenker, B. Vöcking, "Randomized Rumor Spreading", FOCS 2000.
- A. Das, I. Gupta, A. Motivala, "SWIM: Scalable Weakly-consistent Infection-style Process Group Membership Protocol", 2002.
- D. Kempe, A. Dobra, J. Gehrke, "Gossip-Based Computation of Aggregate Information", 2003.
- P. Eugster, R. Guerraoui, A.-M. Kermarrec, L. Massoulié, "Epidemic Information Dissemination in Distributed Systems", 2004.

<!-- detail: 1 -->
## In the lab

`./netlab run 382-gossip-rumor`. Twelve nodes on a LAN; at 2 s n1 starts the rumor `fire!`. Select n1 and open its `gossip` tab: the graph `nodes that know the rumor` is the epidemic curve. In the log (`l`) n1 prints one line per round, "round k — m/12 nodes know", and at the end "everybody knows after R rounds (log2 N = 3.6)". n1 knows the count because every node reports to the origin when it first hears the rumor (a census outside the epidemic); `gossip.reached` and `gossip.residue` are in the final report. Other nodes show their state: susceptible, infective, removed. The counters `gossip.messages` and `gossip.rounds` are in n1's panel. Rounds last 500 ms. Because the process is random, repeat each experiment with a few seeds (`--seed 1`, `--seed 2`, …) and reason about the distribution, not one run.

**Baseline.** Default `mode=pushpull fanout=2`. Compute the recurrence first (the worked example predicts 2 to 3 rounds), then measure. Expected: 1, then 6–8, then 12 nodes.

**Experiment 1: push, pull, push-pull.** Run `mode=push` and `mode=pull`. Prediction: push needs 3 to 6 rounds and spends its last rounds on one or two missing nodes; pull needs 3 to 4, with a slow first round and an abrupt finish. Compare `gossip.messages` between the three modes.

**Experiment 2: the residue.** `./netlab run 382-gossip-rumor mode=push stop=1`. Each node stops pushing after the first peer that answers "already knew". Use the residue equation to predict how many of 12 nodes are never reached (around two), then run several seeds: in most runs the count stops below 12 and, once nothing has changed for some rounds, n1 prints "the rumor died out" with the residue (counter `gossip.residue`; the checks pass, because for push the claim is only that the epidemic ends). Even the default `stop=3` occasionally leaves one node out with `mode=push`. Explain why pull and push-pull have no residue here.

**Experiment 3: fanout and loss.** `fanout=1` against `fanout=4`: predict rounds from the recurrence and observe that rounds fall slowly while messages grow in proportion. Then `loss=0.2`: every link drops 20% of the packets. Prediction: still complete, in one or two more rounds; nothing else changes. In a running lab also crash two nodes before the rumor starts (`node n7 down`, `node n8 down`) and check that the rest is covered.

## Check yourself

- Derive the push recurrence s(t+1) = s(t)·exp(−f(1 − s(t))) and use it to justify the two phases, log2 N and ln N, for f = 1.
- Why does pull converge doubly exponentially at the end but slowly at the beginning? What does this imply for the number of messages of push-pull?
- Solve the logistic equation of the SI model and give the time at which half the population is informed. Which assumption of the model fails on a sparse topology?
- For feedback rumor mongering with a coin of probability 1/k, derive the residue equation and compute the residue for k = 3. Why is the residue not acceptable for a replicated database, and what is added to fix it?
- A replica deletes an item and gossips the deletion as "item absent". Describe how the item comes back, and the mechanism (with its cost) that prevents it.
