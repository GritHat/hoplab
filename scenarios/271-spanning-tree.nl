title      Spanning tree: a loop made safe, and repaired when a link fails
title[it]  Spanning tree: un anello reso sicuro, e riparato quando un collegamento cade
level  2
layer  2
topic  stp
about  The same triangle of switches as scenario 270, now running the Spanning Tree Protocol. The
about  switches elect a root (the lowest bridge identifier: s1), each other switch keeps its best
about  port towards the root, and on the link s2 - s3 one side is blocked: the loop is gone but the
about  link stays ready as a spare. At 15 s the link s1 - s2 fails and the tree rebuilds itself
about  through the blocked port.
about
about  Watch: the panel of each switch (role and state of every port); ports walk through
about  blocking, listening, learning and forwarding, one forward delay per step, so the first ping
about  has to wait; after the failure there is an outage of two forward delays.
about  Try: rapid=1 (a new root port forwards at once); portfast=1 (host ports skip the wait);
about  `link s1 s2 up` later; `node s1 down` to kill the root (recovery now needs max-age as well).
about[it]  Lo stesso triangolo di switch dello scenario 270, ora con lo Spanning Tree Protocol. Gli
about[it]  switch eleggono una radice (l'identificatore di bridge più basso: s1), ogni altro switch tiene la
about[it]  porta migliore verso la radice, e sul collegamento s2 - s3 un lato viene bloccato: l'anello non
about[it]  c'è più ma il collegamento resta pronto come riserva. A 15 s il collegamento s1 - s2 cade e
about[it]  l'albero si ricostruisce attraverso la porta bloccata.
about[it]
about[it]  Osserva: il pannello di ogni switch (ruolo e stato di ogni porta); le porte passano per
about[it]  blocking, listening, learning e forwarding, un forward delay per passo, quindi il primo ping
about[it]  deve aspettare; dopo il guasto c'è un'interruzione di due forward delay.
about[it]  Prova: rapid=1 (una nuova porta radice inoltra subito); portfast=1 (le porte verso gli host
about[it]  saltano l'attesa); più tardi `link s1 s2 up`; `node s1 down` per uccidere la radice (ora il
about[it]  ripristino richiede anche max-age).
param      fwd 3s Forward delay (time spent in listening and again in learning)
param[it]  fwd Forward delay (tempo passato in listening e poi ancora in learning)
param      hello 1s Hello time (interval between BPDUs)
param[it]  hello Hello time (intervallo fra i BPDU)
param+     maxage 6s Max age (how long information survives without being refreshed)
param[it]  maxage Max age (quanto sopravvive un'informazione senza essere rinfrescata)
param+     rapid 0 Rapid transitions of a new root port (1 = on)
param[it]  rapid Transizione rapida di una nuova porta radice (1 = attiva)
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
at 8s run h2 ping h1 count=3 interval=500ms
at 15s link s1 s2 down
at 24s run h2 ping h1 count=3 interval=500ms

note 0s Every switch starts with all ports blocked and claims to be the root. BPDUs are exchanged: s1 has the lowest identifier and wins.
note[it] 0s Ogni switch parte con tutte le porte bloccate e sostiene di essere la radice. Si scambiano i BPDU: s1 ha l'identificatore più basso e vince.
note 2s On the link s2 - s3 both are equally far from the root: s2 has the lower identifier and becomes designated, s3 blocks its port. The loop is broken.
note[it] 2s Sul collegamento s2 - s3 entrambi sono alla stessa distanza dalla radice: s2 ha l'identificatore più basso e diventa designated, s3 blocca la sua porta. L'anello è spezzato.
note 6s Two forward delays have passed: root and designated ports are forwarding. Only now can the hosts talk.
note[it] 6s Sono passati due forward delay: le porte root e designated inoltrano. Solo adesso gli host possono parlare.
note 8s h2 pings h1: the ARP broadcast is flooded along the tree and stops, no storm.
note[it] 8s h2 fa ping verso h1: il broadcast ARP viene inondato lungo l'albero e si ferma, nessuna tempesta.
note 15s The link s1 - s2 fails. s2 has lost its root port and tells s3; s3 turns its blocked port into a designated one, which must again listen and learn before forwarding.
note[it] 15s Il collegamento s1 - s2 cade. s2 ha perso la porta radice e lo dice a s3; s3 trasforma la porta bloccata in designated, che deve di nuovo fare listening e learning prima di inoltrare.
note 22s The tree is whole again: h2 now reaches h1 through s3. The switches flushed their forwarding tables (topology change).
note[it] 22s L'albero è di nuovo completo: h2 ora raggiunge h1 passando per s3. Gli switch hanno svuotato le tabelle di inoltro (topology change).

duration 28s
expect s1 stp.is_root == 1
expect s2 stp.is_root == 0
expect s3 stp.is_root == 0
expect s3 stp.blocked_ports == 0
expect s2 stp.topology_changes >= 1
expect h2 ping.reply == 6
expect s1 bridge.flood < 40
