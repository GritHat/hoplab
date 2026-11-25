title      Spanning tree: a loop made safe, and repaired when a link fails
title[it]  Spanning tree: un anello reso sicuro, e riparato quando un collegamento cade
level  2
layer  2
topic  stp
about  The same triangle of switches as scenario 270, now running the Spanning Tree Protocol. The
about  switches elect a root (the lowest bridge identifier: s1), each other switch keeps its best
about  port towards the root, and on the link s2 - s3 one side is blocked: the loop is gone but the
about  link stays ready as a spare. At 18 s the link s1 - s2 fails and the tree rebuilds itself
about  through the blocked port.
about
about  Watch: the panel of each switch (role and state of every port); ports walk through
about  blocking, listening, learning and forwarding, one forward delay per step, so the first ping
about  has to wait; after the failure there is an outage of two forward delays. Two steady streams
about  between h1 and h2 (50 datagrams per second each way) measure it: the datagrams missing at
about  the sink of h1 are the outage, 20 ms each.
about  Try: rapid=1 (Rapid Spanning Tree: the switches agree with a proposal/agreement handshake
about  instead of waiting, at start-up and after the failure, and the outage shrinks from two forward
about  delays to the few milliseconds the BPDUs need; host ports become edge ports after 3 hellos);
about  portfast=1 (host ports skip the wait from the start); fwd up to 6s fits the timeline;
about  `link s1 s2 up` later; `node s1 down` to kill the root (recovery then also needs max-age,
about  or 3 hellos with rapid=1).
about[it]  Lo stesso triangolo di switch dello scenario 270, ora con lo Spanning Tree Protocol. Gli
about[it]  switch eleggono una radice (l'identificatore di bridge più basso: s1), ogni altro switch tiene la
about[it]  porta migliore verso la radice, e sul collegamento s2 - s3 un lato viene bloccato: l'anello non
about[it]  c'è più ma il collegamento resta pronto come riserva. A 18 s il collegamento s1 - s2 cade e
about[it]  l'albero si ricostruisce attraverso la porta bloccata.
about[it]
about[it]  Osserva: il pannello di ogni switch (ruolo e stato di ogni porta); le porte passano per
about[it]  blocking, listening, learning e forwarding, un forward delay per passo, quindi il primo ping
about[it]  deve aspettare; dopo il guasto c'è un'interruzione di due forward delay. Due flussi costanti
about[it]  fra h1 e h2 (50 datagrammi al secondo per verso) la misurano: i datagrammi che mancano al
about[it]  sink di h1 sono l'interruzione, 20 ms ciascuno.
about[it]  Prova: rapid=1 (Rapid Spanning Tree: gli switch si accordano con uno scambio
about[it]  proposal/agreement invece di aspettare, all'avvio e dopo il guasto, e l'interruzione passa da
about[it]  due forward delay ai pochi millisecondi che servono ai BPDU; le porte verso gli host diventano
about[it]  edge dopo 3 hello); portfast=1 (le porte verso gli host saltano l'attesa fin dall'inizio); fwd
about[it]  fino a 6s sta nella cronologia; più tardi `link s1 s2 up`; `node s1 down` per uccidere la
about[it]  radice (il ripristino richiede allora anche max-age, o 3 hello con rapid=1).
param      fwd 3s Forward delay (time spent in listening and again in learning; up to 6s)
param[it]  fwd Forward delay (tempo passato in listening e poi ancora in learning; fino a 6s)
param      hello 1s Hello time (interval between BPDUs)
param[it]  hello Hello time (intervallo fra i BPDU)
param+     maxage 6s Max age (how long information survives without being refreshed)
param[it]  maxage Max age (quanto sopravvive un'informazione senza essere rinfrescata)
param      rapid 0 Rapid Spanning Tree, 802.1w (1 = on)
param[it]  rapid Rapid Spanning Tree, 802.1w (1 = attivo)
param+     portfast 0 Ports that face a host forward immediately (1 = on)
param[it]  portfast Le porte verso un host inoltrano subito (1 = attivo)

node s1 switch x=50 y=20
node s2 switch x=25 y=65
node s3 switch x=75 y=65
node h1 host x=50 y=3
node h2 host x=8 y=85
node h3 host x=92 y=85
link s1 s2 delay=5ms
link s2 s3 delay=5ms
link s1 s3 delay=5ms
link h1 s1 delay=2ms
link h2 s2 delay=2ms
link h3 s3 delay=2ms
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24
ip h3 10.0.0.3/24

run s1 stp hello=$hello fwd=$fwd maxage=$maxage rapid=$rapid portfast=$portfast
run s2 stp hello=$hello fwd=$fwd maxage=$maxage rapid=$rapid portfast=$portfast
run s3 stp hello=$hello fwd=$fwd maxage=$maxage rapid=$rapid portfast=$portfast
run h1 sink
run h2 sink
at 14s run h2 ping h1 count=3 interval=500ms
at 16s run h2 cbr h1 rate=40k size=100 duration=26s
at 16s run h1 cbr h2 rate=40k size=100 duration=26s
at 18s link s1 s2 down
at 38s run h2 ping h1 count=3 interval=500ms

note 0s Every switch starts with all ports blocked and claims to be the root. BPDUs are exchanged: s1 has the lowest identifier and wins.
note[it] 0s Ogni switch parte con tutte le porte bloccate e sostiene di essere la radice. Si scambiano i BPDU: s1 ha l'identificatore più basso e vince.
note 2s On the link s2 - s3 both are equally far from the root: s2 has the lower identifier and becomes designated, s3 blocks its port. The loop is broken.
note[it] 2s Sul collegamento s2 - s3 entrambi sono alla stessa distanza dalla radice: s2 ha l'identificatore più basso e diventa designated, s3 blocca la sua porta. L'anello è spezzato.
note 6s Plain STP needs two forward delays before root and designated ports forward; with rapid=1 the switches agreed within milliseconds and only the host ports waited (3 hellos, or nothing with portfast=1).
note[it] 6s Lo STP classico richiede due forward delay prima che le porte root e designated inoltrino; con rapid=1 gli switch si sono accordati in pochi millisecondi e hanno atteso solo le porte verso gli host (3 hello, o nulla con portfast=1).
note 14s h2 pings h1: the ARP broadcast is flooded along the tree and stops, no storm. Then two steady streams start.
note[it] 14s h2 fa ping verso h1: il broadcast ARP viene inondato lungo l'albero e si ferma, nessuna tempesta. Poi partono due flussi costanti.
note 18s The link s1 - s2 fails. s2 has lost its root port and tells s3; s3 turns its alternate port into a designated one. Plain STP: it must listen and learn before forwarding. rapid=1: s3 proposes, s2 agrees, done.
note[it] 18s Il collegamento s1 - s2 cade. s2 ha perso la porta radice e lo dice a s3; s3 trasforma la porta alternate in designated. STP classico: deve fare listening e learning prima di inoltrare. rapid=1: s3 propone, s2 accetta, fatto.
note 31s The tree is whole again: h2 reaches h1 through s3. The switches flushed their forwarding tables (topology change). Count the datagrams missing at h1: that was the outage.
note[it] 31s L'albero è di nuovo completo: h2 raggiunge h1 passando per s3. Gli switch hanno svuotato le tabelle di inoltro (topology change). Conta i datagrammi che mancano a h1: quella è stata l'interruzione.

duration 44s
at 43900ms run s1 check tree "s1:stp.is_root == 1 && s2:stp.is_root == 0 && s3:stp.is_root == 0 && s3:stp.blocked_ports == 0 && s2:stp.topology_changes >= 1"
at 43900ms run s1 check reach "$fwd > 6s || h2:ping.reply == 6"
# datagrams of h2 that never reached h1: one every 20 ms of outage
at 43900ms run s1 check outage "h2:cbr.sent - h1:sink.received"
# no storm: s1 floods only what it cannot place while the tree is broken (the stream of h1 during the outage)
at 43900ms run s1 check calm "s1:bridge.flood <= s1:check.outage + 60"
# plain STP: listening + learning = two forward delays; RSTP: the time of a handshake
at 43900ms run s1 check recovery "($rapid == 0 && abs(s1:check.outage - 2 * $fwd / 20) <= 30) || ($rapid == 1 && s1:check.outage <= 10)"
expect s1 check.tree == 1
expect s1 check.reach == 1
expect s1 check.recovery == 1
expect s1 check.calm == 1
