<!-- level: 3 -->
<!-- labs: 380-raft-election, 381-raft-replication -->
# Consensus and Raft

A replicated service stays available when machines crash only if its replicas execute the same commands in the same order: then they are copies of one deterministic state machine. Agreeing on that order is the **consensus** problem. Each process may propose a value, and the protocol must guarantee **agreement** (no two processes decide differently), **validity** (the decided value was proposed) and **termination** (every correct process eventually decides).

It is hard because, in an asynchronous network, a crashed process cannot be told from a slow one. A protocol that waits for everybody blocks at the first crash; one that stops waiting may act on a suspicion that is false, and two groups may then decide differently. The properties split accordingly: agreement is a **safety** property (nothing bad ever happens, whatever the timing), termination is a **liveness** property (something good eventually happens), and a fundamental result says the two cannot both be guaranteed under full asynchrony.

The key ideas are two. **Majority quorums**: any two majorities of the same group share at least one member, so a fact stored on a majority cannot be ignored by a later majority. And **terms**: time is cut into numbered epochs with at most one leader each, so every message can be judged current or stale. Raft keeps safety unconditionally and gives up only liveness, and only while the network misbehaves.

<!-- detail: 2 -->
## Algorithms and models

**Model.** `N = 2f + 1` processes tolerate `f` crash failures (not Byzantine ones); messages may be lost, delayed, duplicated, reordered; processes have stable storage.

**FLP in one paragraph.** Fischer, Lynch and Paterson (1985) proved that no deterministic protocol solves consensus in an asynchronous system if even one process may crash. The proof shows that from an undecided ("bivalent") configuration an adversary scheduler can always delay one message so that the system stays undecided for ever. The result does not say that consensus is impossible in practice; it says that termination needs an extra assumption: partial synchrony (bounds hold eventually), failure detectors, or randomisation. Raft uses timeouts, so it is live only when messages arrive within the election timeout.

**State.** Persistent on every server: `currentTerm`, `votedFor`, `log[]` (each entry holds a command and the term in which a leader created it). Volatile: `commitIndex`, `lastApplied`; on the leader `nextIndex[]`, `matchIndex[]`. Any message with a higher term makes the receiver adopt it and become a follower; a message with a lower term is rejected.

**Leader election.** A follower that hears no leader for a random time in `[T, 2T]` increments its term, votes for itself and sends `RequestVote(term, lastLogIndex, lastLogTerm)`. A server grants its single vote of the term only if the candidate's log is **at least as up to date** as its own:

```
grant  iff  term >= currentTerm  and  votedFor in {null, candidate}
            and (lastLogTerm, lastLogIndex) >= (myLastTerm, myLastIndex)
            (pairs compared lexicographically)
```

A majority of votes makes a leader. Two candidates may split the vote: nobody wins, the timers expire again at different random times.

**Replication.** The leader appends commands and sends

```
AppendEntries(term, prevLogIndex, prevLogTerm, entries[], leaderCommit)
follower: reject if term < currentTerm
                 or log[prevLogIndex].term != prevLogTerm
          else delete any conflicting suffix, append entries,
               commitIndex = min(leaderCommit, index of last new entry)
leader:   on reject, decrement nextIndex[follower] and retry
```

This consistency check gives **log matching**: if two logs hold an entry with the same index and term, they are identical up to that index.

**Commitment rule.**

```
leader sets commitIndex = n   iff   matchIndex[i] >= n on a majority
                                    and log[n].term == currentTerm
```

**Why only current-term entries.** Five servers. S1, leader of term 2, stores entry `(index 2, term 2)` on S1 and S2 and crashes. S5 wins term 3 with the votes of S3 and S4, appends its own `(2, term 3)` and crashes. S1 wins term 4 and copies `(2, term 2)` to S3: the entry is now on a majority. If S1 counted replicas and declared it committed, and then crashed, S5 could still win term 5 with the votes of S2, S3, S4 (its last term, 3, beats their 2) and overwrite index 2: a committed entry would be lost. If instead S1 first replicates an entry of term 4 on a majority, S5 can no longer win, and the old entry is committed indirectly. Hence the no-op that a new leader appends at once.

**Timing.** Liveness needs `broadcast time << election timeout << mean time between failures`.

<!-- detail: 3 -->
## In depth

**The safety argument.** Raft maintains five invariants: election safety (at most one leader per term: one vote per server per term, and majorities intersect), leader append-only, log matching (by induction on the AppendEntries check), **leader completeness** (an entry committed in term `t` is in the log of every leader of a term above `t`), and state-machine safety (no two servers apply different commands at the same index). Leader completeness is the core: a committed entry is on a majority; a later candidate needs a majority of votes; some voter holds the entry; it votes only for a log at least as up to date as its own; by induction over the intervening terms the candidate holds the entry. The current-term restriction is what makes the first step of that induction true.

**Persistence.** `currentTerm`, `votedFor` and the log must reach stable storage before a reply is sent. A server that forgets its vote can vote twice in a term and create two leaders; one that forgets an acknowledged entry breaks the quorum arithmetic. The commit index is volatile: after a restart it is learned again from the leader.

