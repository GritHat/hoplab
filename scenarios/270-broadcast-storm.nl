title      A loop of switches without spanning tree: the broadcast storm
title[it]  Un anello di switch senza spanning tree: la tempesta di broadcast
level  2
layer  2
topic  stp
about  Three switches connected in a triangle for redundancy, and NO spanning tree. Ethernet frames
about  have no hop limit and a switch floods every broadcast: the single ARP request sent by h1
about  enters the loop and circulates for ever in both directions. At every lap each switch delivers
about  one more copy to its host and re-learns the address of h1 from a different port.
about
about  Watch: the map never goes quiet after the first packet; the bridge.flood counters of the
about  switches grow without end; in a forwarding table the port of h1 keeps flipping (detail 3).
about  Try: `link s2 s3 down` breaks the loop by hand and the storm stops at once: that is what the
about  spanning tree does automatically. Then open the next scenario (271).
about[it]  Tre switch collegati a triangolo per ridondanza, e NESSUNO spanning tree. I frame Ethernet
about[it]  non hanno limite di hop e uno switch inonda ogni broadcast: l'unica richiesta ARP inviata da h1
about[it]  entra nell'anello e circola per sempre in entrambe le direzioni. A ogni giro ogni switch ne
about[it]  consegna un'altra copia al proprio host e reimpara l'indirizzo di h1 da una porta diversa.
about[it]
about[it]  Osserva: la mappa non si ferma più dopo il primo pacchetto; i contatori bridge.flood degli
about[it]  switch crescono senza fine; nella tabella di inoltro la porta di h1 continua a cambiare (dettaglio 3).
about[it]  Prova: `link s2 s3 down` spezza l'anello a mano e la tempesta cessa subito: è ciò che lo
about[it]  spanning tree fa automaticamente. Poi apri lo scenario successivo (271).
param      delay 5ms Delay of the links between switches (a lap takes three times as long)
param[it]  delay Ritardo dei collegamenti fra switch (un giro dura il triplo)

node s1 switch x=50 y=20
node s2 switch x=25 y=65
node s3 switch x=75 y=65
node h1 host x=50 y=3
node h2 host x=8 y=85
node h3 host x=92 y=85
link s1 s2 delay=$delay
link s2 s3 delay=$delay
link s1 s3 delay=$delay
link h1 s1 delay=2ms
link h2 s2 delay=2ms
link h3 s3 delay=2ms
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24
ip h3 10.0.0.3/24

at 1s run h1 ping h2 count=3

note 1s h1 broadcasts ONE ARP request. s1 floods it to s2 and s3; each of them floods it to the other... and it comes back to s1, which floods it again.
note[it] 1s h1 invia UNA richiesta ARP in broadcast. s1 la inonda verso s2 e s3; ciascuno dei due la inonda verso l'altro... e torna a s1, che la inonda di nuovo.
note 3s The same frame is still going round, clockwise and anticlockwise. Nothing will ever remove it: no TTL at layer 2.
note[it] 3s Lo stesso frame sta ancora girando, in senso orario e antiorario. Nulla lo eliminerà mai: al livello 2 non c'è TTL.
note 6s Each lap the hosts receive another useless copy. With more broadcasts (every ARP adds two eternal frames) the links saturate and the network dies.
note[it] 6s A ogni giro gli host ricevono un'altra copia inutile. Con altri broadcast (ogni ARP aggiunge due frame eterni) i collegamenti si saturano e la rete muore.

duration 10s
# one broadcast, two directions, a lap every 3 x delay, for 9 seconds: every switch floods 6000/delay(ms) copies
at 9990ms run s1 check storm "s1:bridge.flood >= 0.9 * 6s / $delay - 5 && s2:bridge.flood >= 0.9 * 6s / $delay - 5 && s3:bridge.flood >= 0.9 * 6s / $delay - 5"
expect s1 check.storm == 1
