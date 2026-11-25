title      BGP: autonomous systems, AS paths and withdrawals
title[it]  BGP: sistemi autonomi, AS path e ritiri
level  3
layer  3
topic  bgp
about  Three autonomous systems connected in a triangle, one border router and one LAN each
about  (10.1.0.0/24 in AS 65001, and so on). Each router opens a BGP session with its two neighbors:
about  a TCP connection to port 179, then OPEN and KEEPALIVE (watch the Packets view; when both
about  routers open a connection at the same moment, one of the two is closed again). Then each
about  announces its own prefix; a router that passes a route on prepends its AS number, so every
about  route carries the list of ASes to cross. In the BGP tab of r1 the prefix of AS 65003 is known
about  twice: directly (path "65003") and through r2 (path "65002 65003"); the shorter AS path wins.
about
about  At 25 s the link r1-r3 fails. The session drops, everything learned over it is withdrawn, and
about  r1 falls back to the longer path through AS 65002 that it already had in store. Compare with
about  RIP: no counting, because the path itself says where a route has been. At 40 s AS 65003
about  withdraws its prefix altogether: the withdrawal travels from AS to AS and h1's ping is
about  answered "network unreachable" by its own router.
about
about  Try: prepend=2 (r3 lengthens the path it announces to r1 only: traffic engineering. r1 then
about  prefers the route through AS 65002 from the start, and the first traceroute shows 4 hops;
about  with prepend=1 the two paths are equally long and the lower router id decides);
about  a different keepalive (the sessions are detected dead by the hold timer, three times as
about  long, when a neighbor goes silent: try `node r2 down` in the console).
about[it]  Tre sistemi autonomi collegati a triangolo, ciascuno con un router di bordo e una LAN
about[it]  (10.1.0.0/24 nell'AS 65001, e così via). Ogni router apre una sessione BGP con i due vicini:
about[it]  una connessione TCP alla porta 179, poi OPEN e KEEPALIVE (guarda la vista Pacchetti; quando
about[it]  i due router aprono una connessione nello stesso istante, una delle due viene richiusa). Poi
about[it]  ciascuno annuncia il proprio prefisso; chi inoltra una rotta antepone il proprio numero di
about[it]  AS, così ogni rotta porta l'elenco degli AS da attraversare. Nella scheda BGP di r1 il
about[it]  prefisso dell'AS 65003 è noto due volte: direttamente (path "65003") e tramite r2 (path
about[it]  "65002 65003"); vince l'AS path più corto.
about[it]
about[it]  A 25 s il collegamento r1-r3 si guasta. La sessione cade, tutto ciò che vi era stato
about[it]  imparato viene ritirato, e r1 ripiega sul percorso più lungo via AS 65002 che aveva già da
about[it]  parte. Confronta con RIP: nessun conteggio, perché è il path stesso a dire dove è passata
about[it]  una rotta. A 40 s l'AS 65003 ritira del tutto il proprio prefisso: il ritiro viaggia di AS
about[it]  in AS e al ping di h1 risponde "network unreachable" il suo stesso router.
about[it]
about[it]  Prova: prepend=2 (r3 allunga il path che annuncia al solo r1: traffic engineering. r1
about[it]  preferisce allora fin dall'inizio la rotta via AS 65002, e il primo traceroute mostra 4
about[it]  salti; con prepend=1 i due path sono lunghi uguali e decide il router id più basso);
about[it]  un keepalive diverso (una sessione viene dichiarata morta dall'hold timer, lungo tre
about[it]  volte tanto, quando un vicino tace: prova `node r2 down` nella console).
param  keepalive 3s BGP keepalive interval (hold time is 3 times this)
param[it] keepalive Intervallo di keepalive BGP (l'hold time è 3 volte tanto)
param+ prepend 0 AS-path prepending applied by r3 towards r1 (0, 1, 2...)
param+[it] prepend AS-path prepending applicato da r3 verso r1 (0, 1, 2...)

node h1 host   x=5  y=80
node r1 router x=22 y=80 role=AS65001
node r2 router x=50 y=15 role=AS65002
node r3 router x=78 y=80 role=AS65003
node h2 host   x=50 y=45
node h3 host   x=95 y=80
link h1 r1 delay=1ms
link r1 r2 delay=5ms
link r2 r3 delay=5ms
link r1 r3 delay=5ms
link r2 h2 delay=1ms
link r3 h3 delay=1ms
ip r1:eth0 10.1.0.1/24
ip h1 10.1.0.10/24
ip r2:eth2 10.2.0.1/24
ip h2 10.2.0.10/24
ip r3:eth2 10.3.0.1/24
ip h3 10.3.0.10/24
ip r1:eth1 172.16.12.1/24
ip r2:eth0 172.16.12.2/24
ip r2:eth1 172.16.23.2/24
ip r3:eth0 172.16.23.3/24
ip r1:eth2 172.16.13.1/24
ip r3:eth1 172.16.13.3/24
gw h1 10.1.0.1
gw h2 10.2.0.1
gw h3 10.3.0.1

run r1 bgp as=65001 connected=1 neighbor=r2:65002 neighbor=r3:65003 keepalive=$keepalive
run r2 bgp as=65002 connected=1 neighbor=r1:65001 neighbor=r3:65003 keepalive=$keepalive
run r3 bgp as=65003 connected=1 neighbor=r1:65001 neighbor=r2:65002 keepalive=$keepalive prepend.r1=$prepend

note 0s  TCP connections to port 179, then OPEN and KEEPALIVE establish the sessions; then each AS announces its prefixes.
note[it] 0s  Connessioni TCP alla porta 179, poi OPEN e KEEPALIVE stabiliscono le sessioni; poi ogni AS annuncia i propri prefissi.
note 10s h1 reaches h3 over the direct link: AS path "65003", one AS away (unless r3 prepends: see the parameter).
note[it] 10s h1 raggiunge h3 sul collegamento diretto: AS path "65003", a un AS di distanza (a meno che r3 non usi il prepending: vedi il parametro).
note 25s Link r1-r3 fails: the session goes down and its routes are withdrawn. r1 uses the path via AS 65002.
note[it] 25s Il collegamento r1-r3 si guasta: la sessione cade e le sue rotte vengono ritirate. r1 usa il percorso via AS 65002.

at 10s run h1 ping h3 count=3
at 15s run h1 traceroute h3
at 25s link r1 r3 down
at 32s run h1 ping h3 count=3
at 37s run h1 traceroute h3
note 40s AS 65003 withdraws 10.3.0.0/24: UPDATE messages with a withdrawn route reach r2, then r1. Nobody can reach h3 any more.
note[it] 40s L'AS 65003 ritira 10.3.0.0/24: messaggi UPDATE con una rotta ritirata raggiungono r2, poi r1. Nessuno può più raggiungere h3.
at 40s run r3 bgp withdraw 10.3.0.0/24
at 44s run h1 ping h3 count=1
duration 48s
expect h1 ping.reply == 6
expect h1 traceroute.hops == 4
expect r1 bgp.established == 1
expect r2 bgp.established == 2
expect r1 bgp.session_drops == 1
expect h1 ping.error == 1
expect r1 bgp.routes == 2
