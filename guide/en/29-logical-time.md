<!-- level: 2 -->
<!-- labs: 290-lamport-clocks -->
# Logical time

On one computer "which happened first?" has an obvious answer: read the clock. In a distributed system there is no such clock. Each machine has its own oscillator, which drifts; synchronisation protocols reduce the error but never to zero; and a message takes an unknown, variable time to arrive. If two machines stamp events with their local clocks, a message can appear to be received before it was sent.

Leslie Lamport's observation (1978) is that what programs actually need is rarely real time. They need an order of events that never contradicts **causality**. He defined the relation **happened-before**: event a happened before event b if a comes before b on the same process, or if a is the sending of a message and b its reception, or through a chain of such steps. If neither a happened before b nor b before a, the events are **concurrent**: no information could flow between them, and no observer inside the system can say which was first.

A **Lamport clock** is a counter per process that respects this relation: count your own events, attach the counter to every message, and on receiving a message jump ahead of the timestamp it carries. Then, whenever a happened before b, the timestamp of a is smaller than that of b. With this simple tool all processes can agree on one order of all events.

<!-- detail: 2 -->
## Why physical clocks are not enough

A quartz oscillator drifts by 10 to 100 parts per million: one to nine seconds per day. NTP keeps computers within milliseconds over the Internet, a tenth of that on a LAN; but events on different machines can be microseconds apart, and a clock corrected by NTP may even step backwards. A timestamp comparison between machines is therefore only meaningful beyond the synchronisation error, and software cannot know that error exactly.

## Happened-before

Write a → b for "a happened before b". The definition:

- If a and b are events of the same process and a occurs first, then a → b.
- If a is the sending of a message and b the reception of that message, then a → b.
- If a → b and b → c, then a → c.

It is a *partial* order: some pairs are not ordered at all. Those pairs are concurrent, written a ∥ b. "a → b" means precisely that a *could have influenced* b.

## Lamport's rules

Each process P keeps an integer L, initially 0.

- Before each local event or send: L = L + 1. A message carries the L of its send event.
- On receiving a message with timestamp T: L = max(L, T) + 1.

The **clock condition** follows: if a → b then L(a) < L(b). On one process the counter only grows; across a message the receiver's value is at least T + 1.

## The lab's trace

Lab 290 has three processes; the link of n3 is slow.

```
time    process  event                         clock
1.0 s   n1       local                         1
1.1 s   n1       local                         2
1.2 s   n1       local                         3
2.0 s   n1       send a to n2                  4
2.05 s  n2       receive a (T=4): max(0,4)+1   5
3.0 s   n2       local                         6
4.0 s   n2       send b to n3                  7
4.27 s  n3       receive b (T=7): max(0,7)+1   8
5.0 s   n3       send c to n1                  9
5.26 s  n1       receive c (T=9): max(4,9)+1   10
6.0 s   n1       local                         11
6.0 s   n3       local                         10
```

n3 had done nothing, yet its first event has clock 8: the message brought the past of n1 and n2 with it. Along the causal chain a, b, c the timestamps only grow: 4, 5, 7, 8, 9, 10.

## What the numbers do not tell you

The converse of the clock condition is false. The last two events are concurrent: n1 and n3 act at 6.0 s without having heard from each other since c. Their clocks are 11 and 10, and 10 < 11 does not mean that n3's event caused, or even preceded, n1's. Likewise n3's local event and n1's reception of c both have clock 10 and are unrelated.

## A total order

Many algorithms need every process to agree on a single sequence. Order events by (L, process identifier): compare timestamps and break ties with the identifier. The order is total and consistent with happened-before, although among concurrent events it is arbitrary. Lamport's mutual exclusion algorithm uses it: requests for a resource are stamped and broadcast, every process keeps them in a queue sorted by (L, id), and a process enters when its own request is first and it has heard something later from everybody. It needs 3(N − 1) messages per entry.

<!-- detail: 3 -->
## In depth

