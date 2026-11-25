<!-- level: 4 -->
<!-- labs: 480-raft-partition, 481-raft-split-votes -->
# Consensus under partitions

A consensus protocol such as Raft promises two things of different kinds. **Safety**: the replicas never disagree about a committed entry, whatever the network does. **Liveness**: commands are eventually committed, provided a majority can communicate and the network behaves well enough for long enough. The FLP result says the second promise cannot be unconditional in an asynchronous system, and the CAP theorem says that during a partition some replicas must refuse service if answers are to stay consistent. Neither result says how long the service is unavailable, which replicas suffer, or what clients observe meanwhile. **Those quantities, not the impossibility theorems, are what an engineer chooses, and they are set by timeouts and by details the basic algorithm leaves open.**

The topic is still alive because production failures are rarely clean. Partitions are partial and asymmetric, nodes are slow rather than dead, clocks drift, and several postmortems describe clusters that were safe and yet unavailable for hours with a majority connected. Extensions such as pre-vote, quorum checks and leases repair specific liveness and read-consistency problems, each with its own assumptions.

The two labs separate the issues: one partitions a five-node cluster and shows that two "leaders" do not make a split brain; the other measures how long a cluster stays leaderless as a function of the election timeout.

<!-- detail: 2 -->
## CAP, stated carefully

Gilbert and Lynch's theorem concerns a single read/write register in an asynchronous network that may lose arbitrarily many messages. It is impossible to guarantee both **atomic consistency** (linearizability) and **availability** (every request received by a non-failed node eventually gets a response). The popular "choose two of three" misleads: partitions are not chosen, so the choice is between consistency and availability, and only while a partition lasts. Availability in the theorem is also very strong: every node must answer, including those on the minority side. Abadi's PACELC adds the trade-off that exists without partitions: latency against consistency.

Raft chooses consistency. A minority partition can neither elect a leader nor commit; a majority partition continues. The cluster as a whole is "available" only to clients that can reach the majority side.

## Why two leaders are not a split brain

A leader cut off from the majority does not know it has been replaced. It keeps accepting commands and appending them, but an entry is committed only when stored on a majority, so nothing it accepts can be acknowledged. The majority elects a leader in a higher term. When the partition heals, the old leader sees the higher term, steps down, and its uncommitted suffix is overwritten. Safety rests on quorum intersection and on the election restriction (a vote is granted only to a candidate whose log is at least as up to date), not on there being a single node that believes itself leader.

## Reads

The intersection argument covers writes. A deposed leader that answers a read from its local state returns stale data, which violates linearizability. Three remedies exist. Send reads through the log (correct, slow). **ReadIndex**: the leader records its commit index, confirms with a round of heartbeats that it is still leader, then answers. **Leases**: the leader assumes no other leader can exist until a time-bound expires, which makes reads local but depends on bounded clock drift, an assumption outside the asynchronous model.

## Election timeouts

Raft's availability after a leader failure is detection time plus election time. Followers wait a randomised election timeout; if several expire close together each votes for itself and the term is wasted (a split vote). The original paper requires broadcastTime much smaller than electionTimeout much smaller than MTBF, and recommends drawing the timeout from a range about as wide as its lower bound. Too short a timeout, relative to heartbeat interval or network delay, makes followers depose healthy leaders; too narrow a range, relative to the message round trip, makes split votes likely and repeated.

## Disruption and its remedies

A follower isolated from the leader keeps timing out and incrementing its term. When it reconnects, its higher term forces the working leader to step down, although the returning node cannot win (its log is stale). **Pre-vote** adds a trial round: a node increments its term only if a majority says it would grant the vote, and nodes that have recently heard from a leader refuse. **CheckQuorum** makes a leader step down when it has not heard from a majority, which bounds the time a stale leader serves. With partial partitions (A reaches B, B reaches C, A does not reach C) plain Raft can lose liveness altogether, as leadership bounces between nodes that cannot both hold a quorum.

## Paxos and Viewstamped Replication

Multi-Paxos, Viewstamped Replication and Raft solve the same problem with the same quorum structure. Howard and Mortier locate the essential difference in leader election: Raft only elects a node whose log is already up to date, so entries flow only from the leader; Paxos lets any node win and then recover missing entries from its quorum; VR rotates the primary deterministically by view number. The consequences are in liveness and recovery cost, not safety.

<!-- detail: 3 -->
## Open problems

