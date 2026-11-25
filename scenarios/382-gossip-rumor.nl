title  Gossip: how fast does a rumor reach everybody?
title[it] Gossip: quanto in fretta una voce raggiunge tutti?
level  3
layer  8
topic  gossip
about  Twelve nodes on a LAN. At 2 s node n1 learns a piece of news and the epidemic starts: at every round each node that is spreading the rumor sends it to `fanout` peers chosen at random (push), and/or every node asks random peers for what it is missing (pull). n1 prints, round after round, how many nodes know: the curve is the classic S of an epidemic and everybody is reached in about log2(N) rounds, with no coordinator and no knowledge of the topology.
about  Watch: the graph in n1's gossip panel, "infective" turning into "removed" when a node loses interest, the redundant pushes near the end. Experiments: mode=push with stop=1 (the rumor may die before reaching everyone: the residue), mode=pull (slow start, very fast finish), fanout=1 vs 4 (rounds vs messages: compare gossip.messages), add loss=20% on the links.
about[it] Dodici nodi su una LAN. A 2 s il nodo n1 apprende una notizia e parte l'epidemia: a ogni round ciascun nodo che sta diffondendo la voce la invia a `fanout` nodi scelti a caso (push), e/o ogni nodo chiede a nodi casuali ciò che gli manca (pull). n1 stampa, round dopo round, quanti nodi sanno: la curva è la classica S di un'epidemia e tutti sono raggiunti in circa log2(N) round, senza coordinatore e senza conoscere la topologia.
about[it] Da osservare: il grafico nel pannello gossip di n1, "infective" che diventa "removed" quando un nodo perde interesse, i push ridondanti verso la fine. Esperimenti: mode=push con stop=1 (la voce può estinguersi prima di raggiungere tutti: il residuo), mode=pull (partenza lenta, finale rapidissimo), fanout=1 contro 4 (round contro messaggi: confronta gossip.messages), aggiungere loss=20% sui collegamenti.
param  mode pushpull Style: push, pull or pushpull
param[it] mode Stile: push, pull o pushpull
param  fanout 2 Peers contacted per round
param[it] fanout Nodi contattati a ogni round
param+ interval 500ms Round length
param[it] interval Durata di un round
param+ stop 3 Push: lose interest after this many "already knew"
param[it] stop Push: perdi interesse dopo questo numero di "lo sapevo già"
param+ loss 0 Packet loss on every link
param[it] loss Perdita di pacchetti su ogni collegamento

node s1 switch x=50 y=50
node n1 host x=50 y=10
node n2 host x=71 y=15
node n3 host x=86 y=29
node n4 host x=92 y=50
node n5 host x=86 y=70
node n6 host x=71 y=84
node n7 host x=50 y=90
node n8 host x=29 y=84
node n9 host x=13 y=70
node n10 host x=8 y=50
node n11 host x=13 y=29
node n12 host x=28 y=15
link n1 s1 delay=2ms loss=$loss
link n2 s1 delay=2ms loss=$loss
link n3 s1 delay=2ms loss=$loss
link n4 s1 delay=2ms loss=$loss
link n5 s1 delay=2ms loss=$loss
link n6 s1 delay=2ms loss=$loss
link n7 s1 delay=2ms loss=$loss
link n8 s1 delay=2ms loss=$loss
link n9 s1 delay=2ms loss=$loss
link n10 s1 delay=2ms loss=$loss
link n11 s1 delay=2ms loss=$loss
link n12 s1 delay=2ms loss=$loss
auto all

run n1 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n2 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n3 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n4 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n5 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n6 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n7 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n8 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n9 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n10 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n11 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop
run n12 gossip peers=n1,n2,n3,n4,n5,n6,n7,n8,n9,n10,n11,n12 mode=$mode fanout=$fanout interval=$interval stop=$stop

note 0s Twelve nodes, nobody knows anything yet.
note[it] 0s Dodici nodi, nessuno sa ancora nulla.
at 2s run n1 gossip rumor text=fire!
note 2s n1 starts the rumor. Follow the count printed at every round: 1, 2-3, 5-8, ... it roughly doubles.
note[it] 2s n1 fa partire la voce. Segui il conteggio stampato a ogni round: 1, 2-3, 5-8, ... raddoppia all'incirca.
note 6s Near the end most pushes hit nodes that already know: that waste is the price of having no coordinator.
note[it] 6s Verso la fine quasi tutti i push colpiscono nodi che sanno già: quello spreco è il prezzo dell'assenza di un coordinatore.
duration 20s
expect n1 gossip.rounds >= 2
expect n1 gossip.rounds <= 14
expect n5 gossip.infected == 1
expect n8 gossip.infected == 1
expect n12 gossip.infected == 1
