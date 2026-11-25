title      An IPv6-only Internet: BGP over IPv6, a sub-prefix hijack and origin validation
title[it]  Una Internet solo IPv6: BGP su IPv6, dirottamento di un sotto-prefisso e validazione dell'origine
level  4
layer  3
topic  bgp
about      No IPv4 anywhere: four autonomous systems run their BGP sessions over IPv6 and exchange only IPv6 prefixes (the routers still need a 32-bit BGP identifier, given here with id=). AS 65001 owns 2001:db8:1::/64, where the server lives. At 15 s AS 65066 announces the same /64: an exact-prefix hijack, which wins only where its AS path is shorter (obs is fooled, client is not). At 30 s it announces 2001:db8:1::/65, a more specific prefix that contains the server's address: forwarding follows the longest match, so everybody is fooled, the legitimate announcement notwithstanding.
about      Nothing in the mechanism is new with respect to IPv4, and that is the point: the multiprotocol extensions changed the address family, not the trust model. The defence is the same too. With rov=1 every router checks announcements against a signed authorization (here: 2001:db8:1::/64, origin AS 65001, maximum length 64) and rejects the forged ones: wrong origin for the /64, too specific for the /65.
about      Compare with the IPv4 lab 431. Then think about why the IPv6 address space makes sub-prefix hijacks easier to hide: between a /32 allocation and a /64 subnet there are 32 bits of more specific prefixes to choose from, and the maximum length in an authorization is what closes them.
about[it]  Nessun IPv4 da nessuna parte: quattro sistemi autonomi fanno viaggiare le sessioni BGP su IPv6 e scambiano solo prefissi IPv6 (i router hanno comunque bisogno di un identificatore BGP a 32 bit, dato qui con id=). L'AS 65001 possiede 2001:db8:1::/64, dove vive il server. A 15 s l'AS 65066 annuncia lo stesso /64: un dirottamento a prefisso esatto, che vince solo dove il suo AS path è più corto (obs viene ingannato, client no). A 30 s annuncia 2001:db8:1::/65, un prefisso più specifico che contiene l'indirizzo del server: l'inoltro segue la corrispondenza più lunga, quindi tutti vengono ingannati, nonostante l'annuncio legittimo.
about[it]  Nel meccanismo non c'è nulla di nuovo rispetto a IPv4, ed è proprio questo il punto: le estensioni multiprotocollo hanno cambiato la famiglia di indirizzi, non il modello di fiducia. Anche la difesa è la stessa. Con rov=1 ogni router confronta gli annunci con un'autorizzazione firmata (qui: 2001:db8:1::/64, AS di origine 65001, lunghezza massima 64) e rifiuta quelli falsi: origine sbagliata per il /64, troppo specifico per il /65.
about[it]  Confronta con il laboratorio IPv4 431. Poi rifletti sul perché lo spazio di indirizzi IPv6 rende i dirottamenti di sotto-prefissi più facili da nascondere: fra un'allocazione /32 e una sottorete /64 ci sono 32 bit di prefissi più specifici fra cui scegliere, ed è la lunghezza massima nell'autorizzazione a chiuderli.
param      rov 0 Route origin validation on every router (1 = on)
param[it]  rov Validazione dell'origine su ogni router (1 = attiva)
param+     keepalive 3s BGP keepalive interval
param+[it] keepalive Intervallo di keepalive BGP

node srv    host   x=5  y=75 role=server
node r1     router x=15 y=35 role=AS65001
node r2     router x=40 y=35 role=AS65002
node r3     router x=65 y=35 role=AS65003
node evil   router x=90 y=35 role=AS65066
node client host   x=40 y=80
node obs    host   x=65 y=80
node fake   host   x=95 y=75 role=fake
link srv r1 delay=1ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r3 evil delay=5ms
link r2 client delay=1ms
link r3 obs delay=1ms
link evil fake delay=1ms
ip r1:eth0 2001:db8:1::1/64
ip srv 2001:db8:1::10/64
ip r1:eth1 2001:db8:12::1/64
ip r2:eth0 2001:db8:12::2/64
ip r2:eth1 2001:db8:23::2/64
ip r3:eth0 2001:db8:23::3/64
ip r3:eth1 2001:db8:36::3/64
ip evil:eth0 2001:db8:36::6/64
ip r2:eth2 2001:db8:2::1/64
ip client 2001:db8:2::10/64
ip r3:eth2 2001:db8:3::1/64
ip obs 2001:db8:3::10/64
ip evil:eth1 2001:db8:1::1/64
ip fake 2001:db8:1::10/64
gw srv 2001:db8:1::1
gw client 2001:db8:2::1
gw obs 2001:db8:3::1
gw fake 2001:db8:1::1

run r1   bgp as=65001 id=1.1.1.1 keepalive=$keepalive network=2001:db8:1::/64 neighbor=r2:65002 rov=$rov roa=2001:db8:1::/64:65001:64
run r2   bgp as=65002 id=2.2.2.2 keepalive=$keepalive network=2001:db8:2::/64 neighbor=r1:65001 neighbor=r3:65003 rov=$rov roa=2001:db8:1::/64:65001:64
run r3   bgp as=65003 id=3.3.3.3 keepalive=$keepalive network=2001:db8:3::/64 neighbor=r2:65002 neighbor=evil:65066 rov=$rov roa=2001:db8:1::/64:65001:64
run evil bgp as=65066 id=6.6.6.6 keepalive=$keepalive neighbor=r3:65003

note 0     Sessions come up over IPv6 (TCP port 179 between the routers' global addresses) and carry only the IPv6 family.
note[it] 0 Le sessioni si stabiliscono su IPv6 (porta TCP 179 fra gli indirizzi globali dei router) e trasportano solo la famiglia IPv6.
at 8s  run client ping 2001:db8:1::10 count=2
at 8s  run obs ping 2001:db8:1::10 count=2
note 8s    Before the attack: client and obs both reach the real server in AS 65001.
note[it] 8s Prima dell'attacco: client e obs raggiungono entrambi il vero server nell'AS 65001.
at 15s run evil bgp announce 2001:db8:1::/64
note 15s   AS 65066 announces the victim's /64. For r3 the forged path (65066) is shorter than the real one (65002 65001): obs now talks to the fake.
note[it] 15s L'AS 65066 annuncia il /64 della vittima. Per r3 il percorso falso (65066) è più corto di quello vero (65002 65001): obs ora parla con il falso.
at 20s run client ping 2001:db8:1::10 count=2
at 20s run obs ping 2001:db8:1::10 count=2
at 30s run evil bgp announce 2001:db8:1::/65
note 30s   A more specific /65: no AS path comparison can help now, the longest match wins everywhere.
note[it] 30s Un /65 più specifico: nessun confronto di AS path può aiutare ora, la corrispondenza più lunga vince ovunque.
at 36s run client ping 2001:db8:1::10 count=2
at 36s run obs ping 2001:db8:1::10 count=2
duration 42s
expect client ping.reply == 6
expect obs ping.reply == 6
expect r2 bgp.routes == 0
expect srv icmp6.echo_served == 6 if $rov == 0
expect fake icmp6.echo_served == 6 if $rov == 0
expect srv icmp6.echo_served == 12 if $rov == 1
expect fake icmp6.echo_served == 0 if $rov == 1
expect r3 bgp.rov_rejected >= 2 if $rov == 1
