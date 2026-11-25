<!-- level: 1 -->
<!-- labs: 100-two-hosts, 170-switch-learning -->
# Local networks, MAC addresses and switches

A **local network** (LAN) is the group of devices that can reach each other directly, without passing through a router: the computers in a lab, the devices on your home Wi-Fi. Inside a LAN, frames are delivered using **MAC addresses**, the number written by the manufacturer into every network card.

But programs know IP addresses, not MAC addresses. So before the first frame can leave, the sender must ask a question. Imagine a classroom where you know the name of a classmate but not where they sit. You say aloud: "Who is Marta?". Everyone hears; only Marta answers: "I am, here." From then on you talk to her directly. This is **ARP**: a question shouted to all (a **broadcast**), an answer from one, and a note kept for next time (the **ARP cache**).

The box that joins the cables of a LAN is a **switch**. It starts knowing nothing, and learns by watching: each frame that enters shows, with its source address, where that station is. A frame for a station the switch already knows goes out of one port only; a frame for an unknown station, or for everybody, goes out of all the other ports.

<!-- detail: 2 -->

## How it works

A **MAC address** is 48 bits, written as six bytes in hexadecimal: `02:00:4e:4c:01:00`. The special address `ff:ff:ff:ff:ff:ff` is the **broadcast** address: every station on the LAN accepts such a frame. An Ethernet frame begins with the destination MAC, the source MAC and a 2-byte **type** that says what is inside (`0x0800` an IP packet, `0x0806` ARP).

**ARP step by step**, as in lab 100. h1 (`10.0.0.1`) wants to send an IP packet to `10.0.0.2`.

- h1 looks in its ARP cache: nothing.
- h1 sends an **ARP request** in broadcast: "who has 10.0.0.2? tell 10.0.0.1". The packet waits.
- h2 recognises its own IP address. It first notes h1's addresses in its own cache (they were in the request), then sends an **ARP reply** directly to h1: "10.0.0.2 is at 02:00:4e:4c:02:00".
- h1 stores the pair in its cache and finally sends the waiting packet.

This costs one extra round trip, once: in lab 100 the first ping takes 80 ms, the second 40 ms.

**A learning switch** keeps a table "MAC address → port". For every frame it does two things.

- **Learn**: write down (source MAC, port the frame came in from).
- **Forward**: look up the destination MAC. Found: send the frame out of that port only. Not found, or broadcast: **flood**, that is send a copy out of every port except the one it came from.

In lab 170, h1's ARP request enters s1 on port p0. s1 learns "h1 is on p0" and floods; s2 learns "h1 is on p0" (its port towards s1) and floods to h3. The reply of h3 travels back along a single path, because both switches now know where h1 is, and on the way they learn where h3 is. h2 received the broadcast but never sees the reply or the pings.

A switch does not change the frame and has no address of its own in this exchange: hosts do not know it is there.

<!-- detail: 3 -->

## Going deeper

**Why two addresses?** A MAC address is flat: it says who you are, not where you are, and it works only as far as a broadcast can reach. An IP address has structure (network part and host part) and can be routed across the world. ARP is the glue between the two, used on the last hop only. When the destination is on another network, the host asks ARP for the MAC of its **router**, not of the destination (topic 15).

**Tables forget.** Entries of the switch table and of the ARP cache expire (typically after 300 s in switches, less for ARP), so a station that moves to another port is found again. A table that is full or empty costs nothing but efficiency: the switch just floods.

**Limits of the scheme.** Flooding assumes there is exactly one path between any two stations. If switches are cabled in a loop, a broadcast circulates forever and multiplies: a **broadcast storm** (lab 270). The spanning tree protocol disables some ports to break loops (lab 271). And since every broadcast reaches every station, a LAN cannot grow without limit: large sites split it into **VLANs** (lab 203) joined by routers.

**No security.** ARP believes any answer, and a switch believes any source address. A machine can answer ARP requests on behalf of another (ARP spoofing) and receive its traffic. Real networks add protections in the switches.

**History and vocabulary.** Before switches there were **hubs**, which repeated every bit to every port: all stations shared one channel and frames could collide (topic 18). A switch gives every station its own link and stores frames in queues, so collisions disappeared. The first byte `02` of the hoplab addresses marks a locally assigned address; real cards carry a manufacturer prefix. IPv6 replaces ARP with Neighbor Discovery, based on ICMPv6.

To read more: Kurose and Ross, chapter 6, section "Switched Local Area Networks"; Tanenbaum, chapter 4, "Data Link Layer Switching"; RFC 826 (ARP); IEEE 802.1D for bridges.

<!-- detail: 1 -->

## In the lab

**Lab 100-two-hosts**: ARP between two hosts.

- Start it with `./hoplab run 100-two-hosts` and open the Packets view (`p`).
- The first frame is the ARP request. Press Enter on it: check the destination MAC `ff:ff:ff:ff:ff:ff` and the question inside. Open the reply: to whom is it addressed?
- Select h1 (`n`) and open its ARP tab: one line appears after 40 ms.
- At 3 s h2 pings h1 and sends no ARP request. Why? (Set detail 3 and look for `ARP cache` in the log.)
- Type `:` then `link h1 h2 down`, then `h1 ping h2`. What does the log say?

**Lab 170-switch-learning**: three hosts and two switches.

- Start it with `./hoplab run 170-switch-learning` and press `3` for detail 3, so that every `bridge learned` line is shown.
- Follow the first ARP request on the map: it reaches h2 and h3. Then the reply: it goes only towards h1.
- Select s1 and open its MAC table tab. After the first ping it has two lines; note which port each address is on. After 5 s, when h2 pings h1, a third line appears.
- Select h2 and look for the counter `eth.not_for_me` in its Node tab: it is 0 or absent, because no frame addressed to someone else is ever delivered to it.

**Change this and predict.**

- After 7 s type `h3 ping h2`. Which frames will be flooded and which will follow a single path? Check in the log.
- Run `./hoplab run 170-switch-learning delay=20ms`. A frame from h1 to h3 crosses three links. What ping time do you expect once ARP is done?

## Check yourself

- Why is the ARP request sent in broadcast while the ARP reply is not?
- A switch has just been turned on and receives a frame from A to B. What does it learn, and where does it send the frame?
- In lab 170, why does h2 never see the pings between h1 and h3?
- h2 answered h1's ARP request in lab 100. Why can h2 later ping h1 without asking?
- What happens to a broadcast frame if two switches are connected by two cables?
