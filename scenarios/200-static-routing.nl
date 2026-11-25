title      Static routing: routing tables, next hops and a missing route
title[it]  Instradamento statico: tabelle, next hop e una rotta mancante
level 2
layer 3
topic routing
about      Three routers in a row, configured by hand. A router forwards a packet by looking up the destination in its table and choosing the longest matching prefix; it knows nothing of the rest of the path. At the start r2 has no route to h2's network: h1's ping gets as far as r2, which answers "network unreachable". At 6 s the missing route is added and the ping goes through. But replies need routes too: every router on the way back must know h1's network.
about
about      Things to try: read the Routes tab of each router; remove a return route with  :route r3 del 10.0.1.0/24  and see requests arrive but replies die; add a more specific route and see it win over a shorter prefix.
about[it]  Tre router in fila, configurati a mano. Un router inoltra un pacchetto cercando la destinazione nella sua tabella e scegliendo il prefisso più lungo che corrisponde; non sa nulla del resto del percorso. All'inizio r2 non ha una rotta verso la rete di h2: il ping di h1 arriva fino a r2, che risponde "network unreachable". A 6 s la rotta mancante viene aggiunta e il ping passa. Ma anche le risposte hanno bisogno di rotte: ogni router sulla via del ritorno deve conoscere la rete di h1.
about[it]
about[it]  Da provare: leggi la scheda Rotte di ogni router; togli una rotta di ritorno con  :route r3 del 10.0.1.0/24  e osserva le richieste arrivare ma le risposte morire; aggiungi una rotta più specifica e guardala vincere su un prefisso più corto.

node h1 host
node r1 router
node r2 router
node r3 router
node h2 host
link h1 r1 delay=2ms
link r1 r2 delay=10ms
link r2 r3 delay=10ms
link r3 h2 delay=2ms
ip h1 10.0.1.10/24
ip r1:eth0 10.0.1.1/24
ip r1:eth1 10.0.12.1/24
ip r2:eth0 10.0.12.2/24
ip r2:eth1 10.0.23.2/24
ip r3:eth0 10.0.23.3/24
ip r3:eth1 10.0.4.1/24
ip h2 10.0.4.10/24
gw h1 10.0.1.1
gw h2 10.0.4.1
route r1 10.0.4.0/24 via 10.0.12.2
route r1 10.0.23.0/24 via 10.0.12.2
route r3 10.0.1.0/24 via 10.0.23.2
route r3 10.0.12.0/24 via 10.0.23.2
route r2 10.0.1.0/24 via 10.0.12.1

run h1 ping h2 count=2
note 0     r1 knows the way and forwards; r2 has no route for 10.0.4.0/24 and reports "network unreachable".
note[it] 0 r1 conosce la strada e inoltra; r2 non ha una rotta per 10.0.4.0/24 e segnala "network unreachable".
at 6s route r2 add 10.0.4.0/24 via 10.0.23.3
note 6s    The administrator adds the missing route on r2.
note[it] 6s L'amministratore aggiunge la rotta mancante su r2.
at 7s run h1 ping h2 count=3
at 11s run h1 traceroute h2
note 11s   traceroute lists the three routers: each one only knew the next hop.
note[it] 11s traceroute elenca i tre router: ognuno conosceva solo il salto successivo.
duration 15s
expect h1 ping.error >= 1
expect h1 ping.reply == 3
expect h1 traceroute.hops == 4
