<!-- level: 3 -->
<!-- labs: 320-tcp-congestion, 321-tcp-sack -->
# Congestion control: AIMD, CUBIC and the models behind them

A packet network has no admission control: any host may send at any rate, and a router that receives more than its output link carries can only queue and then drop. If senders answer loss by retransmitting at the same rate, the network enters **congestion collapse**: links stay busy carrying packets that will be dropped further on or that are needless duplicates, and useful throughput falls by orders of magnitude (the NSFNET backbone in October 1986 went from 32 kbit/s to 40 bit/s).

The problem is hard because it is a distributed resource-allocation problem with almost no information. A sender does not know the capacity of the bottleneck, how many flows share it, or how either changes; the only signals are implicit and late: a packet that did not arrive, an ACK that arrived later than usual.

The key idea (Jacobson, 1988) is **conservation of packets**: in equilibrium a new packet enters only when an old one leaves, so ACKs clock the sender. On top of that self-clocking, each sender keeps a congestion window `cwnd` and probes: it increases the window gently while everything is acknowledged and cuts it sharply on a congestion signal. Additive increase with multiplicative decrease (**AIMD**) is the rule that makes independent senders converge to a share that is both efficient and fair.

<!-- detail: 2 -->
## Algorithms and models

The sender may have at most `min(cwnd, rwnd)` bytes outstanding. Two state variables, `cwnd` and `ssthresh`, and four rules (RFC 5681):

```
slow start (cwnd < ssthresh):   cwnd += MSS           per ACK  (x2 per RTT)
cong. avoid. (cwnd >= ssthresh): cwnd += MSS*MSS/cwnd per ACK  (+1 MSS per RTT)
3 dup ACKs:  ssthresh = max(flight/2, 2*MSS); fast retransmit of the hole
RTO expiry:  ssthresh = max(flight/2, 2*MSS); cwnd = 1 MSS; RTO = 2*RTO
```

The variants differ in what follows the three duplicate ACKs:

- **Tahoe**: `cwnd = 1 MSS` and slow start again up to the new `ssthresh`.
- **Reno**: fast recovery. `cwnd = ssthresh + 3 MSS`, plus one MSS per further duplicate ACK (each one means a packet left the network); at the first new ACK `cwnd = ssthresh`. One loss per window is repaired without emptying the pipe; several losses usually end in a timeout.
- **NewReno** (RFC 6582): remembers the highest sequence sent when recovery began (`recover`). An ACK below it is a **partial ACK**: the next hole is retransmitted at once and recovery continues. One hole is repaired per RTT: it is what a sender does without SACK (in netlab, `sack=0`).

**Why AIMD is fair.** Take two flows with rates `x1`, `x2` on a link of capacity `C`. Additive increase adds the same amount to both, so the difference `x1 - x2` is unchanged; multiplicative decrease multiplies both by `b < 1`, so the difference shrinks by `b`. Every cycle the state moves towards the line `x1 = x2`, while oscillating around the efficiency line `x1 + x2 = C`. Additive decrease or multiplicative increase would preserve or amplify unfairness.

**CUBIC** (RFC 9438) replaces the linear probe with a function of the time `t` since the last reduction, independent of the RTT:

```
W(t) = C*(t - K)^3 + Wmax        K = cbrt(Wmax*(1 - beta)/C)
C = 0.4, beta = 0.7  (on loss: Wmax = cwnd; cwnd = beta*cwnd)
```

The window rises quickly, flattens near the previous maximum `Wmax` (where loss is likely) and then accelerates again to probe for new capacity. A "TCP-friendly" lower bound keeps it no slower than Reno on short, slow paths.

**The square-root law.** In steady state with loss probability `p`, a Reno window oscillates between `W/2` and `W`. One cycle lasts `W/2` round trips and carries `3W^2/8` packets, exactly one of which is lost:

```
3*W^2/8 = 1/p   =>   W = sqrt(8/(3p))
rate = (3W/4)*MSS/RTT = (MSS/RTT)*sqrt(3/(2p)) ~ 1.22*MSS/(RTT*sqrt(p))
```

