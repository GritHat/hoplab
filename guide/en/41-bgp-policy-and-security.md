<!-- level: 4 -->
<!-- labs: 430-bgp-policies, 431-bgp-hijack -->
# BGP policy and security

BGP is the only routing protocol in wide use whose objective is not a shortest path. Each autonomous system ranks and filters routes according to private commercial interest, and the global routing system is whatever emerges from tens of thousands of such local decisions. Two questions follow, and both are still research topics.

**Why does it converge at all?** Arbitrary policies can make BGP oscillate forever, and deciding whether a given configuration has a stable solution is computationally hard. The Internet is stable mostly because operators follow, by economic incentive rather than by protocol, a hierarchy of customer, peer and provider relations.

**Why can anyone announce anything?** A BGP speaker cannot verify that a neighbour is entitled to a prefix or that the path it reports exists. Hijacks and route leaks happen every year, by malice and by mistake. The deployed defence, RPKI route origin validation, authenticates only the last AS of the path; path validation is standardised but barely deployed.

The two labs make both points concrete: a five-AS topology in which a perfectly usable physical path is refused for business reasons, and a line of four ASes in which the last one steals a prefix, first partially and then from everybody.

<!-- detail: 2 -->
## Business relationships and the Gao–Rexford conditions

Inter-AS links are, to a first approximation, of two kinds. In a **customer–provider** link the customer pays for transit to the whole Internet. In a **peer–peer** link two ASes exchange traffic for their own customers only, usually without payment. Two rules encode the incentives.

- **Preference**: prefer routes learned from customers over routes from peers, and those over routes from providers (implemented with local preference, compared before AS-path length).
- **Export**: announce customer routes to everyone; announce peer and provider routes only to customers.

Gao and Rexford proved that if every AS follows these rules and the customer–provider graph has no cycle, BGP converges to a unique stable state from any starting point and under any message timing. The export rule alone yields **valley-free** paths: zero or more customer-to-provider hops, at most one peer hop, then zero or more provider-to-customer hops. A consequence visible in the first lab is that connectivity in the policy graph is strictly weaker than physical connectivity.

## Route leaks

A route leak (RFC 7908) is the propagation of an announcement beyond its intended scope, typically a multihomed customer re-exporting one provider's routes to the other. The leaked path is longer, but the receiving provider prefers it because it arrives from a customer. Leaks are valley violations and need no forgery: every AS in the path is real. RFC 9234 adds BGP roles negotiated at session set-up and an Only-To-Customer attribute so that a leak is detectable by the neighbours.

## Prefix and sub-prefix hijacks

An AS that originates someone else's prefix competes with the legitimate origin under normal route selection: each AS picks by preference, then path length, so the Internet splits into two regions. An AS that originates a **more specific** prefix does not compete at all, because forwarding uses the longest match: it attracts all traffic from every AS that accepts the announcement. An attacker can also keep a working path to the victim and forward the traffic after inspecting it (interception).

## RPKI route origin validation

The RPKI (RFC 6480) is a certificate hierarchy that mirrors address allocation. The holder of a prefix signs a Route Origin Authorization: prefix, origin AS, maximum length. A router classifies each announcement (RFC 6811) as **valid** (a covering ROA matches origin and length), **invalid** (covering ROAs exist and none matches) or **not found**; operators drop invalids. This stops accidental mis-origination and sub-prefix hijacks of a prefix whose ROA has a tight maximum length.

What it does not protect: the attacker can announce the prefix with the legitimate AS appended as origin (**forged-origin hijack**), which validates and costs one extra hop; a loose maxLength makes forged-origin sub-prefix hijacks valid as well (RFC 9319 recommends against it); route leaks are untouched; and "not found" must be accepted while coverage is partial.

## BGPsec and ASPA

BGPsec (RFC 8205) has each AS sign the path it forwards, including the next AS, so paths cannot be shortened or forged. It requires every AS on the path to participate and online cryptography on routers, and gives little benefit in partial deployment. ASPA, in progress at the IETF, takes the cheaper route of publishing, per AS, the set of its providers: receivers can then detect valley violations, which catches most leaks and many forged-origin paths without per-update signatures.

