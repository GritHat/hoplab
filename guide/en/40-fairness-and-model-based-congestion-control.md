<!-- level: 4 -->
<!-- labs: 420-tcp-fairness, 421-bbr-anatomy -->
# Fairness and model-based congestion control

Congestion control was designed for a network in which every sender ran the same algorithm. Under that assumption AIMD has a clean property: flows that see the same loss signal converge to equal windows. The Internet of today violates the assumption in every direction. Reno, CUBIC and BBR coexist on the same bottlenecks; round-trip times differ by two orders of magnitude; buffers range from a fraction of a bandwidth-delay product to seconds of queue. The question of this topic is: **when heterogeneous controllers share a bottleneck, who gets what, and by which definition is the outcome "fair"?**

It is still open for three reasons. There is no agreed fairness objective (equal rates, max-min, proportional, or "do no more harm than the incumbent"). The outcome depends on parameters the end hosts cannot observe, notably the buffer size and the queue discipline. And model-based controllers such as BBR replace the loss signal with an explicit model of the path, which changes the dynamics from a shared feedback loop to a set of estimators that perturb each other.

The lab puts two flows on one bottleneck and lets you choose each flow's algorithm, the buffer and the RTT. It is a small, deterministic experiment: useful to form hypotheses, not to settle them.

<!-- detail: 2 -->
## Fairness definitions

- **Jain's index** for rates x1..xn is J = (sum of xi)^2 / (n times the sum of xi^2). It is 1 when all rates are equal and 1/n when one flow takes everything. It measures equality, not efficiency, and it is blind to why a flow is slow (a flow limited elsewhere drags the index down).
- **Max-min fairness**: an allocation in which no rate can be increased without decreasing a rate that is already smaller or equal. It is what per-flow fair queueing enforces at a single bottleneck.
- **Proportional fairness** (Kelly): the allocation that maximises the sum of log(xi). On multi-bottleneck paths it gives less to flows that consume more resources; an idealised AIMD approximates a related weighted objective.
- **TCP-friendliness**: a flow should not take more than a Reno flow would under the same conditions. Reno's rate follows the square-root law, rate ≈ (MSS/RTT) · sqrt(3/2) / sqrt(p), so "friendly" is itself RTT-biased.

## Loss-based controllers

Reno increases the window by one segment per RTT and halves it on loss. Because the increase is per RTT, two Reno flows with synchronised losses obtain rates in a ratio between RTT2/RTT1 and (RTT2/RTT1)^2: the short path wins. CUBIC makes the window a cubic function of the real time since the last loss, W(t) = C(t − K)^3 + Wmax with C = 0.4 and a decrease factor of 0.7; growth is independent of the RTT, which reduces (but does not remove) RTT unfairness and makes CUBIC more aggressive than Reno when the bandwidth-delay product is large. On small windows CUBIC falls back to a "TCP-friendly region" in which it emulates Reno, so on slow, short paths the two behave alike.

## BBR's model

BBR (v1) does not treat loss as a signal. It estimates two quantities: **BtlBw**, a windowed maximum of the measured delivery rate, and **RTprop**, a windowed minimum of the RTT. It paces at gain × BtlBw and caps the data in flight at about 2 × BtlBw × RTprop. A startup phase with gain 2.89 finds the bottleneck rate, a drain phase empties the queue it built, and then an eight-phase gain cycle (1.25, 0.75, 1, 1, 1, 1, 1, 1) probes for more bandwidth; periodically PROBE_RTT shrinks the window to refresh RTprop.

The coexistence problems follow from the model. In a shallow buffer BBR ignores the losses its probing causes while loss-based flows keep halving: BBR takes most of the link and the retransmission rate rises. In a deep buffer a loss-based flow fills the queue, BBR's in-flight cap (computed from a now stale RTprop) binds, and BBR's share is determined by that cap rather than by any fairness rule. Several BBR flows with different RTTs do not converge to equal shares either. BBRv2 and v3 add loss and ECN as bounds on the model to reduce this.

## What ECN and L4S change

Classic ECN (RFC 3168) lets an AQM mark instead of drop, with a mark treated like a loss. L4S (RFC 9330–9332) redefines the signal: a shallow-threshold marking whose frequency, not mere presence, encodes congestion, with scalable controllers (DCTCP-like, TCP Prague) and a dual-queue coupled AQM that isolates them from classic flows while coupling the marking probabilities so that rates remain roughly comparable. Fairness thus moves from the end hosts into an agreement between the hosts and the bottleneck.

<!-- detail: 3 -->
## Open problems

**What is the deployment bar?** Ware et al. argue that Jain's index and TCP-friendliness are the wrong tests for a new algorithm and propose a harm-based criterion: a newcomer should harm the incumbent no more than another incumbent would. There is no consensus, and no test suite everyone accepts.

