<!-- level: 4 -->
<!-- labs: 482-swim-failure-detection, 483-gossip-partition, 490-chord-churn, 491-crdt-partition -->
# Membership, churn and convergence

Consensus buys agreement at the price of availability. A large family of systems makes the opposite trade: every node keeps working with whatever it knows, information spreads by random pairwise exchanges, and the system is only required to **converge** once the disturbances stop. Membership lists, routing tables of overlay networks and replicated data all fit the pattern. The price is that between disturbances nodes hold different views, and the guarantee says nothing about how different, or for how long.

Four questions run through the topic. **Can a node be declared dead without a quorum, and how often is the verdict wrong?** A failure detector in an asynchronous network cannot be both fast and accurate, so the design problem is the shape of the trade-off. **What repairs a partition after it heals**, when the protocol that spread the news has long gone quiet? **How much churn can a structured overlay absorb** before its periodic repair falls behind and lookups go wrong? And **what does "converged" mean for data**: all replicas equal is easy to obtain by throwing updates away, and the interesting part is which updates survive.

Each of the four labs isolates one of these questions on eight to eleven nodes. That is enough to see every mechanism, and too little to measure any scaling law.

<!-- detail: 2 -->
## SWIM and Lifeguard

SWIM separates failure detection from dissemination. Each protocol period a member pings one random peer. Without an ack it asks k others to ping the target on its behalf (**indirect probing**), which filters out a fault of the single path between prober and target. If that fails too, the target is not declared dead but **suspected**, and the suspicion is gossiped. Every member carries an **incarnation number** that only it may increase: on learning that it is suspected at incarnation i it broadcasts alive at i+1, which overrides the suspicion everywhere. Only a suspicion that survives its timeout becomes a death notice. Updates travel piggybacked on the pings and acks themselves, each retransmitted a number of times logarithmic in the group size (**infection-style dissemination**). The load per member is constant, expected detection time is independent of group size, and the false-positive rate is tuned by k and the suspicion timeout.

The weak point is a slow member: a node that is overloaded misses acks from healthy peers, suspects them, and processes their refutations late. **Lifeguard** adds local health awareness: a member that sees evidence of its own slowness (missed acks, refutations of itself) stretches its own timeouts; suspicion timeouts start long and shrink as independent confirmations arrive; and a suspected member is told promptly so that it can refute.

## Gossip under partition

Rumour mongering is fast and self-terminating: a node stops pushing an update after enough peers already had it. That is exactly why it cannot repair a partition: by the time the link returns, nobody is pushing. **Anti-entropy** (Demers et al.) is the complementary mechanism: periodically pick a random peer and reconcile complete states, or digests of them. It is slower and costs traffic forever, but it converges from any state. For membership, the two halves have declared each other dead; healing requires that someone still occasionally contacts the "dead", who then refute with a higher incarnation.

## Chord under churn

Chord's correctness depends only on successor pointers; fingers merely make lookups fast. Joins and failures are repaired by periodic **stabilization**, and failures are survived with a **successor list** of length r: the ring stays connected unless r consecutive nodes fail within one repair interval, and keys survive if they are replicated on those successors. Under continuous churn the question is whether repair keeps up. Liben-Nowell, Balakrishnan and Karger define the half-life of the system and show that each node must perform on the order of log N repair actions per half-life. Rhea et al. showed experimentally that the behaviour under churn is dominated by engineering choices: how timeouts are computed, reactive against periodic recovery, and how neighbours are selected.

## CRDTs after a partition

State-based CRDTs converge because merge is a join on a semilattice: commutative, associative, idempotent. This is **strong eventual consistency**: replicas that have received the same updates are in the same state, with no rollback. What differs between types is the semantics of concurrency. The **OR-Set** tags every add; a remove deletes only the tags it has observed, so an add concurrent with a remove survives. The **last-writer-wins register** keeps the write with the larger timestamp and discards the other. It converges, but "last" is decided by clocks that are not synchronised: a replica with a slow clock loses to earlier writes, and the lost update is silent.

