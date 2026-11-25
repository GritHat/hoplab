<!-- level: 4 -->
<!-- labs: 420-tcp-fairness, 481-raft-split-votes -->
# Doing research with hoplab

The labs of this track are demonstrations: one topology, one run, an effect you can see. An experiment is something else. It starts from a **hypothesis** that could turn out false, varies **one factor** while holding the rest fixed, reports a **metric** with its variability, and says under which conditions the conclusion would not hold. This topic is about turning the emulator from the first kind of tool into the second, and about knowing where it stops being trustworthy.

hoplab has three properties that make it a reasonable small experimental platform. A run is a deterministic function of the scenario file, the parameters and the seed, so results can be reproduced exactly and shared as three lines of text. It runs headless, far faster than real time, so a parameter sweep is a shell loop. And every protocol is one readable C file that registers itself, so a new mechanism can be added and compared with the existing ones on equal terms.

It also has properties that limit what you may conclude: at most 64 nodes, simplified protocols, perfect clocks, zero processing time. **A result obtained here is a statement about a model.** It becomes a statement about networks only through an argument that the simplifications do not matter for the question asked, and making that argument is part of the work.

<!-- detail: 2 -->
## Parameters, seeds and reproducibility

A scenario declares parameters with `param NAME DEFAULT description` and uses them as `$NAME`; the command line overrides them with `NAME=value`. Random numbers come from one generator seeded by `--seed N`, or by a `seed N` line in the file, or else by the clock; the seed in use is printed on the first line of headless output. Record, for every number you report, the scenario file (or its hash), the parameter values, the seed and the program version. `hoplab check` runs scenarios with seed 1.

The seed matters only where something draws random numbers: loss, jitter, randomised timeouts, backoff, gossip peer choice. A scenario with no such element produces the same result for every seed, and repeating it tells you nothing about variability. In that case introduce the variation the real system would have: start times, a little delay on one link, a different flow population.

## Headless runs and metrics

```
./hoplab run NAME k=v ... --headless --detail 1 --seed 7 --lang en
```

prints the event log and then one line per `expect`, with the value found: `ok    h1 tcp.goodput_kbps >= 0   (got 1862.27)`. The exit status is 1 if any expectation fails, and `--quiet` prints only the verdict. `--until TIME` overrides the scenario's duration. Log messages are translated, so fix the language when a script parses them.

Counters are the metric interface. Each module maintains named counters on its node (`tcp.retransmits`, `raft.term`, `swim.mistaken_deaths`, ...); the Node tab lists them at detail 2. To read a counter from a script, add an expectation that always holds, `expect h1 tcp.retransmits >= 0`, to **your own copy** of the scenario and parse the `got` value. A file may hold 32 expectations and a node 48 counters. Time series (cwnd, RTT) are drawn in the interface but are not exported; for a quantity over time, parse the log at detail 2 or add a counter.

A scenario is found by path before the catalog is searched, so copies live wherever you like:

```
cp scenarios/420-tcp-fairness.nl fair.nl
./hoplab run ./fair.nl cc1=bbr queue=10 --headless
```

The full language (topology, link parameters, `at`, `run`, `expect`, queue disciplines) is in `docs/scenario-format.md`.

## Adding a protocol

A module is one C file in `src/` that fills an `nl_module` structure (name, usage, `run`, `stop`, `crash`, `recover`, `panel`) and registers it with `NL_MODULE`. It reacts to callbacks: timers (`nl_after`), UDP, TCP, IP and Ethernet handlers, packet hooks; it reports through `nl_log`, `nl_count` and `nl_sample`. Two rules keep experiments valid: take time only from `nl_now()` and randomness only from `nl_rand*()`, never from the operating system. Queue disciplines and link codes register in the same way. `tools/build-mod.sh NAME src/file.c` builds a private headless binary for development. See `docs/writing-modules.md`; `src/apps.c` and `src/rip.c` are the shortest real examples. Congestion control lives inside `src/tcp.c`: a new controller is a change there, or a transport of your own over UDP.

## Distributed runs

`hoplab plan SCENARIO` prints the commands to run a lab as several processes, `hoplab node SCENARIO a,b` runs some nodes, `hoplab monitor` collects their reports; `place` and `monitor` lines in the scenario say where things run. Links between processes become UDP tunnels. This exercises the same protocol code across real machines, which is valuable as a demonstration and as a robustness test. It is not a measurement mode: each process follows its own real clock, the real network adds latency, `--seed` no longer makes runs identical, and no process gives a global verdict (see `docs/distributed.md`).

