title      RIPng: the same algorithm for IPv6, side by side with RIP
title[it]  RIPng: lo stesso algoritmo per IPv6, fianco a fianco con RIP
level  2
layer  3
topic  ripng
about      A ring of four dual-stack routers runs two routing protocols at once: RIP for IPv4 and RIPng for IPv6. They are separate protocol instances with separate tables, and comparing them is the point. Open the Routes tab of r1: the IPv4 routes have a neighbor's IPv4 address as next hop; the IPv6 routes have a link-local address (fe80::...) and an interface, because RIPng talks from link-local addresses to the multicast group ff02::9 and never needs the global prefixes to find its neighbors.
about
about      At 22 s the link r1-r2 fails. Both protocols mark the routes through it unreachable (metric 16), tell their neighbors, and settle on the other side of the ring. Watch the two tabs `rip` and `ripng` of a router change together.
about
about      Things to try: open a RIPng response in the Packets view and compare its 20-byte entries (prefix, prefix length, metric) with a RIP one (address, mask, next hop, metric); set triggered=0 and see how much longer both families take to recover; type  :r2 ripng poison=1  and read the next update r2 sends.
about[it]  Un anello di quattro router dual stack esegue due protocolli di instradamento insieme: RIP per IPv4 e RIPng per IPv6. Sono istanze separate con tabelle separate, e il confronto è lo scopo del laboratorio. Apri la scheda Rotte di r1: le rotte IPv4 hanno come next hop l'indirizzo IPv4 di un vicino; quelle IPv6 hanno un indirizzo link-local (fe80::...) e un'interfaccia, perché RIPng parla dagli indirizzi link-local verso il gruppo multicast ff02::9 e non ha mai bisogno dei prefissi globali per trovare i vicini.
about[it]
about[it]  A 22 s il collegamento r1-r2 si guasta. Entrambi i protocolli marcano irraggiungibili (metrica 16) le rotte che lo attraversavano, avvisano i vicini e si assestano sull'altro lato dell'anello. Osserva le due schede `rip` e `ripng` di un router cambiare insieme.
about[it]
about[it]  Da provare: apri una risposta RIPng nella vista Pacchetti e confronta le sue voci da 20 byte (prefisso, lunghezza, metrica) con quelle di RIP (indirizzo, maschera, next hop, metrica); imposta triggered=0 e osserva quanto più tempo impiegano entrambe le famiglie a riprendersi; scrivi  :r2 ripng poison=1  e leggi il prossimo aggiornamento inviato da r2.
param      update 5s Period of the updates (RIP and RIPng), from 2s to 10s
param[it]  update Periodo degli aggiornamenti (RIP e RIPng), da 2s a 10s
param+     triggered 1 Triggered updates (1 = on)
param[it]  triggered Aggiornamenti immediati (1 = attivi)
param+     poison 0 Poisoned reverse (1 = on)
param[it]  poison Poisoned reverse (1 = attivo)

node h1 host   x=5  y=50
node r1 router x=25 y=50
node r2 router x=50 y=15
node r3 router x=75 y=50
node r4 router x=50 y=85
node h3 host   x=95 y=50
link h1 r1 delay=2ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r3 r4 delay=5ms
link r4 r1 delay=5ms
link r3 h3 delay=2ms
auto addr
auto addr6

run r1 rip update=$update triggered=$triggered poison=$poison
run r2 rip update=$update triggered=$triggered poison=$poison
run r3 rip update=$update triggered=$triggered poison=$poison
run r4 rip update=$update triggered=$triggered poison=$poison
run r1 ripng update=$update triggered=$triggered poison=$poison
run r2 ripng update=$update triggered=$triggered poison=$poison
run r3 ripng update=$update triggered=$triggered poison=$poison
run r4 ripng update=$update triggered=$triggered poison=$poison

note 0s    Two protocols start on every router. RIPng waits about a second: the link-local addresses must pass duplicate address detection first.
note[it] 0s Su ogni router partono due protocolli. RIPng aspetta circa un secondo: gli indirizzi link-local devono prima superare il rilevamento dei duplicati.
note 14s   Both tables are complete. h1 pings h3 over IPv4 and over IPv6: three router hops either way.
note[it] 14s Entrambe le tabelle sono complete. h1 fa ping a h3 su IPv4 e su IPv6: tre router in entrambi i casi.
note 22s   Link r1-r2 fails. RIP and RIPng each poison the routes that used it; neither knows about the other.
note[it] 22s Il collegamento r1-r2 si guasta. RIP e RIPng avvelenano ciascuno le rotte che lo usavano; nessuno dei due sa dell'altro.
note 46s   Both have reconverged around the ring through r4.
note[it] 46s Entrambi hanno riconvergito lungo l'anello attraverso r4.

at 14s run h1 ping h3 count=3
at 14s run h1 ping h3 count=3 v6=1
at 18s run h1 traceroute h3 v6=1
at 22s link r1 r2 down
at 46s run h3 ping h1 count=3
at 46s run h3 ping h1 count=3 v6=1
at 50s run h1 traceroute h3 v6=1
duration 56s
expect h1 ping.reply == 6
expect h3 ping.reply == 6
expect h1 traceroute.hops == 4
expect r1 rip.routes >= 5
expect r1 ripng.routes >= 5
expect r3 ripng.routes >= 5
