title      Dual stack: IPv4 and IPv6 side by side on the same cables
title[it]  Dual stack: IPv4 e IPv6 fianco a fianco sugli stessi cavi
level 2
layer 3
topic ip6
about      The Internet did not switch from IPv4 to IPv6: it runs both. Every node here has an IPv4 and an IPv6 address on each interface, two routing tables, ARP for one protocol and Neighbor Discovery for the other, and the Ethernet type field (0x0800 or 0x86DD) tells the frames apart. h1 pings h2 twice, once per protocol. Then the IPv4 route on the router is removed: IPv4 stops, IPv6 does not notice. The two networks share the cables and nothing else.
about
about      Things to try: compare the two echo requests in the Packets view, header by header (20 bytes against 40, TTL against hop limit, a header checksum against none). Look at the Routes tab of r1: two tables. Take the middle link down with  :link r1 r2 down  and see both protocols fail together: what they share is the cable.
about[it]  Internet non è passata da IPv4 a IPv6: li usa entrambi. Qui ogni nodo ha un indirizzo IPv4 e uno IPv6 su ogni interfaccia, due tabelle di instradamento, ARP per un protocollo e Neighbor Discovery per l'altro, e il campo tipo di Ethernet (0x0800 o 0x86DD) distingue i frame. h1 fa ping a h2 due volte, una per protocollo. Poi sul router viene tolta la rotta IPv4: IPv4 si ferma, IPv6 non se ne accorge. Le due reti condividono i cavi e nient'altro.
about[it]
about[it]  Da provare: confronta le due echo request nella vista Pacchetti, intestazione per intestazione (20 byte contro 40, TTL contro hop limit, un checksum dell'intestazione contro nessuno). Guarda la scheda Rotte di r1: due tabelle. Abbatti il collegamento centrale con  :link r1 r2 down  e osserva i due protocolli fallire insieme: ciò che condividono è il cavo.
param      delay 5ms Delay of each link
param[it]  delay Ritardo di ogni collegamento

node h1 host
node r1 router
node r2 router
node h2 host
link h1 r1 delay=$delay
link r1 r2 delay=$delay
link r2 h2 delay=$delay
auto all
auto all6

at 2s run h1 ping h2 count=2
note 2s    IPv4: ARP finds the gateway, the echo request travels in frames of type 0x0800.
note[it] 2s IPv4: ARP trova il gateway, la echo request viaggia in frame di tipo 0x0800.
at 5s run h1 ping h2 count=2 v6=1
note 5s    IPv6: Neighbor Discovery finds the gateway, the echo request travels in frames of type 0x86DD.
note[it] 5s IPv6: Neighbor Discovery trova il gateway, la echo request viaggia in frame di tipo 0x86DD.
at 8s route r1 del 10.0.3.0/24
note 8s    r1 loses its IPv4 route to h2's network. Only IPv4 is affected.
note[it] 8s r1 perde la rotta IPv4 verso la rete di h2. Solo IPv4 ne risente.
at 9s run h1 ping h2 count=2
at 12s run h1 ping h2 count=2 v6=1
duration 15s
expect h1 ping.reply == 6
expect h1 ping.error == 2
expect r1 ip.forwarded >= 2
expect r1 ip6.forwarded >= 4