<!-- detail: 3 -->
## Open problems

**Gray failure.** SWIM-style detectors assume crash-stop members and independent packet loss. Members that are slow, partially connected or unreachable from some peers only produce flapping, and a membership view that never settles. Lifeguard treats the slow-observer case; asymmetric reachability is largely open, and membership services that deliberately seek agreement on the view have reappeared for this reason.

**Formal status of overlays.** Zave showed, by lightweight modelling, that the published Chord maintenance protocol does not preserve its own ring invariant under some interleavings of joins and failures, a decade after publication. How many deployed "eventually correct" protocols would survive the same scrutiny is unknown.

**Metadata.** CRDTs pay for coordination-freedom with metadata that grows with history: tombstones, tags, version vectors. Delta-state CRDTs reduce what is shipped, not what is stored; safe garbage collection needs knowledge of what every replica has seen, which is a membership problem and brings coordination back.

**Semantics.** Convergence does not imply that the converged state is what any user intended. Which invariants can be kept without coordination, and how to expose the remaining conflicts to applications, is an active line of work.

## The lab's model versus reality

The `gossip` module implements SWIM's probe, indirect probe, suspicion, incarnation and piggybacking, with fixed timeouts: **no Lifeguard**, and no slow members, since a simulated node processes every message in zero time. The counter `swim.mistaken_deaths` is something a real member can observe: it counts a member that was declared dead and later proved, by refuting within the same run of its process, that it had never gone down (the incarnation number records restarts and refutations separately). During a partition every unreachable member ends up counted this way: unreachable and dead cannot be told apart, which is the point. Loss on a link is independent per packet unless you set a burst length. In the partition lab every rumour is a short string and "anti-entropy" exchanges the full set of rumours: digest size and reconciliation cost, the real concerns at scale, are invisible. The origin of a rumour counts who heard it from the nodes' own reports (`gossip.reached`, `gossip.residue`), not by looking inside them.

The `chord` module uses an **8-bit identifier space** by default, recursive lookups, ping-based liveness each stabilization round, and `succlist` copies of every pair (owner plus successors), restored after each change of neighbourhood. Ten nodes and five keys cannot exhibit logarithmic hop counts or measure lookup failure rates; churn is two scripted crashes and two joins, not a process with a rate. A restarted node has lost its memory by design.

The `crdt` module ships the whole state to one random peer per round, keeps tombstones forever and holds at most a few dozen elements. Clock error is a constant offset (`skew`) on one replica: no drift, no hybrid logical clocks, no per-write causality. The register anomaly it shows is real; its frequency in a real system depends on clock quality and write concurrency, which the lab does not model.

## Primary literature

- A. Demers et al., "Epidemic Algorithms for Replicated Database Maintenance", ACM PODC, 1987.
- T. Chandra, S. Toueg, "Unreliable Failure Detectors for Reliable Distributed Systems", Journal of the ACM, 1996.
- R. van Renesse, Y. Minsky, M. Hayden, "A Gossip-Style Failure Detection Service", Middleware, 1998.
- R. Karp, C. Schindelhauer, S. Shenker, B. Vöcking, "Randomized Rumor Spreading", IEEE FOCS, 2000.
- I. Stoica, R. Morris, D. Karger, M. F. Kaashoek, H. Balakrishnan, "Chord: A Scalable Peer-to-peer Lookup Service for Internet Applications", ACM SIGCOMM, 2001.
- A. Das, I. Gupta, A. Motivala, "SWIM: Scalable Weakly-consistent Infection-style Process Group Membership Protocol", IEEE DSN, 2002.
- D. Liben-Nowell, H. Balakrishnan, D. Karger, "Analysis of the Evolution of Peer-to-Peer Systems", ACM PODC, 2002.
- S. Rhea, D. Geels, T. Roscoe, J. Kubiatowicz, "Handling Churn in a DHT", USENIX Annual Technical Conference, 2004.
- N. Hayashibara, X. Défago, R. Yared, T. Katayama, "The φ Accrual Failure Detector", IEEE SRDS, 2004.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-value Store", ACM SOSP, 2007.
- M. Shapiro, N. Preguiça, C. Baquero, M. Zawirski, "Conflict-free Replicated Data Types", SSS, 2011.
- P. Zave, "Using Lightweight Modeling to Understand Chord", ACM SIGCOMM Computer Communication Review, 2012.
- S. Kulkarni, M. Demirbas, D. Madappa, B. Avva, M. Leone, "Logical Physical Clocks", OPODIS, 2014.
- P. S. Almeida, A. Shoker, C. Baquero, "Delta State Replicated Data Types", Journal of Parallel and Distributed Computing, 2018.
- A. Dadgar, J. Phillips, J. Currey, "Lifeguard: Local Health Awareness for More Accurate Failure Detection", IEEE/IFIP DSN Workshops, 2018.

