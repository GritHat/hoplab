<!-- level: 1 -->
<!-- labs: 120-tcp-handshake, 101-ping -->
# The transport layer: ports, UDP and TCP

IP brings a packet to the right machine. But a machine runs many programs at once: a browser, a chat, a game. Which of them should get the packet? And what if the packet is lost on the way? Answering these two questions is the job of the **transport layer**.

An IP address is like the street address of a building; a **port** is the number of an apartment inside it. Every packet carries a source port and a destination port, so that the operating system can deliver the data to the right program and the answer can find its way back.

There are two transport protocols, with opposite characters.

**UDP** is a postcard. You write, you post, and that is all: no guarantee that it arrives, no notice if it does not. It is simple and immediate.

**TCP** is a phone call. First the two sides establish a **connection** ("hello?" "hello, I hear you" "good"). Then everything that is said arrives, in order, without gaps or repetitions, because TCP numbers the bytes, waits for acknowledgements and sends again what was lost. At the end both sides say goodbye.

Web pages, mail and file transfers use TCP. Name lookups, live voice and many games use UDP.

<!-- detail: 2 -->

## How it works

A **port** is a 16-bit number, 0 to 65535. Servers wait on well-known ports (80 for web, 53 for DNS; in netlab the echo service uses 7). A client lets the system pick a temporary **ephemeral port**: in the labs you will see 49152. A conversation is identified by four values: source address, source port, destination address, destination port.

**UDP** adds a header of only 8 bytes: source port, destination port, length, checksum. One send by the program becomes one **datagram**. If nobody listens on the destination port, the host answers with an ICMP "port unreachable".

**TCP** offers a **stream of bytes**: the program writes bytes at one end and they come out at the other in the same order. TCP cuts the stream into **segments**, each with a 20-byte header containing ports, a **sequence number**, an **acknowledgement number**, flags (SYN, ACK, FIN, RST) and a window.

**Opening: the three-way handshake.**

- The client sends **SYN** with its initial sequence number x.
- The server answers **SYN+ACK**: its own initial number y, and ack = x + 1.
- The client answers **ACK** with ack = y + 1. Data can now flow.

**Data.** The sequence number of a segment is the number of its first byte. The receiver answers with a **cumulative ACK**: the number of the next byte it expects. In lab 120, 4000 bytes travel in three segments, because a segment carries at most MSS = 1460 bytes: 1460 + 1460 + 1080. Counting the first data byte as number 1, the ACKs say 1461, 2921, 4001 (the real numbers start from a large random value, as you will see in the Packets view). A segment that is not acknowledged in time is sent again.

**Closing.** Each direction is closed separately: one side sends **FIN**, the other acknowledges it, then sends its own FIN, which is acknowledged in turn. SYN and FIN each count as one byte in the numbering.

**Refusal.** A SYN sent to a port where no program is listening gets a **RST** (reset): "connection refused".

<!-- detail: 3 -->

## Going deeper

**Why three messages to open?** Each side must learn the other's initial sequence number and confirm it. Two messages would leave the server unsure that its number was received; and an old, delayed SYN could open a connection that nobody wants. Initial numbers are chosen unpredictably, so that segments of an earlier connection, or forged by an attacker, are not accepted as valid.

**Closing is subtle too.** The side that closes first waits in the `TIME_WAIT` state before forgetting the connection, in case its last ACK was lost and the other FIN is repeated. You can read the state names in the netlab log.

**TCP's reliability is the machinery of topic 13**, grown up: sequence numbers count bytes instead of packets; the timeout adapts to the measured round-trip time; three duplicate ACKs trigger a fast retransmit without waiting for the timer (lab 220). On top of it TCP adjusts how much it sends: **flow control** obeys the window advertised by the receiver (lab 221), **congestion control** estimates what the network can carry and backs off on loss (lab 320). The parameter `iw` of lab 120 is the initial value of that estimate.

**What TCP does not give.** No message boundaries: two writes may arrive as one read, so applications must mark where messages end. No guarantee of timing: a lost segment holds back everything behind it (**head-of-line blocking**), which is bad for live audio. No security: that is added by TLS above it.

**Why UDP at all?** Because sometimes the cure is worse than the disease. A question and an answer that fit in one packet each (DNS) do not deserve a handshake. Live voice prefers a gap to a late repetition. And UDP is the base on which new transports are built in applications: **QUIC**, used by HTTP/3, implements connections, reliability and encryption over UDP.

The cost of a connection is real: a handshake is one round trip before the first useful byte, which topic 17 measures on a web page.

To read more: Kurose and Ross, chapter 3 ("Transport Layer": multiplexing, UDP, TCP connection management); Tanenbaum, chapter 6; RFC 768 (UDP), RFC 9293 (TCP), RFC 9000 (QUIC).

<!-- detail: 1 -->

## In the lab

**Lab 120-tcp-handshake**: the whole life of a TCP connection between h1 and h2, with 50 ms of delay each way.

- Start it with `./netlab run 120-tcp-handshake` and press `2` for detail 2.
- Open the Sequence view (`s`): the arrows draw the handshake at 1 s, the data segments, the ACKs coming back, and the two FINs.
- Open the Packets view (`p`) and press Enter on the SYN, the SYN+ACK and the ACK. Read the ports (49152 and 5001), the flags, and check the rule ack = sequence number of the other side + 1.
- Open the three data segments and their ACKs: by how much does the sequence number grow from one to the next? What does each ACK say?
- The log reports `connected (200.2 ms)`. A handshake needs one round trip, 100 ms: where does the rest come from? (Look at what precedes the SYN+ACK.)
- At 6 s h1 tries port 81: find the RST and the `connection refused` line.

**Lab 101-ping**, the UDP part.

- Start `./netlab run 101-ping` and go to 8 s: `udp → 10.0.2.10:7`. No handshake: the datagram leaves at once and the echo comes back after a single round trip.
- This datagram is so large that it is cut into three fragments, and only the first carries the UDP header. For a cleaner view type `:` and `h1 udp h2 msg=ciao`, then open that frame in the Packets view and find the UDP header with its four fields. Compare its 8 bytes with the TCP header of lab 120.

**Change this and predict.**

- `./netlab run 120-tcp-handshake delay=100ms`: after how long will the connection be established?
- `./netlab run 120-tcp-handshake bytes=20000`: how many segments? Watch in the Sequence view how many travel together at the start and later.
- `./netlab run 120-tcp-handshake loss=0.1 bytes=50000`: look for `fast retransmit` and `retransmission timeout` in the log, and read the final summary line. Every run is different. A lost ACK often causes no retransmission at all: why?
- In lab 101 type `:` and `h1 udp h2 port=9 msg=hi`: nobody listens on port 9. What will come back? Compare with what TCP did on port 81.

## Check yourself

- Two browser tabs on the same computer load pages from the same server. How does the reply to each reach the right tab?
- Write the three segments of the handshake, with the flags and the relation between their numbers.
- A receiver has bytes 1 to 2920 and then receives the segment starting at 4381. Which ACK number does it send?
- Give one application for which UDP is the better choice, and say why.
- What is the difference between a SYN that gets no answer and one that gets a RST?