<!-- detail: 3 -->
## Open problems

**Partial deployment.** Lychev, Goldberg and Schapira showed that with partial adoption, and with security ranked below business preference, path validation adds little over origin validation. Which incremental mechanism gives the best marginal benefit, and to whom, is still argued. Collateral benefit and collateral damage (an AS behind a non-validating transit is still deceived) need topology-aware analysis.

**Policy is not Gao–Rexford.** Real relationships include partial transit, paid peering, sibling ASes and per-prefix exceptions. Inferring relationships from public data (Gao 2001; Luckie et al. 2013) is itself a measurement problem with limited ground truth, and ASPA correctness depends on it.

**Trust in the RPKI itself.** Validators, repositories and the certificate authorities become new points of failure; a misissued or withdrawn ROA can make a legitimate route invalid.

**Convergence dynamics.** Even when a stable state exists, path exploration after a withdrawal can take minutes (Labovitz et al. 2000).

## The lab's model versus reality

`./netlab help bgp` states the simplifications: sessions run over UDP port 179 instead of TCP (so use loss-free links, and do not study session resets), eBGP only, one router per AS, no MED, no communities, no aggregation. A consequence is that everything interesting about iBGP, route reflection and hot-potato routing is absent, and local preference is set mechanically from the declared relation. The MRAI timer defaults to 500 ms against tens of seconds in practice, so convergence times in the lab are not comparable with the Internet's.

In the hijack lab the ROA table is a static per-router `roa=` list: no certificates, no validator, no repository, no "not found" majority. The attacker can only originate a prefix; it cannot forge an AS path, so the forged-origin attack that defeats ROV in practice cannot be reproduced with the stock module (it is a project in topic 47). With four ASes in a line, "fraction of the Internet captured" degenerates into which of two observers is hijacked; statements about the real topology need CAIDA-scale graphs and a different simulator.

## Primary literature

- L. Gao, "On Inferring Autonomous System Relationships in the Internet", IEEE/ACM Transactions on Networking, 2001.
- L. Gao, J. Rexford, "Stable Internet Routing Without Global Coordination", IEEE/ACM Transactions on Networking, 2001.
- T. Griffin, F. B. Shepherd, G. Wilfong, "The Stable Paths Problem and Interdomain Routing", IEEE/ACM Transactions on Networking, 2002.
- C. Labovitz, A. Ahuja, A. Bose, F. Jahanian, "Delayed Internet Routing Convergence", ACM SIGCOMM, 2000.
- H. Ballani, P. Francis, X. Zhang, "A Study of Prefix Hijacking and Interception in the Internet", ACM SIGCOMM, 2007.
- R. Lychev, S. Goldberg, M. Schapira, "BGP Security in Partial Deployment: Is the Juice Worth the Squeeze?", ACM SIGCOMM, 2013.
- M. Luckie, B. Huffaker, A. Dhamdhere, V. Giotsas, k. claffy, "AS Relationships, Customer Cones, and Validation", ACM IMC, 2013.
- S. Goldberg, "Why Is It Taking So Long to Secure Internet Routing?", Communications of the ACM, 2014.
- Y. Gilad, A. Cohen, A. Herzberg, M. Schapira, H. Shulman, "Are We There Yet? On RPKI's Deployment and Security", NDSS, 2017.
- Y. Gilad, O. Sagga, S. Goldberg, "MaxLength Considered Harmful to the RPKI", ACM CoNEXT, 2017.
- RFC 4271 (BGP-4), RFC 7908 (route leak classification), RFC 6480 (RPKI architecture), RFC 6811 (prefix origin validation), RFC 9319 (maxLength), RFC 8205 (BGPsec), RFC 9234 (roles and Only-To-Customer).

<!-- detail: 1 -->
## In the lab

