<!-- level: 1 -->
<!-- labs: 162-aloha -->
# Sharing one channel: ALOHA

Radio waves spread everywhere. When several stations use the same frequency, as the devices on a Wi-Fi network do, they all share **one channel**: if two transmit at the same time, their signals mix and neither frame can be understood. This is a **collision**, and both frames are lost.

Picture a dinner table where everybody talks without looking at the others. If two people start together, nobody understands either. A chairperson could give the floor to one speaker at a time, but a network often has no chairperson. The stations must manage alone, with a rule that each one applies by itself: a **medium access** protocol.

The oldest rule is the bluntest. In **ALOHA**, a station that has a frame simply sends it. If no confirmation comes back, it assumes a collision, waits a **random** time and tries again. The randomness is essential: if both stations waited the same time, they would collide again, forever.

**Slotted ALOHA** adds one piece of discipline: a common clock divides time into slots as long as one frame, and a station may start only at the beginning of a slot. With this single change the channel carries about twice as much traffic. Even so, most of the channel's time is wasted in silence or collisions.

<!-- detail: 2 -->

## How it works

Take frames that all last the same time T, the **frame time**. Two numbers describe the channel, both measured in frames per frame time.

- **G**, the load: how many transmissions are attempted, new frames plus retries.
- **S**, the throughput: how many succeed. S can never exceed 1, which would mean a channel busy all the time with good frames.

**Pure ALOHA.** A frame that starts at time t is destroyed if any other station starts between t − T and t + T: a frame that began up to T earlier is still on the air, and one that begins up to T later hits its tail. The **vulnerable period** is 2T long.

```
        t-T          t          t+T
         |-----------|###########|
          danger       my frame
         <----- 2T vulnerable ---->
```

With random, independent stations, the probability that nobody else starts during 2T is e^(−2G), so

```
S = G · e^(−2G)     maximum 0.184 at G = 0.5
```

**Slotted ALOHA.** Frames begin only at slot boundaries, so two frames either overlap completely or not at all. Only the stations that choose the **same slot** are dangerous: the vulnerable period is T.

```
S = G · e^(−G)      maximum 0.368 at G = 1
```

A worked example. With G = 0.5, pure ALOHA gives S = 0.5 x e^(−1) = 0.18: of every 100 frame times, 18 carry a good frame. Slotted ALOHA with the same G gives 0.5 x e^(−0.5) = 0.30.

Both curves rise, reach a peak and then fall. Past the peak, more attempts mean more collisions, more collisions mean more retries, and the throughput **decreases** while the load grows. In lab 162, ten stations together offer 0.4 new frames per frame time. Slotted ALOHA can carry that (S is about 0.36). Pure ALOHA cannot, because 0.4 is above its maximum of 0.18: retries pile up, G grows, and S settles around 0.13 to 0.15.

**Backoff** is the random wait before a retry. The longer the average wait, the lower the load the stations put on the channel.

<!-- detail: 3 -->

## Going deeper

**Where the formulas come from.** They assume a very large number of stations whose attempts form a Poisson process: the number of starts in an interval of length x frame times is random with mean G·x, and the probability of zero starts is e^(−G·x). Put x = 2 or x = 1 and you get the two results. With only ten stations and queues of finite length the measured points follow the shape of the curve, not its last digit.

**Instability.** ALOHA with a fixed retry rule has no stable working point at high load: a burst of collisions increases the backlog, which increases G, which increases collisions. Real systems make the backoff grow with the number of failures (Ethernet's **binary exponential backoff** doubles the range at every collision) so that stations slow down when the channel is crowded.

**The cost of slots** is a common clock, which needs some synchronisation signal, and the wait for the next slot even when the channel is free.

**Doing better: listen first.** ALOHA stations are deaf. If a station listens before talking (**carrier sensing**, CSMA) it avoids destroying a frame already on the air. This works well when the propagation delay is small compared with the frame time. Classic Ethernet also listened **while** transmitting and stopped at once on a collision (CSMA/CD, lab 261). Radios cannot hear others while they transmit, so Wi-Fi waits for silence, counts down a random backoff and relies on acknowledgements (CSMA/CA); lab 460 shows what happens when two stations cannot hear each other. Lab 361 draws the S(G) curves of all these protocols.

**The other family** avoids collisions altogether by assigning turns: time slots (TDMA), frequencies (FDMA), a token, or a base station that schedules everybody, as in mobile networks. These are efficient under heavy load but waste capacity when few stations have something to say; random access is the opposite.

**Where ALOHA lives on.** It was built in 1970 to connect the Hawaiian islands by radio. Slotted ALOHA is still used for the first message a mobile phone sends to a base station, and for reading RFID tags.

To read more: Kurose and Ross, chapter 6, section "Multiple Access Links and Protocols"; Tanenbaum, chapter 4, "Multiple Access Protocols"; Abramson, "The ALOHA System" (1970).

<!-- detail: 1 -->

## In the lab

**Lab 162-aloha**: two independent channels with ten stations each and the same traffic. The node `pure` runs pure ALOHA, the node `slotted` runs slotted ALOHA. No packets travel on the map here: everything happens in the panel of each node.

- Start it with `./netlab run 162-aloha`.
- Select `pure` with `n` and move to its `mac` tab with the arrow keys (press Tab until the node panel has the focus; `z` enlarges it). There is one row per station: green is a delivered frame, red a collision, yellow dots a station waiting to retry.
- Pause with space and look for a red pair that overlaps only by a small piece: both frames are lost entirely.
- Select `slotted` and open the same tab: all frames start aligned. Can you find a partial overlap?
- Below the rows read G and S for both; next to them `theory` is the S that the formula gives for the measured G. Let the lab run to 60 s and compare: about 0.13 to 0.15 for pure, about 0.36 for slotted.

**Change this and predict.** For each run compute S from the two formulas first.

- `./netlab run 162-aloha load=0.1`: will the two protocols differ much?
- `./netlab run 162-aloha load=0.2`: this is close to the limit of pure ALOHA.
- `./netlab run 162-aloha load=1`: what fraction of slots carries a good frame in slotted ALOHA? What happens to the queues of the stations?
- `./netlab run 162-aloha stations=3`: with few stations, are the formulas still a good guide?
- `./netlab run 162-aloha frame=500ms` slows the timeline down so that you can follow single frames.

## Check yourself

- Why are both frames lost in a collision, and not only the second one?
- Why must the wait before a retry be random?
- Explain in words why the vulnerable period of pure ALOHA is two frame times and that of slotted ALOHA is one.
- Compute S for slotted ALOHA at G = 1 and at G = 3. What does the comparison tell you?
- Which information would let a station avoid most collisions, and which protocol uses it?
