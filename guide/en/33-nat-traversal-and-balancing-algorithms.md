<!-- level: 3 -->
<!-- labs: 350-nat-hole-punching, 351-lb-algorithms -->
# NAT traversal and load-balancing algorithms

Two middleboxes rewrite addresses on the path, for opposite reasons: a NAT hides many clients behind one address, a load balancer hides many servers behind one address. Both keep per-flow state, and both raise an algorithmic question.

**NAT traversal.** Two hosts behind different NATs want a direct path (a call, a game, a file transfer). Neither has a reachable address, and each NAT discards packets it did not solicit. It is hard because NAT behaviour was never specified by a standard: what public port a NAT assigns, and whom it lets back in, differ between devices, and the hosts must discover it from the inside. The key idea is **hole punching**: a rendezvous server with a public address tells each peer the other's public endpoint as seen from outside, and both send at the same time, so that each NAT sees an outgoing packet first and takes the peer's packets for replies. Whether it works depends on only two properties of the NATs: mapping and filtering.

**Load balancing.** Requests must be assigned to servers so that no server is overloaded, with a decision taken per flow, in nanoseconds, often by several balancers that share no state. Perfect information (the least loaded server) is expensive and goes stale; no information (random or hash) is cheap but uneven. The key idea is that a very small amount of information is enough: sampling **two** servers at random and choosing the less loaded one improves the worst case exponentially.

<!-- detail: 2 -->
## Algorithms and models

**NAT behaviour (RFC 4787).** Let an inside socket `X:x` send to external endpoints. The **mapping** rule says when the public endpoint `X':x'` is reused:

- **Endpoint-independent mapping**: the same `X':x'` for every destination.
- **Address-dependent**, **address-and-port-dependent mapping**: a new public port for each destination address, or for each destination address and port.

The **filtering** rule says which external sources may send to `X':x'`:

- **Endpoint-independent filtering**: anyone.
- **Address-dependent filtering**: only addresses that `X:x` has sent to.
- **Address-and-port-dependent filtering**: only the exact endpoints that `X:x` has sent to.

The older names of RFC 3489 are four points of this grid: full cone = independent mapping + independent filtering; restricted cone = independent mapping + address-dependent filtering; port-restricted cone = independent mapping + address-and-port-dependent filtering; symmetric = address-and-port-dependent mapping and filtering.

**UDP hole punching.**

```
A -> S : register          S sees A':a'     (A's mapping towards S)
B -> S : register          S sees B':b'
S -> A : B':b'             S -> B : A':a'
A -> B':b'  and  B -> A':a'   repeatedly (PUNCH)
```

A's first packet creates, in A's NAT, a filter entry for `B':b'`; if it reaches B's NAT before B has sent, it is dropped, but B's packet then finds A's NAT open. The scheme assumes that the endpoint S saw is the one the peer will see, that is **endpoint-independent mapping**. With a symmetric NAT on A's side, packets to B leave from a new port `a''`. B can still learn `a''` from the first packet it receives, but only if B's NAT lets a packet from an unknown port through: full cone or address-restricted. Hence:

```
cone x cone                      works
symmetric x full/restricted      works (the cone side answers the observed port)
symmetric x port-restricted      fails
symmetric x symmetric            fails    -> relay (TURN)
```

**Balancing algorithms.** For servers `i` with weight `w_i` and `c_i` active flows:

- **Round robin / weighted round robin**: cyclic, shares proportional to `w_i`; blind to flow duration.
- **Least connections**: choose the minimum of `c_i/w_i`; adapts to long flows, needs exact shared state.
- **Hashing**: server = `h(key) mod N`; stateless and repeatable across balancers, but when `N` becomes `N+1` a fraction `N/(N+1)` of the keys changes server.
- **Consistent hashing**: servers (as `V` virtual nodes each) and keys on a ring, a key goes to the next server clockwise; a change moves about `1/N` of the keys; the load spread shrinks as `1/sqrt(V)`.
- **Maglev**: each server fills a table of prime size `M` following its own permutation; lookup is `table[h(key) mod M]`: nearly perfect balance, minimal but not zero disruption.
- **Power of two choices**: pick two servers uniformly at random, send to the one with fewer flows.

**Balls into bins.** Throw `n` balls into `n` bins. With one random choice the fullest bin holds about `ln n / ln ln n` balls; choosing the emptier of `d >= 2` random bins gives

```
max load = ln ln n / ln d + O(1)
```

**Worked example.** For `n` = 1000 the fullest bin typically holds 5 or 6 balls with one choice (the leading term `ln 1000 / ln ln 1000` is 3.6) and 3 with two (`ln ln 1000 / ln 2` is 2.8). In lab 351 the weights are 3:1:1: WRR and least connections send 60/20/20% of the flows; plain RR sends a third to each, so s2 and s3 carry 1.67 times their capacity share.

<!-- detail: 3 -->
## In depth

**NAT: invariants and failure modes.** Hole punching succeeds if and only if each side can send a packet to an endpoint that the other NAT will accept and that maps back to the peer's socket. Beyond the symmetric cases it fails when: the mapping expires between registration and punch (RFC 4787 asks for at least 2 minutes for UDP; many devices use 30 s, hence keepalives); both peers are behind the same NAT and it does not hairpin (so candidates include private addresses); there are several NAT layers, as with carrier-grade NAT; a firewall blocks UDP. Port-preserving or sequential symmetric NATs can sometimes be beaten by port prediction, unreliably. TCP hole punching uses simultaneous open and works less often. Deployed systems do not guess: ICE (RFC 8445) gathers host, server-reflexive (STUN, RFC 8489) and relayed (TURN, RFC 8656) candidates, probes all pairs and keeps the best that works; a few percent of calls end on a relay. Ford et al. measured about 82% of NATs supporting UDP punching.

