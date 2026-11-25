<!-- level: 0 -->
<!-- labs: 101-ping, 200-static-routing, 102-link-properties -->
# Writing your own labs

A lab is a plain text file with the extension `.nl`. Each line is one directive: a word followed by its arguments. You describe the nodes, connect them with links, give them addresses, say what runs and when, and netlab does the rest. This topic builds a complete lab step by step; the full reference of the language is in `docs/scenario-format.md`.

The quickest start is not an empty file. Choose **Build your own network** in the menu: the wizard writes a scenario for the topology you ask for and saves it in `~/.local/share/netlab/scenarios/`. Open that file in your editor and change it. Or copy a lab of the catalog from `scenarios/` and modify the copy.

## Step 1: nodes and links

Create a file `two-offices.nl`. A PC in one office reaches a server in another through two routers joined by a slow line.

```
node pc  host
node gw1 router
node gw2 router
node srv host

link pc  gw1 bw=100M delay=1ms
link gw1 gw2 bw=1M delay=20ms queue=20
link gw2 srv bw=100M delay=1ms
```

`node NAME KIND` creates a node; the kind is `host`, `router` or `switch`. `link A B` joins two nodes, and the `key=value` pairs describe the link: `bw` is its rate in bits per second, `delay` the propagation delay in one direction, `queue` how many packets can wait to be transmitted. A link with no parameters is 10 Mbit/s, 10 ms, 50 packets and loses nothing.

<!-- detail: 2 -->
Names start with a letter and have at most 12 characters. Interfaces are created as links are added, in order: the first link of a host or router uses `eth0`, the second `eth1`; the ports of a switch are `p0`, `p1`... You can name the interface explicitly with `link gw1:eth1 gw2:eth0`, which is useful when the addresses you assign later must match a drawing.

The other link parameters: `loss` (probability that a frame is lost, `0.05` or `5%`), `burst` (losses come in bursts of this mean length), `jitter` (random extra delay), `ber` (bit error rate), `corrupt` (probability of one flipped bit per frame), `dup`, `reorder`, `mtu`, `qdisc` (the queue discipline: `red`, `codel`, `fq`...) and `codec` (the error detecting or correcting code of the link). Times are written `500us`, `20ms`, `1.5s`; rates `56k`, `10M`, `1G`.

<!-- detail: 3 -->
The map is laid out automatically. To place the nodes yourself add `x=` and `y=` (both, from 0 to 100, the origin is the top left corner) to the `node` lines; `role=TEXT` adds a small label next to the node on the map, such as `role=DNS`. A file can have at most 64 nodes, 160 links and 16 interfaces per node.

<!-- detail: 1 -->
## Step 2: addresses

The easy way is one line:

```
auto all
```

`auto all` gives every network of the topology its own subnet (`10.0.1.0/24`, `10.0.2.0/24`, ... in the order the links are written; routers get `.1`, `.2`, hosts `.10`, `.11`), gives every host a default gateway, and installs on every node shortest-path routes to all the networks. Use it whenever addressing and routing are not what the lab is about.

<!-- detail: 2 -->
When addressing or routing is the subject, do it by hand:

```
ip pc 192.168.1.10/24
ip gw1:eth0 192.168.1.1/24
ip gw1:eth1 10.9.9.1/30
ip gw2:eth0 10.9.9.2/30
ip gw2:eth1 192.168.2.1/24
ip srv 192.168.2.10/24
gw pc gw1
gw srv 192.168.2.1
route gw1 192.168.2.0/24 via gw2
route gw2 192.168.1.0/24 via 10.9.9.1
```

`ip NODE A.B.C.D/LEN` sets the address of the first interface that has none; `ip NODE:IF ...` names the interface. `gw NODE GATEWAY` sets the default route and `route NODE PREFIX via GATEWAY` a static route; the gateway can be an address or the name of a neighbour that shares a subnet with the node.

