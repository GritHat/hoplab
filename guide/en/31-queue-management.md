<!-- level: 3 -->
<!-- labs: 371-bufferbloat-codel, 372-red-ecn, 373-fair-queueing, 374-token-bucket -->
# Queue management: AQM, fair queueing and shaping

Every output port of a router has a buffer, and two decisions are taken there: which packet leaves next (**scheduling**) and which packets are dropped or marked, and when (**queue management**). The default answer, a drop-tail FIFO, is the simplest possible and fails in three ways. A large buffer under persistent load stays full, so every packet waits behind hundreds of others and latency rises to seconds with no gain in throughput (**bufferbloat**). The only congestion signal is overflow, which hits many flows in the same instant, so they back off and ramp up together (**global synchronisation**). And one FIFO offers no isolation: a flow that does not slow down takes the buffer from those that do.

The difficulty is that a queue serves two purposes that look identical from outside: absorbing a short burst is good, a standing queue is pure delay. A static threshold on length cannot tell them apart, and the right length depends on link rate, round-trip times and number of flows.

The key ideas are three. **Active queue management** signals congestion early, to few flows at a time, and judges the queue by how long it persists (RED on an averaged length, CoDel and PIE on delay). **Fair queueing** gives each flow its own queue and serves them in turn. **Token buckets** bound what a flow may send: a long-term rate plus a burst.

<!-- detail: 2 -->
## Algorithms and models

**RED** keeps an exponentially weighted average of the queue length, updated at each arrival, and drops (or marks) arrivals with a probability that grows linearly between two thresholds:

```
avg = (1 - wq)*avg + wq*q
avg < min:         p = 0
min <= avg < max:  pb = maxp*(avg - min)/(max - min);  p = pb/(1 - count*pb)
avg >= max:        p = 1   (gentle: from maxp to 1 between max and 2*max)
```

`count` is the number of packets since the last drop: it spaces the drops uniformly. The average has a memory of about `1/wq` arrivals, which is what lets bursts through.

**ECN** (RFC 3168): a packet whose IP header says ECT may be marked CE instead of dropped; the receiver echoes the mark (ECE) and the sender reduces its window as for a loss, then confirms with CWR.

**CoDel** measures the sojourn time of each packet (timestamp at enqueue, read at dequeue) and controls its minimum over a window:

```
if sojourn < target (5 ms) or queue < 1 MTU:  stop dropping
if sojourn >= target for a whole interval (100 ms):  start dropping, drop head
while dropping:  next_drop = now + interval/sqrt(count);  count++ per drop
```

The drop frequency grows as the square root of the count because TCP throughput is proportional to `1/sqrt(p)`: the control law makes the offered load decrease roughly linearly in time. `interval` should be about a worst-case RTT; nothing depends on the link rate.

**PIE** estimates the queueing delay (queue length divided by the measured departure rate) and every `tupdate` adjusts a drop probability with a proportional-integral controller:

```
p = p + alpha*(delay - target) + beta*(delay - delay_old)
```

**Deficit round robin** approximates fair queueing in O(1) per packet. Each flow `i` has a queue, a quantum `Q` and a deficit counter `D`:

```
each round, for every backlogged flow:  D += Q
    while head packet size <= D:  send it;  D -= size
    if the queue is empty:  D = 0
```

If `Q` is at least one MTU, the byte counts of any two backlogged flows over any interval differ by at most `Q + 2*MTU`. The allocation is **max-min fair**: flows that ask less than the equal share get what they ask, the remainder is divided equally among the others.

**Token bucket** with rate `r` and depth `b`: tokens accumulate at `r` up to `b`; a packet of length `L` leaves only by spending `L` tokens. The output in any interval of length `t` is at most `b + r*t`. A source at peak rate `R > r` passes undelayed for `b/(R - r)` seconds.