**Lab 430-bgp-policies.** Run `./netlab run 430-bgp-policies`. p1 (AS 100) and p2 (AS 200) peer; c1 and c2 are single-homed customers; m is a customer of both. At 20 s the peering link fails. Parameters: `policy` (1 = business relations, 0 = shortest AS path and export everything), `keepalive`. Measure `ping.reply` and `ping.error` on hc1, `bgp.routes` and `bgp.updates_sent` on the routers, the RTT of the pings, and in the log the delay between `link down` and each `route withdrawn` line. Open the BGP tab of m at detail 3 to see routes known but not exported.

- **Experiment 1.** Hypothesis: valley-free export removes reachability that the physical graph still offers. Manipulation: `policy=1` against `policy=0`. Metric: replies to the second `hc1 ping hc2`, `bgp.routes` on p1, RTT. Expected: with policy, three "network unreachable" errors and two routes on p1; without, three replies about 10 ms slower (the path crosses m) and three routes.
- **Experiment 2.** Hypothesis: multihoming protects m, and only m, from a single link failure. Manipulation: before 20 s type `link p1 m down`, then `hc1 ping hm count=3`; repeat the ping after 20 s. Metric: replies, RTT, time to the new best-path line on p1. Expected: after about a second hm is reached through p1 — p2 — m with a longer RTT; after the peering failure it is unreachable from hc1.
- **Experiment 3 (route leak, scenario copy).** Hypothesis: a mislabelled relation violates the Gao–Rexford conditions and the system need not converge. Manipulation: copy the scenario and, in the `run m bgp` line, change `neighbor=p1:100:provider` to `neighbor=p1:100:customer`. Metric: best-path changes for 10.3.0.0/24 on p1, m and c1 over time; `bgp.updates_sent`. Expected: p1 and m each prefer the other's "customer" route and the best path flips about once per second without end: a dispute wheel in miniature.

**Lab 431-bgp-hijack.** Run `./netlab run 431-bgp-hijack`. AS 65001 — 65002 — 65003 — 65066 in a line; `client` sits in 65002, `obs` in 65003; AS 65066 originates the victim's /24 at 15 s and two /25 at 30 s. Parameter: `rov`. Measure `icmp.echo_served` on srv and on fake, the TTL of each echo reply (it reveals who answered), and `bgp.rov_rejected`.

- **Experiment 1.** Hypothesis: a same-length hijack captures exactly the ASes for which the attacker's path is shorter; a more-specific one captures all. Manipulation: default run. Metric: who answers `client` and `obs` in the three rounds. Expected: round 2, obs is answered by fake and client by srv; round 3, both by fake (6 and 6 in total).
- **Experiment 2.** Hypothesis: origin validation with maxLength 24 stops both attacks, at the price of forbidding the victim's own de-aggregation. Manipulation: `rov=1`; then at about 33 s type `r1 bgp announce 10.1.0.0/25` and `r1 bgp announce 10.1.0.128/25`, with `rov=0` and with `rov=1`. Metric: echo counters, `INVALID` lines. Expected: with ROV srv serves 12 and fake 0, and the victim's /25 are rejected as invalid too; without ROV de-aggregation wins back client but not obs.
- **Experiment 3 (partial deployment, scenario copy).** Hypothesis: the protection of an AS depends on where validation happens on its path, not on its own choice. Manipulation: copy the scenario and replace `rov=$rov` with `rov=0` on r1 and r3, then run with `rov=1`; then validate only at r3. Metric: `icmp.echo_served` on fake, `bgp.rov_rejected` per router. Expected: validating at r2 alone protects client and the victim but leaves obs hijacked in both phases (fake serves 4); validating at r3 alone protects everybody.

## Questions for discussion

- The Gao–Rexford theorem needs an acyclic provider hierarchy and uniform rules. Which real practices break each assumption, and what stops the Internet from oscillating anyway?
- With ROV dropping invalids everywhere, describe the cheapest remaining attack on 10.1.0.0/24 and estimate how many ASes it captures in the lab's line topology and in a realistic one.
- Experiment 3 of the hijack lab shows collateral benefit. Construct a topology with collateral damage: a validating AS whose traffic is still hijacked.
- Is maxLength a security parameter or an operational convenience? Argue from the de-aggregation experiment.
- What would you need to add to the bgp module to evaluate ASPA, and which metric would you report?
