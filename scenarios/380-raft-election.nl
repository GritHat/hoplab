title  Raft: electing a leader, and replacing it when it crashes
title[it] Raft: eleggere un leader, e sostituirlo quando cade
level  3
layer  8
topic  raft
about  Five servers start as followers with no leader. The first whose randomized election timeout expires becomes a candidate, asks for votes and, with a majority, becomes the leader of term 1; its heartbeats keep the others quiet. At 8 s the leader crashes: heartbeats stop, a follower times out and a new leader is elected in a higher term. At 16 s the old leader restarts and discovers from the first heartbeat that it is now a follower.
about  Watch: the election timer bar in each node's raft panel, the RequestVote / vote exchange in the sequence view, the term number growing. Try: a narrow election range (election=1000-1050ms) to provoke split votes; crash two more nodes (`node n2 down`) and see that with 2 of 5 alive nobody can be elected. Keep the lower bound of the election range at least three times the heartbeat interval: Raft relies on it.
about[it] Cinque server partono come follower, senza leader. Il primo a cui scade il timeout di elezione (casuale) diventa candidato, chiede i voti e, ottenuta la maggioranza, è il leader del term 1; i suoi heartbeat tengono fermi gli altri. A 8 s il leader cade: gli heartbeat cessano, un follower va in timeout e viene eletto un nuovo leader in un term più alto. A 16 s il vecchio leader riparte e scopre dal primo heartbeat di essere ormai un follower.
about[it] Da osservare: la barra del timer di elezione nel pannello raft di ogni nodo, lo scambio RequestVote / voto nella vista a sequenza, il numero di term che cresce. Da provare: un intervallo di elezione stretto (election=1000-1050ms) per provocare voti divisi; far cadere altri due nodi (`node n2 down`) e vedere che con 2 nodi su 5 nessuno può essere eletto. Il limite inferiore dell'intervallo di elezione deve restare almeno tre volte l'intervallo di heartbeat: Raft ci fa affidamento.
param  election 1000-2000ms Election timeout range
param[it] election Intervallo del timeout di elezione
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

# first=1 shortens n1's very first timeout so that it wins the first election and the story is repeatable
run n1 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat first=1
run n2 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n3 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n4 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n5 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat

note 0s Nobody is leader yet: every follower is counting down its own random election timeout (the first one of n1 is shortened, so it will win).
note[it] 0s Non c'è ancora un leader: ogni follower sta contando il proprio timeout di elezione casuale (il primo di n1 è accorciato, quindi vincerà).
note 3s A leader exists: its heartbeats (empty AppendEntries) reset everyone's election timer.
note[it] 3s C'è un leader: i suoi heartbeat (AppendEntries vuoti) azzerano il timer di elezione di tutti.
at 8s node n1 down
note 8s The leader n1 crashes. Heartbeats stop; watch which follower times out first.
note[it] 8s Il leader n1 cade. Gli heartbeat cessano; osserva quale follower va in timeout per primo.
note 12s A new leader rules a higher term with 4 of 5 members: a majority is enough.
note[it] 12s Un nuovo leader governa un term più alto con 4 membri su 5: basta la maggioranza.
at 16s node n1 up
note 16s The old leader restarts with its stored term; the first heartbeat of the new term makes it a follower.
note[it] 16s Il vecchio leader riparte con il term salvato; il primo heartbeat del nuovo term lo rende follower.
duration 22s
expect n1 raft.is_leader == 0
expect n1 raft.term >= 2
expect n2 raft.term >= 2
expect n1 raft.has_leader == 1
expect n2 raft.has_leader == 1
expect n3 raft.has_leader == 1
expect n4 raft.has_leader == 1
expect n5 raft.has_leader == 1