**Balancing: why two choices suffice, and when they do not.** In the supermarket model with arrival rate `λ < 1` per server, the fraction of queues with at least `k` jobs is `λ^k` under random assignment and `λ^((d^k - 1)/(d - 1))` with `d` choices: doubly exponential decay. More choices bring only a constant-factor gain. A second effect favours sampling: with delayed load information, "least loaded" makes all balancers pick the same server (herding), whereas two random choices stay stable. Hash-based schemes trade balance for consistency: they keep flows on the same server across balancers and failures without a shared table, which is why ECMP routers in front of Maglev or Katran can change without breaking connections; a small per-flow table still protects established flows during pool changes. Weighted and bounded-load variants of consistent hashing cap a server at `(1 + ε)` times the mean.

**In netlab.** The `nat` module implements the four classic types (default port-restricted, port preservation when the port is free, 30 s idle timeout); `rendezvous` is a minimal STUN-like server and the peers answer to the source port they observe. The `lb` module balances one-datagram UDP requests; `p2c` draws its two candidates in proportion to their weight and keeps the one with fewer flows per unit of weight, and of the hash family only `consistent` honours the weights.

**Primary literature.**

- D. Karger et al., "Consistent Hashing and Random Trees: Distributed Caching Protocols for Relieving Hot Spots on the World Wide Web", STOC 1997.
- D. Thaler, C. Ravishankar, "Using Name-Based Mappings to Increase Hit Rates", 1998 (rendezvous hashing).
- Y. Azar, A. Broder, A. Karlin, E. Upfal, "Balanced Allocations", 1994/1999.
- M. Mitzenmacher, "The Power of Two Choices in Randomized Load Balancing", 2001.
- B. Ford, P. Srisuresh, D. Kegel, "Peer-to-Peer Communication Across Network Address Translators", USENIX ATC 2005.
- D. Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer", NSDI 2016.
- RFC 3489 and RFC 8489 (STUN), RFC 4787 (NAT behavioural requirements for UDP), RFC 5128 (state of P2P across NATs), RFC 8445 (ICE), RFC 8656 (TURN).

<!-- detail: 1 -->
## In the lab

**350-nat-hole-punching.** `./netlab run 350-nat-hole-punching`. Hosts a and b sit behind NATs na and nb; rv is the rendezvous server. Open the `nat` tab of na and nb (translation tables) and the sequence view (`s`). With the default port-restricted NATs: a registers at 1 s, b at 2 s, rv introduces them; a's first PUNCH reaches nb before b has sent anything and the log says "unsolicited sender filtered out"; b's first PUNCH passes na; both report "hole punched". Note that both NATs kept the same public port (61001) towards rv and towards the peer.

- Fill in the 4 x 4 matrix on paper first, then test the informative cells: `typea=symmetric typeb=restricted` (works: b logs "the peer reaches us from …, not …: its NAT is symmetric; answering there"), `typea=symmetric typeb=port` (fails: every PUNCH is filtered on both sides, "a relay is needed"), `typea=symmetric typeb=symmetric` (fails), `typea=full typeb=symmetric` (works).
- In the symmetric runs read na's table: two mappings for the same inside socket, one towards rv and one towards the peer, with different public ports. That difference is the whole explanation.
- `waitb=0`: both punch at once and nothing is filtered. Explain why simultaneity matters more for TCP than for UDP.

**351-lb-algorithms.** `./netlab run 351-lb-algorithms`. Client c1 opens many short flows, c2 keeps 20 long-lived ones; s1 has weight 3 (parameters `w1`, `w2`, `w3`). Open the `lb` tab of lb (bars per server, `active flows`) and read the last line of c1 and c2 (`reqgen: … s1 … s2 … s3 …`). Run once per algorithm and record the shares: `algo=wrr`, `rr`, `leastconn`, `p2c`, `hash`, `consistent`, `maglev`.

- Predictions: `wrr`, `leastconn` and `p2c` give about 60/20/20 (`wrr` exactly, the other two within their noise); `rr`, `hash` and `maglev` about a third each (they ignore the weights here); `consistent` favours s1. The lab's checks `weighted`, `exact` and `blind` state these predictions for whatever weights you set: try `w1=5 w2=2`. With only 20 flows from c2 the hash-based shares are noisy: estimate the standard deviation of a binomial with 20 trials before judging them.
- `algo=hash key=src`: two clients, so at most two servers ever work; here each client lands entirely on one.
- `algo=rr affinity=0`: every datagram is scheduled again and reqgen counts more than a hundred "flows changed server". Harmless for these one-datagram requests; say what it would do to a TCP connection.
- In a running lab, `:` then `node s2 down`: after two missed health checks the balancer removes it. Compare how many flows move under `hash` and under `consistent`.

## Check yourself

- Classify, in RFC 4787 terms, a NAT that reuses the public port for all destinations but accepts inbound packets only from addresses already contacted. With which peer NAT types does hole punching succeed, and why?
- Show step by step why symmetric against port-restricted fails although each NAT has sent a packet towards the other. Which single relaxation on either side makes it work?
- With hashing modulo N, compute the fraction of flows remapped when a pool of 10 servers loses one, and compare with consistent hashing. Why does a balancer usually keep a flow table anyway?
- State the balls-into-bins result for d choices and explain why going from one choice to two matters far more than from two to three.
- Several independent balancers use least connections on load figures refreshed once per second. Describe the pathology and explain why power of two choices is less exposed to it.
