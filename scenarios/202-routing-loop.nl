title      A routing loop, and the TTL that saves the network
title[it]  Un ciclo di instradamento, e il TTL che salva la rete
level 2
layer 3
topic routing
about      Two routers each believe the other one leads to network 10.9.0.0/16: a packet for it bounces between them. Nothing in forwarding itself would ever stop it; the time-to-live field does. Every hop decrements it, the router that brings it to zero discards the packet and tells the sender with an ICMP "time exceeded". Watch one ping turn into dozens of transmissions on the r1-r2 link, then the error come back; traceroute shows the two routers alternating.
about
about      Things to try: send the ping with ttl=8 and count the bounces; fix the loop with  :route r2 del 10.9.0.0/16  and see "network unreachable" instead: a clean failure is better than a loop.
about[it]  Due router credono ciascuno che l'altro porti alla rete 10.9.0.0/16: un pacchetto destinato a essa rimbalza fra i due. Nulla nell'inoltro lo fermerebbe mai; ci pensa il campo time-to-live. Ogni salto lo decrementa, il router che lo porta a zero scarta il pacchetto e avvisa il mittente con un ICMP "time exceeded". Osserva un solo ping trasformarsi in decine di trasmissioni sul collegamento r1-r2, poi l'errore tornare indietro; traceroute mostra i due router che si alternano.
about[it]
about[it]  Da provare: invia il ping con ttl=8 e conta i rimbalzi; ripara il ciclo con  :route r2 del 10.9.0.0/16  e osserva "network unreachable" al suo posto: un fallimento pulito è meglio di un ciclo.
param      ttl 16 Initial TTL of the ping
param[it]  ttl TTL iniziale del ping

node h1 host
node r1 router
node r2 router
link h1 r1 delay=2ms
link r1 r2 delay=8ms
ip h1 10.0.1.10/24
ip r1:eth0 10.0.1.1/24
ip r1:eth1 10.0.12.1/24
ip r2:eth0 10.0.12.2/24
gw h1 10.0.1.1
route r2 10.0.1.0/24 via 10.0.12.1
route r1 10.9.0.0/16 via 10.0.12.2
route r2 10.9.0.0/16 via 10.0.12.1

run h1 ping 10.9.1.1 count=1 ttl=$ttl
note 0     The packet bounces between r1 and r2, losing one TTL unit per hop, until one of them discards it.
note[it] 0 Il pacchetto rimbalza fra r1 e r2, perdendo un'unità di TTL a ogni salto, finché uno dei due lo scarta.
at 4s run h1 traceroute 10.9.1.1 maxhops=6
note 4s    traceroute makes the loop visible: r1, r2, r1, r2...
note[it] 4s traceroute rende visibile il ciclo: r1, r2, r1, r2...
duration 12s
expect h1 ping.error == 1
expect h1 ping.reply == 0
expect h1 traceroute.hops == 0
