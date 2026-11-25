# Running a lab across processes and machines

The same scenario file can run in three ways:

| Command | What this process simulates | Links |
|---|---|---|
| `netlab run FILE` | every node | in memory |
| `netlab node FILE a,b` | only the nodes listed | links to nodes hosted elsewhere become UDP tunnels |
| `netlab monitor FILE` | no node | none: it displays what the node processes report |

In the distributed forms each node (or group of nodes) has its own window,
its own interface and its own clock, like a real computer; the emulated
frames really cross process and machine boundaries inside UDP datagrams. The
protocol code is identical in all three: a module cannot tell whether its
neighbour is in the same process.

- [Tiled windows on one machine](#tiled-windows-on-one-machine)
- [Several machines](#several-machines)
- [The commands](#the-commands)
- [`place` and `monitor`](#place-and-monitor)
- [Ports and firewalls](#ports-and-firewalls)
- [Time scale](#time-scale)
- [The start-up rendezvous](#the-start-up-rendezvous)
- [What the monitor shows](#what-the-monitor-shows)
- [How a tunnel works](#how-a-tunnel-works)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)

## Tiled windows on one machine

```
netlab windows 101-ping
netlab windows 230-rip-convergence --nodes r1,r3
netlab windows 320-tcp-congestion cc=cubic --speed 0.05
netlab windows 101-ping --mode print
```

`netlab windows` starts one lab as several processes on this machine and
arranges them in tiled panes. From the menu: launch form → *Run in* → *tiled
windows*.

- **Which nodes get a pane.** By default every host and router, in the order
  of the file, up to five. `--nodes a,b,c` chooses them explicitly.
- **The first pane** shows the whole network. If some nodes have no pane of
  their own (switches, nodes beyond the fifth, nodes not named in
  `--nodes`), the first pane is a *hub*: `netlab node FILE rest... --hub`,
  which simulates those nodes and also collects the reports of the others.
  If every node has a pane, the first pane is a plain `netlab monitor`.
- **The other panes** each run `netlab node FILE NAME --monitor
  127.0.0.1:PORT` for one node.
- **Ports.** A base port is derived from the process id
  (`20000 + (pid mod 1000) × 40`) so that several tiled labs can run at the
  same time; the hub or monitor listens on the port just below it.
- **Common options.** All panes receive the same `--baseport`, `--speed`,
  `--seed`, `--lang` and the same `name=value` parameters.

`--mode` selects how the panes are opened:

| Mode | Behaviour |
|---|---|
| `auto` (default) | `tmux` when already inside tmux; otherwise `kitty` if a graphical session and kitty are available; otherwise `tmux` if installed; otherwise `print`. |
| `kitty` | A new maximised kitty window: the network on the left, the node panes on the right. |
| `tmux` | A new window in the current tmux session, or a new session `netlab-PID` that is attached at once (with the mouse enabled). |
| `print` | Only prints the commands, one per pane, for you to run in your own terminals (the first one first). |

Each pane is an ordinary lab interface: quit each with `q`, or close the
window.

## Several machines

The quick way is the menu. Choose a lab in the catalog, set *Run in* to
*several machines*, and fill in the form: one address per node (the same
address for nodes that share a machine) and the address of the monitor. netlab
then

1. writes a copy of the scenario, `dist-NAME.nl`, in
   `~/.local/share/netlab/scenarios/`, with one `place` line per node
   (ports 47000, 47001, ...) and a `monitor` line (port 46999);
2. prints the command to type on each machine (the output of `netlab plan`).

By hand it is the same three steps:

**1. Say where the nodes run**, at the end of the scenario file:

```
place h1 192.168.1.20
place r1 192.168.1.21
place h2 192.168.1.22
monitor 192.168.1.20
```

**2. Copy netlab and the scenario file to every machine.** The file must be
identical everywhere, and so must any `name=value` parameters given on the
command line: nodes, interfaces and links are identified by their position
in the file, and addresses assigned by `auto` are computed independently by
each process.

**3. Start the processes.** `netlab plan FILE` prints what to run:

```
$ netlab plan my-lab.nl
# Copy netlab and the scenario file to every machine, then run:

  [192.168.1.20]  netlab node my-lab.nl h1
  [192.168.1.21]  netlab node my-lab.nl r1
  [192.168.1.22]  netlab node my-lab.nl h2

# and, on the machine named in the `monitor` line:
  netlab monitor my-lab.nl
```

Nodes placed on the same machine can share one process:
`netlab node my-lab.nl r1,r2,s1`. The processes can be started in any order: they wait for one another
(see [The start-up rendezvous](#the-start-up-rendezvous)). Give every
process the same `--speed`.

## The commands

```
netlab node SCENARIO NODE[,NODE...] [name=value ...] [options]
netlab monitor SCENARIO [name=value ...] [options]
netlab plan SCENARIO
```

| Option | Applies to | Meaning |
|---|---|---|
| `--monitor HOST[:PORT]` | `node` | Report to this monitor and wait for it before starting. Overrides the `monitor` line of the file. Default port: baseport − 1. |
| `--hub` | `node` | Also be the monitor: listen for the reports of the other processes, show the whole network, and start everybody when all nodes are present. |
| `--baseport N` | all | First UDP port (default 47000). Must be the same in every process. |
| `--speed X` | all | Time scale. Must be the same in every process. |
| `--seed N` | all | Random seed of this process. |
| `--detail 1\|2\|3` | all | Detail level of this process's display. |
| `--headless`, `--until TIME`, `--quiet` | `node`, `monitor` | No interface: print the log of this process; run in real time (scaled by `--speed`) until the duration. |

A headless node process prints the log of its own nodes. A headless monitor
collects the reports but prints nothing, so the monitor is useful only with
its interface.

Every process loads the whole scenario, so that it knows the complete
topology and addressing, and then simulates only its own nodes:

- `run` lines (and console commands) for a node hosted elsewhere are
  silently skipped;
- `at TIME link ...` lines are executed by every process at the same virtual
  time, each on its own copy of the link;
- `at TIME node N down|up` acts only in the process that hosts `N`;
- expectations are checked by each process only for the nodes it hosts.

In the interface of a node process the other nodes are drawn dimmer with the
label *(remote)*, and the top bar shows `[node]`.

## `place` and `monitor`

```
place NODE HOST[:PORT]
monitor HOST[:PORT]
```

| Directive | Meaning | Default |
|---|---|---|
| `place` | The machine (address or host name) on which `NODE` runs, and the UDP port it listens on. | host `127.0.0.1`, port baseport + index of the node |
| `monitor` | Where node processes send their reports. | no monitor; port baseport − 1 |

The *index* of a node is the position of its `node` line in the file,
counting from 0. `set baseport N` in the file changes the base port for
everybody (unless `--baseport` is given). Both directives are ignored by
`netlab run`, so a file with `place` lines still works as an ordinary lab.

A process sends frames for a remote node to the `HOST:PORT` of its `place`
line. A node with no `place` line is assumed to be on `127.0.0.1`, which is
right only when all processes share one machine.

## Ports and firewalls

Everything travels in UDP datagrams.

| Who | Listens on | Receives |
|---|---|---|
| each node | UDP *baseport + index* (or the port of its `place` line), on all addresses | the frames sent to it over tunnels; a node process also receives the monitor's start signal on one of these sockets |
| monitor or hub | UDP *baseport − 1* | hello messages, log lines and frame captures from the node processes |

With the default base port 47000, a lab of five nodes uses UDP 47000-47004
and the monitor 46999.

- Open the node ports for inbound UDP on each machine that hosts nodes, and
  the monitor port on the monitor machine.
- Datagrams are sent *from* the node sockets, so the source port of a tunnel
  frame is the port of the sending node, and reports to the monitor come from
  one of the node ports of the reporting process. The monitor answers to the
  address it heard from.
- The monitor always listens on baseport − 1, whatever port the `monitor`
  line names: if you write an explicit port there, keep it equal to
  baseport − 1 (or start the monitor with a matching `--baseport`).
- Machines must reach each other directly at the addresses of the `place`
  lines. Address translation between them is not supported.
- A tunnel datagram is the emulated frame plus a 24-byte header, so a
  full-size frame (1514 bytes) does not fit in one 1500-byte packet of the
  real network and is fragmented by the real IP layer. This is harmless on a
  LAN; across filters that drop fragments, lower the `mtu` of the emulated
  links (for example `mtu=1400`).
- There is no authentication or encryption: any host that can reach the
  ports can inject frames. Use trusted networks only.

## Time scale

In `netlab run` virtual time is driven only by events, can skip idle
periods and can be paused. In a distributed lab that is impossible: each
process advances its own virtual clock from its own real clock,

    virtual time elapsed = real time elapsed × speed

and the processes stay aligned only because they start together and use the
**same speed**. Therefore:

- Pass the same `--speed X` to every process (or put `set speed X` in the
  scenario). `netlab windows` does it for you, choosing a slow-motion speed
  from the link delays and bandwidths (never below 0.02) unless you give
  `--speed`.
- `netlab node` and `netlab monitor` do **not** choose a slow-motion speed
  automatically as `netlab run` does: without `--speed` they run at 1×, real
  time.
- There is no automatic skipping of idle time.
- The real network adds its own latency to every emulated link, multiplied
  by the speed in virtual terms: at `--speed 0.1` a real delay of 2 ms is
  0.2 ms of virtual time. Slow motion therefore also makes the distributed
  run more faithful; at speeds above 1 real latency and scheduling jitter
  are magnified.

## The start-up rendezvous

The processes of a lab are started one after the other, but the lab only
makes sense if their clocks start together: otherwise the first frames are
sent to processes that do not exist yet.

A node process that has a monitor (`--monitor`, or a `monitor` line in the
file) does not start its clock. It shows

    ◷ waiting for the monitor to start the lab — Enter starts now

and sends a *hello* listing its nodes to the monitor every 200 ms. It starts
when the monitor answers with a *start* message.

Who answers, and when:

- The collector — a **monitor** (`netlab monitor`) or a **hub**
  (`netlab node ... --hub`, the first pane of `netlab windows` when it also
  hosts nodes) — holds its own clock too, shows `◷ waiting for: h1 r2 ...`
  with the names of the nodes it has not heard yet, and sends *start* to
  everybody at the moment the last one says hello. This is a true
  simultaneous start, whatever the order in which the processes come up.
- A process that says hello after the lab has started (started late, or
  restarted) is told to start at once; its clock begins from zero.

Pressing Enter in a waiting process starts it without waiting; pressed in a
hub, it starts everybody that has already said hello. A node process with no
monitor at all does not wait for anything. A waiting process run with
`--headless` has no Enter key: it waits until a monitor answers.

## What the monitor shows

The monitor (or hub) is the only place where the whole lab is visible. Node
processes send it, as they happen:

- every **log line** of their nodes, at all detail levels (the monitor
  filters them with its own detail setting);
- a **capture of every frame** their nodes transmit, with its fate (lost,
  discarded for bit errors, corrected, dropped by a queue, link down) and
  its travel time.

From these it shows the map with frames in flight on every link, the event
log of all nodes, the Packets view with full dissection and hex dump, and
the Sequence diagram. The top bar shows `[monitor]`.

What it does **not** show:

- **Node state.** The inspector of a node hosted elsewhere shows the
  addresses and static routes known from the scenario file, not the live
  routing table, ARP cache, counters or graphs, and module tabs say *(this
  node runs in another process)*. Look at the node's own window.
- **Queue lengths** and link state changes made from another process's
  console.
- **Time as the nodes see it.** Lines and frames are stamped with the
  monitor's own clock at the moment the report arrives.

The monitor is passive. A command typed there for a node it does not host
(`h1 ping h2`) is accepted but does nothing: type it in the window of the
process that hosts the node.

## How a tunnel works

When a node transmits on a link whose far end is local, the frame is
scheduled in memory. When the far end is hosted elsewhere:

1. The **sending** process does everything the link would do: queueing and
   the queue discipline, serialisation at the link's bandwidth, the loss
   model, bit errors and the link code. A frame that is lost or discarded
   goes no further.
2. A surviving frame is sent at once in a datagram to the `place` address
   of the destination node, with a header that carries the link, the
   direction, the packet identity and the frame's travel time
   (serialisation + propagation + jitter).
3. The **receiving** process schedules the arrival that travel time after
   its own current virtual time, and the frame then enters the destination
   node's stack as usual.

The header starts with the magic number `NLT1`; datagrams without it are
ignored. The same format carries the hello and start messages and the
reports to the monitor.

Each direction of a link is thus simulated by the process that hosts the
transmitting node; link parameters must agree on both sides, which they do
when the file and the parameters are the same.

## Limits

- **No global pause.** `space` pauses only the process in which it is
  pressed; the others keep running and the frames they send are scheduled by
  the paused process when it resumes. Stepping with `.` is equally local.
- **Clocks are per process.** Timestamps of different windows are close but
  not identical, and they drift apart if one process is paused, suspended or
  started late. Do not compare times across windows to better than the real
  network latency.
- **Not reproducible.** Real timing decides the interleaving of events, and
  each process has its own random generator, so `--seed` no longer makes two
  runs identical.
- **Console commands are local.** `link r1 r2 down` typed in one window
  changes only that process's copy of the link: it stops transmitting on it
  and ignores what arrives, while the other side keeps transmitting into the
  void. To cut a link cleanly, script it with `at` in the scenario, or type
  the command in the processes at both ends.
- **A process that exits takes its nodes with it.** The others are not told;
  frames sent to it are lost. Restarting it brings the nodes back with empty
  state and a clock that starts from zero.
- **Duplication is not tunnelled.** The `dup=` link parameter acts only on
  links whose two ends are in the same process.
- **Underlying loss is invisible.** A tunnel datagram dropped by the real
  network is a lost frame that no log reports. On a LAN this is rare.
- **Same build everywhere.** Processes must run the same version of netlab
  and the same scenario file with the same parameters.
- **Expectations are partial.** Each process checks only the `expect` lines
  about its own nodes; nothing gives a global verdict. `netlab check` always
  uses `netlab run`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `cannot listen on UDP port N` | Another netlab (or an earlier one still running) uses the port. Choose another `--baseport` for all processes. |
| A node window stays on *waiting for the monitor* | No monitor is running at the address of the `monitor` line or `--monitor`, the monitor's port (baseport − 1) is filtered, or the base ports differ. Press Enter to start anyway. |
| The hub stays on *waiting for: ...* | The processes of those nodes are not running or cannot reach the hub. |
| Frames leave a node but never arrive | Missing or wrong `place` line (the default is `127.0.0.1`), or a firewall blocks the destination node's UDP port. |
| Everything runs but timing looks wrong (timeouts, retransmissions) | The processes use different `--speed` values, or one was started much later than the others without a monitor. |
| Routes or addresses disagree between windows | The scenario files or the `name=value` parameters are not the same everywhere. |
| The monitor shows frames but not the log of a node | That node's process was started without `--monitor` and the file has no `monitor` line. |