<!-- detail: 3 -->
## Limits of the emulator

From `src/nl.h` and the scenario documentation: 64 nodes, 160 links, 16 interfaces and 12 modules per node, 256 routes per node, frames of at most 1700 bytes, 48 counters and 8 time series per node (600 samples each). Many modules have smaller limits of their own (40 client flows in `reqgen`, 24 stations per `mac` cell, 8 CRDT replicas, an 8-bit Chord identifier space by default). Nothing about scaling behaviour, heavy tails or rare events can be measured at this size.

Time is virtual, in microseconds, and advances only through events. Nodes compute in zero time: there is no CPU, no scheduling delay, no disk. Anything whose real behaviour is governed by processing cost (fsync latency in consensus, slow members in failure detection, softirq batching in TCP) is absent. Clocks are perfect unless a module adds an offset.

Protocols are simplified, and each module's help text lists how. The ones that most affect conclusions in this track: TCP without timestamps, tail-loss probe or HyStart, and BBR in its first version without policer detection; BGP with eBGP only and one router per AS; DNS without TCP fallback, EDNS or DNSSEC; a tick-based CSMA/CA with no capture; Raft without snapshots, membership change or leases (reads use ReadIndex).

## Threats to validity

- **Construct validity.** Does the counter measure the concept? Whole-transfer goodput is not a share of a bottleneck (which is why `tcp send` also reports a steady-state rate); an oracle's false-positive count is not what an operator sees.
- **Internal validity.** Deterministic simulations synchronise: drop-tail queues with periodic arrivals show phase effects, and one millisecond of extra delay can reverse an outcome. A difference between two configurations is evidence only if it survives small perturbations of irrelevant parameters.
- **Statistical validity.** Report a distribution over seeds, not a run; choose the number of runs before looking at the results; when the seed has no effect, say so rather than reporting a zero-width interval.
- **External validity.** Small topologies, a single bottleneck, synthetic traffic. Floyd and Paxson's argument still stands: look for invariants and explore the parameter space, rather than trusting one "typical" scenario.
- **Implementation validity.** Your new module may be wrong. Test it against a case with a known answer before comparing it with anything.

Good practice follows: write the hypothesis and the analysis before running; keep the scenario, the parameter grid and the script under version control; include a baseline that should show no effect; cross-check one headline number in a different tool (Mininet, ns-3, or a real testbed) before believing it.

## Primary literature

- R. Jain, "The Art of Computer Systems Performance Analysis", Wiley, 1991.
- S. Floyd, V. Jacobson, "On Traffic Phase Effects in Packet-Switched Gateways", Internetworking: Research and Experience, 1992.
- L. Rizzo, "Dummynet: A Simple Approach to the Evaluation of Network Protocols", ACM SIGCOMM Computer Communication Review, 1997.
- S. Floyd, V. Paxson, "Difficulties in Simulating the Internet", IEEE/ACM Transactions on Networking, 2001.
- K. Pawlikowski, H.-D. J. Jeong, J.-S. R. Lee, "On Credibility of Simulation Studies of Telecommunication Networks", IEEE Communications Magazine, 2002.
- A. Vahdat et al., "Scalability and Accuracy in a Large-Scale Network Emulator", USENIX OSDI, 2002.
- S. Kurkowski, T. Camp, M. Colagrosso, "MANET Simulation Studies: The Incredibles", ACM SIGMOBILE Mobile Computing and Communications Review, 2005.
- B. Lantz, B. Heller, N. McKeown, "A Network in a Laptop: Rapid Prototyping for Software-Defined Networks", ACM HotNets, 2010.
- N. Handigol, B. Heller, V. Jeyakumar, B. Lantz, N. McKeown, "Reproducible Network Experiments Using Container-Based Emulation", ACM CoNEXT, 2012.
- F. Yan et al., "Pantheon: The Training Ground for Internet Congestion-Control Research", USENIX Annual Technical Conference, 2018.

<!-- detail: 1 -->
## In the lab

The three exercises below are about method; they use labs you already know.

**Experiment 1 — what the seed does.** Hypothesis: equal seeds give identical runs, and different seeds change the result only where the scenario draws random numbers. Manipulation: run `./hoplab run 481-raft-split-votes --seed 7 --headless --detail 2` twice and compare the outputs with `diff`; then run it with seeds 1 to 10; then do the same with `420-tcp-fairness cc1=reno cc2=reno`. Metric: differing lines; spread of the leaderless time and of the goodput. Expected: identical outputs for equal seeds; a real distribution for Raft; no variation at all for the two Reno flows. Decide what you would vary in the second case to obtain an honest error bar.