**Is starvation unavoidable?** Arun, Alizadeh and Balakrishnan (2022) show that every delay-bounding end-to-end controller in a broad class can starve some flow when paths have non-congestive delay variation. Whether in-network support (fair queueing, L4S) is necessary rather than merely convenient is an open design question.

**Model fidelity.** BBR's behaviour against loss-based flows is governed by its in-flight cap, as the model of Ware et al. (2019) shows, and by estimator dynamics (aggregation, PROBE_RTT, app-limited samples) that small models tend to miss.

## The lab's model versus reality

Read `./netlab help tcp` before drawing conclusions. This TCP has SACK with RFC 6675 recovery (and a RACK-style check for lost retransmissions) and delayed ACKs, but **no timestamps, no tail-loss probe, no proportional rate reduction and no HyStart**: slow start overshoots as far as the buffer lets it, and a flow squeezed to a few segments recovers its losses by timeout (count the timeouts of CUBIC against BBR with `queue=10`). The `bbr` is BBRv1 as in the 2016 paper and draft-cardwell-iccrg-bbr-congestion-control-00: delivery-rate samples per ACK with application-limited marking, a ten-round maximum for BtlBw, a ten-second minimum for RTprop, STARTUP, DRAIN, the eight-phase PROBE_BW cycle, PROBE_RTT, pacing, an in-flight cap of twice the estimated BDP and packet conservation in recovery; only the long-term (policer) sampling is missing. Lab `421-bbr-anatomy` shows the state machine on one flow. Published measurements find that BBRv1 favours the longer-RTT flow: here `cc1=bbr cc2=bbr delay2=80ms` gives the long-RTT flow about four times the rate of the other, the same sign. It is BBRv1, not v2 or v3: none of the loss- and ECN-based bounds that later versions added.

Two further threats. First, phase effects: the two flows start within a random half second of each other (`jitter=500ms`), so different seeds give different runs, but once they are synchronised by a drop-tail queue the pattern of who loses at each overflow can persist, as described by Floyd and Jacobson, and a small change of delay can flip the outcome: the RTT sweep of experiment 1 is not monotone. Second, scale: the steady-state rates are measured over the thirty seconds in which both flows are past start-up, which removes the head-start confound, but two flows and one bottleneck are far from the many-flow regime in which fairness results are usually stated.

## Primary literature

- D.-M. Chiu, R. Jain, "Analysis of the Increase and Decrease Algorithms for Congestion Avoidance in Computer Networks", Computer Networks and ISDN Systems, 1989.
- R. Jain, D.-M. Chiu, W. Hawe, "A Quantitative Measure of Fairness and Discrimination for Resource Allocation in Shared Computer Systems", DEC Research Report TR-301, 1984.
- F. Kelly, A. Maulloo, D. Tan, "Rate Control for Communication Networks: Shadow Prices, Proportional Fairness and Stability", Journal of the Operational Research Society, 1998.
- M. Mathis, J. Semke, J. Mahdavi, T. Ott, "The Macroscopic Behavior of the TCP Congestion Avoidance Algorithm", ACM CCR, 1997.
- S. Floyd, V. Jacobson, "On Traffic Phase Effects in Packet-Switched Gateways", Internetworking: Research and Experience, 1992.
- S. Ha, I. Rhee, L. Xu, "CUBIC: A New TCP-Friendly High-Speed TCP Variant", ACM SIGOPS Operating Systems Review, 2008; RFC 9438 (2023).
- N. Cardwell, Y. Cheng, C. S. Gunn, S. H. Yeganeh, V. Jacobson, "BBR: Congestion-Based Congestion Control", ACM Queue, 2016.
- M. Hock, R. Bless, M. Zitterbart, "Experimental Evaluation of BBR Congestion Control", IEEE ICNP, 2017.
- R. Ware, M. Mukerjee, S. Seshan, J. Sherry, "Modeling BBR's Interactions with Loss-Based Congestion Control", ACM IMC, 2019.
- R. Ware, M. Mukerjee, S. Seshan, J. Sherry, "Beyond Jain's Fairness Index: Setting the Bar for the Deployment of Congestion Control Algorithms", ACM HotNets, 2019.
- V. Arun, M. Alizadeh, H. Balakrishnan, "Starvation in End-to-End Congestion Control", ACM SIGCOMM, 2022.
- M. Alizadeh et al., "Data Center TCP (DCTCP)", ACM SIGCOMM, 2010.
- RFC 5681 (TCP congestion control), RFC 3168 (ECN), RFC 9330, 9331, 9332 (L4S architecture, signalling, DualQ coupled AQM).

<!-- detail: 1 -->
## In the lab

Run `./netlab run 420-tcp-fairness`. Topology: h1 and h2 send through r1 — r2 (4 Mbit/s, 15 ms, 40-packet drop-tail queue) to d1 and d2. Both flows start at 1 s plus a random offset of up to half a second and send for 40 s. Parameters: `cc1`, `cc2` (reno, tahoe, newreno, cubic, bbr), `queue`, `bw`, `delay`, `delay2`. The base RTT of flow 1 is about 38 ms, so one bandwidth-delay product is about 13 packets.

