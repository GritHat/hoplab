title     DHCP leases: renewal, expiry and pool exhaustion
title[it] Lease DHCP: rinnovo, scadenza ed esaurimento del pool
level  2
layer  7
topic  dhcp
about     Leases are temporary. With a short lease, watch a bound client reach T1 and renew with the same server, keeping its address. A tiny pool (two addresses) for three clients shows the third getting no offer. At 14 s the server crashes for 26 seconds: the clients keep their addresses, cannot renew them, and must give them up when the lease runs out (with a lease longer than the outage they never notice). When the server returns it still has its lease table (it is on disk), and whoever asks first gets the two addresses: maybe not the same two hosts as before. Try lease=5, 30, 60.
about[it] I lease sono temporanei. Con un lease breve, osserva un client legato raggiungere T1 e rinnovare con lo stesso server, mantenendo l'indirizzo. Un pool minuscolo (due indirizzi) per tre client mostra il terzo senza offerta. A 14 s il server va in crash per 26 secondi: i client tengono i loro indirizzi, non riescono a rinnovarli, e devono abbandonarli quando il lease scade (con un lease più lungo dell'interruzione non se ne accorgono mai). Quando il server torna ha ancora la sua tabella dei lease (è su disco), e chi chiede per primo ottiene i due indirizzi: forse non gli stessi due host di prima. Prova lease=5, 30, 60.
param  lease 10 Lease time in seconds
param[it] lease Durata del lease, in secondi
node s  router role=DHCP
node h1 host
node h2 host
node h3 host
node sw switch
link s  sw delay=1ms
link h1 sw delay=1ms
link h2 sw delay=1ms
link h3 sw delay=1ms
ip s:eth0 10.0.5.1/24
run s dhcp server pool=10.0.5.100-10.0.5.101 lease=${lease}s router=10.0.5.1
at 1s run h1 dhcp client
at 2s run h2 dhcp client
at 3s run h3 dhcp client
note 1s h1 and h2 take the two addresses of the pool; h3 arrives third and gets no offer.
note[it] 1s h1 e h2 prendono i due indirizzi del pool; h3 arriva terzo e non riceve offerte.
note 6s At T1 (half the lease) a client renews by unicast with its server and keeps the address.
note[it] 6s A T1 (metà del lease) un client rinnova in unicast con il suo server e tiene l'indirizzo.
at 13900ms run s check before "h1:dhcp.bound + h2:dhcp.bound == 2 && h3:dhcp.bound == 0 && s:dhcp.exhausted >= 1 && s:dhcp.leases == 2 && ($lease > 20 || h1:dhcp.bound_events >= 2)"
at 14s node s down
note 14s The server crashes. Nothing changes for the clients until they try to renew: T1 by unicast, T2 by broadcast, then the lease expires and the address must be dropped.
note[it] 14s Il server va in crash. Per i client non cambia nulla finché non provano a rinnovare: a T1 in unicast, a T2 in broadcast, poi il lease scade e l'indirizzo va abbandonato.
at 39s run h1 check outage "($lease <= 20 && h1:dhcp.bound + h2:dhcp.bound == 0) || ($lease >= 50 && h1:dhcp.bound + h2:dhcp.bound == 2) || ($lease > 20 && $lease < 50)"
at 40s node s up
note 40s The server is back with its lease table. Clients without an address are still asking: the first two to be heard get the pool.
note[it] 40s Il server è tornato con la sua tabella dei lease. I client senza indirizzo stanno ancora chiedendo: i primi due a farsi sentire ottengono il pool.
at 57s run s check after "h1:dhcp.bound + h2:dhcp.bound + h3:dhcp.bound == 2"
duration 58s
expect s check.before == 1
expect h1 check.outage == 1
expect s check.after == 1