**Worked examples (the labs).** Bufferbloat: 200 packets of about 540 bytes on a 1 Mbit/s link take `200*540*8/1e6`, about 0.87 s, to drain: that is the delay of every packet once the FIFO is full. Fair queueing: on 1 Mbit/s with demands 3 Mbit/s and 100 kbit/s the equal share is 500 kbit/s; the mouse gets its 100, the hog the remaining 900. In a FIFO both would lose the same fraction, `(3.1 - 1)/3.1`, about 68%. Token bucket: with `b` = 10000 bytes, `r` = 500 kbit/s and a 2 Mbit/s source the burst lasts `80000/(1.5e6)`, about 53 ms; then the output is 500 kbit/s and the 100-packet queue adds up to 0.87 s.

<!-- detail: 3 -->
## In depth

**Why length is the wrong variable.** A good queue is one that empties within an RTT; a bad queue is one whose minimum over an RTT stays above zero. CoDel tracks exactly that minimum, in time units, so its two constants work from kbit/s to Gbit/s. RED's thresholds are in packets and its behaviour depends on `wq`, `maxp` and the load: too little averaging and it reacts to bursts, too much and it oscillates. This sensitivity, documented by many measurement papers, is why RED was standardised and then rarely enabled.

**AQM needs a responsive sender.** The control loop closes through the transport. Against a constant-rate source the queue still overflows the AQM's intentions: CoDel's square-root law accelerates slowly and the delay settles well above the target; RED's average passes `max` and everything is dropped. The labs here use constant-rate UDP on purpose, to show that AQM bounds delay but cannot remove a loss that is arithmetic. Isolation from unresponsive flows needs scheduling, not AQM.

**Trade-offs.** Head drop (CoDel) delivers the signal one queue-time sooner than tail drop. ECN removes the loss but must fall back to dropping under heavy overload, or a non-reacting ECT flow would be favoured. Fair queueing costs per-flow state; stochastic FQ hashes flows into a fixed number of queues. Strict fairness between flows is not fairness between users, who may open many flows. A shaper delays the excess, a policer drops it: shaping needs a buffer and adds delay, policing interacts badly with TCP bursts.

**Buffer sizing.** The classic rule is one bandwidth-delay product, `C*RTT`; with `n` desynchronised long flows `C*RTT/sqrt(n)` suffices (Appenzeller et al.).

**What deployed systems do.** `fq_codel` (RFC 8290), the Linux default, runs CoDel inside each DRR queue and gives priority to new flows; CAKE adds shaping and per-host fairness; DOCSIS 3.1 modems use PIE; data centres use DCTCP with a single instantaneous marking threshold; L4S (RFC 9330) defines a dual queue for scalable congestion controls.

**In hoplab.** `fq` takes flows from the IP 5-tuple with a single shared buffer, dropping from the longest queue when full; `tbf` runs above the link rate; RED thresholds are in packets. The sources in these four labs do not react to drops or marks.

**Primary literature.**

- J. Turner, "New Directions in Communications (or Which Way to the Information Age?)", 1986 (the leaky bucket).
- A. Demers, S. Keshav, S. Shenker, "Analysis and Simulation of a Fair Queueing Algorithm", SIGCOMM 1989.
- A. Parekh, R. Gallager, "A Generalized Processor Sharing Approach to Flow Control in Integrated Services Networks", 1993.
- S. Floyd, V. Jacobson, "Random Early Detection Gateways for Congestion Avoidance", 1993.
- M. Shreedhar, G. Varghese, "Efficient Fair Queuing Using Deficit Round Robin", SIGCOMM 1995.
- G. Appenzeller, I. Keslassy, N. McKeown, "Sizing Router Buffers", SIGCOMM 2004.
- J. Gettys, K. Nichols, "Bufferbloat: Dark Buffers in the Internet", 2011.
- K. Nichols, V. Jacobson, "Controlling Queue Delay", 2012.
- R. Pan et al., "PIE: A Lightweight Control Scheme to Address the Bufferbloat Problem", 2013.
- RFC 3168 (ECN), RFC 7567 (AQM recommendations), RFC 8289 (CoDel), RFC 8033 (PIE), RFC 8290 (FQ-CoDel).