**Worked example (the lab defaults).** Bottleneck 2 Mbit/s, base RTT about 44 ms, 1500-byte packets: the bandwidth-delay product is `2e6*0.044/(8*1500)`, about 7 packets. The queue holds 25, so loss occurs at `Wmax` near 7 + 25 + 2, and the lab log indeed shows `cwnd 35.0 → ssthresh 17.0` at every loss. Between two losses the log shows about 500 segments, so `p` is 0.002 and the formula gives `W = sqrt(8/0.006)`, about 36. Since the minimum window (17) exceeds the BDP (7), the queue never empties and the link stays fully used: a Reno flow needs a buffer of at least one BDP for that.

<!-- detail: 3 -->
## In depth

**Invariants and stability.** Chiu and Jain proved that, among linear controls driven by a binary feedback, additive increase with multiplicative decrease is the only combination that converges to both efficiency and fairness. The fairness reached is in windows, not in rates: since rate is `W/RTT`, flows with a short RTT obtain more (the law above says rate ∝ 1/RTT). CUBIC's time-based growth removes most of that bias.

**What is the signal?** Loss-based control (Reno, CUBIC) must fill the buffer to find the limit, so its standing queue and delay grow with the buffer size; random non-congestive loss is misread as congestion. Delay-based control (Vegas, and later FAST, Copa) backs off when the RTT exceeds its minimum, keeping queues short, but it loses to loss-based flows sharing the same queue, which is why it never became the default. Model-based control (BBR) estimates the bottleneck bandwidth (windowed maximum of the delivery rate) and the propagation RTT (windowed minimum), paces at that rate and periodically probes with gains 1.25 and 0.75. It ignores isolated losses, but version 1 can be unfair to loss-based flows and lossy with shallow buffers; later versions also react to loss and ECN. ECN (RFC 3168) and DCTCP (RFC 8257) turn the signal into explicit marks.

**Failure modes.** Slow start overshoots: it doubles until the queue overflows and loses up to half a window at once. Without SACK that burst costs one RTT per hole (NewReno) or a timeout (Reno); with SACK (RFC 6675) about one round trip. The square-root law ignores timeouts; the Padhye model adds them and predicts much lower rates at high `p`.

**What deployed stacks do differently.** SACK (RFC 2018) and proportional rate reduction (RFC 6937), RACK-TLP time-based loss detection (RFC 8985), an initial window of 10 segments (RFC 6928), HyStart++ to leave slow start before the overshoot (RFC 9406), pacing, and CUBIC as the default in Linux, Windows and most QUIC stacks.

**In netlab.** The `tcp` module implements SACK with the recovery of RFC 6675 (plus a RACK-style check for lost retransmissions), delayed ACKs, and BBR version 1 with its four states and a real pacing timer. It has no timestamps, no tail-loss probe, no HyStart and no proportional rate reduction: the slow-start overshoot still loses a burst, a lost tail still waits for the timer, and after a recovery the window restarts from ssthresh at once. `sack=0` gives NewReno recovery (classic Reno with `cc=reno`), to see what SACK buys.

**Primary literature.**

- J. Nagle, "Congestion Control in IP/TCP Internetworks", RFC 896, 1984.
- V. Jacobson, "Congestion Avoidance and Control", SIGCOMM 1988.
- D.-M. Chiu, R. Jain, "Analysis of the Increase and Decrease Algorithms for Congestion Avoidance in Computer Networks", 1989.
- L. Brakmo, L. Peterson, "TCP Vegas: End to End Congestion Avoidance on a Global Internet", 1995.
- M. Mathis, J. Semke, J. Mahdavi, T. Ott, "The Macroscopic Behavior of the TCP Congestion Avoidance Algorithm", 1997.
- J. Padhye, V. Firoiu, D. Towsley, J. Kurose, "Modeling TCP Throughput: A Simple Model and its Empirical Validation", SIGCOMM 1998.
- S. Ha, I. Rhee, L. Xu, "CUBIC: A New TCP-Friendly High-Speed TCP Variant", 2008.
- N. Cardwell, Y. Cheng, C. S. Gunn, S. H. Yeganeh, V. Jacobson, "BBR: Congestion-Based Congestion Control", 2016.
- RFC 5681 (TCP congestion control), RFC 6582 (NewReno), RFC 9438 (CUBIC).

<!-- detail: 1 -->
## In the lab

Run `./netlab run 320-tcp-congestion`. Host h1 sends 5 MB to h2 through the bottleneck r1–r2 (2 Mbit/s, 20 ms, 25 packets). Select h1 (`n`), open its `tcp` inspector tab and watch the graph `cwnd (segments)`, with `ssthresh` and `rtt (ms)`; press `g` for the graphs view and `l` for the log. On r1 look at the queue towards r2.

