title      BGP policies: customers, peers, providers and valley-free paths
title[it]  Politiche BGP: clienti, peer, provider e percorsi valley-free
level  4
layer  3
topic  bgp
about  Routing between autonomous systems follows money, not distance. Two providers, p1 and p2,
about  peer with each other (they exchange their customers' traffic for free). c1 buys transit from
about  p1, c2 from p2, and m is multihomed: a customer of both. Each AS prefers routes through its
about  customers (they pay), then peers, then providers, and exports with the Gao-Rexford rule:
about  routes learned from a peer or a provider are announced only to customers.
about
about  First hc1 pings hc2: the path climbs to p1, crosses the peering link, descends to c2: a
about  valley-free path. At 20 s the peering link p1-p2 fails. Physically c1 could still reach c2
about  through p1 - m - p2, but that would make m carry traffic between its two providers for free:
about  m never announced c2's prefix to p1, so p1 has no route and the ping fails with "network
about  unreachable". m itself still reaches everybody. Look at the BGP tab of m at detail 3: it
about  knows the routes, it just does not export them upwards.
about
about  Rerun with policy=0 (plain shortest AS path, export everything): after the failure the
about  traffic between c1 and c2 flows through the unlucky customer m. (The checks at the end of
about  the run describe policy=1; with policy=0 three of them report the difference.)
about[it]  Il routing fra sistemi autonomi segue il denaro, non la distanza. Due provider, p1 e p2,
about[it]  sono in peering (si scambiano gratis il traffico dei rispettivi clienti). c1 compra transito
about[it]  da p1, c2 da p2, e m è multihomed: cliente di entrambi. Ogni AS preferisce le rotte tramite
about[it]  i clienti (che pagano), poi i peer, poi i provider, ed esporta con la regola di Gao-Rexford:
about[it]  le rotte imparate da un peer o da un provider si annunciano solo ai clienti.
about[it]
about[it]  Prima hc1 fa ping a hc2: il percorso sale a p1, attraversa il peering, scende a c2: un
about[it]  percorso valley-free. A 20 s il collegamento di peering p1-p2 si guasta. Fisicamente c1
about[it]  potrebbe ancora raggiungere c2 via p1 - m - p2, ma m trasporterebbe gratis traffico fra i
about[it]  suoi due provider: m non ha mai annunciato a p1 il prefisso di c2, quindi p1 non ha rotta e
about[it]  il ping fallisce con "network unreachable". m invece raggiunge ancora tutti. Guarda la
about[it]  scheda BGP di m a dettaglio 3: conosce le rotte, semplicemente non le esporta verso l'alto.
about[it]
about[it]  Riprova con policy=0 (solo AS path più corto, si esporta tutto): dopo il guasto il traffico
about[it]  fra c1 e c2 passa dallo sfortunato cliente m. (Le verifiche a fine esecuzione descrivono
about[it]  policy=1; con policy=0 tre di esse segnalano la differenza.)
param  policy 1 Apply business relations (0 = shortest AS path only, export everything)
param[it] policy Applica le relazioni commerciali (0 = solo AS path più corto, esporta tutto)
param+ keepalive 3s BGP keepalive interval
param+[it] keepalive Intervallo di keepalive BGP

node p1  router x=30 y=15 role=AS100
node p2  router x=70 y=15 role=AS200
node c1  router x=12 y=55 role=AS65001
node m   router x=50 y=55 role=AS65002
node c2  router x=88 y=55 role=AS65003
node hc1 host   x=12 y=90
node hm  host   x=50 y=90
node hc2 host   x=88 y=90
link p1 p2 delay=5ms
link p1 c1 delay=5ms
link p1 m  delay=5ms
link p2 m  delay=5ms
link p2 c2 delay=5ms
link c1 hc1 delay=1ms
link m  hm  delay=1ms
link c2 hc2 delay=1ms
ip p1:eth0 172.16.0.1/24
ip p2:eth0 172.16.0.2/24
ip p1:eth1 172.16.1.1/24
ip c1:eth0 172.16.1.2/24
ip p1:eth2 172.16.2.1/24
ip m:eth0  172.16.2.2/24
ip p2:eth1 172.16.3.1/24
ip m:eth1  172.16.3.2/24
ip p2:eth2 172.16.4.1/24
ip c2:eth0 172.16.4.2/24
ip c1:eth1 10.1.0.1/24
ip hc1 10.1.0.10/24
ip m:eth2 10.2.0.1/24
ip hm 10.2.0.10/24
ip c2:eth1 10.3.0.1/24
ip hc2 10.3.0.10/24
gw hc1 10.1.0.1
gw hm 10.2.0.1
gw hc2 10.3.0.1

run p1 bgp as=100 policy=$policy keepalive=$keepalive neighbor=p2:200:peer neighbor=c1:65001:customer neighbor=m:65002:customer
run p2 bgp as=200 policy=$policy keepalive=$keepalive neighbor=p1:100:peer neighbor=c2:65003:customer neighbor=m:65002:customer
run c1 bgp as=65001 policy=$policy keepalive=$keepalive network=10.1.0.0/24 neighbor=p1:100:provider
run m  bgp as=65002 policy=$policy keepalive=$keepalive network=10.2.0.0/24 neighbor=p1:100:provider neighbor=p2:200:provider
run c2 bgp as=65003 policy=$policy keepalive=$keepalive network=10.3.0.0/24 neighbor=p2:200:provider

note 0s  Sessions come up. Customers announce their prefix upwards; providers pass customer routes to everyone.
note[it] 0s  Le sessioni si attivano. I clienti annunciano il proprio prefisso verso l'alto; i provider passano le rotte dei clienti a tutti.
note 10s hc1 → hc2: up to p1, across the peering link, down to c2. A valley-free path.
note[it] 10s hc1 → hc2: su fino a p1, attraverso il peering, giù fino a c2. Un percorso valley-free.
note 20s The peering link fails. A physical path through m exists, but m does not sell transit to its providers.
note[it] 20s Il peering si guasta. Un percorso fisico via m esiste, ma m non vende transito ai suoi provider.
note 26s hc1 → hc2 now fails (with policy=1), while hm still reaches both.
note[it] 26s hc1 → hc2 ora fallisce (con policy=1), mentre hm raggiunge ancora entrambi.

at 10s run hc1 ping hc2 count=3
at 14s run hm ping hc1 count=2
at 20s link p1 p2 down
at 26s run hc1 ping hc2 count=3
at 30s run hm ping hc2 count=2
at 33s run hm ping hc1 count=2
duration 38s
expect hc1 ping.reply == 3 if $policy == 1
expect hc1 ping.error >= 2 if $policy == 1
expect hc1 ping.reply == 6 if $policy == 0
expect hc1 ping.error == 0 if $policy == 0
expect hm ping.reply == 6
expect p1 bgp.routes == 2 if $policy == 1
expect p1 bgp.routes == 3 if $policy == 0
expect m bgp.routes == 2