<!-- detail: 1 -->
## In the lab

**Lab 482-swim-failure-detection.** `./netlab run 482-swim-failure-detection`: eight members, n8 behind a lossy link, n3 crashes at 8 s and returns at 22 s. Parameters: `loss`, `period`, `suspect`, `helpers`. Counters: `swim.detected`, `swim.mistaken_deaths`, `swim.refutations`, `swim.incarnation`, `swim.restarts`, `swim.alive`, `gossip.messages`. Use several seeds.

- **Experiment 1.** Hypothesis: indirect probes reduce suspicions, and the suspicion timeout turns the remaining ones into refutations instead of deaths. Manipulation: `helpers=3` against `helpers=0` at the default loss. Metric: `swim.refutations` and `swim.incarnation` on n8, `swim.mistaken_deaths` everywhere (the final report prints them). Expected: with helpers n8 almost never has to refute and nobody is buried by mistake; without them n8 refutes repeatedly and in some runs a member declares it dead before the refutation arrives.
- **Experiment 2.** Hypothesis: mistaken deaths appear when the suspicion timeout is short relative to the time a refutation needs under loss. Manipulation: `suspect=1s loss=40%` with `helpers=3` and `helpers=0`. Metric: mean `swim.mistaken_deaths` per member. Expected: a few per member with helpers, clearly more without; with the default `suspect=5s` none at any loss up to 50%. The real crash of n3 is detected in every case: completeness does not depend on these parameters.
- **Experiment 3.** Hypothesis: detection time scales with the period, traffic with its inverse. Manipulation: `period=200ms` against `1s`. Metric: time from the crash at 8 s to the first "declares DEAD" line for n3, and `gossip.messages` on n1. Expected: about five times the messages; detection time dominated by `suspect` unless you scale that too.

**Lab 483-gossip-partition.** `./netlab run 483-gossip-partition`: two racks of five, separated from 2 s to 14 s; rumour A starts on one side, B on the other. Parameters: `mode`, `swim`, `interval`, `suspect`. Counters: `gossip.infected`, `gossip.reached`, `gossip.residue`, `gossip.messages`, `swim.dead`, `swim.alive`, `swim.mistaken_deaths`.

- **Experiment 1.** Hypothesis: rumour mongering alone does not heal a partition. Manipulation: `mode=push`, `mode=pull`, `mode=pushpull`. Metric: `gossip.reached` and `gossip.residue` on n1 and n10 at the end. Expected: with push each origin reports that its rumour died out with a residue of 5 (the other rack); with the others 10 reached and no residue.
- **Experiment 2.** Hypothesis: time to reconverge after the heal is a few anti-entropy rounds, at a standing traffic cost. Manipulation: `interval=500ms` against `2s`. Metric: time from 14 s to the "everybody knows" line of each rumour; `gossip.messages`. Expected: about three rounds in each case, so four times longer with the slower interval and roughly half the messages.
- **Experiment 3.** Hypothesis: data repair and membership repair are independent mechanisms. Manipulation: `swim=0` against `swim=1`; then a longer partition by copying the scenario. Metric: `gossip.infected`, `swim.alive` at the end, and `swim.mistaken_deaths` after the heal. Expected: rumours converge either way; with SWIM each side declares the other five dead and later readmits them, each counted as a mistaken death: a failure detector cannot tell a partition from a crash.