**Liveness under realistic faults.** Safety proofs are mature and mechanised; liveness under partial partitions, flapping links, slow disks and gray failures is argued case by case. Jensen, Howard and Mortier show that pre-vote and CheckQuorum, each sensible alone, interact badly in some partial partitions. Alquraan et al. found that most partition-related failures in production systems needed only a simple partition and a few events to trigger, and that many were catastrophic.

**Timeouts.** The election timeout is a static guess about a dynamic network. Adaptive or measurement-driven timeouts, and their interaction with safety-neutral extensions, lack a commonly accepted design.

**Reads and clocks.** Leases buy latency with a synchrony assumption. How to state, monitor and test that assumption (as Spanner does with TrueTime) is deployment-specific.

**Reconfiguration and geo-replication** add further availability trade-offs (flexible quorums, leaderless variants such as EPaxos) that the basic algorithm does not address.

## The lab's model versus reality

The `raft` module implements elections, replication, the election restriction, the no-op at the start of a term, persistence of term, vote and log, and optional pre-vote. Its help text lists what real systems add and it does not: **log compaction, membership change, ReadIndex and leases**. `raft get` reads the local state machine, and warns that the value may be stale only when the node knows it is not the leader; the lab can therefore show a stale read but not its cure. There is no CheckQuorum: an isolated leader stays leader indefinitely.

Links are symmetric, and members reach each other through switches, so a partition is always clean: either two members can talk in both directions or in neither. **Asymmetric and partial partitions**, the cases in which liveness is really at stake, need a different topology (point-to-point links between members) and cannot express one-way loss at all. Clocks are perfect and there is no processing or disk delay: a follower answers in zero time, so broadcastTime is pure link delay and the timeouts that work here are optimistic for a real deployment. Clients are fire-and-forget commands with no retry and no session, so "what the client sees" must be reasoned about rather than measured.

One more caution. With `prevote=1` the node configured with the short timeout is no longer reliably the first leader, because its initial pre-vote is refused by peers whose own timers have not expired. In lab 480 a member of the majority side then leads, the minority never appends anything, and the scenario's expectations about truncation fail: a different experiment from the one described.

## Primary literature

- M. Fischer, N. Lynch, M. Paterson, "Impossibility of Distributed Consensus with One Faulty Process", Journal of the ACM, 1985.
- C. Dwork, N. Lynch, L. Stockmeyer, "Consensus in the Presence of Partial Synchrony", Journal of the ACM, 1988.
- B. Oki, B. Liskov, "Viewstamped Replication: A New Primary Copy Method to Support Highly-Available Distributed Systems", ACM PODC, 1988.
- C. Gray, D. Cheriton, "Leases: An Efficient Fault-Tolerant Mechanism for Distributed File Cache Consistency", ACM SOSP, 1989.
- M. Herlihy, J. Wing, "Linearizability: A Correctness Condition for Concurrent Objects", ACM TOPLAS, 1990.
- L. Lamport, "The Part-Time Parliament", ACM Transactions on Computer Systems, 1998; "Paxos Made Simple", ACM SIGACT News, 2001.
- S. Gilbert, N. Lynch, "Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services", ACM SIGACT News, 2002.
- T. Chandra, R. Griesemer, J. Redstone, "Paxos Made Live: An Engineering Perspective", ACM PODC, 2007.
- E. Brewer, "CAP Twelve Years Later: How the 'Rules' Have Changed", IEEE Computer, 2012; D. Abadi, "Consistency Tradeoffs in Modern Distributed Database System Design", IEEE Computer, 2012.
- J. Corbett et al., "Spanner: Google's Globally-Distributed Database", USENIX OSDI, 2012.
- D. Ongaro, J. Ousterhout, "In Search of an Understandable Consensus Algorithm", USENIX ATC, 2014; D. Ongaro, "Consensus: Bridging Theory and Practice", PhD thesis, Stanford University, 2014 (pre-vote, leases, ReadIndex).
- P. Bailis, K. Kingsbury, "The Network is Reliable", ACM Queue, 2014.
- A. Alquraan, H. Takruri, M. Alfatafta, S. Al-Kiswany, "An Analysis of Network-Partitioning Failures in Cloud Systems", USENIX OSDI, 2018.
- H. Howard, R. Mortier, "Paxos vs Raft: Have We Reached Consensus on Distributed Consensus?", ACM PaPoC, 2020.
- C. Jensen, H. Howard, R. Mortier, "Examining Raft's Behaviour During Partial Network Failures", ACM HAOC, 2021.

<!-- detail: 1 -->
## In the lab

