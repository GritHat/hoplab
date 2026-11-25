<!-- level: 3 -->
<!-- labs: 391-two-phase-commit, 392-commit-blocking -->
# Atomic commit: two-phase commit, blocking and three-phase commit

A distributed transaction updates data held by several independent databases, and it must be **atomic**: either every one of them commits it, or none does. A transfer that debits one bank and never credits the other is the failure to avoid.

The problem looks like consensus but has a twist that makes it harder in one respect. Every participant has a veto: any of them may be unable to commit (a constraint violated, a deadlock, a full disk), so the outcome cannot be decided by a majority; "commit" needs unanimity. And each site is autonomous until it promises: before promising it may abort on its own, after promising it must be able to go either way, even across a crash, until someone tells it the outcome.

The key idea of **two-phase commit** is that promise, made durable. In phase one a coordinator asks everyone to prepare; a participant that votes YES first writes to stable storage everything needed to commit or to abort later, and thereby gives up its right to decide. In phase two the coordinator, alone, decides, writes the decision to its own log (that write is the instant the transaction commits) and announces it. The protocol is safe under any crash or message loss. Its weakness is the **window of uncertainty** between a YES vote and the decision: if the coordinator fails there, the participants must wait, holding their locks. Removing that blocking is possible only with stronger assumptions or with consensus.

<!-- detail: 2 -->
## Algorithms and models

**Specification.** Each process votes YES or NO and decides COMMIT or ABORT.

- **Agreement**: no two processes decide differently, and a decision is never reversed.
- **Validity**: COMMIT only if all voted YES; if all voted YES and nothing fails, the decision is COMMIT.
- **Termination**: if failures are eventually repaired, every process eventually decides. The protocol is **non-blocking** if correct processes decide even while others remain failed.

**Two-phase commit with logging.** Records marked `force` must be on stable storage before the next message leaves.

```
coordinator                        participant
log BEGIN(participants)
send PREPARE to all        ---->   if able:  force-log READY;  send YES
                                   else:     log ABORT;        send NO
collect votes (timeout => NO)
all YES: force-log COMMIT   <== commit point
else:    log ABORT
send decision to all       ---->   force-log decision; apply; release locks
                           <----   send ACK
all ACKs: log END  (repeat decision until then)
```

Cost in the failure-free case for `n` participants: `4n` messages, three message delays until participants know, `2n + 1` forced log writes.

**Timeouts and recovery.** What a process may do depends only on what it has logged:

```
participant, nothing logged, no PREPARE:   abort unilaterally
participant, READY logged, no decision:    UNCERTAIN: ask and wait (blocked)
participant, decision logged:              redo it, ACK
coordinator, waiting for votes:            decide ABORT
coordinator, restart, no decision logged:  ABORT   (presumed abort)
coordinator, restart, decision logged:     resend it
```

**The blocking window.** An uncertain participant cannot abort (the coordinator may have logged COMMIT and told someone) and cannot commit (someone may have voted NO). Asking the other participants helps only if one of them knows the decision or has not voted. If the coordinator and one participant are both down, the others cannot exclude that the dead participant already committed: no termination protocol can unblock them.

**Three-phase commit.** Insert a round so that no process can commit while another is still uncertain:

```
PREPARE / votes  ->  PRE-COMMIT / acks  ->  COMMIT
participant timeout rules:
   voted YES, no PRE-COMMIT:   nobody can have committed          => ABORT
   PRE-COMMIT received:        everybody voted YES and is prepared => COMMIT
```

The rules are safe only if a timeout proves a crash: a synchronous network, crash failures, no partition. Cost: `6n` messages, five delays.

**Worked example (the labs).** Links are 5 ms and 10 ms, so one hop coordinator–participant is 15 ms. PREPARE at `t`, votes back at `t + 30`, COMMIT received at `t + 45`, ACKs at `t + 60` ms: three delays to commit. In lab 392 the coordinator crashes 5 ms after PREPARE and returns after 5 s: the participants vote at `t + 15 ms`, declare themselves blocked after two timeouts (2 s), and release their locks only at `t + 5.045 s`. Lock time equals the outage, however long.

<!-- detail: 3 -->
## In depth

**Why 2PC is safe.** The decision is a single durable fact at a single site, the coordinator's COMMIT record; every other action is a function of logged state and can be repeated. Messages are idempotent and retransmitted. A participant never decides COMMIT without the coordinator's word, and the coordinator never logs COMMIT without all YES votes, each backed by a READY record that survives a crash.

**Why blocking is fundamental.** Skeen's theorem: a commit protocol is non-blocking (under site failures only) if and only if no state is such that one site could be in it while another has committed and a third is still uncertain. 2PC violates the condition; 3PC satisfies it by adding the buffer state. But with message loss or partitions, non-blocking atomic commit is impossible, and in a purely asynchronous system it is unsolvable even with a single crash, like consensus. 3PC shows the failure concretely: if PRE-COMMIT reaches some participants and a partition isolates the others, the first group times out and commits, the second times out and aborts.

**Relation to consensus.** Atomic commit is not consensus (its validity rule depends on all votes and failures bias it towards ABORT), but the remedy is consensus: make the decision itself fault-tolerant. In Paxos Commit (Gray and Lamport) each participant's vote is recorded by a Paxos instance on `2F + 1` acceptors; the transaction commits when every instance has chosen "prepared"; any `F` failures are tolerated and classic 2PC is the special case `F = 0`. Equivalently, replicate the coordinator's log with Raft.