**Baseline (reno).** Before running, compute the BDP in packets and predict the window at which the queue overflows. In the log, the lines `SACK shows a hole: fast retransmit … (reno+sack, cwnd X → Y)` give the top and the bottom of each tooth (35 → 17 segments, one tooth every 2.8 s): measure X, Y and the period, and compare with the worked example. The final line reports goodput, the steady-state rate after the first quarter of the transfer, and the number of retransmissions, fast retransmits and timeouts. Note the first second: the slow-start overshoot drops a whole burst at r1 (cwnd 67 → 33.5), and SACK repairs it in one recovery without a timeout. Run it again with `sack=0`: the same burst now costs a timeout.

**Experiment 1: the algorithm.** Run again with `cc=tahoe`, `cc=newreno`, `cc=cubic`, `cc=bbr`. Predict the shape of each cwnd graph first. Expected: Tahoe falls to 1 at every loss; CUBIC shows the concave-then-convex curve and smaller reductions (factor 0.7); `bbr` leaves STARTUP after about 0.6 s, stays near the BDP, keeps the queue almost empty and finishes with zero retransmissions. Goodput is about 1.9 Mbit/s for Reno, CUBIC and BBR alike (the link carries 1.92 Mbit/s of payload): on one flow and a 25-packet buffer the algorithms differ in queue and losses, not in rate. Tahoe is the exception (about 1.65 Mbit/s and hundreds of retransmissions): it restarts from one segment and resends what the receiver already has.

**Experiment 2: the buffer.** `./netlab run 320-tcp-congestion queue=5`. Prediction: the buffer is now smaller than the BDP, so after each halving Reno's window is below the BDP and the link idles; goodput drops (about 1.83 Mbit/s) and fast retransmits become frequent (38 instead of 8). CUBIC, which only cuts to 70%, should lose less (it stays at 1.91). Then `queue=200`: slow start now overshoots by more than a hundred segments before the first loss, the recovery that follows resends several hundred of them, and from then on the `rtt (ms)` series climbs to several times the base RTT with each tooth. That is bufferbloat, bought for no extra throughput. `cc=bbr` on the same deep buffer never fills it.

**Experiment 3: non-congestive loss.** `./netlab run 320-tcp-congestion loss=0.05`, with `cc=reno` and then `cc=bbr`. Use the square-root law to predict Reno's rate (W about 5.5 segments, below the BDP), then compare: Reno measures about 1.1 Mbit/s and CUBIC 1.2, while `bbr`, which does not read loss as congestion, holds about 1.8 Mbit/s with a similar number of retransmissions. You can also inject loss in a running lab with `:` and `link r1 r2 loss=0.05`.

**Experiment 4: what SACK buys.** `./netlab run 321-tcp-sack`, then `./netlab run 321-tcp-sack sack=0`. The slow-start overshoot loses about two dozen segments of one window on a path with an 84 ms RTT. Predict the cost of repairing them one per round trip. Expected: with SACK one recovery, about 24 retransmissions, no timeout, 1.0 s in all; with cumulative ACKs only, NewReno starts well, its timer expires twice, most of the window is sent again (about 75 retransmissions) and the transfer takes 2.4 s. Open a duplicate ACK in the Packets view and read its SACK blocks; at detail 2 the tcp panel of h1 counts segments in flight, SACKed and lost.

## Check yourself

- Derive the relation rate ≈ 1.22·MSS/(RTT·√p) from the sawtooth, stating every assumption. Which assumption fails first as p grows, and in which direction is the formula then wrong?
- Two Reno flows with RTTs of 20 ms and 200 ms share a drop-tail bottleneck and see the same loss probability. What is the ratio of their throughputs? How does CUBIC change the answer, and why?
- With a bottleneck of capacity C, base round-trip time T and a drop-tail buffer of B packets, give the condition on B for a single Reno flow to keep the link fully utilised. Repeat for a multiplicative decrease factor of 0.7.
- Show with the two-flow vector diagram why additive-increase/additive-decrease does not converge to fairness, and why multiplicative-increase/multiplicative-decrease does not either.
- A delay-based flow and a loss-based flow share one deep drop-tail buffer. Describe the equilibrium, and explain what BBR does differently from Vegas to avoid being starved.
