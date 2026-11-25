title      BGP prefix hijack, more-specific hijack and origin validation
title[it]  BGP: dirottamento di un prefisso, prefisso più specifico e validazione dell'origine
level  4
layer  3
topic  bgp
about  BGP believes what it is told. AS 65001 owns 10.1.0.0/24 and hosts the server srv
about  (10.1.0.10). The ASes are in a line: 65001 - 65002 - 65003 - 65066, with a client in 65002
about  and an observer in 65003. AS 65066 is malicious and has its own machine "fake" configured
about  with the server's address.
about
about  At 15 s AS 65066 announces 10.1.0.0/24 as if it were its own. Each AS now hears two origins
about  for the same prefix and picks the shorter AS path: 65003, next to the attacker, is hijacked
about  (the observer's pings are answered by fake), while 65002 stays with the true origin. At 30 s
about  the attacker announces the two halves 10.1.0.0/25 and 10.1.0.128/25: forwarding always
about  prefers the longest matching prefix, so now everyone, however close to the victim, sends the
about  traffic to the attacker. Count who answers: the Counters of srv and fake (icmp.echo_served).
about
about  Defence: rerun with rov=1. Every router then checks announcements against a route origin
about  authorization (10.1.0.0/24, origin AS 65001, maximum length 24), as with RPKI, and rejects
about  both the wrong origin and the too-specific prefixes. (The checks at the end of the run
about  describe rov=0; with rov=1 the real server answers all twelve pings and fake none.)
about[it]  BGP crede a ciò che gli si dice. L'AS 65001 possiede 10.1.0.0/24 e ospita il server srv
about[it]  (10.1.0.10). Gli AS sono in fila: 65001 - 65002 - 65003 - 65066, con un client nel 65002 e
about[it]  un osservatore nel 65003. L'AS 65066 è malevolo e ha una propria macchina "fake"
about[it]  configurata con l'indirizzo del server.
about[it]
about[it]  A 15 s l'AS 65066 annuncia 10.1.0.0/24 come fosse suo. Ogni AS ora sente due origini per lo
about[it]  stesso prefisso e sceglie l'AS path più corto: il 65003, accanto all'attaccante, viene
about[it]  dirottato (ai ping dell'osservatore risponde fake), mentre il 65002 resta con l'origine
about[it]  vera. A 30 s l'attaccante annuncia le due metà 10.1.0.0/25 e 10.1.0.128/25: l'inoltro
about[it]  preferisce sempre il prefisso più lungo, quindi ora tutti, per quanto vicini alla vittima,
about[it]  mandano il traffico all'attaccante. Conta chi risponde: i contatori di srv e fake
about[it]  (icmp.echo_served).
about[it]
about[it]  Difesa: riprova con rov=1. Ogni router confronta gli annunci con una route origin
about[it]  authorization (10.1.0.0/24, origine AS 65001, lunghezza massima 24), come con RPKI, e
about[it]  rifiuta sia l'origine sbagliata sia i prefissi troppo specifici. (Le verifiche a fine
about[it]  esecuzione descrivono rov=0; con rov=1 il vero server risponde a tutti e dodici i ping e
about[it]  fake a nessuno.)
param  rov 0 Route origin validation on every router (1 = on)
param[it] rov Validazione dell'origine su ogni router (1 = attiva)
param+ keepalive 3s BGP keepalive interval
param+[it] keepalive Intervallo di keepalive BGP

node srv    host   x=5  y=75 role=server
node r1     router x=15 y=35 role=AS65001
node r2     router x=40 y=35 role=AS65002
node r3     router x=65 y=35 role=AS65003
node evil   router x=90 y=35 role=AS65066
node client host   x=40 y=80
node obs    host   x=65 y=80
node fake   host   x=95 y=75 role=fake
link srv r1 delay=1ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r3 evil delay=5ms
link r2 client delay=1ms
link r3 obs delay=1ms
link evil fake delay=1ms
ip r1:eth0 10.1.0.1/24
ip srv 10.1.0.10/24
ip r1:eth1 172.16.12.1/24
ip r2:eth0 172.16.12.2/24
ip r2:eth1 172.16.23.2/24
ip r3:eth0 172.16.23.3/24
ip r3:eth1 172.16.36.3/24
ip evil:eth0 172.16.36.6/24
ip r2:eth2 10.2.0.1/24
ip client 10.2.0.10/24
ip r3:eth2 10.3.0.1/24
ip obs 10.3.0.10/24
ip evil:eth1 10.1.0.1/24
ip fake 10.1.0.10/24
gw srv 10.1.0.1
gw client 10.2.0.1
gw obs 10.3.0.1
gw fake 10.1.0.1

run r1   bgp as=65001 keepalive=$keepalive network=10.1.0.0/24 neighbor=r2:65002 rov=$rov roa=10.1.0.0/24:65001:24
run r2   bgp as=65002 keepalive=$keepalive network=10.2.0.0/24 neighbor=r1:65001 neighbor=r3:65003 rov=$rov roa=10.1.0.0/24:65001:24
run r3   bgp as=65003 keepalive=$keepalive network=10.3.0.0/24 neighbor=r2:65002 neighbor=evil:65066 rov=$rov roa=10.1.0.0/24:65001:24
run evil bgp as=65066 keepalive=$keepalive neighbor=r3:65003

note 0s  Normal operation: AS 65001 originates 10.1.0.0/24 and everyone reaches the real server.
note[it] 0s  Funzionamento normale: l'AS 65001 origina 10.1.0.0/24 e tutti raggiungono il vero server.
note 15s Hijack: AS 65066 announces the same /24. AS 65003 prefers it (shorter AS path); AS 65002 does not.
note[it] 15s Dirottamento: l'AS 65066 annuncia lo stesso /24. L'AS 65003 lo preferisce (AS path più corto); l'AS 65002 no.
note 30s More-specific hijack: two /25 announced. Longest prefix match beats any AS path: everyone is hijacked.
note[it] 30s Dirottamento con prefisso più specifico: annunciati due /25. Il longest prefix match batte qualsiasi AS path: tutti sono dirottati.

at 8s  run client ping 10.1.0.10 count=2
at 8s  run obs ping 10.1.0.10 count=2
at 15s run evil bgp announce 10.1.0.0/24
at 20s run client ping 10.1.0.10 count=2
at 20s run obs ping 10.1.0.10 count=2
at 30s run evil bgp announce 10.1.0.0/25
at 30s run evil bgp announce 10.1.0.128/25
at 36s run client ping 10.1.0.10 count=2
at 36s run obs ping 10.1.0.10 count=2
duration 42s
expect client ping.reply == 6
expect obs ping.reply == 6
expect srv icmp.echo_served == 6 if $rov == 0
expect fake icmp.echo_served == 6 if $rov == 0
expect srv icmp.echo_served == 12 if $rov == 1
expect fake icmp.echo_served == 0 if $rov == 1