**What deployed systems do.** XA transaction managers implement 2PC with presumed abort (no forced abort record, no ACK for aborts) or presumed commit, and a read-only optimisation that lets participants without updates drop out after phase one; operators resolve in-doubt transactions by hand ("heuristic" decisions, which can violate atomicity). Spanner and CockroachDB run 2PC across shards whose participants and coordinator are each a Paxos or Raft group, so no single machine failure blocks. Other designs avoid distributed commit: deterministic ordering before execution (Calvin), or sagas with compensating actions when isolation can be relaxed. 3PC is essentially never deployed.

**In netlab.** The `twopc` module logs READY before voting and the decision before announcing it, resends the decision until acknowledged and answers queries from in-doubt participants. A participant declares itself blocked after two timeouts. Its `three=1` mode omits the termination protocol that would elect a new coordinator: participants act on timeouts alone. After a restart the coordinator re-announces all the decisions in its log.

**Primary literature.**

- J. Gray, "Notes on Data Base Operating Systems", 1978 (two-phase commit).
- B. Lampson, H. Sturgis, "Crash Recovery in a Distributed Data Storage System", 1979.
- D. Skeen, "Nonblocking Commit Protocols", SIGMOD 1981.
- D. Skeen, M. Stonebraker, "A Formal Model of Crash Recovery in a Distributed System", 1983.
- C. Mohan, B. Lindsay, R. Obermarck, "Transaction Management in the R* Distributed Database Management System", 1986 (presumed abort and presumed commit).
- P. Bernstein, V. Hadzilacos, N. Goodman, "Concurrency Control and Recovery in Database Systems", 1987, chapter 7.
- J. Gray, L. Lamport, "Consensus on Transaction Commit", 2006.
- J. Corbett et al., "Spanner: Google's Globally-Distributed Database", OSDI 2012.

<!-- detail: 1 -->
## In the lab

**391-two-phase-commit.** `./netlab run 391-two-phase-commit`. A coordinator c and participants p1, p2, p3. Open the `twopc` tab of c and of one participant: each shows its write-ahead log. Use the sequence view (`s`) for the two phases. Three transactions: `pay1` commits (measure the delays: votes 30 ms after PREPARE, commit known 45 ms after); `pay2` aborts because p2 votes NO; `pay3` starts while p3 is down, so the coordinator waits one timeout (1 s) and aborts, then repeats the decision every second until p3, restarted at 8 s, acknowledges. In each log check the order: READY before the vote, the decision before its announcement.

- `loss=0.3` (on p1's link). Predict the possible outcomes for each transaction: a lost PREPARE or vote becomes an ABORT by timeout; a lost decision or ACK only causes "sending it again". The counters of committed and aborted transactions change from run to run; the participants never disagree.
- `timeout=20ms`: shorter than the 30 ms round trip. Every transaction aborts although nobody failed. State the rule for choosing the timeout.
- In a running lab: `:` then `p1 twopc vote=no` and `c twopc begin tx=x1`; then crash a participant after it has voted (pause with space, step with `.`, select it and press `k`) and read what it does on restart.

**392-commit-blocking.** `./netlab run 392-commit-blocking`. Transaction `t2` (2PC): the coordinator crashes 5 ms after PREPARE. The participants log READY, vote, and two seconds later print "BLOCKED — voted yes and heard nothing", then query the coordinator every second. At 8 s the coordinator restarts, finds no decision, aborts, and the log reports "unblocked; locks were held for 5.0 s" (counter `twopc.blocked`). Transaction `t4` (3PC, `three=1`) repeats the crash: after the timeout each participant aborts by itself, "no PRE-COMMIT arrived, so nobody can have committed".

- `outage=8s` (not longer: the next transaction starts at 12 s): predict the lock time for 2PC (8 s) and for 3PC (unchanged, about 2 s).
- `crash=60ms`: the coordinator now crashes after deciding and announcing. Predict: `t2` commits with no blocking; in `t4` the participants have received PRE-COMMIT and commit on timeout. Then `crash=20ms`: the votes arrive at a dead coordinator; 2PC blocks as before.
- Breaking 3PC by hand. Pause shortly after 14 s and step (`.`) until the coordinator logs "PRE-COMMIT sent"; before it is delivered, enter `:` `link p3 s1 down` and `:` `node c down`, then resume. Prediction: p1 and p2 receive PRE-COMMIT and commit after their timeout, p3 never receives it and aborts. Atomicity is violated: a partition is not a crash.

## Check yourself

- For each state of a 2PC participant and of the coordinator, give the action on timeout and on restart, and justify it from the log contents. Which log writes must be forced, and what breaks if the READY record is written lazily?
- Describe precisely the failure pattern in which the surviving participants of 2PC cannot terminate even by consulting each other.
- State Skeen's condition for non-blocking commit and show that 3PC meets it while 2PC does not. Then give an execution in which 3PC violates agreement, and name the assumption that was broken.
- Compare atomic commit and consensus: specification, what a failed process's vote means, and solvability in asynchronous systems. How does Paxos Commit use consensus, and why is 2PC its F = 0 case?
- A system runs 2PC over shards that are each replicated with Raft. Which failures still block a transaction, and for how long?