**Vector clocks.** To *detect* concurrency a process must remember what it knows about each of the others. A vector clock (Fidge and Mattern, independently, 1988) is an array V with one entry per process: P_i increments V[i] at each event, sends the whole vector, and on reception takes the component-wise maximum before incrementing its own entry. Then a → b if and only if V(a) ≤ V(b) in every component and they differ; if neither dominates, the events are concurrent. In the lab's vector mode the last two events are [6,3,2] and [4,3,3]: n1 is ahead in the first component, n3 in the third. The cost is a timestamp of size N, and Charron-Bost (1991) proved that no smaller structure can characterise causality in general. Version vectors in replicated stores (Dynamo, Riak) use the same idea to detect conflicting updates.

**What Lamport clocks are enough for.** A total order consistent with causality is sufficient for replicated state machines: if all replicas apply the same commands in the same order they stay identical. This is the true subject of Lamport's paper, and the seed of consensus protocols. The mutual exclusion algorithm, however, assumes that no process fails and no message is lost: one crashed process blocks everybody, since its acknowledgement never arrives. Fault tolerance requires consensus (Paxos, Raft), and the FLP result (Fischer, Lynch and Paterson, 1985) shows that consensus cannot be guaranteed to terminate in a fully asynchronous system with even one crash.

**Limits of logical time.** Logical clocks see only events inside the system. If a user gets a confirmation from one server and then telephones someone who writes to another, the two writes are causally related in the real world but concurrent for the system. To capture such external causality, systems bound physical clock error: Google Spanner's TrueTime exposes time as an interval obtained from GPS and atomic clocks and waits out the uncertainty before committing; hybrid logical clocks (Kulkarni et al., 2014) combine a physical timestamp with a Lamport counter, staying close to real time while preserving the clock condition. "Last writer wins" by wall-clock timestamp, as in Cassandra, silently loses updates when clocks disagree.

**Related tools.** Consistent global snapshots (Chandy and Lamport, 1985) rely on the same notion of causal cut. Physical synchronisation: NTP (RFC 5905), PTP (IEEE 1588) with sub-microsecond accuracy on LANs.

**Reference.** Lamport, "Time, Clocks, and the Ordering of Events in a Distributed System", Communications of the ACM, 1978.

**What hoplab does.** Events are scripted with `clock event` and `clock send`; processes do not fail; the process identifier is the position in the `peers` list.

<!-- detail: 1 -->
## In the lab

**Lab 290: `./hoplab run 290-lamport-clocks`**

- Keep the log (`l`) at detail 2: each line marked ● is an event with the clock after it. Follow the table above and recompute every receive with max(L, T) + 1.
- Open the sequence view (`s`): the three messages a, b, c form a causal chain, and the slow link of n3 is visible as slanted arrows. Select a node (`n`) and open its `clock` tab.
- Open one of the messages in the packets view (`p`) and find the timestamp it carries.
- At 6 s n1 and n3 each have a local event. Decide from the definition whether they are concurrent, then compare their Lamport values.
- Predict, then try: `./hoplab run 290-lamport-clocks mode=vector`. Write the vector of each of the twelve events before looking. Which pairs of events are concurrent? Compare the two events at 6 s yourself, then look at the log: after each event hoplab compares its vector with the latest event of every other process and marks the concurrent ones with ∥ (an outside observer's view, which no process has).
- Predict, then try: `./hoplab run 290-lamport-clocks slow=500ms`. Message b now reaches n3 *after* n3 has sent c. Recompute the whole table: what is the clock of the send of c, of n1's event at 6 s, and of n1's reception of c? Is causality still respected? (The final clock values change with this setting; the lab's expectations are lower bounds that still hold: check them against your table.)
- From the command line add your own events, for example `:n3 clock send n2 text=d` and `:n2 clock event`, predicting each value first.

## Check yourself

- Define happened-before and concurrency. Give an example with two processes and one message containing two concurrent events and two ordered ones.
- Three processes start at 0. P1: local event, send m1 to P2, local event. P2: receive m1, send m2 to P3. P3: local event, receive m2, local event. Assign Lamport timestamps to all events.
- L(a) < L(b). What can you conclude about a and b, and what can you not? What does L(a) = L(b) imply?
- Compute vector clocks for the previous exercise and find a pair of concurrent events whose Lamport timestamps are different.
- How is a total order obtained from Lamport clocks, and why is it not enough, alone, for a fault-tolerant mutual exclusion?