**Lab 480-raft-partition.** Run `./netlab run 480-raft-partition`. n1 and n2 hang off switch sa; n3, n4, n5 off sb. n1 becomes leader, the sa — sb link fails at 5 s and returns at 14 s. Parameters: `election`, `heartbeat`, `delay`, `prevote`. Counters on every member: `raft.term`, `raft.max_term`, `raft.is_leader`, `raft.leaders`, `raft.elections`, `raft.truncated`, `raft.commit_index`, `raft.applied`, `raft.agree`, `raft.state_hash`. Watch the term and role chips and the uncommitted cell on n1 and n2.

- **Experiment 1 — safety.** Hypothesis: no entry acknowledged as committed is ever lost, and the minority's entry never commits. Manipulation: default run; then during the partition type `n1 raft put z=1` several times and `n2 raft get x`. Metric: `raft.commit_index` on n1 between 5 s and 14 s, `raft.truncated` after the heal, equality of `raft.state_hash` on all five at the end. Expected: n1's commit index is frozen, every minority entry is discarded after 14 s, the hashes agree.
- **Experiment 2 — stale reads.** Hypothesis: a deposed leader serves stale data. Manipulation: default run; at about 12 s type `n1 raft get x` and `n4 raft get x`. Metric: the two values. Expected: n1 answers 1 while the majority has committed `majority`: not linearizable. Note which answer carries the "may be stale" warning: the wrong one does not, because n1 still believes it leads. Discuss what ReadIndex and a lease would each have done at that instant.
- **Experiment 3 — term inflation and pre-vote.** Hypothesis: an isolated follower disrupts a healthy cluster on return, and pre-vote prevents it. Manipulation: copy the scenario, delete its `at` and `expect` lines and add `at 5s link n3 sb down` and `at 15s link n3 sb up`; run with `prevote=0` and `prevote=1` (check in the log which node leads and isolate a follower). Metric: `raft.max_term`, `raft.elections` on n3, and the interval without a leader after 15 s. Expected: without pre-vote n3 reaches term 7 or 8, the leader steps down at the heal and a new election costs about half a second; with pre-vote the term never moves and nothing is disturbed.

**Lab 481-raft-split-votes.** Run `./netlab run 481-raft-split-votes`. Leader n1 crashes at 5 s; the four survivors draw their timeout from `election` (default 1000-1060ms, deliberately narrow). Parameters: `election`, `heartbeat`, `delay`. Metric throughout: leaderless time, from 5 s to the first `elected LEADER` line, and the number of terms consumed (`raft.max_term` minus 1). Use at least ten seeds per point, for example:

```
for s in 1 2 3 4 5 6 7 8 9 10; do ./netlab run 481-raft-split-votes election=1000-1010ms --seed $s --headless --detail 1 --lang en | grep LEADER; done
```

- **Experiment 1 — width of the range.** Hypothesis: the expected number of wasted terms falls as the range widens relative to the message round trip. Manipulation: `election=1000-1010ms`, `1000-1060ms`, `1000-2000ms`. Metric: distribution of leaderless time. Expected: several rounds (2 to 4 s) for the narrowest, one or two for the default, a single round of about one second for the widest, whose mean detection time is however longer.
- **Experiment 2 — network delay.** Hypothesis: split votes depend on range width divided by delay, not on width alone. Manipulation: default range with `delay=5ms`, `20ms`, `50ms`. Metric: as above, and whether the run passes. Expected: at 50 ms about ten wasted terms and a leaderless period of ten seconds or more; some seeds elect nobody before the scenario ends.
- **Experiment 3 — timeout below the heartbeat.** Hypothesis: when electionTimeout is not well above the heartbeat interval there is no stable leader even without failures. Manipulation: `election=150-300ms` (heartbeat is 300 ms). Metric: `raft.max_term` at the end, `raft.agree`. Expected: more than a hundred terms in 25 s and continual changes of leader; commands still commit between elections, which is worth explaining.

## Questions for discussion

- State precisely which clients observe unavailability in lab 480, for how long, and whether any of them could observe an inconsistency. Which CAP "letter" does each observation correspond to?
- Suppose n1 had answered "ok" to `x=minority` before committing. Construct the client-visible anomaly after the heal.
- A lease lets a leader read locally for the lease period. What exactly must be true of the clocks, and how would you test that in an emulator with perfect clocks?
- In experiment 2 of lab 481 the cluster is safe and unavailable. Propose a change of the timeout rule that would have avoided it, and say what it costs when the network is healthy.
- Paxos lets a node with a stale log become leader; Raft does not. In which failure pattern does this difference change availability?