The two halves of `auto` can be used separately. `auto addr` assigns addresses and the default gateways of the hosts, but leaves the routers without routes to distant networks: this is what you want when a routing protocol such as RIP or OSPF must discover them. `auto routes` computes the static routes from addresses you wrote yourself.

<!-- detail: 3 -->
`auto` is applied once, after the whole file has been read, and never overwrites what you set by hand: a network where some interface already has an address is left alone, and a host that already has a default route keeps it. Wherever the `auto` line is in the file, it sees all the `ip` lines. Switches have no addresses; all the nodes attached to the same switch (or to the same VLAN, set with `vlan SWITCH:PORT N`) share one subnet.

<!-- detail: 1 -->
## Step 3: what happens

Now make something happen. `run NODE MODULE arguments` starts a protocol or a tool on a node at time zero; `at TIME command` runs a command at a later time.

```
run pc ping srv count=3
at 5s link gw1 gw2 down
at 6s run pc ping srv count=2
at 9s link gw1 gw2 up
at 10s run pc ping srv count=2
duration 14s
```

`duration` says how long the lab lasts when it runs without the interface. In the interface it keeps running until you quit.

After `at` you can write anything you could type on the command line of a running lab: `run ...`, `link A B down`, `link A B loss=0.2`, `node N down`, `route ...`, `stop NODE MODULE`.

<!-- detail: 2 -->
The modules and their arguments are listed in the **Modules** entry of the menu and by `./netlab modules` and `./netlab help MODULE`. A destination can be a node name (`srv`), an interface (`gw1:eth1`) or an address.

Events that involve chance (losses, random timers) depend on a seed. A run started with `--seed N` always goes the same way; without it every run is different. Writing `seed 7` in the file fixes the seed for everybody who does not pass `--seed`.

<!-- detail: 1 -->
## Step 4: tell the story

Put the metadata at the top of the file, and explain what happens with notes:

```
title  Two offices and a slow line
level  0
layer  3
about  A PC reaches a server across a slow line between two routers. At 5 s the line fails; at 9 s it is repaired.

note 0   The first ping waits for ARP on every hop, the next ones do not.
note 5s  The line is down: gw1 has no usable route and answers with an ICMP error.
note 9s  The line is back, and so are the replies.
```

`title` is the name shown in the catalog and `about` the description (several `about` lines are joined into one paragraph). `note TIME text` is the commentary: when the lab reaches that time the text appears at the bottom of the map, and the Guide view lists all the notes as a timeline.

<!-- detail: 2 -->
`level` is the track (1 Foundations, 2 Core, 3 Advanced, 4 Frontier) and `layer` the protocol layer (1 physical, 2 link, 3 network, 4 transport, 7 application, 8 distributed): the catalog uses them to group and filter. `topic WORD` adds a keyword for the catalog search. An `about` line with no text starts a new paragraph.

Lines that start with `#` are comments.

<!-- detail: 1 -->
## Step 5: parameters

A parameter turns one lab into a family of experiments. Declare it with a name, a default value and a label, then use it as `$name` anywhere in the file:

```
param  loss 0 Loss on the slow line (0..1)
param+ delay 20ms One-way delay of the slow line

link gw1 gw2 bw=1M delay=$delay loss=$loss queue=20
```

The launch form of the catalog shows one field per parameter. `param+` declares an **advanced** parameter, shown only when the form is switched to advanced settings. From the command line: `./netlab run two-offices.nl loss=0.1`.

<!-- detail: 2 -->
A `param` line must come before the lines that use the parameter. `${name}` works too, for when a letter follows directly. A value given on the command line wins over the default.

<!-- detail: 1 -->
## Step 6: expectations

An expectation says what must be true at the end of the run. It turns the lab into a test of itself:

```
expect pc ping.reply == 5
expect pc ping.error >= 1
```

