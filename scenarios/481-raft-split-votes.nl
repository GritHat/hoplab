title  Raft: election timeouts, split votes and availability
title[it] Raft: timeout di elezione, voti divisi e disponibilità
level  4
layer  8
topic  raft
about  How long is a Raft cluster without a leader after a failure? It depends on the election timeout and on how random it is. Here the followers draw their timeout from a deliberately narrow range: when leader n1 crashes at 5 s, several of them time out almost together, each votes for itself, nobody reaches a majority (a split vote) and the term is wasted. Randomness eventually breaks the tie. The counter raft.elections on each node and the term number tell how many attempts it took.
about  Experiments: widen the range (election=1000-2000ms) and compare the time without a leader; shrink it to the link round-trip time (election=1000-1010ms); make it much shorter than the heartbeat interval and watch followers depose a healthy leader; raise the link delay. The original paper recommends broadcastTime << electionTimeout << MTBF and a range about as wide as its lower bound.
about[it] Per quanto tempo un cluster Raft resta senza leader dopo un guasto? Dipende dal timeout di elezione e da quanto è casuale. Qui i follower estraggono il timeout da un intervallo volutamente stretto: quando il leader n1 cade a 5 s, molti vanno in timeout quasi insieme, ognuno vota per sé, nessuno raggiunge la maggioranza (voto diviso) e il term va sprecato. Prima o poi la casualità rompe la simmetria. Il contatore raft.elections di ogni nodo e il numero di term dicono quanti tentativi sono serviti.
about[it] Esperimenti: allargare l'intervallo (election=1000-2000ms) e confrontare il tempo senza leader; restringerlo fino al tempo di andata e ritorno sul collegamento (election=1000-1010ms); renderlo molto più corto dell'intervallo di heartbeat e osservare i follower deporre un leader sano; aumentare il ritardo dei collegamenti. L'articolo originale raccomanda broadcastTime << electionTimeout << MTBF e un intervallo ampio circa quanto il suo estremo inferiore.
param  election 1000-1060ms Election timeout range (narrow = split votes)
param[it] election Intervallo del timeout di elezione (stretto = voti divisi)
param+ heartbeat 300ms Leader heartbeat interval
param[it] heartbeat Intervallo degli heartbeat del leader
param+ delay 5ms Link delay
param[it] delay Ritardo dei collegamenti

node s1 switch x=50 y=50
node n1 host x=50 y=10
node n2 host x=88 y=38
node n3 host x=74 y=88
node n4 host x=26 y=88
node n5 host x=12 y=38
link n1 s1 delay=$delay
link n2 s1 delay=$delay
link n3 s1 delay=$delay
link n4 s1 delay=$delay
link n5 s1 delay=$delay
auto all

run n1 raft peers=n1,n2,n3,n4,n5 election=400-500ms heartbeat=$heartbeat
run n2 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n3 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n4 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n5 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat

at 2s run n1 raft put x=1
at 5s node n1 down
note 5s Leader n1 crashes. The four followers have almost the same timeout: expect several candidates at once.
note[it] 5s Il leader n1 cade. I quattro follower hanno quasi lo stesso timeout: aspettati più candidati insieme.
note 7s Count the "election timeout" lines since the crash: every extra round is a term lost to a split vote.
note[it] 7s Conta le righe "election timeout" dopo il guasto: ogni giro in più è un term perso per un voto diviso.
at 20s run n2 raft put y=2
duration 25s
expect n2 raft.leaders == 1
expect n2 raft.agree == 1
expect n2 raft.applied >= 4
