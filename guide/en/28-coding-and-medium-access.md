<!-- level: 2 -->
<!-- labs: 260-coding-bench, 261-csma-cd, 272-priority-queueing -->
# Coding, medium access and priority queueing

This topic collects three mechanisms of the lower layers. Each answers a question of the form "there is not enough of something: who pays?".

**Bits get corrupted.** A detecting code lets the receiver discard a damaged frame; a **correcting code** adds enough redundancy to locate the error and repair it without retransmission. The classic **Hamming code** protects 4 data bits with 3 parity bits arranged so that the pattern of failed checks, the **syndrome**, is the position of the wrong bit written in binary. The price is the **code rate**: only 4 of every 7 bits sent are data.

**One channel, many stations.** If two transmit together both frames are lost. **Carrier sensing** (CSMA) means listening before talking; classic Ethernet adds **collision detection** (CSMA/CD): keep listening while transmitting, stop at once on a collision, and retry after a random delay drawn from a window that doubles at each further collision, the **binary exponential backoff**.

**One output link, more traffic than it can carry.** Something must wait or be dropped. A FIFO queue treats all packets alike; **strict priority queueing** keeps several queues and always serves the highest non-empty one, so a small amount of delay-sensitive traffic such as voice is untouched by congestion, and the rest absorbs all the damage.

<!-- detail: 2 -->
## Hamming(7,4)

Number the codeword positions 1 to 7. Positions 1, 2 and 4 (the powers of two) hold parity bits p1, p2, p3; positions 3, 5, 6, 7 hold the data bits d1 to d4. Each parity bit covers the positions whose number has the corresponding binary digit set:

- p1 covers positions 1, 3, 5, 7: p1 = d1 ⊕ d2 ⊕ d4
- p2 covers positions 2, 3, 6, 7: p2 = d1 ⊕ d3 ⊕ d4
- p3 covers positions 4, 5, 6, 7: p3 = d2 ⊕ d3 ⊕ d4

The receiver recomputes the three checks: s1, s2, s3 are the XOR of the bits in each group, parity bit included. The syndrome is the number s3 s2 s1 in binary: 0 means no error detected, otherwise it is the position to flip.

Worked example: data 1011. p1 = 1⊕0⊕1 = 0, p2 = 1⊕1⊕1 = 1, p3 = 0⊕1⊕1 = 0, so the codeword is `0110011`. The channel flips position 5: `0110111`. Then s1 = 0⊕1⊕1⊕1 = 1, s2 = 1⊕1⊕1⊕1 = 0, s3 = 0⊕1⊕1⊕1 = 1: syndrome 101 = 5. Flip bit 5 back.

Any two codewords differ in at least 3 positions (minimum distance 3), so one error is corrected. Two errors produce a non-zero syndrome that points to a third, innocent bit: the decoder "corrects" it and delivers wrong data. **Hamming(8,4)** adds an overall parity bit (distance 4): a non-zero syndrome with correct overall parity means two errors, and the block is discarded. This is SECDED: single error correction, double error detection.

In general r parity bits protect up to 2^r − r − 1 data bits: (7,4), (15,11), (31,26). Code rate = data bits / total bits: 4/7 = 0.57 for Hamming(7,4), 1/2 for (8,4), 1/3 for triple repetition.

## CSMA and CSMA/CD

Let a = propagation delay / frame time. Carrier sensing fails when two stations start within one propagation delay of each other, so it works well only when a is small. Variants when the channel is busy: **1-persistent** waits and transmits as soon as it is free (all waiting stations then collide); **non-persistent** goes away for a random time.

CSMA/CD as in Ethernet:

- Sense the channel; when it is idle, transmit (1-persistent).
- While transmitting, listen. On a collision stop, send a short **jam** signal (32 bits) so that everyone notices.
- After the n-th consecutive collision of this frame choose K uniformly in {0, 1, ..., 2^min(n,10) − 1} and wait K **slot times**; then start again. After 16 attempts give up.

A slot is twice the maximum propagation delay (51.2 µs, 512 bit times, at 10 Mbit/s): after one slot without a collision the station has captured the channel. For the same reason a frame must last at least one slot, or the sender could finish before the collision returns: hence Ethernet's minimum of 64 bytes. After the first collision K ∈ {0,1}; after the second {0,1,2,3}; after the third 0 to 7. Two stations colliding for the first time collide again with probability 1/2, then 1/4: the window adapts to the unknown number of contenders.

Efficiency is approximately 1 / (1 + 5a): with a = 0.01, as in lab 261, about 95%; with a = 0.1, 67%.

## Strict priority queueing

The classifier puts each packet into a band, in lab 272 by the DSCP field of the IP header: band 0 for Expedited Forwarding (DSCP 46, written `tos=184` because the DSCP occupies the upper six bits of the old TOS byte: 46 × 4 = 184), band 2 for best effort. The scheduler always sends from the lowest-numbered non-empty band. It is non-preemptive: a packet being transmitted is finished first.

Numbers of the lab: the link carries 500 kbit/s. Voice sends 160-byte packets at 64 kbit/s, 50 per second; bulk offers 700 kbit/s in 1000-byte packets. A voice packet waits at most for one bulk packet, about 1000 × 8 / 500000 = 16 ms, and none is lost. Bulk gets what is left, about 430 kbit/s, and loses about 40% of its packets. With a single FIFO of 30 packets the queue stays full: up to 30 × 16 ms ≈ 0.5 s of delay for everyone, and both flows lose packets.

<!-- detail: 3 -->
## In depth: coding

