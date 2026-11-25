title     DHCP leases: renewal, expiry and pool exhaustion
title[it] Lease DHCP: rinnovo, scadenza ed esaurimento del pool
level  2
layer  7
topic  dhcp
about     Leases are temporary. With a short lease, watch a bound client reach T1 and renew with the same server, keeping its address. A tiny pool (two addresses) for three clients shows the third getting no offer. Crash the server and see leases survive on the client until they expire.
about[it] I lease sono temporanei. Con un lease breve, osserva un client legato raggiungere T1 e rinnovare con lo stesso server, mantenendo l'indirizzo. Un pool minuscolo (due indirizzi) per tre client mostra il terzo senza offerta. Fai cadere il server e osserva i lease sopravvivere sul client finché non scadono.
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
duration 26s
expect s  dhcp.acks >= 2
expect s  dhcp.exhausted >= 1
expect h1 dhcp.bound_events >= 2
