<!-- level: 4 -->
<!-- labs: 460-hidden-terminal -->
# Wireless medium access: hidden terminals and what 802.11 does about them

Carrier sensing rests on an assumption that is true on a cable and false on radio: that the transmitter can hear whatever would interfere at the receiver. Interference happens at the receiver; sensing happens at the sender; on a radio channel the two are different places with different views. A **hidden terminal** is a station the receiver hears and the sender does not: carrier sensing reports an idle channel and the frames collide. An **exposed terminal** is the opposite error: the sender hears a transmission that would not disturb its own receiver, and stays silent for nothing.

The classical remedy is a short reservation handshake, RTS and CTS, so that the receiver itself announces that the channel is taken. It costs airtime on every frame and it does not solve the exposed-terminal case, so 802.11 makes it optional and most networks leave it off for ordinary frames. **When is a reservation worth its overhead, and how much of the textbook picture survives in real radios, where a stronger frame can be decoded through a weaker one and the range of interference exceeds the range of communication?**

The question matters again today: dense deployments, wide channels and uplink multi-user transmission have changed both the cost of a collision and the tools available to avoid one. The lab compares three cells under identical load.

<!-- detail: 2 -->
## CSMA/CA as specified

A station with a frame waits until the medium has been idle for DIFS, then counts down a backoff drawn uniformly from [0, CW] slots, freezing the counter whenever the medium is busy, and transmits when it reaches zero. The receiver answers with an ACK after SIFS, which is shorter than DIFS so that nobody can interleave. A missing ACK is read as a collision: CW doubles, up to CWmax, and the frame is retried up to a limit, after which it is dropped. Collisions among stations that hear each other happen only when two backoff counters expire in the same slot, so their probability is governed by CW and the number of contenders (Bianchi's model), and their cost is one whole frame, because a radio cannot listen while it transmits.

## Hidden and exposed terminals

With hidden terminals the vulnerable period is no longer one slot but twice the frame duration, as in pure ALOHA: carrier sensing gives no protection between the two groups, and backoff only spreads the attempts. Tobagi and Kleinrock showed in 1975 that CSMA throughput then degrades towards ALOHA's. Long frames are hit hardest.

## RTS/CTS and the virtual carrier

The sender transmits a short RTS carrying the duration of the coming exchange; the receiver replies with a CTS carrying the same information. Every station that hears either frame sets its network allocation vector (NAV) and stays silent for that time: this is virtual carrier sensing. Hidden stations hear the CTS even though they missed the RTS. A collision can now destroy only an RTS, and is detected after a few tens of microseconds rather than a full frame.

The overhead is two control frames and two SIFS per data frame, sent at a robust low rate, so in relative terms it is small for long frames and large for short ones. Hence the RTS threshold: use the handshake only above a frame size. Bianchi's analysis shows that even without hidden terminals RTS/CTS increases saturation throughput when frames are long and contenders are many, because it shortens collisions.

## Capture, and interference range

Two things spoil the clean model. **Capture**: a receiver can often decode the stronger of two overlapping frames, so not every overlap is a loss, and near stations win systematically over far ones. **Interference range**: a transmitter can corrupt reception at distances from which its CTS cannot be decoded, so a station may be outside the reach of the reservation and still inside the reach of the damage (Xu, Gerla and Bae). RTS/CTS also makes the exposed-terminal problem worse, by silencing stations that could have transmitted.

## What 802.11 does today

RTS/CTS is off by default for ordinary frames and is used selectively: to protect long aggregates (A-MPDU), as a cheap probe before a wide-channel transmission, and for coexistence with older PHYs (CTS-to-self). Rate adaptation must distinguish collision losses from channel losses, or it lowers the rate, lengthens the frames and makes hidden-terminal collisions more likely. 802.11ax adds scheduled uplink (OFDMA with trigger frames, MU-RTS), which takes contention away from the stations the access point serves, and BSS colouring with adaptive sensing thresholds, an explicit attack on the exposed-terminal problem.

<!-- detail: 3 -->
## Open problems

**Sensing thresholds.** A single carrier-sense threshold cannot be right for both hidden and exposed terminals; choosing it per link, per neighbour and per rate, and doing so stably when every station adapts, is unresolved, and spatial reuse in 802.11ax has reopened it.

**Decoding instead of avoiding.** ZigZag decoding recovers both frames from two collisions of the same pair, and successive interference cancellation and full-duplex radios (Choi et al.) change what counts as a collision. How much of that is practical at modern bandwidths and with commodity hardware is still being worked out.

**Diagnosis.** A station cannot directly tell a hidden-terminal loss from a weak-signal loss. Inferring it from loss patterns, and deciding when to enable RTS/CTS, is an old and still imperfect art.

**Scheduled versus random access.** As access points take control of the uplink, the residual problem moves to interference between overlapping cells, where no one schedules.

## The lab's model versus reality

The `mac` module is a slotted abstraction, and its help text says so: time advances in ticks of one propagation delay, every station is one tick from every other, frames have a fixed length, and the CSMA/CA slot, DIFS, RTS, CTS and ACK durations are counted in ticks, **not 802.11 timings**. An RTS and a CTS each last one twentieth of a data frame (at least two ticks), so the overhead ratio is fixed by the model rather than by frame size and PHY rate; you can change it only indirectly through `prop`. The initial backoff is uniform over 32 ticks.

Hidden terminals are modelled as two groups, the even-numbered and odd-numbered stations, that hear their own group perfectly and the other not at all, while the access point hears everybody. There is no geometry, no path loss, **no capture** (any overlap destroys both frames), no interference range beyond decoding range, and a CTS silences every station without fail. Exposed terminals cannot be represented at all, since there is a single receiver. There is no rate adaptation and no aggregation. The model therefore shows the mechanism in its purest form and overstates both the damage of hidden terminals and the cure offered by RTS/CTS. The default run lasts 600 frame times, short enough for two seeds to differ by several points: use `frame=10ms` and several seeds before comparing numbers.

## Primary literature

- F. Tobagi, L. Kleinrock, "Packet Switching in Radio Channels: Part II — The Hidden Terminal Problem in Carrier Sense Multiple-Access and the Busy-Tone Solution", IEEE Transactions on Communications, 1975.
- P. Karn, "MACA — A New Channel Access Method for Packet Radio", ARRL/CRRL Amateur Radio 9th Computer Networking Conference, 1990.
- V. Bharghavan, A. Demers, S. Shenker, L. Zhang, "MACAW: A Media Access Protocol for Wireless LAN's", ACM SIGCOMM, 1994.
- G. Bianchi, "Performance Analysis of the IEEE 802.11 Distributed Coordination Function", IEEE Journal on Selected Areas in Communications, 2000.
- K. Xu, M. Gerla, S. Bae, "How Effective is the IEEE 802.11 RTS/CTS Handshake in Ad Hoc Networks?", IEEE GLOBECOM, 2002.
- M. Heusse, F. Rousseau, G. Berger-Sabbatel, A. Duda, "Performance Anomaly of 802.11b", IEEE INFOCOM, 2003.
- K. Jamieson, B. Hull, A. Miu, H. Balakrishnan, "Understanding the Real-World Performance of Carrier Sense", ACM SIGCOMM Workshop E-WIND, 2005.
- S. Gollakota, D. Katabi, "ZigZag Decoding: Combating Hidden Terminals in Wireless Networks", ACM SIGCOMM, 2008.
- J. I. Choi, M. Jain, K. Srinivasan, P. Levis, S. Katti, "Achieving Single Channel, Full Duplex Wireless Communication", ACM MobiCom, 2010.
- IEEE Std 802.11-2020, and the 802.11ax amendment (2021) for OFDMA, trigger frames and BSS colouring.

<!-- detail: 1 -->
## In the lab

Run `./hoplab run 460-hidden-terminal`. Three independent cells run the same CSMA/CA under the same load: `open` (everyone hears everyone), `hidden` (two groups that cannot hear each other), `rtscts` (the same hidden groups, with RTS/CTS). Parameters: `load` (new frames per frame time, all stations together), `stations`, `frame`. Counters on each cell: `mac.throughput_pct`, `mac.sent`, `mac.success`, `mac.collisions`, `mac.dropped`. In the mac panels, compare S and the amount of red.

With the defaults expect roughly 58% in `open`, 14% in `hidden` and 58% in `rtscts`. For numbers worth comparing use a longer run in frame times and several seeds:

```
./hoplab run 460-hidden-terminal frame=10ms --seed 1 --headless
```

**Experiment 1 — throughput is the wrong metric at low load.** Hypothesis: hidden terminals cause collisions at any load, but lose throughput only when retransmissions can no longer be absorbed. Manipulation: `load=0.05`, `0.2`, `0.6`, `1.0`. Metric: `mac.throughput_pct`, and the collision ratio `mac.collisions / mac.sent`, in `hidden` against `open`. Expected: at 0.2 all three cells carry about 20% (the lab's check `light` verifies it), while `hidden` already shows one to two orders of magnitude more collisions; from 0.6 upwards `hidden` saturates near 14% and starts dropping frames.

**Experiment 2 — size of the hidden population.** Hypothesis: the damage grows with the number of stations hidden from each other, not merely with the load. Manipulation: `stations=2`, `4`, `10` at `load=0.6`. Metric: throughput of `hidden` relative to `open`. Expected: about 42%, 33% and 14% against a nearly constant 58% in `open`.

**Experiment 3 — the price of RTS/CTS when nobody is hidden.** Hypothesis: without hidden terminals RTS/CTS costs throughput with few contenders and gains with many, and the crossover depends on the ratio between control and data frame length. Manipulation: at the `:` prompt restart two cells at saturation, `open mac proto=csmaca load=1.5 stations=2 frame=10ms` and `rtscts mac proto=csmaca rts=1 load=1.5 stations=2 frame=10ms`; repeat with `stations=24` (the maximum); then add `prop=1ms` to both, which makes an RTS a fifth of a frame. Metric: `mac.throughput_pct` of the two cells. Expected: with 2 stations RTS/CTS loses about five points, with 24 it gains about ten, and with the long control frames it loses in both cases: a threshold, as Bianchi predicts, though its position is the model's and not 802.11's.

## Questions for discussion

- The lab has no capture effect. Predict, with an argument, how the `hidden` result changes if the stronger of two overlapping frames survives, and who benefits.
- RTS/CTS reserves the channel around the receiver. Sketch a topology in which it silences a station that could have transmitted safely, and estimate the throughput lost.
- A rate-adaptation algorithm sees 30% frame loss. What observations would let it decide between "hidden terminal" and "weak signal", and why does the wrong decision make things worse?
- Experiment 3 finds a crossover in the number of stations. Which 802.11 parameters would you need in the model to locate the real RTS threshold?
- Does scheduled uplink access in 802.11ax remove the hidden-terminal problem or relocate it? Where?
