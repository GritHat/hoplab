title      Two hosts and a cable: frames, addresses and ARP
title[it]  Due host e un cavo: frame, indirizzi e ARP
level 1
layer 2
topic link
about      The smallest network there is. h1 wants to send an IP packet to h2, but a cable carries frames addressed by MAC address, and h1 only knows the IP address. Watch ARP ask "who has 10.0.0.2?" in broadcast, the answer come back, and only then the ping go out. The second ping finds the answer in the ARP cache and leaves at once.
about
about      Things to try: open the Packets view (p) and press Enter on a frame to see every header field and the bytes that carry it. Open the ARP tab of h1. Type  :link h1 h2 down  and ping again.
about[it]  La rete più piccola che esista. h1 vuole inviare un pacchetto IP a h2, ma un cavo trasporta frame indirizzati con l'indirizzo MAC, e h1 conosce solo l'indirizzo IP. Osserva ARP chiedere in broadcast "chi ha 10.0.0.2?", la risposta tornare indietro, e solo allora partire il ping. Il secondo ping trova la risposta nella cache ARP e parte subito.
about[it]
about[it]  Da provare: apri la vista Pacchetti (p) e premi Invio su un frame per vedere ogni campo delle intestazioni e i byte che lo trasportano. Apri la scheda ARP di h1. Scrivi  :link h1 h2 down  e riprova il ping.
param      delay 20ms Propagation delay of the cable
param[it]  delay Ritardo di propagazione del cavo
param+     bw 10M Bandwidth
param[it]  bw Larghezza di banda

node h1 host x=15 y=50
node h2 host x=85 y=50
link h1 h2 delay=$delay bw=$bw
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24

run h1 ping h2 count=2
note 0     h1 has an IP packet for 10.0.0.2 but no MAC address for it: it broadcasts an ARP request.
note[it] 0 h1 ha un pacchetto IP per 10.0.0.2 ma non il suo indirizzo MAC: invia in broadcast una richiesta ARP.
note 1s    Second ping: the address is in the ARP cache, no question needed.
note[it] 1s Secondo ping: l'indirizzo è nella cache ARP, non serve chiedere.
at 3s run h2 ping h1 count=1
note 3s    h2 pings back: it learned h1's MAC address from the ARP request it answered.
note[it] 3s h2 risponde con un suo ping: ha imparato il MAC di h1 dalla richiesta ARP a cui ha risposto.
duration 5s
expect h1 ping.reply == 2
expect h2 ping.reply == 1
