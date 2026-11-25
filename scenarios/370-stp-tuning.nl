title      Engineering a spanning tree: priorities, port costs, timers and a root failure
title[it]  Progettare uno spanning tree: priorità, costi delle porte, timer e guasto della radice
level  3
layer  2
topic  stp
about  Four switches in a ring. Left alone, the root would be whichever switch happens to have the
about  lowest MAC address; here the administrator chooses it with the bridge priority (param root).
about  Port costs follow the bandwidth (10^9 / bit rate: 100 for 10 Mbit/s, 10 for 100 Mbit/s), so
about  s1 reaches the root through the fast link s4 - s3 even though both paths have two hops, and
about  the redundant port is blocked on the slow side. At 14 s the root crashes: a dead bridge is
about  noticed only when its information reaches max-age, then the survivors elect a new root and
about  the blocked port walks through listening and learning.
about
about  Watch: role, state and cost of each port in the switch panels; the outage after the crash
about  lasts about max-age + 2 x forward delay; counter stp.last_change_ms gives the convergence time.
about  Try: root=s4 (the hosts hang off s1 and s2, so a root there takes them down with it); shorter timers (hello=200ms maxage=1s fwd=500ms) and how far you can go
about  before a lost BPDU is mistaken for a failure (add loss=0.3 to a link from the console);
about  `s1 stp cost p1 500` to push s1 to the other path; rapid=1.
about[it]  Quattro switch ad anello. Lasciati a sé stessi, la radice sarebbe lo switch che per caso ha
about[it]  l'indirizzo MAC più basso; qui la sceglie l'amministratore con la priorità del bridge (parametro
about[it]  root). I costi delle porte seguono la banda (10^9 / bit rate: 100 per 10 Mbit/s, 10 per 100 Mbit/s),
about[it]  quindi s1 raggiunge la radice attraverso il collegamento veloce s4 - s3 anche se entrambi i
about[it]  percorsi hanno due hop, e la porta ridondante viene bloccata sul lato lento. A 14 s la radice va
about[it]  in crash: un bridge morto viene notato solo quando la sua informazione raggiunge max-age, poi i
about[it]  superstiti eleggono una nuova radice e la porta bloccata passa per listening e learning.
about[it]
about[it]  Osserva: ruolo, stato e costo di ogni porta nei pannelli degli switch; l'interruzione dopo il
about[it]  crash dura circa max-age + 2 x forward delay; il contatore stp.last_change_ms dà il tempo di convergenza.
about[it]  Prova: root=s4 (gli host sono attaccati a s1 e s2, quindi una radice lì li porta giù con sé); timer più corti (hello=200ms maxage=1s fwd=500ms) e fin dove puoi spingerti
about[it]  prima che un BPDU perso venga scambiato per un guasto (aggiungi loss=0.3 a un collegamento dalla
about[it]  console); `s1 stp cost p1 500` per spingere s1 sull'altro percorso; rapid=1.
param      root s3 Switch that gets the best priority (4096) and becomes the root
param[it]  root Switch che riceve la priorità migliore (4096) e diventa la radice
param      hello 1s Hello time
param[it]  hello Hello time
param      maxage 6s Max age
param[it]  maxage Max age
param      fwd 3s Forward delay
param[it]  fwd Forward delay
param+     rapid 0 Rapid transitions of a new root port (1 = on)
param[it]  rapid Transizione rapida di una nuova porta radice (1 = attiva)
param+     fast 100M Bandwidth of the fast link s3 - s4
param[it]  fast Banda del collegamento veloce s3 - s4

node s1 switch x=25 y=30
node s2 switch x=75 y=30
node s3 switch x=75 y=75
node s4 switch x=25 y=75
node h1 host x=8 y=12
node h2 host x=92 y=12
link s1 s2 bw=10M delay=2ms
link s2 s3 bw=10M delay=2ms
link s3 s4 bw=$fast delay=2ms
link s4 s1 bw=10M delay=2ms
link h1 s1 delay=1ms
link h2 s2 delay=1ms
ip h1 10.0.0.1/24
ip h2 10.0.0.2/24

run s1 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s2 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s3 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run s4 stp hello=$hello maxage=$maxage fwd=$fwd rapid=$rapid portfast=1
run $root stp priority 4096
at 9s run h1 ping h2 count=3 interval=500ms
at 14s node $root down
at 29s run h1 ping h2 count=3 interval=500ms

note 0s Priority 4096 beats the default 32768: the chosen switch wins the election whatever its MAC address.
note[it] 0s La priorità 4096 batte il valore predefinito 32768: lo switch scelto vince l'elezione qualunque sia il suo indirizzo MAC.
note 2s s1 has two paths to the root: through s2 (100 + 100) or through s4 (100 + 10). It takes the cheaper one; the port towards s2 becomes alternate (blocked).
note[it] 2s s1 ha due percorsi verso la radice: attraverso s2 (100 + 100) o attraverso s4 (100 + 10). Sceglie il meno costoso; la porta verso s2 diventa alternate (bloccata).
note 9s h1 pings h2: they are neighbours, but the frames go the long way round s1 - s4 - s3 - s2, because the direct link is the blocked one. A spanning tree is loop-free, not shortest-path.
note[it] 9s h1 fa ping verso h2: sono vicini, ma i frame fanno il giro lungo s1 - s4 - s3 - s2, perché il collegamento diretto è quello bloccato. Uno spanning tree è privo di cicli, non a cammino minimo.
note 14s The root crashes. Its neighbours' links stay up, so nothing is noticed yet: the old information just gets older.
note[it] 14s La radice va in crash. I collegamenti dei vicini restano attivi, quindi per ora nessuno se ne accorge: la vecchia informazione semplicemente invecchia.
note 20s Max-age reached: the information about the dead root is discarded, the survivors hold a new election and the lowest identifier left wins.
note[it] 20s Raggiunto max-age: l'informazione sulla radice morta viene scartata, i superstiti tengono una nuova elezione e vince l'identificatore più basso rimasto.
note 27s The formerly blocked port is forwarding: h1 and h2 now talk over the direct link.
note[it] 27s La porta prima bloccata ora inoltra: h1 e h2 parlano sul collegamento diretto.

duration 33s
expect s1 stp.is_root == 1
expect s1 stp.root_changes >= 2
expect s1 stp.blocked_ports == 0
expect s2 stp.blocked_ports == 0
expect s1 stp.last_change_ms < 28500
expect h1 ping.reply == 6
