title      IPv6 never fragments on the way: Packet Too Big and path MTU discovery
title[it]  IPv6 non frammenta lungo il percorso: Packet Too Big e scoperta della MTU
level 2
layer 3
topic ip6
about      In IPv4 a router that meets a link too small for a packet cuts the packet in pieces (lab 201). An IPv6 router never does: it discards the packet and tells the source "Packet Too Big, the next link carries only N bytes". The source remembers N for that destination and from then on fragments its own packets, with a Fragment extension header, before sending them. Here the middle link carries 1280 bytes (the minimum IPv6 allows) and h1 pings with 1400 bytes of data. The first request dies at r1. The second leaves h1 already fragmented and arrives, but the 1400-byte reply dies at r2: the path MTU is learned separately in each direction. From the third on, both ends fragment and the ping works.
about
about      Things to try: in the Packets view open a fragment and find the Fragment header (next header 44) with its offset, M flag and identification. Compare with lab 201, where the router did the cutting. Send  :h1 ping h2 v6=1 size=1200  and see it pass untouched.
about[it]  In IPv4 un router che incontra un collegamento troppo piccolo per un pacchetto lo taglia a pezzi (laboratorio 201). Un router IPv6 non lo fa mai: scarta il pacchetto e dice alla sorgente «Packet Too Big, il prossimo collegamento porta solo N byte». La sorgente ricorda N per quella destinazione e da quel momento frammenta da sé i propri pacchetti, con un'intestazione di estensione Fragment, prima di inviarli. Qui il collegamento centrale porta 1280 byte (il minimo ammesso da IPv6) e h1 fa ping con 1400 byte di dati. La prima richiesta muore su r1. La seconda parte da h1 già frammentata e arriva, ma la risposta da 1400 byte muore su r2: la MTU del percorso si impara separatamente in ciascun verso. Dalla terza in poi entrambi gli estremi frammentano e il ping funziona.
about[it]
about[it]  Da provare: nella vista Pacchetti apri un frammento e trova l'intestazione Fragment (next header 44) con offset, flag M e identificazione. Confronta con il laboratorio 201, dove a tagliare era il router. Invia  :h1 ping h2 v6=1 size=1200  e guardalo passare intatto.
param      mtu 1280 MTU of the middle link (IPv6 requires at least 1280)
param[it]  mtu MTU del collegamento centrale (IPv6 ne richiede almeno 1280)
param      size 1400 Bytes of data in each ping
param[it]  size Byte di dati in ogni ping

node h1 host
node r1 router
node r2 router
node h2 host
link h1 r1 delay=2ms
link r1 r2 delay=10ms mtu=$mtu
link r2 h2 delay=2ms
auto all6

at 2s run h1 ping h2 count=4 size=$size
note 2s    1400 bytes do not fit in 1280: r1 does not fragment, it answers "Packet Too Big, MTU 1280".
note[it] 2s 1400 byte non entrano in 1280: r1 non frammenta, risponde «Packet Too Big, MTU 1280».
note 3s    h1 now fragments at the source and the request arrives; the big reply is refused by r2, which tells h2.
note[it] 3s Ora h1 frammenta alla sorgente e la richiesta arriva; la risposta troppo grande è rifiutata da r2, che avvisa h2.
note 4s    Both ends know the path MTU: fragments in both directions, reassembled only at the destination.
note[it] 4s Entrambi gli estremi conoscono la MTU del percorso: frammenti nei due versi, riassemblati solo a destinazione.
at 7s run h2 echo
at 8s run h1 udp h2 msg=hello size=3000
note 8s    A 3000-byte UDP datagram: three fragments made by h1, none by the routers.
note[it] 8s Un datagramma UDP da 3000 byte: tre frammenti fatti da h1, nessuno dai router.
duration 11s
expect h1 ping.error == 1
expect h1 ping.reply == 2
expect r1 ip6.too_big == 1
expect r2 ip6.too_big == 1
expect h1 udp.reply == 1
