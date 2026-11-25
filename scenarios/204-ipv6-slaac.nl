title      IPv6 autoconfiguration: router advertisements and SLAAC
title[it]  Autoconfigurazione IPv6: router advertisement e SLAAC
level 2
layer 3
topic ip6
about      Nobody configures the two hosts of this lab, and there is no DHCP server. Each host gives itself a link-local address, asks "is there a router?" (Router Solicitation), and the router answers with a Router Advertisement that carries the prefix of the link. The host glues the prefix to its own interface identifier, checks that the result is unique (DAD), and takes the router's link-local address as default gateway. That is stateless address autoconfiguration. At 10 s h1 is switched off and on again: it has forgotten everything, and configures itself once more.
about
about      Things to try: watch the Node and Routes tabs of h1 fill in during the first two seconds (the default route is marked "ra"). Open a Router Advertisement in the Packets view and read the prefix option. Stop the advertisements with  :stop r1 ra  and restart h1 (k): it keeps only its link-local address and cannot leave the link.
about[it]  Nessuno configura i due host di questo laboratorio, e non c'è un server DHCP. Ogni host si assegna un indirizzo link-local, chiede «c'è un router?» (Router Solicitation), e il router risponde con un Router Advertisement che porta il prefisso del collegamento. L'host unisce il prefisso al proprio identificatore di interfaccia, verifica che il risultato sia unico (DAD) e prende l'indirizzo link-local del router come gateway predefinito. Questa è l'autoconfigurazione senza stato. A 10 s h1 viene spento e riacceso: ha dimenticato tutto, e si configura di nuovo.
about[it]
about[it]  Da provare: osserva le schede Nodo e Rotte di h1 riempirsi nei primi due secondi (la rotta predefinita è marcata «ra»). Apri un Router Advertisement nella vista Pacchetti e leggi l'opzione del prefisso. Ferma gli annunci con  :stop r1 ra  e riavvia h1 (k): conserva solo l'indirizzo link-local e non può uscire dal collegamento.
param      interval 5s Interval between unsolicited router advertisements
param[it]  interval Intervallo fra i router advertisement non sollecitati
param+     lifetime 60s Router lifetime announced to the hosts
param[it]  lifetime Durata del router annunciata agli host

node h1 host
node r1 router
node r2 router
node h2 host
link h1 r1 delay=3ms
link r1 r2 delay=10ms
link r2 h2 delay=3ms
ip r1:eth0 2001:db8:a::1/64
ip r1:eth1 2001:db8:12::1/64
ip r2:eth0 2001:db8:12::2/64
ip r2:eth1 2001:db8:b::1/64
route r1 2001:db8:b::/64 via 2001:db8:12::2
route r2 2001:db8:a::/64 via 2001:db8:12::1
run r1 ra interval=$interval lifetime=$lifetime
run r2 ra interval=$interval lifetime=$lifetime

note 0     Link-local addresses first: each interface builds fe80:: + its identifier and checks it (DAD).
note[it] 0 Prima gli indirizzi link-local: ogni interfaccia costruisce fe80:: + il proprio identificatore e lo verifica (DAD).
note 1s    The hosts ask for routers; the advertisement brings the prefix: address = prefix + identifier, then DAD again.
note[it] 1s Gli host cercano i router; l'annuncio porta il prefisso: indirizzo = prefisso + identificatore, poi di nuovo DAD.
at 4s run h1 ping h2 count=3 v6=1
note 4s    Both hosts are configured: ping from one autoconfigured address to the other.
note[it] 4s Entrambi gli host sono configurati: ping da un indirizzo autoconfigurato all'altro.
at 8s run h1 traceroute h2 v6=1
at 10s node h1 down
at 11s node h1 up
note 11s   h1 restarts with nothing: link-local, DAD, solicitation, advertisement, address. No server remembered it.
note[it] 11s h1 riparte da zero: link-local, DAD, solicitation, advertisement, indirizzo. Nessun server lo ricordava.
at 15s run h1 ping h2 count=2 v6=1
duration 19s
expect h1 slaac.addresses == 2
expect h2 slaac.addresses == 1
expect h1 ping.reply == 5
expect h1 traceroute.hops == 3
