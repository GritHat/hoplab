<!-- level: 1 -->
<!-- labs: 160-error-detection, 161-error-correction -->
# Detecting and correcting bit errors

Bits travel as electrical, optical or radio signals, and signals get disturbed: a motor starts nearby, another radio talks, the signal is simply weak. Now and then the receiver reads a 1 where a 0 was sent. A single flipped bit can turn an amount of 100 euros into 228, so the receiver must at least **know** when a frame is damaged.

The trick is always the same: the sender adds a few extra bits computed from the data, called **redundancy**. The receiver repeats the computation and compares. If the results differ, something changed on the way.

An everyday example is the last character of an Italian tax code or the last digit of a bank card number: it is computed from the others, so that a typing mistake is noticed at once.

With little redundancy the receiver can only say "this is wrong" and throw the frame away: **error detection**. With more redundancy it can work out **which** bit is wrong and flip it back: **error correction**. Detection is cheap but then someone must send the data again; correction needs no second try but costs extra bits on every frame, damaged or not.

<!-- detail: 2 -->

## How it works

The **bit error rate** (BER) is the probability that one bit is flipped. With BER 1e-4, one bit in 10 000 is wrong on average. A 500-byte datagram becomes a frame of 546 bytes = 4368 bits, so about 35% of the frames contain at least one error: a small BER damages a large share of long frames.

**Parity bit.** Add one bit so that the total number of 1s is even. Data `1011001` has four 1s, so the parity bit is 0. If one bit flips, the count becomes odd and the receiver notices. If two bits flip, the count is even again and the error goes through. Parity detects every odd number of flips and no even number.

**Checksum.** Treat the data as a list of 16-bit numbers, add them up, and send the (complemented) sum. This is the Internet checksum used by IP, UDP and TCP. It is easy to compute in software and catches most errors, but two changes that cancel in the sum pass unnoticed.

**CRC** (cyclic redundancy check). Treat the whole frame as one huge binary number, divide it by a fixed number agreed in advance, and send the remainder. Ethernet and Wi-Fi append a 32-bit CRC. It detects every burst of errors up to 32 bits long, and any other damage escapes with a probability of about one in four billion.

**Correction.** The simplest code sends every bit three times and takes a majority vote: `1` becomes `111`, and a received `101` is read as `1`. It corrects one error per triple and triples the frame. **Hamming(7,4)** is smarter: for every 4 data bits it adds 3 check bits, each covering a different subset of the 7 positions. The pattern of checks that fail, the **syndrome**, read as a binary number, is the position of the wrong bit. It corrects one error in every block of 7 at a cost of 1.75 times the size.

<!-- detail: 3 -->

## Going deeper

The idea that unifies all these codes is **distance**. The Hamming distance between two valid codewords is the number of bit positions in which they differ. If the minimum distance of a code is d, the code can detect up to d − 1 errors, or correct up to (d − 1) / 2 of them, rounded down. Parity has d = 2 (detects 1, corrects 0); triple repetition and Hamming(7,4) have d = 3 (correct 1). And here is the catch: a code used for correction cannot tell one error from two. With two flips in the same block Hamming(7,4) confidently "repairs" a third bit and delivers wrong data. The (8,4) variant adds one overall parity bit, reaching d = 4: correct one, detect two (SECDED, the scheme of server memory).

Detect and retransmit, or correct? It depends on the channel. On a cable or fibre errors are rare, so a CRC plus retransmission (topic 13) wastes almost nothing. On a noisy radio link, or when the round trip is long (a space probe) or there is no return channel (broadcast TV), **forward error correction** wins. Most real systems use both: Wi-Fi and mobile networks correct inside the radio layer, then check with a CRC, then retransmit what is still wrong.

Real errors often come in **bursts**, which defeat codes designed for isolated flips. Reed-Solomon codes work on whole bytes, and interleaving spreads a burst over many codewords (labs 260 and 360).

The layers repeat the check on purpose. The link CRC protects one hop; the IP and UDP/TCP checksums protect against damage inside routers and against links with no check at all. In lab 160 you see the UDP checksum act as the last line of defence.

To read more: Kurose and Ross, chapter 6, section "Error-Detection and -Correction Techniques"; Tanenbaum, chapter 3, "Error Detection and Correction"; RFC 1071 ("Computing the Internet Checksum"); Hamming's 1950 paper "Error Detecting and Error Correcting Codes".

<!-- detail: 1 -->

## In the lab

**Lab 160-error-detection**: three identical noisy links, one above the other. Top (a1–a2): no check on the link. Middle (b1–b2): one parity bit. Bottom (c1–c2): CRC-32.

- Start it with `./netlab run 160-error-detection` and read the log (`l`) at detail 2.
- Look for the lines with `bit errors`. They end either with `detected, frame discarded` or with `NOT detected by the link: delivered damaged`. On which link does each kind appear? On the parity link, look at the number of flipped bits in each line: what do you notice?
- When a damaged frame gets through, a line `UDP wrong checksum: datagram discarded` follows a few milliseconds later: the transport layer caught it.
- Select a2, then b2, then c2 with `n` and compare the counter `udp.bad_checksum` (at detail 2 the Node tab lists the counters of the selected node). Expect about 70, about 15 and exactly 0 out of 200 datagrams.
- Type `:` and `a1 coding show code=parity pos=3,4`: two flips fool the parity bit and the text HELLO is delivered changed. Then `a1 coding show code=parity pos=3`.

**Lab 161-error-correction**: three links again, with more noise. Top: CRC only. Middle: Hamming(7,4). Bottom: three copies.

- Start it with `./netlab run 161-error-correction`. The log now also shows `corrected by the link code`.
- At the end compare `sink.received` on c2, h2 and r2: about 60 of 200 on the CRC link, 200 of 200 on the others, with no retransmission at all.
- Compare `sink.delay_ms`: about 9.4 ms with the CRC, 12.6 ms with Hamming, 18 ms with three copies. Correction is paid in time on the wire.

**Change this and predict.**

- `./netlab run 160-error-detection ber=1e-3`: how many datagrams will c2 receive? Does a perfect detector help?
- `./netlab run 161-error-correction ber=3e-3`: what will start to happen on the Hamming link? Watch `udp.bad_checksum` on h2.
- `./netlab run 161-error-correction ber=1e-5`: which link is now the best deal?
- In lab 161 type `c1 coding show code=hamming74 pos=3` and then `pos=3,4`.

## Check yourself

- Data `1100101` is sent with an even parity bit. What is the bit? Which received patterns would be accepted although they are wrong?
- Why are longer frames more likely to be damaged than short ones at the same BER?
- A CRC link delivers only frames it believes correct. Why did c2 still receive fewer datagrams than were sent?
- What does Hamming(7,4) do when two bits of the same block are flipped?
- Give one situation where retransmission is impossible or too slow, so that correction is the only choice.
