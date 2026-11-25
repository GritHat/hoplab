<!-- level: 2 -->
<!-- labs: 220-tcp-loss, 221-tcp-flow-control, 120-tcp-handshake -->
# TCP: reliability and flow control

IP delivers packets on a best-effort basis: they can be lost, duplicated or reordered. TCP builds on top of it a **reliable, ordered stream of bytes** between two applications. Three ideas do most of the work.

**Numbering.** Every byte of the stream has a sequence number. Each segment says which bytes it carries, and the receiver answers with a **cumulative acknowledgement**: the number of the next byte it expects, meaning "I have everything before this".

**Retransmission.** The sender keeps a copy of what it has sent until it is acknowledged. It learns about a loss in two ways. If no acknowledgement arrives within the **retransmission timeout** (RTO), a value continuously estimated from measured round-trip times, it sends the oldest unacknowledged segment again. If instead later segments do arrive, the receiver repeats the same acknowledgement; three such **duplicate ACKs** trigger a **fast retransmit** without waiting for the timer.

**Flow control.** The receiver has a finite buffer, emptied at the pace of its application. In every segment it advertises a **receive window**: how many more bytes it can accept. The sender never has more unacknowledged data outstanding than that. A slow reader thus slows the sender down to its own pace, without losing a byte.

<!-- detail: 2 -->
## Header fields that matter here

The 20-byte TCP header carries source and destination port, **sequence number** (32 bits: the number of the first data byte of the segment), **acknowledgement number** (32 bits: next byte expected, valid when the ACK flag is set), header length, flags (SYN, ACK, FIN, RST, PSH, URG), **window** (16 bits), checksum and urgent pointer. SYN and FIN each consume one sequence number, which is why they can be acknowledged like data.

## Connection and teardown

The three-way handshake synchronises the initial sequence numbers: SYN (seq = x), SYN+ACK (seq = y, ack = x + 1), ACK (ack = y + 1). The MSS option announces the largest segment each side accepts (1460 on Ethernet). Each direction is closed separately: FIN, ACK, and later FIN, ACK from the other side. The side that closes first waits in TIME_WAIT for twice the maximum segment lifetime, so that a lost final ACK can be repeated and old segments die out. A SYN to a port without a listener is answered with RST.

In lab 120, 4000 bytes travel as three segments (1460 + 1460 + 1080); the ACK numbers, relative to the initial one, are 1461, 2921 and 4001, and the FIN is acknowledged with 4002.

## The retransmission timer (RFC 6298)

For each new round-trip sample R:

```
first sample:   SRTT = R              RTTVAR = R / 2
afterwards:     RTTVAR = 3/4 RTTVAR + 1/4 |SRTT - R|
                SRTT   = 7/8 SRTT   + 1/8 R
RTO = SRTT + 4 * RTTVAR       (initially 1 s; lower bound 1 s in the RFC)
```

Example with the first sample of lab 220, R = 88 ms: SRTT = 88, RTTVAR = 44, RTO = 88 + 176 = 264 ms. A second sample of 50 ms gives RTTVAR = 33 + 9.5 = 42.5, SRTT = 77 + 6.25 = 83.25, RTO = 253 ms. The variance term is what keeps the RTO safely above an irregular RTT.

Two more rules. **Karn's algorithm**: never take a sample from a retransmitted segment, because the ACK is ambiguous. **Exponential backoff**: at every expiry the RTO doubles (1, 2, 4 s, ... up to 60 s), and returns to the computed value when new data is acknowledged.

netlab uses a minimum RTO of 200 ms (parameter `rto_min`), as Linux does, instead of the RFC's 1 s.

## Fast retransmit

A receiver that gets a segment beyond a hole buffers it and immediately re-sends the ACK for the hole. The sender counts: the third duplicate (four identical ACKs in all) is taken as proof of loss, and the missing segment is retransmitted at once. The threshold of three tolerates mild reordering. Fast retransmit needs later segments to arrive: a loss at the end of a transfer, or with a tiny window, can only be repaired by the timer. Either kind of loss also reduces the congestion window, the subject of another topic.

## Flow control

The sender obeys: LastByteSent − LastByteAcked ≤ min(cwnd, rwnd). The receiver advertises rwnd = buffer size − (bytes received − bytes read by the application).

When rwnd reaches 0 the sender stops. The window update that reopens it is a plain ACK, sent once and never retransmitted; if it were lost both sides would wait for ever. So the sender starts a **persist timer** and periodically sends a **zero-window probe** of one byte, which forces an ACK carrying the current window.

In lab 221 the buffer is 8000 bytes and the application reads 16 kbit/s = 2000 bytes/s. The first segments fill the buffer in milliseconds; from then on 40000 bytes take 20 s: the goodput reported is 15.95 kbit/s on a 10 Mbit/s link.

<!-- detail: 3 -->
## In depth

**Cumulative ACKs are robust but poor in information.** A lost ACK is harmless, because the next one covers it. But a sender that has lost several segments of a window learns about one hole per round trip. **Selective acknowledgements** (SACK, RFC 2018) list the blocks received beyond the hole; the sender keeps a scoreboard of them and repairs every hole of a window in about one round trip (RFC 6675). NewReno (RFC 6582) is the best that can be done without them. netlab negotiates SACK by default; `sack=0` on a `tcp send` falls back to NewReno, and lab `321-tcp-sack` compares the two.