`expect NODE COUNTER OP VALUE` compares a counter of a node with a number; the operators are `==`, `!=`, `>=`, `<=`, `>` and `<`. Counters are kept by the modules (`ping.reply`, `tcp.sink_bytes`, `ip.forwarded`...): to see which ones exist, run the lab, select the node and read the **Counters** section of its Node tab at detail 2.

<!-- detail: 2 -->
Expectations are checked when a run without the interface reaches `duration`, and printed when you quit the interface. A counter that was never touched counts as 0. Write expectations for the default values of the parameters: with `loss=0.5` the lab above rightly fails.

<!-- detail: 1 -->
## Step 7: run and check

```
./netlab run two-offices.nl
./netlab run two-offices.nl --headless --detail 1
./netlab check two-offices.nl
```

The first command opens the interface. The second prints the log, then the expectations with their values and `PASS` or `FAIL`. The third does the same quietly with a fixed seed, for one file or for many. A mistake in the file is reported with its line number:

```
two-offices.nl:7: unknown node or interface in 'link gw1 gw3'
```

To see your lab in the catalog, put the file in `~/.local/share/netlab/scenarios/`: it appears under **My scenarios**.

<!-- detail: 2 -->
## Translations

A lab can carry its own translations. Add the language code in square brackets to `title`, `about`, `note` and `param`:

```
title      Two offices and a slow line
title[it]  Due uffici e una linea lenta
about[it]  Un PC raggiunge un server attraverso una linea lenta tra due router.
note 5s    The line is down.
note[it] 5s La linea è guasta.
param      loss 0 Loss on the slow line (0..1)
param[it]  loss Perdita sulla linea lenta (0..1)
```

When netlab runs in Italian the `[it]` lines replace the plain ones; in any other language they are ignored. A translated note replaces the note with the same time, and `param[it]` gives only the name and the translated label: the default value stays with the plain `param` line, which must come first.

<!-- detail: 3 -->
## The whole file

```
title      Two offices and a slow line
title[it]  Due uffici e una linea lenta
level 0
layer 3
about      A PC reaches a server across a slow line between two routers. At 5 s the line fails; at 9 s it is repaired.
about[it]  Un PC raggiunge un server attraverso una linea lenta tra due router. A 5 s la linea si guasta; a 9 s viene riparata.
param      loss 0 Loss on the slow line (0..1)
param[it]  loss Perdita sulla linea lenta (0..1)
param+     delay 20ms One-way delay of the slow line
param[it]  delay Ritardo della linea lenta (una direzione)

node pc  host
node gw1 router
node gw2 router
node srv host role=server
link pc  gw1 bw=100M delay=1ms
link gw1 gw2 bw=1M delay=$delay loss=$loss queue=20
link gw2 srv bw=100M delay=1ms
auto all

run pc ping srv count=3
note 0     The first ping waits for ARP on every hop, the next ones do not.
note[it] 0 Il primo ping attende ARP a ogni salto, i successivi no.
at 5s link gw1 gw2 down
at 6s run pc ping srv count=2
note 5s    The line is down: gw1 has no usable route and answers with an ICMP error.
note[it] 5s La linea è guasta: gw1 non ha più una rotta utilizzabile e risponde con un errore ICMP.
at 9s link gw1 gw2 up
at 10s run pc ping srv count=2
note 9s    The line is back, and so are the replies.
note[it] 9s La linea è tornata, e con lei le risposte.
duration 14s
expect pc ping.reply == 5
expect pc ping.error >= 1
```

Things worth knowing when a lab grows. A `run` line is executed at time zero, after addressing, in the order of the file; `at 0 ...` is the same thing. In a file the time of `at` is absolute, from the start of the lab. `node N down` and `node N up` are commands for a running lab, so in a file they need `at`: `at 3s node gw2 down`. A lab can be run across several machines by adding `place` and `monitor` lines: see `docs/distributed.md`. And when the protocol you need does not exist yet, `docs/writing-modules.md` explains how to add one.
