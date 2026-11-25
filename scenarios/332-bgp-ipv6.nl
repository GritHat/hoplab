title      BGP for two protocols: IPv6 prefixes in a multiprotocol session
title[it]  BGP per due protocolli: prefissi IPv6 in una sessione multiprotocollo
level  3
layer  3
topic  bgp
about      Three autonomous systems in a triangle, every network dual stack. BGP-4 was built for IPv4; the multiprotocol extensions let the same session also carry IPv6 prefixes, inside the MP_REACH_NLRI attribute with a next hop of their own. Open the Packets view and compare two UPDATE messages: the IPv4 prefix sits in the classic fields with NEXT_HOP, the IPv6 one inside MP_REACH_NLRI with the neighbor's global and link-local addresses. The OPEN shows the capability with which the two routers agree on the address families.
about      The two families share the session, the AS paths and the policies, but each prefix lives on its own. When the direct link r1-r3 fails, both families move to the path through AS 65002. At 38 s AS 65003 withdraws only its IPv6 prefix: h3 answers the IPv4 ping and is unreachable over IPv6.
about      Try transport=6: the TCP connections of BGP then run over IPv6, and still carry the IPv4 routes. Look at the Routes tab of r1: the IPv6 routes learned from BGP point at link-local next hops (fe80::...).
about[it]  Tre sistemi autonomi a triangolo, ogni rete in doppio stack. BGP-4 è nato per IPv4; le estensioni multiprotocollo permettono alla stessa sessione di trasportare anche prefissi IPv6, dentro l'attributo MP_REACH_NLRI con un next hop proprio. Apri la vista Pacchetti e confronta due messaggi UPDATE: il prefisso IPv4 sta nei campi classici con NEXT_HOP, quello IPv6 dentro MP_REACH_NLRI con gli indirizzi globale e link-local del vicino. L'OPEN mostra la capability con cui i due router concordano le famiglie di indirizzi.
about[it]  Le due famiglie condividono la sessione, gli AS path e le politiche, ma ogni prefisso vive per conto suo. Quando il collegamento diretto r1-r3 si guasta, entrambe le famiglie passano al percorso attraverso l'AS 65002. A 38 s l'AS 65003 ritira solo il suo prefisso IPv6: h3 risponde al ping IPv4 ed è irraggiungibile su IPv6.
about[it]  Prova transport=6: le connessioni TCP di BGP viaggiano allora su IPv6, e trasportano comunque le rotte IPv4. Guarda la scheda Rotte di r1: le rotte IPv6 imparate da BGP puntano a next hop link-local (fe80::...).
param      transport 4 Protocol that carries the BGP sessions (4 = IPv4, 6 = IPv6)
param[it]  transport Protocollo che trasporta le sessioni BGP (4 = IPv4, 6 = IPv6)
param+     keepalive 3s BGP keepalive interval (hold time is 3 times this)
param+[it] keepalive Intervallo di keepalive BGP (l'hold time è 3 volte tanto)

node h1 host   x=5  y=80
node r1 router x=22 y=80 role=AS65001
node r2 router x=50 y=15 role=AS65002
node r3 router x=78 y=80 role=AS65003
node h2 host   x=50 y=45
node h3 host   x=95 y=80
link h1 r1 delay=1ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r1 r3 delay=5ms
link r2 h2 delay=1ms
link r3 h3 delay=1ms
# addresses for both protocols, and the hosts' default routes; routing between
# the autonomous systems is left to BGP
auto addr
auto addr6

run r1 bgp as=65001 connected=1 neighbor=r2:65002 neighbor=r3:65003 keepalive=$keepalive transport=$transport
run r2 bgp as=65002 connected=1 neighbor=r1:65001 neighbor=r3:65003 keepalive=$keepalive transport=$transport
run r3 bgp as=65003 connected=1 neighbor=r1:65001 neighbor=r2:65002 keepalive=$keepalive transport=$transport

note 0     Each pair of routers opens one TCP connection to port 179 and exchanges OPENs offering IPv4 and IPv6 unicast.
note[it] 0 Ogni coppia di router apre una connessione TCP verso la porta 179 e scambia OPEN che offrono IPv4 e IPv6 unicast.
at 10s run h1 ping h3 count=2
at 13s run h1 ping h3 count=2 v6=1
note 10s   Both protocols reach h3 through the direct link: AS path 65003, one AS long, for the IPv4 and the IPv6 prefix alike.
note[it] 10s Entrambi i protocolli raggiungono h3 dal collegamento diretto: AS path 65003, lungo un solo AS, sia per il prefisso IPv4 sia per quello IPv6.
at 17s run h1 traceroute h3 v6=1
at 22s link r1 r3 down
note 22s   The link r1-r3 fails: the session is lost, its routes of both families are withdrawn, the path through AS 65002 takes over.
note[it] 22s Il collegamento r1-r3 si guasta: la sessione cade, le sue rotte di entrambe le famiglie sono ritirate, subentra il percorso attraverso l'AS 65002.
at 30s run h1 ping h3 count=2 v6=1
at 33s run h1 traceroute h3 v6=1
at 38s run r3 bgp withdraw 2001:db8:6::/64
note 38s   AS 65003 withdraws only its IPv6 prefix (an MP_UNREACH_NLRI attribute). Its IPv4 prefix stays.
note[it] 38s L'AS 65003 ritira solo il suo prefisso IPv6 (un attributo MP_UNREACH_NLRI). Il suo prefisso IPv4 resta.
at 42s run h1 ping h3 count=1 v6=1
at 44s run h1 ping h3 count=1
duration 48s
expect h1 ping.reply == 7
expect h1 ping.error == 1
expect h1 traceroute.hops == 4
expect r1 bgp.established == 1
expect r2 bgp.established == 2
expect r1 bgp.session_drops == 1
expect r1 bgp.routes == 3
expect r1 bgp.routes6 == 2
