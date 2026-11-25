<!-- level: 1 -->
<!-- labs: 102-link-properties -->
# Links: delay, bandwidth, loss and queues

A link is the cable or radio hop that joins two devices. It would be nice if it were a perfect pipe: bits in, the same bits out at once. Real links do four things to your packets, and almost everything you will ever notice about a network ("it is slow", "the call is choppy") comes from these four.

Think of a road with a toll booth. The **distance** decides how long the trip takes even with no traffic: that is **propagation delay**. The booth lets through only so many cars per minute: that is **bandwidth**. When cars arrive faster than the booth serves them, they wait in line: that is the **queue**. The parking area in front of the booth is finite, and when it is full new cars are turned away: that is **loss** by overflow. Sometimes a car simply breaks down on the way: that is random loss.

So a packet can be late for two different reasons, far away or stuck in line, and it can vanish for two different reasons, noise or a full queue. Learning to tell them apart is the first skill of anyone who studies networks.

<!-- detail: 2 -->

## How it works

**Bandwidth** (or rate, R) is how many bits per second the link can carry: 1 Mbit/s is one million bits each second. A frame of L bits needs a **transmission time** of L / R to be pushed onto the link, from its first bit to its last.

**Propagation delay** is the time one bit takes to travel the length of the link: distance divided by signal speed (about 200 000 km/s in copper or fibre). It does not depend on the frame size or on the bandwidth.

The frame has arrived when its last bit arrives:

```
one-way time = transmission time + propagation delay = L/R + delay
```

A worked example with the numbers of lab 102 (1 Mbit/s, 20 ms). A ping frame is 102 bytes = 816 bits, so its transmission time is 816 / 1 000 000 s = 0.816 ms. One way takes 20.816 ms; the reply takes the same; the ping reports 41.63 ms. A 1000-byte datagram becomes a frame of 1046 bytes = 8368 bits, and takes 8.368 ms to transmit: ten times longer on the same link.

**Queue.** A link sends one frame at a time. Frames that arrive while it is busy wait in a queue, first in first out. In lab 102 a source sends one 1000-byte datagram every 4 ms (2 Mbit/s), but the link needs 8.368 ms for each. The queue grows by roughly one packet every 8 ms. It has room for 20 packets, so after about 0.15 s it is full, and from then on about every second packet is thrown away: this is **tail drop**.

**Queueing delay.** A packet that arrives when 20 packets are ahead of it waits 20 x 8.368 ms = 167 ms before its own transmission starts. That is why the ping sent during the flood takes about 207 ms instead of 41.6.

**Random loss** is different: a frame is destroyed on the way by noise, with a probability such as `loss=0.1`, even on an empty link.

<!-- detail: 3 -->

## Going deeper

On a path with several links the delays add up, and each router must receive a whole packet before forwarding it (**store and forward**), so the transmission time is paid again on every hop. The textbook sum for one hop is processing + queueing + transmission + propagation. Processing is negligible here; queueing is the only term that changes from packet to packet, which makes it the source of **jitter**, the variation of delay that disturbs voice and video.

The **throughput** of a path is limited by its slowest link, the **bottleneck**. Bandwidth and delay are independent: a satellite link can be fast (many Mbit/s) and still have a 250 ms propagation delay, and no money spent on bandwidth will shorten it. For short messages delay dominates; for big files bandwidth dominates.

How long should a queue be? A short queue drops packets during short bursts that it could have absorbed. A long queue never drops, but under sustained overload it only adds delay: lab 102 with `queue=100` loses the same fraction of packets and makes the survivors wait almost a second. Oversized buffers in home routers caused exactly this problem, known as **bufferbloat**. Note also that no queue can fix a source that keeps sending faster than the link: the loss fraction tends to 1 − R/offered rate whatever the buffer. The real remedy is that senders slow down, which is what TCP congestion control does (lab 320), helped by smarter queues that drop or mark packets early (labs 371 and 372) or serve flows fairly (lab 373).

Simplifications in netlab: loss is independent from frame to frame, while real noise often comes in bursts; and the queue is counted in packets, while real devices often count bytes.

To read more: Kurose and Ross, chapter 1, section "Delay, Loss, and Throughput in Packet-Switched Networks"; Peterson and Davie, "Computer Networks: A Systems Approach", section on performance (bandwidth and latency); RFC 7567 on queue management.

<!-- detail: 1 -->

## In the lab

**Lab 102-link-properties**: two hosts and one link of 1 Mbit/s with 20 ms of delay and a queue of 20 packets.

- Start it with `./netlab run 102-link-properties`.
- From 0 to 1.5 s the link is idle and h1 pings h2 three times. Read the times in the log (`l`). The first is about 82 ms because it waits for ARP; the others are 41.63 ms. Check this against 2 x (20 + 0.816).
- At 3 s h1 starts a 2 Mbit/s stream. Watch the queue number next to h1 on the map climb to 20, then the lines `queue full` appear in the log.
- At 4 s h1 pings again, eight times. About half of the requests find the queue full and are dropped like the packets of the stream; those that get in come back after about 207 ms. Nothing changed in the cable: the extra time is all waiting (20 packets of about 8.4 ms each).
- Select h1 with `n` and open its Links tab; select h2 and press `g` for the Graphs view to see the delay measured by the sink.

**Change this and predict.** Write your prediction before each run.

- `./netlab run 102-link-properties delay=100ms`: what will the idle ping time be?
- `./netlab run 102-link-properties bw=500k`: the idle ping time changes only slightly, why? What fraction of the stream will be lost? (The pings at 4 s now almost never find room: the queue is full nearly all the time.)
- `./netlab run 102-link-properties bw=4M`: will there be a queue at all? Will anything be lost? (The lab's check `stream` computes the loss to expect from `bw` and `queue`: compare it with your number.)
- `./netlab run 102-link-properties queue=100`: does the sink lose fewer packets? What happens to the delay of the ping at 4 s?
- `./netlab run 102-link-properties loss=0.2`: a ping needs both the request and the reply to survive. What fraction of pings do you expect to succeed?

You can also change a link while the lab runs: `:` then `link h1 h2 loss=0.3`, or `link h1 h2 down`.

## Check yourself

- A 1500-byte frame is sent on a 10 Mbit/s link with 5 ms of propagation delay. When does its last bit arrive?
- You double the bandwidth of a link. Which of the two delays changes, and by how much?
- A ping takes 40 ms at night and 300 ms in the evening on the same path. Which of the four effects is the most likely cause?
- Why does a longer queue not reduce the loss of a source that sends at twice the link rate?
- Name one situation where propagation delay matters more than bandwidth, and one where the opposite is true.
