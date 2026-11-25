title      IPv4 islands over an IPv6-only core: BGP with IPv6 next hops
title[it]  Isole IPv4 su una dorsale solo IPv6: BGP con next hop IPv6
level  3
layer  3
topic  bgp
about      Two customer networks still speak IPv4, but the links between the routers carry IPv6 addresses only: no IPv4 address is spent on them, and the core router p has no IPv4 address at all. Classic BGP cannot announce an IPv4 prefix here, because its NEXT_HOP attribute holds an IPv4 address and there is none to give. With the extended next hop encoding (RFC 8950) the routers agree in the OPEN that IPv4 prefixes may travel in MP_REACH_NLRI with an IPv6 next hop; the route is installed as "10.3.0.0/24 via fe80::..." and the IPv4 packets are forwarded in ordinary IPv4 frames whose destination MAC address comes from Neighbor Discovery instead of ARP.
about      Watch the OPEN messages in the Packets view (capability "extended next hop"), the UPDATEs (AFI 1 with a 32-byte next hop: global and link-local), and the Routes tab of r1 and p. The ping from h1 to h2 crosses the core untouched: it is IPv4 all the way. The traceroute tells a true story too: r1 and r3 answer from the IPv4 address of their customer interface, while p, which has no IPv4 address to put in the source field, cannot answer and shows up as a silent hop.
about      At 20 s AS 65003 withdraws its IPv4 prefix (an MP_UNREACH_NLRI with AFI 1) and h2 becomes unreachable; at 28 s it announces it again. Set enh to 0 to see the same network without the capability: the sessions still come up for IPv6, but no IPv4 prefix can be exchanged and the islands stay isolated.
about[it]  Due reti di clienti parlano ancora IPv4, ma i collegamenti fra i router portano solo indirizzi IPv6: su di essi non si spende alcun indirizzo IPv4, e il router di dorsale p non ha proprio indirizzi IPv4. Il BGP classico qui non può annunciare un prefisso IPv4, perché il suo attributo NEXT_HOP contiene un indirizzo IPv4 e non ce n'è nessuno da dare. Con la codifica estesa del next hop (RFC 8950) i router si accordano nell'OPEN sul fatto che i prefissi IPv4 possono viaggiare in MP_REACH_NLRI con un next hop IPv6; la rotta viene installata come "10.3.0.0/24 via fe80::..." e i pacchetti IPv4 sono inoltrati in normali frame IPv4 il cui indirizzo MAC di destinazione viene da Neighbor Discovery invece che da ARP.
about[it]  Osserva i messaggi OPEN nella vista Pacchetti (capability "extended next hop"), gli UPDATE (AFI 1 con un next hop di 32 byte: globale e link-local) e la scheda Rotte di r1 e di p. Il ping da h1 a h2 attraversa la dorsale intatto: è IPv4 dall'inizio alla fine. Anche il traceroute racconta una storia vera: r1 e r3 rispondono dall'indirizzo IPv4 della loro interfaccia verso il cliente, mentre p, che non ha alcun indirizzo IPv4 da mettere nel campo sorgente, non può rispondere e compare come un salto muto.
about[it]  A 20 s l'AS 65003 ritira il suo prefisso IPv4 (un MP_UNREACH_NLRI con AFI 1) e h2 diventa irraggiungibile; a 28 s lo annuncia di nuovo. Imposta enh a 0 per vedere la stessa rete senza la capability: le sessioni si stabiliscono comunque per IPv6, ma nessun prefisso IPv4 può essere scambiato e le isole restano isolate.
param      enh 1 Extended next hop encoding, RFC 8950 (1 = on, 0 = off)
param[it]  enh Codifica estesa del next hop, RFC 8950 (1 = attiva, 0 = disattivata)
param+     keepalive 3s BGP keepalive interval (hold time is 3 times this)
param+[it] keepalive Intervallo di keepalive BGP (l'hold time è 3 volte tanto)

node h1 host   x=5  y=50
node r1 router x=25 y=50 role=AS65001
node p  router x=50 y=50 role=AS65000
node r3 router x=75 y=50 role=AS65003
node h2 host   x=95 y=50
link h1 r1 delay=1ms
link r1 p  delay=5ms
link p r3  delay=5ms
link r3 h2 delay=1ms

# the customer networks are IPv4; the core links have IPv6 addresses only
ip h1      10.1.0.10/24
ip r1:eth0 10.1.0.1/24
ip r1:eth1 2001:db8:1::1/64
ip p:eth0  2001:db8:1::2/64
ip p:eth1  2001:db8:3::2/64
ip r3:eth0 2001:db8:3::3/64
ip r3:eth1 10.3.0.1/24
ip h2      10.3.0.10/24
gw h1 10.1.0.1
gw h2 10.3.0.1

# p has no IPv4 address to derive its BGP identifier from: it is given one
run r1 bgp as=65001 network=10.1.0.0/24 neighbor=p:65000 keepalive=$keepalive enh=$enh
run p  bgp as=65000 id=10.255.0.1 neighbor=r1:65001 neighbor=r3:65003 keepalive=$keepalive enh=$enh
run r3 bgp as=65003 network=10.3.0.0/24 neighbor=p:65000 keepalive=$keepalive enh=$enh

note 0     The routers share only IPv6 networks: the BGP sessions run over IPv6, and the OPENs offer IPv4 unicast with IPv6 next hops.
note[it] 0 I router condividono solo reti IPv6: le sessioni BGP viaggiano su IPv6, e gli OPEN offrono IPv4 unicast con next hop IPv6.
at 8s run h1 ping h2 count=3
note 8s    An IPv4 ping across a core without IPv4 addresses: each router forwards it to an IPv6 next hop, resolved with Neighbor Discovery.
note[it] 8s Un ping IPv4 attraverso una dorsale senza indirizzi IPv4: ogni router lo inoltra a un next hop IPv6, risolto con Neighbor Discovery.
at 12s run h1 traceroute h2
note 12s   traceroute: r1 answers, p stays silent (it has no IPv4 address to answer from), r3 answers from its customer-side address.
note[it] 12s traceroute: r1 risponde, p resta muto (non ha un indirizzo IPv4 da cui rispondere), r3 risponde dal suo indirizzo lato cliente.
at 20s run r3 bgp withdraw 10.3.0.0/24
note 20s   AS 65003 withdraws 10.3.0.0/24: the withdrawal of an IPv4 prefix also travels in a multiprotocol attribute.
note[it] 20s L'AS 65003 ritira 10.3.0.0/24: anche il ritiro di un prefisso IPv4 viaggia in un attributo multiprotocollo.
at 24s run h1 ping h2 count=1
at 28s run r3 bgp announce 10.3.0.0/24
note 28s   The prefix is announced again and the island comes back.
note[it] 28s Il prefisso viene annunciato di nuovo e l'isola torna raggiungibile.
at 34s run h1 ping h2 count=2
duration 40s

# with the capability: reachable, withdrawn, reachable again
expect h1 ping.reply == 5 if $enh == 1
expect h1 ping.error == 1 if $enh == 1
expect h1 traceroute.hops == 4 if $enh == 1
expect r1 bgp.routes == 1 if $enh == 1
expect p bgp.routes == 2 if $enh == 1
expect p ip.forwarded >= 10 if $enh == 1
# without it: the sessions are up, but carry no IPv4 route
expect h1 ping.reply == 0 if $enh == 0
expect r1 bgp.routes == 0 if $enh == 0
expect p bgp.routes == 0 if $enh == 0
expect r1 bgp.established == 1
expect p bgp.established == 2
