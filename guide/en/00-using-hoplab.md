<!-- level: 0 -->
<!-- labs: 100-two-hosts, 101-ping, 102-link-properties, 120-tcp-handshake -->
# Using hoplab

hoplab is a network laboratory that lives in your terminal. It emulates hosts, routers and switches that exchange real frames, byte for byte (Ethernet, ARP, IPv4, ICMP, UDP, TCP and the protocols above them), over links that have a bandwidth, a delay, a queue and, when you want, losses and bit errors. Nothing touches your real network: everything runs on a virtual clock that you can slow down, pause and step.

A **lab** (or scenario) is a small text file that describes a network and what happens on it. hoplab ships with a catalog of labs ordered from the first ping to consensus algorithms, and this guide tells you what to look for in each one.

## Starting

Run the program with no arguments to get the menu:

```
./hoplab
```

Move with the arrow keys and press Enter, or press the number of an entry. **Scenario catalog** lists the ready-made labs: choose one, press Enter, then Enter again on **Start**. From this guide you can also start the labs listed at the bottom of a topic by pressing their letter (`a`, `b`, `c`, ...).

If you already know the name of a lab you can skip the menu:

```
./hoplab run 101-ping
./hoplab run ping
```

<!-- detail: 2 -->
The menu has seven entries: **Scenario catalog**, **Study guide** (what you are reading), **Build your own network** (a form that writes a scenario for you: topology, link parameters, routing, traffic), **Open a scenario file**, **Modules** (the reference of every protocol and tool), **Reliable transfer demos** (the original stop-and-wait, Go-Back-N and Selective Repeat programs over real UDP) and **Quit**.

In the catalog, `/` searches titles and topics, `Tab` filters by layer, and the right-hand side shows the description of the selected lab, its level, the number of nodes and links, and its parameters. Networks built with the wizard are saved in `~/.local/share/hoplab/scenarios/` and appear in the catalog under **My scenarios**.

In this guide: `↑` `↓` choose a topic, `→` or Enter moves to the text, `←` goes back to the list, `PgUp` `PgDn` scroll, `1` `2` `3` choose the reading depth, `/` searches, `Esc` leaves.

<!-- detail: 3 -->
A scenario name on the command line can be a path to a file, a catalog name with or without `.nl`, or just the part after the number: `ping` finds `101-ping.nl`. `./hoplab list` prints the catalog, `./hoplab guide` this guide, `./hoplab guide TOPIC` one topic (as plain text when the output is piped).

hoplab looks for `scenarios/`, `guide/` and `locale/` next to its executable, or in `../share/hoplab` relative to it; set the environment variable `HOPLAB_HOME` to point somewhere else.

<!-- detail: 1 -->
## The screen

A running lab has three areas. At the top left is the **network map**: hosts are drawn as `●`, routers as `◆`, switches as `▣`, and a node that has crashed as `✗`. Frames travel along the links as small coloured labels carrying the name of their protocol (ARP, ICMP, TCP, DNS, ...); a frame that is lost turns into a red `✗` part of the way. The line of text at the bottom of the map is the lab's commentary: it changes as the story unfolds.

At the top right is the **node inspector** for the selected node: its interfaces and addresses, its routing table, its ARP cache, its links, and one tab for each protocol running on it.

The **bottom panel** shows one of five views: the event log, the list of captured packets, a sequence diagram, graphs, and the guide of the lab. The top line shows the virtual time in milliseconds, whether time is running or paused, the speed and the detail level.

<!-- detail: 2 -->
### The node inspector

