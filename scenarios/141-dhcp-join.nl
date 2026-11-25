title     Joining a network with DHCP
title[it] Entrare in una rete con DHCP
level  1
layer  7
topic  dhcp
about     A host arrives with no address and gets everything from a DHCP server: address, mask and gateway. Watch the DISCOVER / OFFER / REQUEST / ACK exchange, all by broadcast, then the host immediately pings across the gateway. The server is also the router.
about[it] Un host arriva senza indirizzo e riceve tutto da un server DHCP: indirizzo, maschera e gateway. Osserva lo scambio DISCOVER / OFFER / REQUEST / ACK, tutto in broadcast, poi l'host fa subito ping oltre il gateway. Il server è anche il router.
node h1 host
node gw router role=DHCP+DNS
node h2 host
link h1 gw delay=3ms
link gw h2 delay=8ms
ip gw:eth0 10.0.1.1/24
ip gw:eth1 10.0.2.1/24
ip h2 10.0.2.10/24
gw h2 gw
run gw dhcp server pool=10.0.1.100-10.0.1.150 lease=60s router=10.0.1.1
at 1s run h1 dhcp client
at 4s run h1 ping h2 count=2
duration 8s
expect h1 dhcp.bound_events >= 1
expect gw dhcp.acks >= 1
expect h1 ping.reply >= 1
