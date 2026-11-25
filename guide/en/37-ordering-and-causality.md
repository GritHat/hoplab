<!-- level: 3 -->
<!-- labs: 390-causal-broadcast -->
# Ordering and causality: vector clocks and ordered broadcast

In a distributed system there is no global clock and no instantaneous view: each process sees only its own events and the messages it receives, after arbitrary delays. Yet applications need order. A reply must not be shown before the question it answers; replicas must apply the same updates in an order that gives the same state. Physical timestamps cannot provide it: clocks drift, and a message can arrive "before" it was sent according to two unsynchronised clocks.

What can be defined without any clock is **causality**. Event `a` **happened before** `b` if they are on the same process in that order, if `a` is the sending of a message and `b` its receipt, or by transitivity. This relation is only a partial order: events not related by it are **concurrent**, and no observer inside the system can say which came first. The whole subject is the tension between this partial order, which is real, and the total orders that applications would like, which must be constructed and paid for.

The key idea is to make causality computable and then to enforce as much order as needed, and no more. A **vector clock** summarises the causal past of an event in `N` integers, so that causality is decided by comparing vectors. A **broadcast** layer can then delay the delivery of a message until the messages it depends on have been delivered. Stronger guarantees cost more: FIFO order is almost free, causal order costs a vector per message, total order costs as much as consensus.

<!-- detail: 2 -->
## Algorithms and models

**Lamport clocks.** One counter `L` per process: increment at every event; stamp messages; on receipt set `L = max(L, received) + 1`. Then `a → b` implies `L(a) < L(b)`. The converse fails: timestamps cannot tell causality from coincidence. Ordering by `(L, process id)` gives a total order consistent with causality.

**Vector clocks.** Process `i` keeps `V[1..N]`:

```
local event or send at i:   V[i] += 1                (messages carry V)
receive m at i:             V[k] = max(V[k], m.V[k]) for all k;  V[i] += 1

V(a) <= V(b)   iff  V(a)[k] <= V(b)[k] for all k
a -> b         iff  V(a) <= V(b) and V(a) != V(b)
a || b         iff  neither V(a) <= V(b) nor V(b) <= V(a)
```

Vector clocks characterise causality exactly: this is the property Lamport clocks lack.

**Broadcast specifications.** A process **broadcasts** a message; the layer **delivers** it to the application, possibly later than it was received.

- **FIFO**: if a process broadcasts `m` before `m'`, no process delivers `m'` before `m`.
- **Causal**: if the broadcast of `m` happened before the broadcast of `m'`, no process delivers `m'` before `m`. Causal implies FIFO.
- **Total**: if two processes both deliver `m` and `m'`, they deliver them in the same order. Total order is independent of the other two and can be combined with them.

**Causal delivery condition.** Let each process `i` keep `D[1..N]`, where `D[k]` counts the broadcasts of `k` delivered at `i`; a broadcast by `j` carries `m.V` = the sender's `D` with `m.V[j]` incremented. Process `i` may deliver `m` from `j` when

```
m.V[j] == D[j] + 1          (m is the next broadcast of j)
m.V[k] <= D[k]  for k != j  (i has delivered everything j had delivered)
```

Otherwise `m` waits in a **hold-back queue**, re-examined after each delivery. FIFO broadcast is the first line alone, with a single sequence number per sender.

**Total order.** Three constructions: a fixed sequencer that numbers all messages (simple, a single point of failure); Lamport's algorithm (every broadcast is queued by `(timestamp, id)`, acknowledged by all, and delivered when it heads the queue and every process has been heard from with a larger timestamp); consensus on the next message, slot by slot.

**Worked example (the lab).** Three processes. p1 broadcasts the question `q` with `V = [1,0,0]`. p2 delivers it and broadcasts the answer `a` with `V = [1,1,0]`. The link p1–p3 is slow, so `a` reaches p3 first, where `D = [0,0,0]`. Test: `a.V[2] = 1 = D[2] + 1`, fine; but `a.V[1] = 1 > D[1] = 0`: p3 has not delivered something the sender had. `a` is held back. When `q` arrives, `q.V[1] = 1 = D[1] + 1`: delivered, `D = [1,0,0]`; now `a` passes and is delivered at once. FIFO order would not have helped: `q` and `a` come from different senders.

**Cost.**

```
FIFO     1 integer per message, hold-back per sender
causal   N integers per message, hold-back across senders
total    extra rounds or a leader; equivalent to consensus
```

<!-- detail: 3 -->
## In depth

**Correctness of causal delivery.** Safety: if `m → m'` then `m.V <= m'.V`, and the condition forbids delivering `m'` at a process whose `D` does not yet dominate the dependencies of `m'`, which include `m`. Liveness: every held message is eventually delivered, provided every message its sender had delivered eventually arrives. That proviso is essential: causal order must sit on **reliable** broadcast (if any correct process delivers `m`, all do). With a best-effort transport one lost message blocks, for ever, everything that causally follows it.

**How much metadata is necessary.** Charron-Bost proved that characterising causality among `N` processes needs vectors of `N` entries in general: there is no shortcut for the general case. Practical systems reduce the cost by sending only changed entries, by exploiting topology (a tree or a single hub needs less), or by tracking causality of data rather than of messages: version vectors and dotted version vectors in Dynamo-style stores have one entry per replica, not per client. Hybrid logical clocks combine physical time with a Lamport counter in 64 bits and give a causality-consistent order close to real time; Spanner's TrueTime bounds clock uncertainty instead and waits it out.

