title      IPv6, first contact: link-local addresses and Neighbor Discovery
title[it]  IPv6, primo contatto: indirizzi link-local e Neighbor Discovery
level 1
layer 3
topic ip6
about      The same two networks and router as the ping lab, this time with IPv6. Three things are new. Every interface gives itself a link-local address (fe80::...) built from its MAC address, before anybody configures anything. Before using any address a node asks the link "is anybody already using this?" (duplicate address detection). And there is no ARP: to find the MAC address of a neighbor a node sends a Neighbor Solicitation, an ICMPv6 message, to a multicast group that only a few nodes listen to, instead of shouting to everybody.
about
about      Things to try: open the Node tab at detail 2 to see the addresses of each interface, and the ARP tab for the neighbor cache with its states. In the Packets view open a Neighbor Solicitation: destination ff02::1:ff..., MAC 33:33:ff:.... Ping the router's link-local address with  :h1 ping fe80::...  (copy it from r1's Node tab).
about[it]  Le stesse due reti e lo stesso router del laboratorio sul ping, questa volta con IPv6. Tre cose sono nuove. Ogni interfaccia si assegna da sola un indirizzo link-local (fe80::...) costruito dal proprio indirizzo MAC, prima che qualcuno configuri qualcosa. Prima di usare un indirizzo un nodo chiede al collegamento «qualcuno lo sta già usando?» (duplicate address detection). E non c'è ARP: per trovare l'indirizzo MAC di un vicino un nodo invia una Neighbor Solicitation, un messaggio ICMPv6, a un gruppo multicast ascoltato solo da pochi nodi, invece di gridare a tutti.
about[it]
about[it]  Da provare: apri la scheda Nodo a dettaglio 2 per vedere gli indirizzi di ogni interfaccia, e la scheda ARP per la cache dei vicini con i suoi stati. Nella vista Pacchetti apri una Neighbor Solicitation: destinazione ff02::1:ff..., MAC 33:33:ff:.... Fai ping all'indirizzo link-local del router con  :h1 ping fe80::...  (copialo dalla scheda Nodo di r1).
param      loss 0 Packet loss on the h1 link (0..1)
param[it]  loss Perdita di pacchetti sul collegamento di h1 (0..1)

node h1 host
node r1 router
node h2 host
link h1 r1 delay=5ms loss=$loss
link r1 h2 delay=10ms
auto all6

note 0     Every interface checks its link-local and its global address: a Neighbor Solicitation from :: for each (DAD).
note[it] 0 Ogni interfaccia verifica il proprio indirizzo link-local e quello globale: una Neighbor Solicitation da :: per ciascuno (DAD).
at 2s run h1 ping h2 count=3
note 2s    ping over IPv6: h1 asks "who has" its gateway with a Neighbor Solicitation, the router does the same for h2.
note[it] 2s ping su IPv6: h1 chiede «chi ha» il suo gateway con una Neighbor Solicitation, il router fa lo stesso per h2.
at 6s run h1 traceroute h2
note 6s    traceroute: hop limit 1 dies at r1, which answers with ICMPv6 "time exceeded".
note[it] 6s traceroute: l'hop limit 1 muore su r1, che risponde con ICMPv6 «time exceeded».
at 8s run h2 echo
at 9s run h1 udp h2 msg=hello
note 9s    A UDP datagram over IPv6: same ports, checksum now mandatory.
note[it] 9s Un datagramma UDP su IPv6: stesse porte, checksum ora obbligatorio.
duration 12s
expect h1 ping.reply == 3
expect h1 traceroute.hops == 2
expect h1 udp.reply == 1
expect h1 nd.dad_failed == 0
