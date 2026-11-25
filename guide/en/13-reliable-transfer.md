<!-- level: 1 -->
<!-- labs: 120-tcp-handshake -->
# Reliable transfer over an unreliable channel

The network below you loses packets, damages them and sometimes delivers them twice. Yet a file must arrive complete, in order, with nothing repeated. How do you build something reliable out of parts that are not?

Think of dictating a phone number over a bad line. You say a few digits and wait for "ok". If you hear nothing for a while, you say them again. If the other person hears the same group twice, they must understand that it is a repetition and not new digits, so you number the groups: "group one: 347...".

Those are the three tools of every reliable protocol. An **acknowledgement** (ACK) is the "ok" sent back by the receiver. A **timeout** is the decision to send again when no ACK arrives in time. A **sequence number** on every packet lets the receiver recognise duplicates and put things in order.

The simplest protocol, **stop-and-wait**, sends one packet and waits for its ACK before sending the next. It is correct but slow. Faster protocols keep several packets travelling at once, inside a **window**, and differ in what they do when one of them is lost: **Go-Back-N** resends everything from the lost packet on, **Selective Repeat** resends only the lost one.

<!-- detail: 2 -->

## How it works

**Stop-and-wait.** The sender transmits DATA with sequence bit 0, starts a timer and waits. Three things can go wrong, and each has an answer.

- The DATA is lost: no ACK comes, the timer expires, the sender retransmits.
- The DATA arrives damaged: the receiver sees a wrong checksum and does not acknowledge it as good; the sender ends up retransmitting.
- The ACK is lost: the sender retransmits a packet the receiver already has. The receiver sees the same sequence bit again, does **not** deliver the data twice, and repeats the ACK.

One bit is enough because the only doubt is "new packet or repetition of the last one?". The price is speed: one packet per round trip. With 1 s of delay each way, that is one packet every 2 s, however fast the link is.

**Sliding window.** The sender may have up to N unacknowledged packets in flight: the window. Sequence numbers use k bits and wrap around, 0 … 2^k − 1. When the oldest packet is acknowledged the window slides forward and a new packet can leave.

**Go-Back-N.** The receiver is simple: it accepts only the next packet in order and discards everything else, always answering with the number of the last in-order packet. ACKs are **cumulative**: ACK 5 means "everything up to 5 arrived". The sender keeps one timer, for the oldest unacknowledged packet; when it expires it resends **all** packets in the window. One loss can cost N retransmissions.

**Selective Repeat.** The receiver also has a window and a buffer: it keeps packets that arrive out of order and acknowledges each one **individually**. The sender has one timer per packet and resends only the one whose timer expires. Fewer retransmissions, more memory and bookkeeping.

An example with N = 4 and packet 2 lost out of 0 1 2 3 4 5. Go-Back-N: the receiver discards 3, 4 and 5, the sender times out and resends 2 3 4 5. Selective Repeat: the receiver buffers 3, 4 and 5, the sender resends only 2, and then four packets are delivered at once.

<!-- detail: 3 -->

## Going deeper

**How large can the window be?** Sequence numbers are reused, so a late or repeated packet must never be mistaken for a new one with the same number.

For Go-Back-N the limit is **N ≤ 2^k − 1**. Take k = 2 (numbers 0 to 3) and N = 4. The sender transmits 0 1 2 3, all arrive, all four ACKs are lost. The sender times out and resends 0. The receiver is now waiting for the next packet, whose number is again 0: it accepts an old packet as new data. With N = 3 the receiver would be waiting for 3 and would reject the repeated 0.

For Selective Repeat the limit is stricter, **N ≤ 2^(k−1)**, half the number space. Take k = 2 and N = 3. The sender transmits 0 1 2, all arrive, the receiver moves its window to 3 0 1, and all the ACKs are lost. The sender resends the old 0, which falls inside the receiver's new window and is stored as new. The windows of old and new packets must never overlap, hence at most half.

**The timeout** is a compromise. Too short, and packets that were only slow are sent twice; too long, and every loss freezes the transfer. It must exceed the round-trip time, which real protocols measure continuously.

