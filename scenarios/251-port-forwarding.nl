title      Port forwarding: publishing a server that sits behind a NAT
title[it]  Port forwarding: pubblicare un server che sta dietro un NAT
level  2
layer  3
topic  nat
about  NAT mappings are created by traffic going out, so nobody on the Internet can start a conversation
about  with a host inside. Here the inside host `web` runs a service (a UDP echo on port 7) and the outside
about  client `ext` tries to reach it through the public address of r1, port 8080.
about
about  The first attempt finds no mapping: r1 has nothing to deliver the datagram to and answers with an
about  ICMP "port unreachable". At 4 s a static rule is added (public port 8080 → web:7) and the second
about  attempt gets through. Note that `ext` only ever sees the public address: the answer leaves r1 with
about  source 203.0.113.2:8080.
about
about  Try: hairpin=1 and then `h1 udp 203.0.113.2 port=8080` from the console, to reach the server by
about  its public address from inside the same network.
about[it]  Le associazioni del NAT nascono dal traffico in uscita, quindi nessuno da Internet può iniziare
about[it]  una conversazione con un host interno. Qui l'host interno `web` offre un servizio (un echo UDP
about[it]  sulla porta 7) e il client esterno `ext` prova a raggiungerlo tramite l'indirizzo pubblico di r1,
about[it]  porta 8080.
about[it]
about[it]  Il primo tentativo non trova alcuna associazione: r1 non sa a chi consegnare il datagramma e
about[it]  risponde con un ICMP "port unreachable". A 4 s viene aggiunta una regola statica (porta pubblica
about[it]  8080 → web:7) e il secondo tentativo passa. Nota che `ext` vede sempre e solo l'indirizzo
about[it]  pubblico: la risposta lascia r1 con sorgente 203.0.113.2:8080.
about[it]
about[it]  Prova: hairpin=1 e poi `h1 udp 203.0.113.2 port=8080` dalla console, per raggiungere il server
about[it]  con il suo indirizzo pubblico dall'interno della stessa rete.
param  port 8080 Public port that is forwarded to the inside server
param[it] port Porta pubblica inoltrata al server interno
param+ hairpin 0 Let inside hosts use the public address too (0/1)
param[it] hairpin Permetti anche agli host interni di usare l'indirizzo pubblico (0/1)

node web host x=8 y=25 role=SERVER
node h1 host x=8 y=75
node sw switch x=26 y=50
node r1 router x=46 y=50 role=NAT
node isp router x=68 y=50 role=ISP
node ext host x=90 y=50
link web sw delay=1ms
link h1 sw delay=1ms
link sw r1 delay=1ms
link r1 isp delay=10ms
link isp ext delay=5ms

ip web 192.168.1.10/24
ip h1 192.168.1.11/24
ip r1:eth0 192.168.1.1/24
ip r1:eth1 203.0.113.2/30
ip isp:eth0 203.0.113.1/30
ip isp:eth1 198.51.100.1/24
ip ext 198.51.100.10/24
gw web 192.168.1.1
gw h1 192.168.1.1
gw r1 203.0.113.1
gw ext 198.51.100.1

run r1 nat out=eth1 hairpin=$hairpin
run web echo
at 1s run ext udp 203.0.113.2 port=$port msg=anyone-there
note 1s ext knocks on the public address: there is no mapping for that port, so nothing inside receives it.
note[it] 1s ext bussa all'indirizzo pubblico: non c'è alcuna associazione per quella porta, quindi nessuno all'interno lo riceve.
at 4s run r1 nat forward proto=udp port=$port to=web:7
note 4s Static rule on r1: public port → web:7. It never expires and accepts any sender.
note[it] 4s Regola statica su r1: porta pubblica → web:7. Non scade mai e accetta qualunque mittente.
at 5s run ext udp 203.0.113.2 port=$port msg=hello-web
at 7s run h1 udp 203.0.113.2 port=$port msg=from-inside
note 7s h1 tries the public address from inside: it works only with hairpin=1.
note[it] 7s h1 prova l'indirizzo pubblico dall'interno: funziona solo con hairpin=1.
duration 10s

expect ext udp.reply == 1
expect r1 nat.blocked >= 1
expect web echo.served >= 1
