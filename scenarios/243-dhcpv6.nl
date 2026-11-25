title      IPv6 autoconfiguration: SLAAC, stateless DHCPv6, stateful DHCPv6
title[it]  Autoconfigurazione IPv6: SLAAC, DHCPv6 stateless, DHCPv6 stateful
level 2
layer 7
topic dhcp6
about      An IPv6 host can get its address in three ways, and the router decides which, with two flags of its router advertisements. With mode=slaac (no flag) the hosts build their own address from the advertised prefix and nobody tells them a DNS server: watch the name lookup at 8 s fail for lack of one. With mode=stateless (the O flag) they still build their own address, but ask a DHCPv6 server for the other settings with INFORMATION-REQUEST and learn the DNS server. With mode=stateful (the M flag, and prefixes no longer marked autonomous) the address itself is leased by the server: SOLICIT, ADVERTISE, REQUEST, REPLY, then RENEW when half the lifetime has passed. In all three the default route comes from the router advertisement: DHCPv6 has no way to say it.
about
about      The DHCPv6 server d needs no address configured by hand: clients reach it from their link-local address through the multicast group ff02::1:2, and it answers their link-local address. Things to try: open the dhcp6 tab of h1 and of d; open a SOLICIT in the Packets view and find the DUID and the IA_NA; set lease to 6s and count the renewals; type  :node d down  in stateful mode and watch RENEW go unanswered, then REBIND, then the address expire; type  :h1 dhcp6 release .
about[it]  Un host IPv6 può ottenere il suo indirizzo in tre modi, e a decidere quale è il router, con due flag dei suoi router advertisement. Con mode=slaac (nessun flag) gli host costruiscono da soli l'indirizzo dal prefisso annunciato e nessuno comunica loro un server DNS: osserva la ricerca del nome a 8 s fallire perché manca. Con mode=stateless (flag O) costruiscono ancora da soli l'indirizzo, ma chiedono a un server DHCPv6 le altre impostazioni con INFORMATION-REQUEST e imparano il server DNS. Con mode=stateful (flag M, e prefissi non più marcati come autonomi) l'indirizzo stesso è dato in prestito dal server: SOLICIT, ADVERTISE, REQUEST, REPLY, poi RENEW a metà della durata. In tutti e tre i casi la rotta predefinita viene dal router advertisement: DHCPv6 non ha modo di indicarla.
about[it]
about[it]  Il server DHCPv6 d non ha bisogno di alcun indirizzo configurato a mano: i client lo raggiungono dal loro indirizzo link-local tramite il gruppo multicast ff02::1:2, e lui risponde al loro indirizzo link-local. Da provare: apri la scheda dhcp6 di h1 e di d; apri un SOLICIT nella vista Pacchetti e trova il DUID e l'IA_NA; imposta lease a 6s e conta i rinnovi; scrivi  :node d down  in modalità stateful e osserva RENEW restare senza risposta, poi REBIND, poi l'indirizzo scadere; scrivi  :h1 dhcp6 release .
param      mode stateful How hosts are configured: slaac, stateless or stateful
param[it]  mode Come si configurano gli host: slaac, stateless o stateful
param      lease 10s Preferred lifetime of a leased address (renewal at half of it)
param[it]  lease Durata preferita di un indirizzo in prestito (rinnovo a metà)
param+     rapid 0 Rapid commit: two messages instead of four (1 = on)
param[it]  rapid Rapid commit: due messaggi invece di quattro (1 = attivo)

node h1 host
node h2 host
node s1 switch
node d host role=DHCPv6
node r1 router
node srv host role=DNS+www
link h1 s1 delay=2ms
link h2 s1 delay=2ms
link d s1 delay=2ms
link s1 r1 delay=2ms
link r1 srv delay=8ms
ipv6 on
ip r1:eth0 2001:db8:1::1/64
ip r1:eth1 2001:db8:2::1/64
ip srv 2001:db8:2::53/64

run r1 ra interval=4s mode=$mode
run d dhcp6 server pool=2001:db8:1::1000-2001:db8:1::100f lease=$lease dns=2001:db8:2::53 rapid=$rapid
run srv dns zone=example.com aaaa=www:2001:db8:2::53 ttl=60
run h1 dhcp6 client rapid=$rapid
run h2 dhcp6 client rapid=$rapid

note 1s    Each host has checked its link-local address and asks "is there a router?". The answer carries the prefix and the M and O flags.
note[it] 1s Ogni host ha verificato il suo indirizzo link-local e chiede "c'è un router?". La risposta porta il prefisso e i flag M e O.
at 4s run h1 ping 2001:db8:2::53 count=2
note 4s    Whatever the mode, h1 has an address and a default route by now: a ping by address works.
note[it] 4s Qualunque sia la modalità, h1 ha ormai un indirizzo e una rotta predefinita: un ping per indirizzo funziona.
at 8s run h1 dig www.example.com type=AAAA then="ping count=2"
note 8s    A lookup by name needs a DNS server: h1 knows one only if DHCPv6 told it (stateless or stateful).
note[it] 8s Una ricerca per nome richiede un server DNS: h1 ne conosce uno solo se gliel'ha detto DHCPv6 (stateless o stateful).
note 14s   In stateful mode the leases are renewed at T1, half the preferred lifetime: watch RENEW and REPLY.
note[it] 14s In modalità stateful i prestiti vengono rinnovati a T1, metà della durata preferita: osserva RENEW e REPLY.
duration 24s
expect h1 ping.reply == 4 if $mode != slaac
expect h1 ping.reply == 2 if $mode == slaac
expect h1 dig.aaaa == 1 if $mode != slaac
expect h1 dhcp6.unused == 1 if $mode == slaac
expect h1 dhcp6.info == 1 if $mode == stateless
expect h2 dhcp6.info == 1 if $mode == stateless
expect h1 slaac.addresses == 1 if $mode != stateful
expect h1 slaac.addresses == 0 if $mode == stateful
expect h1 dhcp6.bound == 1 if $mode == stateful
expect h2 dhcp6.bound == 1 if $mode == stateful
expect h1 dhcp6.bound == 0 if $mode != stateful
expect d dhcp6.leases == 2 if $mode == stateful
expect d dhcp6.leases == 0 if $mode != stateful