**Efficiency.** Stop-and-wait uses a fraction of the link roughly equal to transmission time / (transmission time + round-trip time): tiny on fast, long links. A window large enough to cover the round trip keeps the link busy all the time; this is called pipelining.

**What real systems add.** TCP is a hybrid: cumulative ACKs like Go-Back-N, a receiver buffer like Selective Repeat, 32-bit sequence numbers that count bytes, an adaptive timeout, fast retransmit after three duplicate ACKs, and optional selective acknowledgements (SACK). It also changes the window size all the time, to protect the receiver (flow control) and the network (congestion control). Link layers such as Wi-Fi use stop-and-wait on each hop.

To read more: Kurose and Ross, chapter 3, section "Principles of Reliable Data Transfer" (the protocols rdt 1.0 to 3.0, Go-Back-N and Selective Repeat); Tanenbaum, chapter 3, "Sliding Window Protocols"; RFC 9293 for TCP.

<!-- detail: 1 -->

## In the lab

The labs of this topic are not hoplab scenarios but the three original demo programs of this repository: a sender and a receiver that exchange **real UDP packets** on your machine through a channel that loses, corrupts and delays them on purpose. Start them from the main menu, entry "Reliable transfer demos", or from a shell.

- `./demo.sh` starts stop-and-wait, `./demo.sh gbn` Go-Back-N, `./demo.sh sr` Selective Repeat. The sender is on the left, the receiver on the right; `q` quits. `./demo.sh -h` lists all options.
- Each side shows the packets travelling on the channel, the event log, the rules of the protocol with the one just applied highlighted, and the strip of sequence numbers with the window and the timers.
- By default each packet is lost with probability 15% and corrupted with probability 10%, the delay is 1000 ms each way and the timeout 3000 ms.
- `-w N` sets the window and `-b K` the bits of the sequence numbers (defaults 4 and 3). `-s "..."` passes extra options to the sender and `-r "..."` to the receiver: `-l` loss, `-c` corruption, `-d` delay in ms, `-t` timeout in ms, `-S` a seed to repeat the same run. The sender's `-l` and `-c` act on DATA, the receiver's on ACKs.

**What to watch.** Run `./demo.sh` and wait for each of the three accidents listed above: a lost DATA, a corrupted packet, a lost ACK. For each, find the rule that the sender and the receiver apply. Then run `./demo.sh gbn` and `./demo.sh sr` and compare what is retransmitted after a loss.

**Change this and predict.**

- `./demo.sh -s "-l 0 -c 0" -r "-l 0 -c 0"`: a perfect channel. How many packets per second does stop-and-wait deliver? Now the same options with `gbn`.
- `./demo.sh -s "-t 1500"`: the timeout is shorter than the round trip (2000 ms). What will happen to every packet?
- `./demo.sh gbn -s "-l 0.3 -c 0.2" -r "-l 0.3"` and the same with `sr`: which one sends fewer packets in total?
- `./demo.sh gbn -w 8 -b 3`: the window breaks N ≤ 2^k − 1 (the header of the window warns you). Wait for a whole window of ACKs to be lost.
- `./demo.sh sr -w 3 -b 2 -s "-l 0.4" -r "-l 0.4"`: the window breaks N ≤ 2^(k−1). Read the text delivered by the receiver: what is wrong with it?

**Next step.** Lab 120-tcp-handshake shows the same ideas inside TCP; run it with `./hoplab run 120-tcp-handshake loss=0.1 bytes=50000` and look for the retransmissions in the log.

## Check yourself

- Why does the receiver send an ACK again when it gets a duplicate, instead of staying silent?
- In stop-and-wait, what goes wrong if packets carry no sequence number and an ACK is lost?
- With 3-bit sequence numbers, what is the largest safe window for Go-Back-N? And for Selective Repeat?
- Packet 2 of 0 … 5 is lost with a window of 4. List the packets retransmitted by Go-Back-N and by Selective Repeat.
- What happens if the timeout is shorter than the round-trip time? And if it is ten times longer?
