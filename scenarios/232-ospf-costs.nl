title      OSPF: one map for everybody, and costs that steer the traffic
title[it]  OSPF: una mappa uguale per tutti, e costi che guidano il traffico
level  2
layer  3
topic  ospf
about  Four routers in a diamond. The upper path r1-r2-r4 is fast (100 Mbit/s, OSPF cost 1 per
about  link), the lower path r1-r3-r4 is slow (10 Mbit/s, cost 10 per link). Routers first meet with
about  Hello packets, then flood their link-state advertisements until all four hold the same
about  database (open the OSPF tab of any two routers: same LSAs, same sequence numbers), and each
about  runs Dijkstra on it. Traffic from h1 to h4 takes the upper path: total cost 3 against 21.
about
about  At 20 s the administrator raises the cost of r1's interface towards r2 to 50: r1 floods a new
about  LSA, everyone recomputes, and r1 now sends through r3. Note that r4 still answers through r2:
about  costs are per direction, so the two directions of a conversation may take different paths.
about  At 32 s the link r1-r2 fails for good and both directions move to the lower path within a
about  fraction of a second.
about
about  Try: `r1 ospf cost eth1 1` in the console; `link r1 r3 bw=1G` (the cost follows the
about  bandwidth only at start); different hello intervals on two routers (no adjacency forms).
about[it]  Quattro router a rombo. Il percorso alto r1-r2-r4 è veloce (100 Mbit/s, costo OSPF 1 per
about[it]  collegamento), quello basso r1-r3-r4 è lento (10 Mbit/s, costo 10). I router prima si
about[it]  conoscono con i pacchetti Hello, poi inondano la rete con i loro link-state advertisement
about[it]  finché tutti e quattro hanno lo stesso database (apri la scheda OSPF di due router: stessi
about[it]  LSA, stessi numeri di sequenza), e ognuno vi esegue Dijkstra. Il traffico da h1 a h4 prende
about[it]  il percorso alto: costo totale 3 contro 21.
about[it]
about[it]  A 20 s l'amministratore alza a 50 il costo dell'interfaccia di r1 verso r2: r1 diffonde un
about[it]  nuovo LSA, tutti ricalcolano, e r1 ora inoltra tramite r3. Nota che r4 risponde ancora via
about[it]  r2: i costi valgono per direzione, quindi andata e ritorno possono seguire percorsi diversi.
about[it]  A 32 s il collegamento r1-r2 si guasta del tutto e in una frazione di secondo entrambe le
about[it]  direzioni passano dal percorso basso.
about[it]
about[it]  Prova: `r1 ospf cost eth1 1` nella console; intervalli hello diversi su due router (non si
about[it]  forma l'adiacenza).
param  hello 2s Hello interval (the dead interval is 4 times this)
param[it] hello Intervallo Hello (il dead interval è 4 volte tanto)
param+ newcost 50 Cost set on r1 towards r2 at 20 s
param+[it] newcost Costo impostato su r1 verso r2 a 20 s

node h1 host   x=5  y=50
node r1 router x=25 y=50
node r2 router x=50 y=15
node r3 router x=50 y=85
node r4 router x=75 y=50
node h4 host   x=95 y=50
link h1 r1 bw=100M delay=1ms
link r1 r2 bw=100M delay=3ms
link r1 r3 bw=10M  delay=3ms
link r2 r4 bw=100M delay=3ms
link r3 r4 bw=10M  delay=3ms
link r4 h4 bw=100M delay=1ms
auto addr

run r1 ospf hello=$hello
run r2 ospf hello=$hello
run r3 ospf hello=$hello
run r4 ospf hello=$hello

note 0s  Hello packets: each router lists the neighbors it has heard; seeing yourself listed means two-way.
note[it] 0s  Pacchetti Hello: ogni router elenca i vicini che ha sentito; vedersi nell'elenco significa comunicazione bidirezionale.
note 3s  Adjacencies are Full: LSAs flood, every router runs SPF on the same database.
note[it] 3s  Le adiacenze sono Full: gli LSA si propagano, ogni router esegue SPF sullo stesso database.
note 10s h1 pings h4: the packets take the fast upper path through r2.
note[it] 10s h1 fa ping a h4: i pacchetti prendono il percorso veloce in alto, via r2.
note 20s Cost of r1→r2 raised: a new LSA is flooded and r1 switches to r3 (r4 still answers via r2).
note[it] 20s Alzato il costo r1→r2: un nuovo LSA viene diffuso e r1 passa a r3 (r4 risponde ancora via r2).
note 32s Link r1-r2 fails: the neighbors are declared down at once, new LSAs, new SPF.
note[it] 32s Il collegamento r1-r2 si guasta: i vicini sono dichiarati caduti subito, nuovi LSA, nuovo SPF.

at 10s run h1 ping h4 count=3
at 14s run h1 traceroute h4
at 20s run r1 ospf cost eth1 $newcost
at 24s run h1 ping h4 count=3
at 32s link r1 r2 down
at 36s run h4 ping h1 count=3
duration 42s
expect h1 ping.reply == 6
expect h4 ping.reply == 3
expect h1 traceroute.hops == 4
expect r1 ospf.neighbors == 1
expect r4 ospf.neighbors == 2
expect r3 ip.forwarded >= 9
expect r2 ospf.lsdb == 4
