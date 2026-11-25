<!-- level: 1 -->
<!-- labs: 100-two-hosts, 101-ping -->
# Layers and packets

A network moves **messages** between programs running on different machines. No single piece of software does the whole job. The work is split into **layers**, each one solving a single problem and trusting the layer below for the rest.

Think of sending a letter. You write the message. You put it in an envelope with the name of the person. The post office puts the envelope in a bag labelled with the destination city. A van carries the bag to the next sorting centre, whatever is inside. Each level adds its own label and ignores the labels of the others.

Networks do exactly this, and it is called **encapsulation**: the data of a program is wrapped in a transport header, then in a network header, then in a link header. What travels on the cable is one block of bytes, the **frame**, with the headers one inside the other like nested envelopes. The receiver opens them in the opposite order.

Each layer has its own kind of **address**: a port names a program, an IP address names a machine anywhere in the world, a MAC address names a network card on the same cable.

<!-- detail: 2 -->

## How it works

The Internet uses five layers. From the top:

- **Application** (layer 7 in the netlab log, `L7`): the program and its messages, for example a web request. Lower layers treat it as plain bytes.
- **Transport** (`L4`): delivers data to the right program on a machine, using 16-bit **port** numbers. Its unit is the **segment** (TCP) or **datagram** (UDP).
- **Network** (`L3`): carries a **packet** from the source machine to the destination machine across many networks, using 32-bit **IP addresses** such as `10.0.0.2`.
- **Link** (`L2`): carries a **frame** over one cable or one radio hop, using 48-bit **MAC addresses** such as `02:00:4e:4c:01:00`.
- **Physical** (`L1`): turns the bits of the frame into signals.

The names matter: a frame contains a packet, a packet contains a segment, a segment contains application data.

A worked example, the `ping` you will see in lab 100. The program asks for 56 bytes of data to be sent. ICMP (the protocol ping uses) adds an 8-byte header. IP adds a 20-byte header with source address `10.0.0.1`, destination `10.0.0.2` and a few other fields. Ethernet adds 14 bytes in front (destination MAC, source MAC, a type field saying "an IP packet follows") and a 4-byte check at the end. Total on the cable:

```
 14        20        8       56        4      = 102 bytes
[Ethernet][IP      ][ICMP  ][data    ][check]
```

Only 56 of 102 bytes are data: the rest is **overhead**, the price of the labels.

The key rule is that addresses of different layers have different lifetimes. When a packet crosses a router, the **IP addresses stay the same** from the first machine to the last, but the **MAC addresses change at every hop**, because each frame only lives on one link. The router removes the old link header, reads the IP header, and builds a new frame for the next link.

<!-- detail: 3 -->

## Going deeper

Why layers at all? Because each one can change without touching the others. Wi-Fi replaced cables under IP and no web browser had to be rewritten; HTTP got new versions and no router noticed. The cost is some inefficiency (repeated headers, repeated checks) and the fact that a layer sometimes needs information that the layering hides from it.

Two models are used to talk about layers. The **OSI model** has seven; the Internet in practice has the five listed above, and the OSI numbers survived as names: people say "layer 2 switch" and "layer 7 load balancer", and netlab labels its log lines the same way. Do not expect every protocol to fit cleanly: ARP sits between layers 2 and 3, ICMP is carried inside IP but is part of the network layer, and tunnels put a whole packet inside another packet.

The IP layer is deliberately minimal: it promises only **best effort**, meaning packets may be lost, duplicated, delayed or reordered. Everything else (reliability, order, security) is built at the edges, in the hosts. This is the **end-to-end argument**, and it is the reason the network core could stay simple and grow so much. It is also called the "hourglass": many applications above, many link technologies below, one narrow protocol, IP, in the middle.

Sizes are bounded: an Ethernet frame carries at most 1500 bytes of packet, the **MTU**. Larger packets must be split (lab 101 shows a 3000-byte datagram cut into three fragments).

To read more: Kurose and Ross, "Computer Networking: A Top-Down Approach", chapter 1 ("Computer Networks and the Internet", the section on protocol layers); Tanenbaum, "Computer Networks", chapter 1 on reference models; RFC 1122 ("Requirements for Internet Hosts") for the official list of layers; Saltzer, Reed and Clark, "End-to-End Arguments in System Design".

<!-- detail: 1 -->

## In the lab

**Lab 100-two-hosts** is the smallest network: two hosts and a cable.

- Start it with `./netlab run 100-two-hosts`. Press space to pause when you want to read.
- Press `p` for the Packets view. Each line is one frame. Move to the first ICMP frame and press Enter: the dissection shows the headers one inside the other, Ethernet, then IP, then ICMP, with every field and the bytes that carry it. Use `[` and `]` to walk through the fields.
- Find the three kinds of address: the MAC addresses in the Ethernet header, the IP addresses in the IP header. Ping has no ports: those appear in lab 101, in the UDP datagram at 8 s.
- Add up the header sizes and compare with the 102 bytes computed above.

**Lab 101-ping** adds a router between two networks.

- Start it with `./netlab run 101-ping` and open the Packets view.
- Compare the same echo request on the two links, `h1 → r1` and `r1 → h2`. Predict first: which addresses are the same, which are different? Then check in the dissection. Look also at the TTL field of the IP header: 64 on the first link, 63 on the second.
- At 8 s a UDP datagram is sent: open it and find the fourth header, UDP, with its two ports.

**Change this and predict.** In lab 101 type `:` and then `h1 ping h2 size=1000`. How many bytes will each frame have on the cable? (Write your sum before opening the frame.) Then try `h1 ping h2 size=2000`: this packet does not fit in 1500 bytes. What do you expect to see in the Packets view?

## Check yourself

- Put these in order from outermost to innermost in a frame carrying a web request: TCP segment, HTTP message, IP packet, Ethernet frame.
- A packet goes from h1 to h2 through one router. Which addresses does the router change, and which does it leave alone?
- Why does a 56-byte ping occupy 102 bytes on the cable?
- A port, an IP address and a MAC address each answer a different "who?" question. Which one?
- What would have to be rewritten in a web browser if your computer moved from a cable to Wi-Fi? Why?
