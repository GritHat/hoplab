title     Joining a network with DHCP
title[it] Entrare in una rete con DHCP
level  1
layer  7
topic  dhcp
about     A host arrives with no address and gets everything from a DHCP server: address, mask, gateway and the address of a DNS server. Watch the DISCOVER / OFFER / REQUEST / ACK exchange, all by broadcast, then the host immediately pings across the gateway, and finally looks up a name with the DNS server it was told about. The router gw is also the DHCP and the DNS server.
about[it] Un host arriva senza indirizzo e riceve tutto da un server DHCP: indirizzo, maschera, gateway e l'indirizzo di un server DNS. Osserva lo scambio DISCOVER / OFFER / REQUEST / ACK, tutto in broadcast, poi l'host fa subito ping oltre il gateway, e infine cerca un nome con il server DNS che gli è stato indicato. Il router gw è anche il server DHCP e DNS.
node h1 host
node gw router role=DHCP+DNS
node h2 host
link h1 gw delay=3ms
link gw h2 delay=8ms
ip gw:eth0 10.0.1.1/24
ip gw:eth1 10.0.2.1/24
ip h2 10.0.2.10/24
gw h2 gw
run gw dns zone=lab.example a=h2:h2
run gw dhcp server pool=10.0.1.100-10.0.1.150 lease=60s router=10.0.1.1 dns=10.0.1.1
at 1s run h1 dhcp client
at 4s run h1 ping h2 count=2
at 7s run h1 dig h2.lab.example
note 1s h1 has no address: it can only shout. DISCOVER and REQUEST are broadcasts from 0.0.0.0.
note[it] 1s h1 non ha un indirizzo: può solo gridare. DISCOVER e REQUEST sono broadcast da 0.0.0.0.
note 4s Configured: the ping goes to the gateway learned from DHCP.
note[it] 4s Configurato: il ping va al gateway imparato da DHCP.
note 7s dig asks the DNS server that came in option 6 of the same ACK.
note[it] 7s dig interroga il server DNS arrivato nell'opzione 6 dello stesso ACK.
duration 10s
expect h1 dhcp.bound_events >= 1
expect gw dhcp.acks >= 1
expect h1 ping.reply >= 1
expect h1 dig.answers == 1
