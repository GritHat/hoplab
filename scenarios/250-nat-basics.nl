title      NAT: why private addresses need a translator
title[it]  NAT: perché gli indirizzi privati hanno bisogno di un traduttore
level  2
layer  3
topic  nat
about  A home network (192.168.1.0/24) reaches a server on the Internet through router r1. The ISP router
about  has no route to the private network, exactly like the real Internet. First h1 pings the server with
about  NAT switched off: the requests arrive, but the answers die at the ISP ("no route"). At 4 s NAT is
about  turned on at r1 and everything works: watch the source address change on the r1 — isp link and the
about  translation table in the panel of r1.
about
about  Then h1 and h2 both send a UDP datagram from the same source port: r1 gives them two different
about  public ports, which is how it tells the answers apart. Finally a traceroute crosses the NAT: the ICMP
about  errors coming back quote the translated packet, and r1 must translate the quote too.
about
about  Try: type=symmetric (a new port for each destination), preserve=1 (keep the source port when free),
about  a short timeout with a slow ping (interval longer than the timeout) to see mappings expire.
about[it]  Una rete domestica (192.168.1.0/24) raggiunge un server su Internet attraverso il router r1. Il
about[it]  router dell'ISP non ha una rotta verso la rete privata, proprio come nell'Internet reale. All'inizio
about[it]  h1 fa ping al server con il NAT spento: le richieste arrivano, ma le risposte muoiono all'ISP
about[it]  ("no route"). A 4 s il NAT viene acceso su r1 e tutto funziona: osserva l'indirizzo sorgente che
about[it]  cambia sul collegamento r1 — isp e la tabella di traduzione nel pannello di r1.
about[it]
about[it]  Poi h1 e h2 inviano entrambi un datagramma UDP dalla stessa porta sorgente: r1 assegna loro due
about[it]  porte pubbliche diverse, ed è così che distingue le risposte. Infine un traceroute attraversa il
about[it]  NAT: gli errori ICMP di ritorno citano il pacchetto tradotto, e r1 deve tradurre anche la citazione.
about[it]
about[it]  Prova: type=symmetric (una porta nuova per ogni destinazione), preserve=1 (conserva la porta
about[it]  sorgente se libera), un timeout breve con un ping lento per vedere le associazioni scadere.
param  type port NAT behaviour: full, restricted, port, symmetric
param[it] type Comportamento del NAT: full, restricted, port, symmetric
param+ timeout 30s Idle time after which a mapping is forgotten
param[it] timeout Tempo di inattività dopo cui un'associazione viene dimenticata
param+ preserve 0 Keep the original source port when it is free (0/1)
param[it] preserve Conserva la porta sorgente originale se è libera (0/1)

node h1 host x=8 y=25
node h2 host x=8 y=75
node sw switch x=26 y=50
node r1 router x=44 y=50 role=NAT
node isp router x=66 y=50 role=ISP
node srv host x=90 y=50 role=SERVER
link h1 sw delay=1ms
link h2 sw delay=1ms
link sw r1 delay=1ms
link r1 isp delay=10ms
link isp srv delay=5ms

ip h1 192.168.1.10/24
ip h2 192.168.1.11/24
ip r1:eth0 192.168.1.1/24
ip r1:eth1 203.0.113.2/30
ip isp:eth0 203.0.113.1/30
ip isp:eth1 198.51.100.1/24
ip srv 198.51.100.10/24
gw h1 192.168.1.1
gw h2 192.168.1.1
gw r1 203.0.113.1
gw srv 198.51.100.1

run srv echo
run h1 ping srv count=2
note 0s Without NAT: the echo requests from 192.168.1.10 reach the server, but isp cannot route the replies to a private address.
note[it] 0s Senza NAT: le richieste da 192.168.1.10 arrivano al server, ma isp non sa instradare le risposte verso un indirizzo privato.
at 4s run r1 nat out=eth1 type=$type timeout=$timeout preserve=$preserve
note 4s NAT on: r1 now rewrites the source to its public address 203.0.113.2 and remembers the mapping.
note[it] 4s NAT acceso: r1 ora riscrive la sorgente con il suo indirizzo pubblico 203.0.113.2 e ricorda l'associazione.
at 5s run h1 ping srv count=3
at 9s run h1 udp srv msg=from-h1
at 9s run h2 udp srv msg=from-h2
note 9s h1 and h2 use the same UDP source port: look at the two different public ports in the table of r1.
note[it] 9s h1 e h2 usano la stessa porta sorgente UDP: guarda le due porte pubbliche diverse nella tabella di r1.
at 12s run h1 traceroute srv
note 12s Traceroute through the NAT: r1 translates the packet quoted inside the ICMP "time exceeded" from isp.
note[it] 12s Traceroute attraverso il NAT: r1 traduce il pacchetto citato dentro l'ICMP "time exceeded" di isp.
duration 16s

expect h1 ping.reply == 3
expect isp ip.noroute >= 2
expect h1 udp.reply == 1
expect h2 udp.reply == 1
expect r1 nat.mappings >= 3
expect h1 traceroute.hops == 3