**Failure modes and remedies.** Split votes and repeated elections under tight or badly randomised timeouts. A partitioned server inflates its term and, on healing, deposes a healthy leader: PreVote and CheckQuorum prevent it. A deposed leader may still answer reads from stale state: linearisable reads need a round of heartbeats (ReadIndex) or a lease with bounded clock drift. A client that retries a command after a leader change may have it applied twice unless commands carry session identifiers. Membership changes must never allow two disjoint majorities: joint consensus, or one server at a time. Logs grow without bound: snapshots and InstallSnapshot.

**Relatives.** Viewstamped Replication (1988) and Multi-Paxos reach the same guarantees; Raft differs by requiring the new leader to already hold all committed entries, so data flows only from leader to followers. Zab (ZooKeeper) is similar. Byzantine faults need `3f + 1` replicas (PBFT). Deployed Raft: etcd, Consul, CockroachDB, TiKV, Kafka's KRaft.

**In netlab.** The `raft` module persists term, vote and log across `node N down/up`, appends a no-op at the start of each term, forwards commands from followers to the leader and offers `prevote=1`. It has no snapshots and no membership change, and `raft get` reads the local copy of the state machine, which may be stale: it is not a linearisable read. Followers learn a new commit index from the next AppendEntries, so they apply one heartbeat later than the leader.

**Primary literature.**

- M. Fischer, N. Lynch, M. Paterson, "Impossibility of Distributed Consensus with One Faulty Process", 1985.
- B. Oki, B. Liskov, "Viewstamped Replication: A New Primary Copy Method to Support Highly-Available Distributed Systems", 1988.
- C. Dwork, N. Lynch, L. Stockmeyer, "Consensus in the Presence of Partial Synchrony", 1988.
- F. Schneider, "Implementing Fault-Tolerant Services Using the State Machine Approach: A Tutorial", 1990.
- T. Chandra, S. Toueg, "Unreliable Failure Detectors for Reliable Distributed Systems", 1996.
- L. Lamport, "The Part-Time Parliament", 1998; "Paxos Made Simple", 2001.
- D. Ongaro, J. Ousterhout, "In Search of an Understandable Consensus Algorithm", USENIX ATC 2014; D. Ongaro, "Consensus: Bridging Theory and Practice", PhD thesis, 2014.

<!-- detail: 1 -->
## In the lab

**380-raft-election.** `./netlab run 380-raft-election`. Five servers around a switch; n1 has a shorter timeout (400–500 ms) so that it wins term 1. Select a node (`n`) and open its `raft` tab: role, term, the election timer bar, the log. Press `s` for the sequence view and follow RequestVote, the votes, then the first AppendEntries carrying the no-op. At 8 s n1 crashes: measure the time until "elected LEADER for term 2" (expected a little over 1 s: the smallest of four timeouts drawn in 1–2 s). At 16 s n1 restarts "from stable storage: term=1", receives an AppendEntries of term 2 and becomes a follower.

- `election=1000-1050ms`: the timeouts are almost equal. Predict split votes: several "candidate for term …" lines within a few milliseconds, sometimes a term with no leader. Count the elections (counter `raft.elections`).
- Partition a follower: `:` then `link n5 s1 down`; watch n5's term grow at every timeout. After some seconds `link n5 s1 up`. Prediction: the healthy leader logs "steps down: a higher term exists" and an election follows although nothing had failed. This is the disruption that PreVote removes.
- Lose the quorum: crash three servers (`node n2 down`, `node n3 down`, `node n4 down`, or `k` on the selected node). Predict what the two survivors can do. If the leader is among the dead, terms climb for ever and nobody is elected; if it survives, check whether it keeps its role and whether anything can commit.

**381-raft-replication.** `./netlab run 381-raft-replication`. Open the `raft` tab of the leader n1 (`nextIndex`/`matchIndex` per follower) and of a follower. Log cells are blue when stored and green when applied. Follow one command in the log at detail `2`: "appended at index 2", four AppendEntries with `prev=1/1`, "committed entry 2 … stored on a majority", and the followers applying it about one heartbeat later. At 6 s n4 crashes and commits continue with four servers; at 12 s n4 restarts with log length 4 and one AppendEntries with `prev=4/1 entries=3` brings it to 7.

- `:` then `n3 raft put color=red` and `n5 raft get color`. Explain why the get may return nothing if issued immediately.
- Crash three servers and submit a command: the cell stays blue. Bring one server back with `node N up` and predict what happens to the pending entry.
- Pause (space), submit `n1 raft put a=1`, advance with `.` until the AppendEntries have left, then crash n1 with `k`. Predict from the up-to-date rule which servers can become leader and whether `a=1` survives; verify with `raft.truncated` and the logs.

## Check yourself

- State agreement, validity and termination, classify each as safety or liveness, and say precisely what FLP forbids and which assumption Raft adds to escape it.
- Prove that two leaders cannot exist in the same term. Which persistent variable does the proof rely on, and what goes wrong if it is written to disk after replying?
- Reconstruct the scenario in which an entry stored on a majority is overwritten, and show how the commitment rule prevents the loss.
- A follower's log is longer than the leader's and differs from index 5 on. Describe the messages exchanged until the logs agree, and argue that no committed entry is deleted.
- Why is reading the leader's local state not linearisable? Give two mechanisms that make reads linearisable and the assumption each needs.