Watch the cwnd graphs of h1 and h2 and the final lines: each `done` line gives the whole-transfer goodput and the **steady** rate (bytes acknowledged after the first ten seconds, divided by the time), then one line gives Jain's index over the two steady rates. For batch work run headless and keep only those lines:

```
./netlab run 420-tcp-fairness cc1=bbr cc2=cubic queue=10 --headless --detail 1 --lang en | grep -E 'done|Jain'
```

The counters `tcp.steady_kbps`, `tcp.share_pct`, `tcp.jain_pct`, `tcp.goodput_kbps`, `tcp.retransmits` and `tcp.timeouts` exist on h1 and h2. Use the steady rate, not the goodput, for shares; repeat every point with at least five seeds (`--seed 1` … `--seed 5`) and report the spread.

**Experiment 1 — RTT unfairness.** Hypothesis: with the same loss-based algorithm, the share of a flow falls as its RTT grows, with an exponent between 1 and 2. Manipulation: `cc1=reno cc2=reno` with `delay2=2ms`, `10ms`, `20ms`, `40ms`, `80ms`; repeat with `cc1=cubic cc2=cubic` and with `cc1=bbr cc2=bbr`. Metric: ratio of the two steady rates against the RTT ratio. Expected: for Reno a noisy, non-monotone curve (in our runs flow 2 had 0.6 of flow 1's rate at 10 ms, 1.4 at 20 ms, 0.3 at 40 ms and 0.7 at 80 ms): with two flows on a drop-tail queue you are measuring phase as much as RTT, so average over seeds and neighbouring delays before fitting an exponent, or randomise the drops with `link r1 r2 qdisc=red` at the `:` prompt. For BBR the sign is reversed: the long-RTT flow wins (about 3.1 against 0.7 Mbit/s at 80 ms), because its larger BDP estimate lets it keep more data in the queue.

**Experiment 2 — buffer depth decides between BBR and CUBIC.** Hypothesis: BBR dominates in a shallow buffer and is dominated in a deep one. Manipulation: `cc1=bbr cc2=cubic` and the reverse `cc1=cubic cc2=bbr`, each with `queue=10`, `40`, `200`. Metric: steady rates, Jain's index, retransmissions and timeouts of each flow. Expected: at `queue=10` (less than one BDP) BBR takes about 3.6 of the 3.9 Mbit/s and CUBIC, cut at every one of the overflows that BBR ignores, lives on timeouts (about 70 of them), J about 0.6; at `queue=40` the shares are within a factor of two, CUBIC ahead (J about 0.95); at `queue=200` it is CUBIC that takes 3.4 Mbit/s, because it fills the buffer and BBR's cap of twice its BDP estimate leaves it a small part of that queue. Swapping `cc1` and `cc2` changes nothing beyond noise now that both start together.

**Experiment 3 — fairness from the network.** Hypothesis: per-flow fair queueing makes the split independent of the algorithms. Manipulation: rerun experiment 2 with `queue=200` and, as soon as the lab opens (pause it with the space bar first), type `link r1 r2 qdisc=fq`; then try `qdisc=codel`. Metric: as above, plus the RTT graphs. Expected: with `fq` near-equal sharing (J about 1.0) whatever the two algorithms, though CUBIC still overflows its own queue; with `codel` a short queue, but shares that may still depend on the algorithm and on who started first.

**Experiment 4 — the model, one flow at a time.** Before trusting what BBR does against another flow, watch it alone: `./netlab run 421-bbr-anatomy`. Hypothesis: BtlBw follows a drop of the bottleneck rate only after ten round trips, and a rise at 25% per probing cycle. Manipulation: the lab halves the bottleneck at 14 s and restores it at 22 s; read the `bbr btlbw`, `pacing` and `rtt` series in the Graphs view of h1 and the state transitions in the log at detail 2. Metric: time from each change to the new BtlBw, and the queue of r1 meanwhile. Expected: STARTUP ends after about 0.6 s, PROBE_RTT appears every ten seconds, the estimate takes about half a second (ten round trips) to come down while the queue grows, and a few seconds of 1.25 probes to climb back. With `queue=5` STARTUP loses packets and BBR does not slow down for it.

## Questions for discussion

- Jain's index over which interval, and over which population of flows? Construct two measurement windows of the same run that give opposite conclusions.
- If per-flow queueing at the bottleneck makes end-host fairness irrelevant, why has it not been deployed everywhere, and what does L4S's dual queue concede to that argument?
- BBR's in-flight cap bounds its share in deep buffers. Is that a fairness mechanism or an accident? What would an explicit one look like?
- The lab's TCP has SACK but no tail-loss probe, pacing only for BBR, and no HyStart. For each, say which of your conclusions it could change, and design the experiment that would tell.
- Is "do no more harm than CUBIC" a stable deployment criterion once CUBIC is no longer the majority?
