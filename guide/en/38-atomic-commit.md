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
coordinator silent: a participant takes over, asks the others' state and decides
   somebody ABORTED or never voted          => ABORT
   somebody COMMITTED                       => COMMIT
   somebody PRE-COMMITTED, nobody decided   => PRE-COMMIT to all, then COMMIT
   everybody uncertain (only voted YES)     => ABORT: nobody can have committed
```

The rules are safe only if a timeout proves a crash: a synchronous network, crash failures, no partition. Cost: `6n` messages, five delays.

**Worked example (the labs).** Links are 5 ms and 10 ms, so one hop coordinator–participant is 15 ms. PREPARE at `t`, votes back at `t + 30`, COMMIT received at `t + 45`, ACKs at `t + 60` ms: three delays to commit. In lab 392 the coordinator crashes 5 ms after PREPARE and returns after 5 s: the participants vote at `t + 15 ms`, declare themselves blocked after two timeouts (2 s), and release their locks only at `t + 5.045 s`. Lock time equals the outage, however long.

<!-- detail: 3 -->
## In depth

**Why 2PC is safe.** The decision is a single durable fact at a single site, the coordinator's COMMIT record; every other action is a function of logged state and can be repeated. Messages are idempotent and retransmitted. A participant never decides COMMIT without the coordinator's word, and the coordinator never logs COMMIT without all YES votes, each backed by a READY record that survives a crash.

**Why blocking is fundamental.** Skeen's theorem: a commit protocol is non-blocking (under site failures only) if and only if no state is such that one site could be in it while another has committed and a third is still uncertain. 2PC violates the condition; 3PC satisfies it by adding the buffer state. But with message loss or partitions, non-blocking atomic commit is impossible, and in a purely asynchronous system it is unsolvable even with a single crash, like consensus. 3PC shows the failure concretely: if PRE-COMMIT reaches some participants and a partition isolates the others, the first group times out and commits, the second times out and aborts.

**Relation to consensus.** Atomic commit is not consensus (its validity rule depends on all votes and failures bias it towards ABORT), but the remedy is consensus: make the decision itself fault-tolerant. In Paxos Commit (Gray and Lamport) each participant's vote is recorded by a Paxos instance on `2F + 1` acceptors; the transaction commits when every instance has chosen "prepared"; any `F` failures are tolerated and classic 2PC is the special case `F = 0`. Equivalently, replicate the coordinator's log with Raft.

**What deployed systems do.** XA transaction managers implement 2PC with presumed abort (no forced abort record, no ACK for aborts) or presumed commit, and a read-only optimisation that lets participants without updates drop out after phase one; operators resolve in-doubt transactions by hand ("heuristic" decisions, which can violate atomicity). Spanner and CockroachDB run 2PC across shards whose participants and coordinator are each a Paxos or Raft group, so no single machine failure blocks. Other designs avoid distributed commit: deterministic ordering before execution (Calvin), or sagas with compensating actions when isolation can be relaxed. 3PC is essentially never deployed.

**In hoplab.** The `twopc` module logs READY before voting and the decision before announcing it, repeats PREPARE a few times within the timeout, resends the decision until acknowledged and answers queries from in-doubt participants. A 2PC participant declares itself blocked after two timeouts. With `three=1` the participants run the termination protocol: the first whose timer fires takes over, collects the states and applies the rules above; a restarted coordinator makes the same inquiry instead of trusting its log. The counter `twopc.contradicted` reveals a participant told the opposite of what it decided.

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

**391-two-phase-commit.** `./hoplab run 391-two-phase-commit`. A coordinator c and participants p1, p2, p3. Open the `twopc` tab of c and of one participant: each shows its write-ahead log. Use the sequence view (`s`) for the two phases. Three transactions: `pay1` commits (measure the delays: votes 30 ms after PREPARE, commit known 45 ms after); `pay2` aborts because p2 votes NO; `pay3` starts while p3 is down, so the coordinator waits one timeout (1 s) and aborts, then repeats the decision every second until p3, restarted at 8 s, acknowledges. In each log check the order: READY before the vote, the decision before its announcement.

- `loss=0.3` (on p1's link, from 0.5 s on). Predict the possible outcomes for each transaction: a lost PREPARE or vote is asked again within the timeout and becomes an ABORT only if it keeps failing; a lost decision or ACK only causes "sending it again". The number of commits may change from run to run; the participants never disagree: the checks ask for `twopc.ended == 3` on c and `twopc.contradicted == 0` everywhere.
- `timeout=20ms`: shorter than the 30 ms round trip. Every transaction aborts although nobody failed. State the rule for choosing the timeout.
- In a running lab: `:` then `p1 twopc vote=no` and `c twopc begin tx=x1`; then crash a participant after it has voted (pause with space, step with `.`, select it and press `k`) and read what it does on restart.

**392-commit-blocking.** `./hoplab run 392-commit-blocking`. After a normal 2PC (`t1`) and a normal 3PC (`t2`), transaction `t3` (3PC) loses its coordinator 5 ms after PREPARE: two seconds later p1 logs "3PC termination: the coordinator is silent, I take over", finds everybody uncertain and aborts for all; at 11 s the coordinator restarts, asks the participants and adopts their decision. Transaction `t4` (2PC, 14 s) repeats the crash: the participants print "BLOCKED — voted yes and heard nothing" and query the coordinator every second until it restarts after `outage`, finds no decision and aborts; the log reports "unblocked; locks were held for 5.0 s" (counter `twopc.blocked`).

- `outage=20s` (up to about 40 s): predict the lock time of the 2PC transaction (the outage itself) and observe that the 3PC one was settled in about three seconds whatever the outage.
- `crash=60ms`: the coordinator now crashes after it has the votes. Predict: `t4` commits with no blocking (the decision was already announced); in `t3` the participants are pre-committed and the takeover commits instead of aborting. Then `crash=20ms`: the votes arrive at a dead coordinator; 2PC blocks as before.
- Breaking 3PC with a partition: `crash=45ms cable=down`. 40 ms after the PREPARE of `t3` the cable of p3 is cut, just before PRE-COMMIT reaches it. Prediction: p1 and p2 are pre-committed and commit; p3, alone and uncertain, aborts; when the cable returns p3 logs "ATOMICITY VIOLATED" and `twopc.contradicted` is 1 on p3. A partition is not a crash.

## Check yourself

- For each state of a 2PC participant and of the coordinator, give the action on timeout and on restart, and justify it from the log contents. Which log writes must be forced, and what breaks if the READY record is written lazily?
- Describe precisely the failure pattern in which the surviving participants of 2PC cannot terminate even by consulting each other.
- State Skeen's condition for non-blocking commit and show that 3PC meets it while 2PC does not. Then give an execution in which 3PC violates agreement, and name the assumption that was broken.
- Compare atomic commit and consensus: specification, what a failed process's vote means, and solvability in asynchronous systems. How does Paxos Commit use consensus, and why is 2PC its F = 0 case?
- A system runs 2PC over shards that are each replicated with Raft. Which failures still block a transaction, and for how long?
