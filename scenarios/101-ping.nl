title      Two networks and a router: ping, traceroute, fragmentation
title[it]  Due reti e un router: ping, traceroute, frammentazione
level 1
layer 3
topic ip
about      h1 and h2 live on different networks, joined by the router r1. Every packet from h1 to h2 is sent to the default gateway, which forwards it and decrements its TTL. Three experiments follow each other: a ping, a traceroute that discovers the router by sending packets with a small TTL, and a 3000-byte UDP datagram that does not fit in one 1500-byte frame and is fragmented.
about      Things to try: select r1 (n) and read its routing table; watch the Sequence view (s); raise the loss parameter and see ping report the missing replies.
about[it]  h1 e h2 stanno su reti diverse, unite dal router r1. Ogni pacchetto da h1 a h2 viene inviato al gateway predefinito, che lo inoltra e ne decrementa il TTL. Seguono tre esperimenti: un ping, un traceroute che scopre il router inviando pacchetti con TTL piccolo, e un datagramma UDP da 3000 byte che non entra in un frame da 1500 byte e viene frammentato.
about[it]  Da provare: seleziona r1 (n) e leggi la sua tabella di instradamento; guarda la vista Sequenza (s); aumenta il parametro di perdita e osserva il ping segnalare le risposte mancanti.
param      loss 0 Packet loss on the h1 link (0..1)
param[it]  loss Perdita di pacchetti sul collegamento di h1 (0..1)

node h1 host
node r1 router
node h2 host
link h1 r1 delay=5ms loss=$loss
link r1 h2 delay=10ms
auto all

run h2 echo
run h1 ping h2 count=3
note 0     ping: an ICMP echo request crosses the router and the reply comes back. The first one waits for ARP twice.
note[it] 0 ping: una richiesta ICMP echo attraversa il router e la risposta torna indietro. La prima attende ARP due volte.
at 5s run h1 traceroute h2
note 5s    traceroute: TTL 1 dies at r1, which reveals itself with "time exceeded"; TTL 2 reaches h2.
note[it] 5s traceroute: il TTL 1 muore su r1, che si rivela con "time exceeded"; il TTL 2 raggiunge h2.
at 8s run h1 udp h2 msg=hello size=3000
note 8s    A 3000-byte datagram over a 1500-byte MTU: three fragments, reassembled only at the destination.
note[it] 8s Un datagramma da 3000 byte su una MTU di 1500: tre frammenti, riassemblati solo a destinazione.
duration 12s
expect h1 ping.reply == 3
expect h1 traceroute.hops == 2
expect h1 udp.reply == 1