<!-- detail: 1 -->
## In the lab

**371-bufferbloat-codel.** `./hoplab run 371-bufferbloat-codel`. Two identical paths with 1.3 Mbit/s offered to a 1 Mbit/s link: a1–ra–a2 with a 200-packet FIFO, b1–rb–b2 with CoDel. Open the `ping` tab of a1 and of b1 (graph `ping rtt (ms)`), the `qdisc` tab of ra and rb (queue and wait graphs), and at the end the `sink` tab of a2 and b2. Measure: ping on A climbs towards 0.9 s, on B it stays around 0.1 s; both sinks receive about the same number of datagrams. Predict before trying:

- `buffer=20`: the FIFO delay is bounded by `20*540*8/1e6`, about 90 ms. A small buffer hides the problem.
- `load=900k`: no overload, CoDel never drops and the two paths are identical (about 12 ms).
- `aqm=pie`, then `aqm=red`: PIE holds the delay lower than CoDel against this unresponsive source (tens of ms); RED with its default thresholds on a 200-packet buffer does much worse. Explain both from the control laws.

**372-red-ecn.** `./hoplab run 372-red-ecn`. On r1 open the `qdisc` tab: graphs `queue` and `avg queue`, the drop probability, counters `red.early`, `red.forced`, `red.marks`. In the packets view (`p`) find a datagram of h1 after r1 and read the ECN field (CE). Experiments: `wq=0.002` (the average needs about 500 arrivals, over a second, to follow: predict more marks, because it lingers between `min` and `max`, and a later reaction); `wq=0.2` (the average is nearly the instantaneous queue); `ecn=0` (marks become drops: `red.marks` is 0); `gentle=0` (above `max` every arrival is a forced drop: `red.forced` grows, `red.early` shrinks).

**373-fair-queueing.** `./hoplab run 373-fair-queueing`. On r1 the `qdisc` tab shows the backlog per flow; sinks d1 (hog) and d2 (mouse) show loss and delay. Expected: the mouse loses nothing and waits about 15 ms. Then `qdisc=droptail` (prediction: the mouse loses about 68%, like the hog, and inherits its delay), `mouse=600k` (above the fair share: both are held to about 500 kbit/s and both lose), `quantum=300` (finer interleaving, slightly lower mouse delay).

**374-token-bucket.** `./hoplab run 374-token-bucket`. On r1 watch `tokens (B)` and `queue`; on h2 the `sink` tab, graph `rx (kbit/s)`. Three phases: 300 kbit/s (bucket full, no wait), 2 Mbit/s for 4 s (burst, then clamp to 500 kbit/s, queue full, loss), 300 kbit/s again. Compute first how many datagrams pass in the second phase (burst + 4 s at the shaping rate + one queue). Experiments: `bucket=100000` (predict the extra packets delivered: about 90000 bytes more of burst), `shape=1M`, `queue=500` (less loss, but a delay of `500*540*8/5e5`, over 4 s).

## Check yourself

- CoDel's next drop is scheduled at `interval/sqrt(count)`. Derive why a square root, starting from the TCP throughput relation, and say what happens to that argument when the traffic is constant-rate UDP.
- RED computes `p = pb/(1 - count*pb)`. What distribution of the gap between drops does this produce, compared with using `pb` directly, and why does it matter for synchronisation?
- State the max-min fair allocation for demands 0.1, 0.4, 0.8 and 2 Mbit/s on a 2 Mbit/s link, and give the DRR fairness bound in bytes for a quantum of 1514.
- A flow is constrained by a token bucket (r, b) and crosses a link of rate R ≥ r served FIFO with no other traffic. Bound its backlog and its delay. What changes if the flow first passes a shaper at peak rate P?
- Why does an ECN-marking AQM have to start dropping ECT packets beyond some level of congestion? Construct the attack that the alternative would allow.
