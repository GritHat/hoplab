title      OSPFv3: link state for IPv6, where topology and prefixes part ways
title[it]  OSPFv3: link state per IPv6, dove topologia e prefissi si separano
level  2
layer  3
topic  ospf6
about      Four routers run OSPFv3 on IPv6-only links of two speeds: a fast upper path through r2 (cost 1 per link) and a slow lower one through r3 (cost 10). Hellos go to ff02::5 from link-local addresses; when two routers see each other the adjacency is Full and they exchange their databases. Open the `ospf6` tab of a router: the database holds, for every router, a Router-LSA that lists only neighbors and costs (no address anywhere) and an Intra-Area-Prefix-LSA that lists the IPv6 prefixes attached to it. Topology and addressing are separate, which is the main difference from OSPF for IPv4. At detail 3 you also see the Link-LSAs, which never leave their link and announce the link-local address to use as next hop.
about
about      h1 first reaches h4 through r2. At 20 s the cost of r1 towards r2 is raised: one new Router-LSA is flooded, every router reruns Dijkstra, and r1 moves to the lower path if the upper one (new cost + 1) has become dearer than 20. At 32 s the link r1-r2 fails and everything goes through r3.
about
about      Things to try: open an LS Update in the Packets view and read the LSA headers (type, advertising router, sequence number); compare the Routes tab with lab 232: the next hops are fe80:: addresses; change newcost to 19 and to 20 and explain the difference.
about[it]  Quattro router eseguono OSPFv3 su collegamenti solo IPv6 di due velocità: un percorso superiore veloce attraverso r2 (costo 1 per collegamento) e uno inferiore lento attraverso r3 (costo 10). Gli Hello vanno a ff02::5 dagli indirizzi link-local; quando due router si vedono a vicenda l'adiacenza è Full e si scambiano i database. Apri la scheda `ospf6` di un router: il database contiene, per ogni router, un Router-LSA che elenca solo vicini e costi (nessun indirizzo) e un Intra-Area-Prefix-LSA che elenca i prefissi IPv6 a esso collegati. Topologia e indirizzamento sono separati, ed è la differenza principale rispetto a OSPF per IPv4. A dettaglio 3 si vedono anche i Link-LSA, che non lasciano mai il proprio collegamento e annunciano l'indirizzo link-local da usare come next hop.
about[it]
about[it]  h1 raggiunge h4 dapprima attraverso r2. A 20 s il costo di r1 verso r2 viene alzato: un solo nuovo Router-LSA viene diffuso, ogni router riesegue Dijkstra, e r1 passa al percorso inferiore se quello superiore (nuovo costo + 1) è diventato più caro di 20. A 32 s il collegamento r1-r2 si guasta e tutto passa da r3.
about[it]
about[it]  Da provare: apri un LS Update nella vista Pacchetti e leggi le intestazioni degli LSA (tipo, router che lo annuncia, numero di sequenza); confronta la scheda Rotte con il laboratorio 232: i next hop sono indirizzi fe80::; cambia newcost in 19 e in 20 e spiega la differenza.
param      hello 2s Hello interval in whole seconds, 1s to 5s (the dead interval is 4 times this)
param[it]  hello Intervallo degli Hello in secondi interi, da 1s a 5s (il dead interval è 4 volte tanto)
param+     newcost 50 Cost set on r1 towards r2 at 20 s
param[it]  newcost Costo impostato su r1 verso r2 a 20 s

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
auto addr6

run r1 ospf6 hello=$hello
run r2 ospf6 hello=$hello
run r3 ospf6 hello=$hello
run r4 ospf6 hello=$hello

note 0s    Hellos to ff02::5 from link-local addresses: seeing yourself in a neighbor's list means two-way.
note[it] 0s Hello verso ff02::5 dagli indirizzi link-local: vedersi nell'elenco di un vicino significa two-way.
note 8s    Adjacencies are Full: Router-LSAs (topology) and Intra-Area-Prefix-LSAs (prefixes) have flooded; every router ran Dijkstra on the same database.
note[it] 8s Le adiacenze sono Full: i Router-LSA (topologia) e gli Intra-Area-Prefix-LSA (prefissi) sono stati diffusi; ogni router ha eseguito Dijkstra sullo stesso database.
note 12s   h1 pings h4 over IPv6: the packets take the fast upper path through r2.
note[it] 12s h1 fa ping a h4 su IPv6: i pacchetti prendono il percorso superiore veloce attraverso r2.
note 20s   Cost of r1 towards r2 set to $newcost: one Router-LSA changes; r1 moves to r3 if $newcost + 1 is more than 20.
note[it] 20s Costo di r1 verso r2 impostato a $newcost: cambia un solo Router-LSA; r1 passa a r3 se $newcost + 1 è più di 20.
note 32s   Link r1-r2 fails: the neighbors go down at once, new LSAs, new SPF, everything through r3.
note[it] 32s Il collegamento r1-r2 si guasta: i vicini cadono subito, nuovi LSA, nuovo SPF, tutto attraverso r3.

at 12s run h1 ping h4 count=3 v6=1
at 16s run h1 traceroute h4 v6=1
at 20s run r1 ospf6 cost eth1 $newcost
at 24s run h1 ping h4 count=3 v6=1
at 32s link r1 r2 down
at 36s run h4 ping h1 count=3 v6=1
duration 43s
at 42s run h1 check reach "h1:ping.reply == 6 && h4:ping.reply == 3 && h1:traceroute.hops == 4"
at 42s run h1 check database "r1:ospf6.neighbors == 1 && r4:ospf6.neighbors == 2 && r2:ospf6.lsdb >= 8"
# r3 carries the 3 requests of the second ping only if the new cost made the upper path dearer
# (cost + 1 against 20), then both directions after the failure
at 42s run h1 check steer "($newcost > 19 && r3:ip6.forwarded >= 9) || ($newcost < 19 && r3:ip6.forwarded == 6) || $newcost == 19"
at 42s run h1 check quiet "r1:ospf6.retransmits + r2:ospf6.retransmits + r3:ospf6.retransmits + r4:ospf6.retransmits <= 2"
expect h1 check.reach == 1
expect h1 check.database == 1
expect h1 check.steer == 1
expect h1 check.quiet == 1