The tabs are **Node** (interfaces, addresses and the node on the other side of each one; from detail 2 also MAC addresses, frame counts and the node's counters), **Routes** (destination, next hop, interface, metric and where the route comes from; freshly learned routes are highlighted) or **MAC table** on a switch, **ARP** (not on switches), **Links** (rate, delay, loss, MTU, how full the output queue is and which queue discipline manages it) and then one tab per module with something to show: `ping`, `tcp`, `rip`, `dns`, `raft`... Change tab with `←` `→` when the inspector has the focus.

### The bottom panel

- **Log** (`l`): one line per event with the time, the node, the layer (`L2`, `L3`, `L4`, `L7`...) and what happened. `f` shows only the selected node. With the focus on the panel, `↑` `↓` `PgUp` `PgDn` scroll back and `End` returns to the newest line.
- **Packets** (`p`): every frame put on a link, newest first, with its fate (lost, damaged, dropped by a queue, corrected). `↑` `↓` select a frame; Enter opens its dissection, layer by layer and field by field. `[` and `]` move from field to field, and on a wide window a hex dump highlights the bytes of the selected field.
- **Sequence** (`s`): the same frames as arrows between node columns, the classic time-sequence diagram. Enter opens the selected frame in the Packets view.
- **Graphs** (`g`): the time series recorded by the selected node (a congestion window, a round-trip time, a queue length). If the selected node records none, select another with `n`.
- **Guide** (`u`): the description of the lab, its timeline with a tick on what has already happened, the theory of the modules running on the selected node, and commands to try.

<!-- detail: 3 -->
On the map, from detail 2 each node shows its first IP address and a link whose queue is not empty shows a small number: the packets waiting. At detail 3 (and with at most 12 links) every link also shows its rate and delay. A damaged frame that is still delivered is marked `↯`, one repaired by an error-correcting link code `✓`. A node with a role (`DNS`, `NAT`, `LB`...) carries a small label.

In a window narrower than 100 columns or shorter than 28 rows the layout becomes a single column: the map or the inspector (whichever had the focus last) above the bottom panel. Press `z` to give the whole window to the focused area. The minimum is 44 by 12.

When you quit a lab that declares expectations, hoplab prints them with their result: a quick way to see whether the run went as the author intended.

<!-- detail: 1 -->
## Keys

- `space` pauses and resumes; `.` when paused advances to the next event.
- `+` and `-` make time run faster or slower; `a` turns the automatic skipping of idle time on and off.
- `1` `2` `3` choose the detail level: overview, protocol messages, every frame.
- `Tab` moves the focus between network, node and bottom panel; the arrow keys act on the focused area: select a node on the map, change tab in the inspector, scroll in the bottom panel.
- `n` and `N` select the next and the previous node.
- `l` `p` `s` `g` `u` choose the bottom view: log, packets, sequence, graphs, guide.
- `Enter` in the packet list opens or closes the dissection of the selected frame; `[` and `]` choose the previous or next field.
- `f` restricts the log to the selected node; `z` zooms the focused area to the whole window.
- `k` crashes the selected node, or recovers it if it is down.
- `:` opens the command line; `?` shows the keys; `q` quits.

<!-- detail: 2 -->
A few keys have synonyms: `h` also opens the help, `/` also opens the command line, `=` and `_` work like `+` and `-`, and `Shift+Tab` moves the focus backwards. With the focus on the map, Enter moves it to the inspector.

<!-- detail: 1 -->
## Time and detail

Time in a lab is virtual. A frame that takes 10 ms to cross a link would be invisible at real speed, so hoplab chooses a slow-motion speed at which a frame takes about a second to cross a typical link, and shows it in the top line (for example `1/50×`). Press `+` or `-` to double or halve it. When nothing is moving, hoplab jumps straight to the next event instead of making you wait: that is the `+skip` next to the speed, and `a` turns it off when you want to feel real pauses such as a retransmission timeout.

Pause with `space` and then press `.` repeatedly to advance one event at a time: it is the best way to read a handshake or a routing update.

The three **detail levels** decide how much you see. Level 1 is the overview: only the outcome of each action, what a beginner should notice. Level 2 adds the protocol messages: ARP requests, TCP segments, routing updates, queue drops. Level 3 shows every frame and internal step. The level filters the log, adds information to the map and the inspector, and reveals more of the guide.

<!-- detail: 3 -->
The speed can go from 1/100000 of real time to 1000 times faster. If you pass `--speed X` on the command line, or the scenario sets one, the automatic choice is skipped. Because virtual time only advances by processing events, a run with the same random seed (`--seed N`) repeats exactly, whatever the speed.

<!-- detail: 1 -->
## The command line

Press `:` and type a command, then Enter. The most common one starts a protocol or a tool on a node: the node, the module, then its arguments.

```
h1 ping h2
h1 ping 10.0.2.10 count=10 interval=200ms
h1 traceroute h2
link r1 r2 down
link r1 r2 up
link h1 r1 loss=0.3 delay=50ms
node r2 down
node r2 up
help
help ping
```

`link A B down` cuts a link and `up` restores it; `link A B key=value` changes its parameters while the lab runs. `node N down` crashes a node: its timers stop and the modules on it lose their volatile state. `help` lists the modules and `help MODULE` prints the syntax of one in the log.

<!-- detail: 2 -->
On the command line, `↑` and `↓` recall earlier commands, `Tab` completes a node name, `Ctrl+U` clears the line and `Esc` cancels. The result appears on the bottom line for a few seconds: `✓` and the command, or `✗` and the reason.

More commands:

- `stop NODE MODULE` stops a module on a node.
- `at TIME command` runs a command later; on the command line the time is counted from now (`at 5s link r1 r2 down`).
- `route NODE add PREFIX via GATEWAY`, `route NODE del PREFIX`, `gw NODE GATEWAY` and `ip NODE:IF A.B.C.D/LEN` edit routes and addresses.
- `set speed X` and `set detail N` do what the keys do; `pause`, `resume` and `quit` too.
- `log some text` writes a line of your own in the log.

Everything a module accepts is described in the **Modules** entry of the menu and by `./hoplab help MODULE`.

<!-- detail: 1 -->
## Parameters

Many labs have parameters: a loss probability, a bandwidth, the congestion control algorithm. When you start a lab from the catalog a form shows them; change a value and press Enter on **Start**. The **Settings** line switches between **simple** (the parameters that matter most) and **advanced** (all of them, plus the detail level, the time scale and the random seed).

On the command line, parameters are `name=value` pairs after the scenario:

```
./hoplab run 101-ping loss=0.2
./hoplab run 320-tcp-congestion cc=cubic queue=100
```

The catalog shows the parameters of the selected lab with their default values.

<!-- detail: 2 -->
## Without the interface

With `--headless`, or whenever the output is not a terminal, hoplab runs the lab as fast as it can and prints the event log, one line per event. It stops at the scenario's `duration`, or at the time given with `--until`.

```
./hoplab run 101-ping --headless --detail 2
./hoplab run 101-ping --headless --until 20s --seed 7 | less
```

A lab may end with **expectations**, statements such as "h1 received 3 ping replies". A headless run checks them and prints `PASS` or `FAIL`; `--quiet` prints only that. `./hoplab check` runs every lab of the catalog this way (or the ones you name) and reports how many failed: use it after changing a scenario or a protocol.

```
./hoplab check
./hoplab check 101-ping my-lab.nl
```

<!-- detail: 3 -->
`hoplab check` always uses seed 1, so its verdicts do not change from run to run. The exit status of a headless run is 0 when every expectation holds, 1 when one fails, 2 when the scenario could not be loaded.

<!-- detail: 2 -->
## Tiled windows

`./hoplab windows NAME` (or **Run in: tiled windows** in the launch form) runs the same lab as several processes side by side: one pane shows the whole network, and up to five other panes each run a single node with its own interface, exactly as if each were a separate computer. The links between panes are real UDP datagrams on your machine.

```
./hoplab windows 101-ping
./hoplab windows 230-rip-convergence --nodes r1,r3
```

hoplab uses kitty or tmux, whichever it finds; `--mode print` only prints the commands, to run by hand in your own terminals. By default every host and router gets a pane (the first five); `--nodes` chooses them.

## Several machines

A lab can also be spread over real computers. Each one runs some of the nodes with `hoplab node`, and one of them can show the global view with `hoplab monitor`:

```
./hoplab plan my-lab.nl
./hoplab node my-lab.nl h1
./hoplab node my-lab.nl r1,h2
./hoplab monitor my-lab.nl
```

Three rules make it work. Every machine must use **the same scenario file**, which says where each node runs with `place NODE HOST` lines and where the monitor is with a `monitor HOST` line. The machines must reach each other over **UDP**: by default node number i of the file (counting from 0) listens on port 47000 + i and the monitor on 46999; `--baseport N` moves them all. And all processes must use the same `--speed`.

The easy way is the menu: choose a lab, set **Run in** to **several machines**, type the address of the machine for each node, and hoplab writes the scenario copy with the `place` lines and prints the command for each machine.

<!-- detail: 3 -->
A process that reports to a monitor does not start its clock at once: it shows "waiting for the monitor to start the lab" until the monitor answers, so that the first frames are not sent to processes that do not exist yet. The monitor starts everybody together when it has heard every node, so the order in which you start the processes does not matter; pressing Enter in a waiting process starts it anyway. There is no global pause in a distributed lab: each process has its own clock, and pausing one only stops that one. See `docs/distributed.md` for the details.

<!-- detail: 1 -->
## Language

hoplab speaks the language of your system when it has a translation for it (English and Italian at the moment). To choose explicitly:

```
./hoplab --lang it
HOPLAB_LANG=it ./hoplab
```

The interface, the lab descriptions and this guide follow the language; protocol jargon in the log (`ARP who has ...`, `[SYN, ACK]`) stays as it is on the wire.

## Where to go next

Read **The curriculum** to see how the labs are organised and in which order to take them. Then open the first one, **Two hosts and a cable**: press `a` below. When you want to build a network of your own, the menu entry **Build your own network** gets you started in a minute, and **Writing your own labs** explains the scenario language.