A Hamming code is *perfect*: the 2^r syndromes are exactly "no error" plus one per position, with nothing to spare; that is why two errors cannot be flagged and are miscorrected. Hamming codes suit channels with rare, independent errors: SECDED codes such as (72,64) protect server memory. Networks see bursts, which put several errors in one block; an interleaver spreads a burst over many blocks, and Reed-Solomon codes correct whole bytes. Forward error correction pays its redundancy always, ARQ (detect and retransmit) only on errors, at the cost of a round trip: wired links use detection and retransmission by upper layers, while radio, satellite and optical links combine FEC with ARQ. Shannon (1948) proved that reliable communication is possible at any rate below the channel capacity; Hamming's "Error Detecting and Error Correcting Codes" (1950) gave the first practical construction. In netlab frames are coded whole and Reed-Solomon uses fixed (255,223) blocks.

## In depth: medium access

Backoff is unfair in the short term: the station that wins resets its window while the loser's keeps doubling (the *capture effect*). CSMA/CD limits network diameter: at 100 Mbit/s the slot shrinks to 5.12 µs and the collision domain to about 200 m; Gigabit Ethernet needed carrier extension. All this is history: with switches and full-duplex links there are no collisions, and CSMA/CD is disabled. Radios cannot listen while transmitting, so Wi-Fi uses collision *avoidance*. References: Metcalfe and Boggs, "Ethernet: Distributed Packet Switching for Local Computer Networks" (1976); Kleinrock and Tobagi's analysis of CSMA (1975); IEEE 802.3. In netlab's model time advances in ticks of one propagation delay, every station is one tick from every other, the slot is two ticks, the jam three, and each station queues at most 16 frames.

## In depth: priority

Strict priority can **starve** the lower bands: if band 0 alone exceeds the link rate nothing else is ever sent. Real routers therefore police the priority class to a fixed rate (low-latency queueing) and admit only traffic that deserves it. Markings are trusted only inside one administrative domain and usually rewritten at its border. Fair queueing, by contrast, isolates flows without trusting any marking. The delay bound depends on the largest lower-priority packet: on slow links a 1500-byte packet delays voice by tens of milliseconds, which is why such links fragment and interleave. References: RFC 2474 and RFC 2475 (Differentiated Services), RFC 3246 (Expedited Forwarding). netlab's `prio` has three bands: DSCP ≥ 40 and ARP, other non-zero DSCP, best effort.

<!-- detail: 1 -->
## In the lab

**Lab 260: `./netlab run 260-coding-bench`**

- Select the node lab and open its `coding` tab: data bits blue, redundancy yellow, flipped bits red. Each step is explained in the log.
- 9 s: Hamming(7,4), bit 9 flipped (bits are numbered from 0, so it is the third bit of the second block): `syndrome 3`, corrected.
- 12 s: bits 7 and 8, positions 1 and 2 of that block: syndrome 1 ⊕ 2 = 3, the innocent bit 3 is flipped, and the text becomes «@ELLO». 15 s: the same error against Hamming(8,4) is detected and the frame discarded.
- 24 s: the sweep. For each bit error rate read delivered, discarded and silently wrong.
- Predict, then try: `:lab coding show text=HELLO code=hamming74 pos=0,7,14`. Three flipped bits: is the text right? Why? Then `pos=0,1,2`.
- Predict, then try: `./netlab run 260-coding-bench code=hamming74` and compare the "silently wrong" column and the cost per data bit with the default hamming84. Which would you choose at a bit error rate of 1e-2?

**Lab 261: `./netlab run 261-csma-cd`**

- Three nodes run the same load under three disciplines. Open the `mac` tab of `nonpers`, `pers` and `ethernet` in turn: green is a success, red a collision. Compare the width of the red areas.
- The log prints S (throughput) and G (attempts) every 20 frame times. Typical final values: about 0.8 for non-persistent, 0.4 for 1-persistent, 0.88 for CSMA/CD, with an offered load of 0.9.
- Predict, then try: `./netlab run 261-csma-cd prop=10ms` (a = 0.1). Which protocol loses most? Compare CSMA/CD with 1 / (1 + 5a).
- Predict, then try: `./netlab run 261-csma-cd load=2` and `load=0.3`. Which discipline degrades under overload, and which is limited only by the offered load?

**Lab 272: `./netlab run 272-priority-queueing`**

- Select r1 and open the `qdisc` tab: three bands, their lengths and drops. Select v2 and b2 for loss, delay and jitter of the two flows. The graphs (`g`) show delay over time.
- Until 3 s voice is alone; then the bulk flow starts and r1 drops only bulk packets (`✗ prio` in the log).
- Predict, then try: `./netlab run 272-priority-queueing qdisc=droptail`. Estimate the voice delay from queue length and link rate before looking.
- Predict, then try: `./netlab run 272-priority-queueing bulk=2M`. Does anything change for voice? And for bulk? Then `qdisc=fq`.

## Check yourself

- Encode the data bits 0110 with Hamming(7,4). Then decode the received word 1011011: compute the syndrome and give the data bits.
- Why does Hamming(7,4) deliver wrong data when two bits of a block are flipped, and how does the eighth bit of Hamming(8,4) avoid it?
- A station has just had its third consecutive collision. From which set does it draw its backoff, what is the mean wait in slots, and why is the slot time twice the propagation delay?
- Why does Ethernet need a minimum frame length, and how is it related to the length of the cable?
- A 256 kbit/s link serves a priority queue for voice (80 kbit/s, 200-byte packets) and a best-effort queue with 1500-byte packets. What is the worst-case queueing delay of a voice packet? What happens to best-effort traffic if the voice class sends 300 kbit/s?