**Delayed ACKs.** Real receivers acknowledge every second full segment, or after at most 500 ms (RFC 1122; typically 40 to 200 ms), to save packets. This interacts badly with Nagle's algorithm (RFC 896), which holds small segments while data is unacknowledged: the classic 40 ms stalls of request/response protocols, and the reason for the TCP_NODELAY option. netlab does both: ACKs wait for a second segment or for 40 ms (`delack=`, 0 to acknowledge everything at once) and Nagle is on (`nagle=0` to disable it).

**Timers in real stacks.** The RFC's 1 s minimum was chosen against spurious timeouts; Linux uses 200 ms, and data centres go lower. The timestamp option (RFC 7323) gives a sample for every ACK, including retransmissions, making Karn's restriction unnecessary, and protects against wrapped sequence numbers (PAWS) on fast links. The same RFC defines window scaling: the 16-bit window field limits the window to 64 kB, far too little for a long fast path, so both sides agree on a shift factor in the SYN. A spurious timeout can be detected and undone (F-RTO, RFC 5682; Eifel). Modern Linux uses time-based loss detection (RACK-TLP, RFC 8985), which also sends a probe to turn tail losses into fast recoveries.

**Silly window syndrome.** A receiver that advertises every tiny amount of freed space, or a sender that uses it, fills the network with tiny segments. Clark's rule: do not advertise an increase until it is at least one MSS or half the buffer. In real stacks the persist timer backs off exponentially (from about 5 s up to 60 s); netlab probes at a fixed interval of 500 ms.

**Teardown corner cases.** Both sides may close simultaneously; one side may close its sending direction and keep receiving (half-close); an application that closes with unread data causes a RST. TIME_WAIT keeps a port pair busy for 2 MSL, a nuisance for busy servers. Initial sequence numbers must be unpredictable (RFC 6528), or an off-path attacker could inject data.

**References.** RFC 9293 (TCP, 2022, replacing RFC 793), RFC 6298 (RTO), RFC 5681 (fast retransmit and recovery), RFC 1122; Jacobson, "Congestion Avoidance and Control" (1988), where the RTT variance estimator comes from; Karn and Partridge, "Improving Round-Trip Time Estimates in Reliable Transport Protocols" (1987).

<!-- detail: 1 -->
## In the lab

**Lab 120: `./netlab run 120-tcp-handshake`**

- Open the sequence view (`s`) at detail 2: SYN, SYN+ACK, ACK, three data segments, their ACKs, FIN in both directions. In the packets view (`p`) press Enter on each and read Seq, Ack, flags and Win.
- At 6 s a SYN to port 81 gets RST: `connection refused`.
- Predict, then try: `./netlab run 120-tcp-handshake bytes=20000 iw=2`. How many segments are in flight in the first round trip, and in the second? Then `loss=0.1`: find a retransmission and decide whether it was fast or by timeout.

**Lab 220: `./netlab run 220-tcp-loss`**

- Select h1 and open its `tcp` tab: srtt, rto, cwnd, ssthresh, and the counters of fast retransmits and timeouts.
- In the log at detail 2 find `SACK shows a hole: fast retransmit` (with `sack=0`: `3 duplicate ACKs: fast retransmit`) and `retransmission timeout (RTO ...)`. Open the graphs (`g`) for the RTT and the window.
- In the packets view find a group of ACKs with the same Ack number followed by the retransmitted segment.
- Predict, then try: `./netlab run 220-tcp-loss loss=0.2`. Which kind of repair dominates now, and why? Look for RTO values of 1 s, 2 s, 4 s: where do they come from?
- Predict, then try: `./netlab run 220-tcp-loss delay=100ms`. Estimate the RTT and the RTO after the first sample before you look.

**Lab 221: `./netlab run 221-tcp-flow-control`**

- In the packets view watch the Win field of the ACKs from h2 fall to 0 within about 50 ms. In the log: `the receiver advertises window 0`, then every 500 ms `zero-window probe`.
- At the end compare the goodput with `readrate`, and note 0 retransmissions: nothing was lost.
- Predict, then try: `./netlab run 221-tcp-flow-control rwnd=32000`. Does the transfer finish sooner? How does the number of probes change?
- Predict, then try: `./netlab run 221-tcp-flow-control readrate=64k`. Compute the duration first.

## Check yourself

- Host A sends three segments of 1000 bytes with sequence numbers 5000, 6000 and 7000; the second is lost. Which ACK numbers does B send, and what does A do, assuming A keeps sending new segments afterwards?
- Compute SRTT, RTTVAR and RTO after the samples 100 ms, 120 ms and 80 ms, starting from no estimate.
- Why does TCP need Karn's algorithm, and why does the RTO double after a timeout?
- A receiver has a 4000-byte buffer and its application reads nothing. Describe the segments exchanged from the first data byte until the application finally reads 2000 bytes. What would happen without the persist timer?
- Why do SYN and FIN consume a sequence number, and why does the side that closes first wait in TIME_WAIT?
