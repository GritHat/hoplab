title      TCP and HTTP over IPv6, and how TCP finds the path MTU
title[it]  TCP e HTTP su IPv6, e come TCP scopre la MTU del percorso
level 2
layer 4
topic ip6
about      TCP does not care which network layer carries it. The same web page is fetched from www twice, over IPv4 and over IPv6: same handshake, same sequence numbers, same HTTP. What changes is underneath: the checksum covers a different pseudo-header, and the 40-byte IPv6 header leaves 1440 bytes for the segment where IPv4 leaves 1460.
about
about      The middle link carries smaller packets than the others. The MSS announced in the SYN only reflects each end's own link, so www starts with segments that do not fit. Its IPv4 segments carry the don't-fragment flag and IPv6 routers never fragment: r2 discards the first big segment and reports its MTU ("fragmentation needed", "Packet Too Big"). TCP lowers the MSS and sends the data again in smaller segments. This is path MTU discovery. The last two transfers show the alternatives: with pmtud=0 the IPv4 segments are fragmented by the router, and the last IPv6 transfer never sends a segment that is too big, because www remembers the path MTU and announces a smaller MSS in its SYN+ACK.
about
about      Things to try: in the Packets view compare a SYN over IPv4 with one over IPv6 (the MSS option), and find the ICMP error that carries the MTU. Open the tcp tab of www and watch the MSS change. Set mtu=1500 and nothing is discovered because nothing is too big. IPv6 requires every link to carry 1280 bytes, so do not go below.
about[it]  A TCP non importa quale livello di rete lo trasporti. La stessa pagina web viene scaricata da www due volte, su IPv4 e su IPv6: stesso handshake, stessi numeri di sequenza, stesso HTTP. Ciò che cambia sta sotto: il checksum copre uno pseudo-header diverso, e l'intestazione IPv6 da 40 byte lascia 1440 byte al segmento dove IPv4 ne lascia 1460.
about[it]
about[it]  Il collegamento centrale trasporta pacchetti più piccoli degli altri. L'MSS annunciato nel SYN riflette solo il collegamento di ciascun estremo, quindi www parte con segmenti che non passano. I suoi segmenti IPv4 portano il flag don't-fragment e i router IPv6 non frammentano mai: r2 scarta il primo segmento grande e riferisce la propria MTU («fragmentation needed», «Packet Too Big»). TCP abbassa l'MSS e rimanda i dati in segmenti più piccoli. È la scoperta della MTU del percorso. Gli ultimi due trasferimenti mostrano le alternative: con pmtud=0 i segmenti IPv4 vengono frammentati dal router, e l'ultimo trasferimento IPv6 non invia mai un segmento troppo grande, perché www ricorda la MTU del percorso e annuncia un MSS più piccolo nel suo SYN+ACK.
about[it]
about[it]  Da provare: nella vista Pacchetti confronta un SYN su IPv4 con uno su IPv6 (l'opzione MSS), e trova l'errore ICMP che porta la MTU. Apri la scheda tcp di www e guarda l'MSS cambiare. Con mtu=1500 non si scopre nulla perché nulla è troppo grande. IPv6 richiede che ogni collegamento trasporti 1280 byte: non scendere sotto.
param      mtu 1280 MTU of the middle link (1280 to 1500)
param[it]  mtu MTU del collegamento centrale (da 1280 a 1500)
param+     delay 5ms Delay of each link
param[it]  delay Ritardo di ogni collegamento

node h1 host
node r1 router
node r2 router
node www host
link h1 r1 delay=$delay
link r1 r2 delay=$delay mtu=$mtu
link r2 www delay=$delay
auto all
auto all6

run www http server objects=2 objsize=4000
run www tcp sink
at 2s run h1 http get www/
note 2s    The page over IPv4. www's first full segment (DF set) does not fit the middle link: r2 answers "fragmentation needed" and www lowers its MSS.
note[it] 2s La pagina su IPv4. Il primo segmento pieno di www (con DF) non passa sul collegamento centrale: r2 risponde «fragmentation needed» e www abbassa l'MSS.
at 6s run h1 http get www/ v6=1
note 6s    The same page over IPv6: same TCP, another pseudo-header, MSS 1440. r2 answers "Packet Too Big" and the MSS drops again.
note[it] 6s La stessa pagina su IPv6: stesso TCP, un altro pseudo-header, MSS 1440. r2 risponde «Packet Too Big» e l'MSS scende di nuovo.
at 10s run h1 tcp send www bytes=30k pmtud=0
note 10s   IPv4 without path MTU discovery: no DF, so r1 cuts every big segment into fragments and www reassembles them.
note[it] 10s IPv4 senza scoperta della MTU: niente DF, quindi r1 taglia ogni segmento grande in frammenti e www li riassembla.
at 14s run h1 tcp send www bytes=30k v6=1
note 14s   IPv6 again, upload this time: www remembers the path MTU and announces a small MSS in its SYN+ACK, so nothing is refused and no router fragments.
note[it] 14s Di nuovo IPv6, stavolta in salita: www ricorda la MTU del percorso e annuncia un MSS piccolo nel SYN+ACK, quindi nulla viene rifiutato e nessun router frammenta.
duration 18s
expect h1 http.pages == 2
expect h1 tcp.send_done == 2
expect www tcp.sink_done == 2
expect www tcp.pmtu_reductions == 2 if $mtu != 1500
expect www tcp.pmtu_reductions == 0 if $mtu == 1500
expect h1 tcp.retransmits == 0
expect www ip.reassembled >= 1 if $mtu != 1500
expect r2 ip6.too_big >= 1 if $mtu != 1500