**Experiment 2 — from a lab to a measurement.** Hypothesis: who wins between BBR and CUBIC depends on the buffer, and one run per point is not enough to say how. Manipulation: the fairness lab already starts both flows together, with a random offset, and reports steady-state rates and Jain's index; sweep the queue and the seed and keep the numbers:

```
for q in 10 20 40 80 160; do for s in 1 2 3 4 5; do
  ./hoplab run 420-tcp-fairness cc1=bbr cc2=cubic queue=$q --seed $s --headless --detail 1 --lang en \
    | grep Jain | awk -v q=$q -v s=$s '{print q, s, $(NF-4)}'
done; done > fair.dat
```

Metric: Jain's index per queue size, with its spread over seeds; add the two `steady` figures of the `done` lines to see who is ahead. Expected: BBR ahead with a heavy cost in timeouts for CUBIC at `queue=10`, the two close around 40 packets, CUBIC ahead at 160: a crossover near a few BDPs of buffer. Check how much the index moves between seeds at each point before drawing the curve: where the spread is as large as the difference between neighbouring points, you have not located the crossover.

**Experiment 3 — a sensitivity check.** Hypothesis: a conclusion drawn from one configuration can be a phase artefact. Manipulation: `./hoplab run 420-tcp-fairness cc1=reno cc2=reno` with `delay2` from 2 ms to 12 ms in steps of 1 ms, five seeds each. Metric: ratio of the steady rates against `delay2`. Expected: the ratio jumps between neighbouring values, and even changes sign, rather than varying smoothly, and the seeds of one value agree with each other more than neighbouring values do: you are looking at phase effects, not at RTT, and must average over perturbations (delays, or a discipline such as `qdisc=red` that randomises drops) before claiming anything about RTT.

## Ten project ideas

- **Loss recovery.** Add tail-loss probes and the full RACK-TLP of RFC 8985, or proportional rate reduction (RFC 6937), to `src/tcp.c`, next to the SACK recovery that is there. Measure timeouts, retransmissions and goodput when the last segments of a window are lost, and rerun the fairness experiments: which conclusions of topic 40 change?
- **A QUIC-like transport over UDP** (RFC 9000, RFC 9002): monotonic packet numbers, ACK ranges, independent streams. Measure head-of-line blocking against TCP for two streams under loss, and handshake latency.
- **A delay-based controller** (Vegas, Copa). Measure standing queue and RTT inflation alone, then the share obtained against Reno and CUBIC as a function of buffer size.
- **L4S in miniature.** A DCTCP-style sender and a step-marking or dual-queue discipline. Measure queueing delay and the rate ratio between scalable and classic flows.
- **ECMP or segment routing** (RFC 2992, RFC 8402) in the forwarding path. Measure load split across paths, packet reordering seen by TCP, and failover time after a link failure.
- **Routing validation extensions.** Add provider-authorisation checks in the spirit of ASPA, or roles as in RFC 9234, to `src/bgp.c`. Measure the fraction of leaked or implausible routes rejected as a function of which ASes deploy the check.
- **Multi-Paxos or Viewstamped Replication** beside Raft, with the same client commands. Measure leaderless time after a crash, messages per committed command, and recovery work for a lagging replica, on identical scenarios.
- **Leases for Raft reads**: the module answers reads by ReadIndex; add leader leases with a clock-offset parameter. Measure read latency against ReadIndex, and find the clock error at which leases start to return stale data.
- **A Kademlia DHT.** Compare lookup hops and lookup failures with Chord under the same scripted churn, and the maintenance traffic of the two.
- **Delta-state CRDTs and Lifeguard.** Ship deltas instead of full state and measure bytes per update and convergence time; add processing delay to members and measure SWIM's false positives with and without local health awareness.

For each project state the hypothesis first, validate the module on a case with a known answer, and report what the emulator's limits prevent you from concluding.

## Questions for discussion

- Pick one result from this track and list the simplifications of the relevant module. For each, argue whether it could reverse the result, weaken it, or is irrelevant.
- When the seed has no effect, what is the right source of variation, and how do you avoid choosing the one that confirms your hypothesis?
- Distributed runs are not reproducible. Is there a research question for which that mode is nevertheless the right instrument?
- What is the smallest change to the emulator that would let it export time series, and which experiments of this track would it improve most?
- Emulation, simulation, testbed, production measurement: for one of the ten projects, say what each would add and in which order you would use them.
