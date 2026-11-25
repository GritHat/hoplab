<!-- level: 4 -->
<!-- labs: 440-dns-poisoning, 240-dns-hierarchy, 340-dns-ttl -->
# DNS security: what a resolver can trust

A resolver asks a question over UDP and believes the first answer that looks right. "Looks right" means only that the answer comes from the address that was asked, arrives on the port the question left from, repeats the question, and carries the same 16-bit identifier. None of this proves who wrote it. Whoever can make an answer look right before the real one arrives decides what the resolver caches, and therefore where every client of that resolver is sent, for as long as the record's TTL says.

The defence that the whole Internet deployed in 2008 is simply to make the answer harder to guess: choose the identifier and the source port at random, so that a forger who cannot see the question has tens of bits to guess instead of a handful. The lasting defence is different in kind: sign the data (DNSSEC), so that the answer proves its own origin regardless of the path it took.

<!-- detail: 2 -->
## Why unpredictability is the defence

An attacker who sits on the path sees the question and can answer it; only cryptography helps against that. The interesting case is the **off-path** attacker, who sees nothing and must guess. What is there to guess is the resolver's entropy:

- the 16-bit transaction identifier: 65 536 possibilities when random, almost none when sequential
- the UDP source port: about 2^16 more when randomised, none when the resolver always uses the same port
- optionally the capitalisation of the query name (0x20 encoding), a few more bits echoed by honest servers

With a fixed port and a random identifier, a forger who can deliver n answers inside the window before the genuine one wins with probability about n / 65 536 per attempt. The insight published by Dan Kaminsky in 2008 was that attempts are not limited by the TTL: by asking for names that do not exist under the victim domain, each attempt opens a fresh race, and a forged answer can carry authority records for the whole domain. The response was source-port randomisation (RFC 5452), which multiplies the search space by tens of thousands.

Other measures narrow what a resolver accepts: **bailiwick checking** (ignore records outside the zone the server is authoritative for), preferring data by trustworthiness (RFC 2181 ranking), and limiting how many identical questions are outstanding at once (the birthday effect).

**DNSSEC** changes the question from "did this arrive the right way?" to "is this signed by the zone?". Zones sign record sets (RRSIG) with keys (DNSKEY) vouched for by the parent (DS), up to a trust anchor at the root; signed denial of existence (NSEC/NSEC3) covers negative answers. It gives integrity and origin authentication, not confidentiality. Encrypted transports — DNS over TLS (RFC 7858), over HTTPS (RFC 8484), over QUIC (RFC 9250) — protect the hop between a client and its resolver, which is a different link in the chain.

<!-- detail: 3 -->
## Open issues and the limits of the lab

Randomisation is a probabilistic defence, and research keeps finding ways to take entropy back. Fragmentation attacks (Herzberg and Shulman, "Fragmentation Considered Poisonous", 2013) exploit the fact that the identifier and port travel in the first fragment only. Side channels can reveal the ephemeral port (Man et al., "DNS Cache Poisoning Attack Reloaded: Revolutions with Side Channels", ACM CCS 2020, known as SAD DNS). Network address translators in front of a resolver may undo its port randomisation. Each of these was answered by a specific mitigation; the structural answer remains validation of signed data, whose deployment is partial: many zones are unsigned, and validation at the stub is rare, so the last hop relies on the transport.

DNSSEC has costs worth studying: larger answers (and their use in amplification), key management and rollover failures that make a zone unresolvable, zone enumeration through NSEC, and the operational burden that explains its slow adoption.

About the lab's model. `dnsspoof` is a fully simulated forger: it is told the resolver's address and, with `randomize=0`, relies on the fixed port and predictable identifiers; it does not implement any of the refinements above. With `randomize=1` the scripted forger keeps aiming at the fixed port, so it fails with certainty rather than with a small probability: the lab shows that the defence works, not how strong it is. Estimating the residual probability is an exercise for pencil and paper, or for a module of your own. Links here do not reorder or fragment, the resolver does not validate signatures, and there is no bailiwick lab: conclusions about real resolvers must come from the literature, not from this emulator.

Primary sources: RFC 1034/1035 (the protocol), RFC 3833 (threat analysis of the DNS), RFC 5452 (measures against forged answers), RFC 4033–4035 (DNSSEC), RFC 7858, 8484, 9250 (encrypted transports).

<!-- detail: 1 -->
## In the lab

Run `440-dns-poisoning` and open the Sequence view (`s`) and the `dns` tab of the resolver.

- **Hypothesis: a predictable resolver accepts a forged answer.** Run with the defaults (`randomize=0`). Metric: the address `h1` obtains for bank.example.com at 4 s, and the cache entry in the resolver's panel with its TTL. Expected: the forged address, cached for about an hour — every later client is affected without any further action.
- **Hypothesis: unpredictability removes the off-path forger.** Run `./netlab run 440-dns-poisoning randomize=1`. Metric: the same answer, and the counter `dns.mismatched` on the resolver. Expected: the genuine address; the forged answers are counted as mismatched and dropped. Read the limits above before generalising.
- **Hypothesis: the TTL bounds the damage, not the risk.** In `340-dns-ttl` change `ttl` and watch how long a cached answer keeps being served after the authoritative data changes. Metric: time until clients see the new address. Expected: proportional to the TTL — which is why a poisoned record with a long TTL is so valuable, and why shortening TTLs is not a defence.

Compare with `240-dns-hierarchy`: follow one resolution and mark, for each message, what the receiver checks before believing it.

## Questions for discussion

- Port randomisation and DNSSEC both answer cache poisoning. Which threat models does each cover that the other does not?
- Why does encrypting the client–resolver hop not make DNSSEC unnecessary, and vice versa?
- A resolver sits behind a NAT that rewrites source ports sequentially. What has happened to its entropy, and who can fix it?
- DNSSEC adoption has been slow for two decades. Which costs fall on whom, and who gets the benefit?
- How would you extend the lab to measure, rather than assert, the success probability of an off-path forger as a function of the entropy available?