**Lab 490-chord-churn.** `./netlab run 490-chord-churn`: ten nodes, five keys, n3 and n10 crash at 14 s, n11 joins at 21 s, n3 returns empty at 23 s. Parameters: `succlist`, `stabilize`. Counters: `chord.found`, `chord.notfound`, `chord.failed`, `chord.hops`, `chord.keys`.

- **Experiment 1.** Hypothesis: a pair is lost only when its owner and all the holders of its copies fail within one repair interval. Manipulation: `succlist=1`, `2`, `3`. Metric: `chord.found` and `chord.notfound` of the readers (every read is answered: `chord.answered`). Expected: with `succlist=1` there is one copy of every pair, so the keys owned by n3 and n10 are gone and most reads return "not found"; with 2 or more every read succeeds, and the heirs copy the keys again to their own successors ("keys copied again" at detail 2), so a later crash would be survived too. Verify that in a copy of the scenario by crashing one more node at 26 s.
- **Experiment 2.** Hypothesis: lookups issued inside the repair window are delayed by about one stabilization period. Manipulation: in a copy add `at 14.05s run n6 chord get key=cherry`; run with `stabilize=100ms`, `400ms`, `2s`. Metric: delay between request and answer line, unanswered requests. Expected: about a second at 400 ms; at 2 s some early reads are never answered.
- **Experiment 3.** Hypothesis: a join moves only the keys of one arc. Manipulation: default run. Metric: `chord.keys` on n3 and its successor before 14 s and after 30 s; "stored a copy" lines at detail 2. Expected: n3 is handed back the keys of its arc and nothing else moves.

**Lab 491-crdt-partition.** `./netlab run 491-crdt-partition`: three replicas with an OR-Set and an LWW register; n3 is cut off from 3 s to `heal`. Parameters: `skew`, `heal`. Counters: `crdt.size`, `crdt.register`, `crdt.lost`, `crdt.merges`.

- **Experiment 1.** Hypothesis: the OR-Set preserves a concurrent add against a remove; the register silently drops one write. Manipulation: default run. Metric: final set and register on all three, `crdt.lost`. Expected: {bread, milk, eggs} everywhere, register 30, one lost update reported by n1.
- **Experiment 2.** Hypothesis: which write is "last" is decided by clock error once it exceeds the real interval between the writes (here 500 ms). Manipulation: `skew=-400ms`, `-600ms`, `-3s`. Metric: final register, which replica reports a loss (`crdt.lost`), and `crdt.converged`. Expected: 30 wins at −400 ms and n1 reports the loss; from −600 ms the earlier write 20 wins everywhere and it is n3, whose write was really the later one, that reports its update lost. In every case the three replicas converge: last-writer-wins is consistent, just not faithful to real time.
- **Experiment 3.** Hypothesis: availability during the partition is unconditional, and convergence time after it depends only on the gossip period. Manipulation: `heal=8s` against `heal=12s`; type extra `n3 crdt add X` commands during the partition. Metric: time from heal to the last merge line; final states. Expected: identical outcomes and about one second to converge, whatever the partition length.

## Questions for discussion

- `swim.mistaken_deaths` counts a member that proves it never went down. What does this metric miss, and what would an oracle with global knowledge count differently during a partition?
- After a partition heals, each side holds death notices for the other. Which property of incarnation numbers guarantees that the group reunites, and what breaks if a restarted member reuses an old incarnation?
- Chord's ring invariant was found not to hold a decade after publication. What would you model-check in this emulator's implementation, and against which interleavings?
- Experiment 2 of the CRDT lab produces a lost update that no counter reports. What would a replica need to store in order to detect it, and what is that structure called?
- Add-wins is a choice. Give an application for which remove-wins is the right semantics, and one for which neither is acceptable without coordination.