**Total order is consensus.** Chandra and Toueg showed that total order (atomic) broadcast and consensus are equivalent: each can be built from the other. So total order inherits the FLP impossibility and the need for a quorum and for timing assumptions. Lamport's 1978 construction makes this concrete from the other side: it is not fault-tolerant, since delivery waits for every process, so one crash blocks all.

**Limits of the abstraction.** Cheriton and Skeen argued that message-level causality captures neither semantic dependencies that pass outside the system (a shared database, a human), nor constraints such as "these two updates together", and that the hold-back buffers and vectors do not scale; often the application can order what matters with version numbers of its own. The counter-argument (Birman) is that getting it right once in a layer is safer. Both views survive: group-communication toolkits (Isis, Horus, Spread) offered virtual synchrony with causal and total order; modern systems mostly take total order from a replicated log (Raft, Zab, Kafka partitions) and causality from data-level version vectors or CRDTs.

**In netlab.** The `clock` module's vectors count every event, receptions included, so the vectors on screen are larger than the delivered-count vectors `D` used above; the dependency test is the same. `order=total` is Lamport's construction: it relies on FIFO channels and on every process being alive, and has no failure handling. `order=none` delivers on arrival and counts causality violations after the fact.

**Primary literature.**

- L. Lamport, "Time, Clocks, and the Ordering of Events in a Distributed System", 1978.
- K. Birman, T. Joseph, "Reliable Communication in the Presence of Failures", 1987.
- C. Fidge, "Timestamps in Message-Passing Systems That Preserve the Partial Ordering", 1988.
- F. Mattern, "Virtual Time and Global States of Distributed Systems", 1989.
- K. Birman, A. Schiper, P. Stephenson, "Lightweight Causal and Atomic Group Multicast", 1991.
- B. Charron-Bost, "Concerning the Size of Logical Clocks in Distributed Systems", 1991.
- D. Cheriton, D. Skeen, "Understanding the Limitations of Causally and Totally Ordered Communication", SOSP 1993.
- V. Hadzilacos, S. Toueg, "A Modular Approach to Fault-Tolerant Broadcasts and Related Problems", 1994.
- X. Défago, A. Schiper, P. Urbán, "Total Order Broadcast and Multicast Algorithms: Taxonomy and Survey", 2004.

<!-- detail: 1 -->
## In the lab

`./netlab run 390-causal-broadcast`. Three processes in a triangle; the link n1–n3 is slow (150 ms). The same scene is played twice. Select n3 (`n`), open its `clock` tab, and press `s` for the sequence diagram: the crossing arrows are the whole story. Use the log at detail `2` to read the vectors.

**Round 1 (1 s), no ordering.** n1 broadcasts "what time is it?", n2 delivers it and broadcasts "half past nine". At n3 the answer arrives first and is delivered at once: the log prints "causality violated … it depends on a message not delivered yet, from n1", and `clock.violations` becomes 1. Before looking, write the vector carried by each message and decide by comparison that the question happened before the answer.

**Round 2 (4 s), causal order.** Same exchange. At n3 the log shows "held back «ten o'clock» from n2: a message it depends on has not arrived yet", then the question is delivered and "leaves the hold-back queue" follows immediately. Measure the time the answer spends in the queue (about 40 ms: the difference between the two paths) and note `clock.heldback` = 1.

**Experiment 1: FIFO is not enough.** `./netlab run 390-causal-broadcast order2=fifo`. Predict the outcome before running: the two messages have different senders, so FIFO imposes nothing between them and n3 violates causality again (`clock.violations` = 2: the lab's check `causality` expects exactly that for `none` and `fifo`, and 1 for `causal` and `total`).

**Experiment 2: the price of total order.** `order2=total`. Every process now queues each message ("queued … waiting for every process to acknowledge") and delivers only when all have acknowledged. Measure when n1 delivers its own question: about 300 ms after sending, a round trip on the slow link, against 0 ms before. All three deliver in the same order. Then raise `slow=500ms` and predict the new delivery times for causal and for total.

**Experiment 3: concurrency and a crash.** In a running lab with `order2=causal`, pause (space), enter `:` `n1 clock bcast text=A` and `:` `n3 clock bcast text=B`, and resume: the two are concurrent; check that their vectors are incomparable and whether all processes deliver them in the same order (they need not). Repeat with `order2=total`: now they must. Finally, under total order, `node n3 down` and broadcast from n1: predict what happens to the message, and say why consensus-based total order would behave differently.

## Check yourself

- Prove that V(a) < V(b) if and only if a happened before b, and give two events for which Lamport timestamps are ordered but the events are concurrent.
- State the causal delivery condition and prove that it never delivers a message before one that causally precedes it. What must the underlying transport guarantee for held-back messages to be delivered eventually?
- Give an execution that satisfies FIFO and total order but violates causal order.
- Explain why total order broadcast is equivalent to consensus by sketching both reductions. What does this imply about a fault-tolerant, asynchronous total order broadcast?
- A chat service with a million clients wants replies never to appear before the messages they answer. Why are vector clocks with one entry per client the wrong design, and what would you do instead?
