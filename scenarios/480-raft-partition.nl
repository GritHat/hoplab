title  Raft under a network partition: no split brain
title[it] Raft durante una partizione di rete: nessuno split brain
level  4
layer  8
topic  raft
about  Two racks joined by one link: n1 and n2 on switch sa, n3 n4 n5 on switch sb. n1 is elected (its first timeout is shortened). At 5 s the inter-rack link fails. The old leader n1 still believes it leads and accepts a command, but with only 2 of 5 members it can never commit it. The majority side elects a new leader in a higher term and keeps serving. At 14 s a client asks the minority side for x: a linearizable read is refused there (n1 cannot confirm that a majority still follows it), while the same read with stale=1 returns the old value, the anomaly the check exists to prevent. At 16 s the link is repaired: the minority adopts the majority's leader and the uncommitted entry of n1 and n2 is overwritten by its log. At 18 s a second, smaller failure: follower n2 alone loses its cable for six seconds. In the end all five state machines are identical.
about
about  Watch: the term and role chips on both sides, the blue (never committed) cell on n1 and n2, the two reads at 14 s, the "discarding uncommitted entries" line after the heal. The second failure is about pre-vote. With prevote=0 the isolated follower n2 keeps timing out and raises its term again and again; when its cable returns, that high term deposes a perfectly healthy leader and forces a needless election. With prevote=1 n2 first asks whether it could win, nobody answers, its term stays put and the rejoin is silent: compare the counter raft.futile_elections of n2 and the final term in the two runs. The election range can be raised to about 1500-3000ms before the majority side runs out of time to elect its leader ahead of the commands at 12 s.
about[it] Due rack uniti da un solo collegamento: n1 e n2 sullo switch sa, n3 n4 n5 sullo switch sb. Viene eletto n1 (il suo primo timeout è accorciato). A 5 s il collegamento tra i rack si guasta. Il vecchio leader n1 si crede ancora leader e accetta un comando, ma con soli 2 membri su 5 non potrà mai farne il commit. Il lato di maggioranza elegge un nuovo leader in un term più alto e continua a servire. A 14 s un client chiede x al lato di minoranza: lì una lettura linearizzabile viene rifiutata (n1 non può confermare che una maggioranza lo segua ancora), mentre la stessa lettura con stale=1 restituisce il valore vecchio, l'anomalia che il controllo serve a evitare. A 16 s il collegamento viene riparato: la minoranza adotta il leader della maggioranza e la voce non committed di n1 e n2 viene sovrascritta dal suo log. A 18 s un secondo guasto, più piccolo: il solo follower n2 perde il cavo per sei secondi. Alla fine le cinque macchine a stati sono identiche.
about[it]
about[it] Da osservare: term e ruolo sui due lati, la cella blu (mai committed) su n1 e n2, le due letture a 14 s, la riga "discarding uncommitted entries" dopo la riparazione. Il secondo guasto riguarda il pre-vote. Con prevote=0 il follower isolato n2 continua ad andare in timeout e alza il proprio term più volte; quando il cavo torna, quel term alto depone un leader perfettamente sano e costringe a un'elezione inutile. Con prevote=1 n2 chiede prima se potrebbe vincere, nessuno risponde, il suo term resta fermo e il rientro è silenzioso: confronta il contatore raft.futile_elections di n2 e il term finale nelle due esecuzioni. L'intervallo di elezione si può alzare fino a circa 1500-3000ms prima che al lato di maggioranza manchi il tempo di eleggere il proprio leader entro i comandi a 12 s.
param  election 1000-2000ms Election timeout range
param[it] election Intervallo del timeout di elezione
param+ heartbeat 300ms Leader heartbeat interval
param[it] heartbeat Intervallo degli heartbeat del leader
param+ delay 5ms Link delay
param[it] delay Ritardo dei collegamenti
param+ prevote 0 Pre-vote extension (0/1)
param[it] prevote Estensione pre-vote (0/1)

node sa switch x=30 y=50
node sb switch x=70 y=50
node n1 host x=10 y=20
node n2 host x=10 y=80
node n3 host x=90 y=12
node n4 host x=95 y=50
node n5 host x=90 y=88
link n1 sa delay=$delay
link n2 sa delay=$delay
link n3 sb delay=$delay
link n4 sb delay=$delay
link n5 sb delay=$delay
link sa sb delay=$delay
auto all

run n1 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat first=1 prevote=$prevote
run n2 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat prevote=$prevote
run n3 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat prevote=$prevote
run n4 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat prevote=$prevote
run n5 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat prevote=$prevote

at 2s run n1 raft put x=1
note 2s x=1 is committed by all five members.
note[it] 2s x=1 è committed da tutti e cinque i membri.
at 5s link sa sb down
note 5s Partition! n1 (leader) and n2 are cut off from n3, n4, n5.
note[it] 5s Partizione! n1 (leader) e n2 sono isolati da n3, n4, n5.
at 5100ms run n1 raft put x=minority
note 5100ms n1 still thinks it leads and appends x=minority; only n2 can store it: 2 of 5 is not a majority, it stays uncommitted.
note[it] 5100ms n1 si crede ancora leader e aggiunge x=minority; solo n2 può memorizzarlo: 2 su 5 non è maggioranza, resta non committed.
note 9s The majority side has elected its own leader in a higher term. On the minority side nothing can commit.
note[it] 9s Il lato di maggioranza ha eletto un proprio leader in un term più alto. Sul lato di minoranza nulla può fare commit.
at 12s run n3 raft put y=2
at 13s run n4 raft put x=majority
at 14s run n1 raft get x
at 14s run n1 raft get x stale=1
note 14s Two reads of x on the minority side: the linearizable one is refused, the stale one answers 1 although the cluster now says x=majority.
note[it] 14s Due letture di x sul lato di minoranza: quella linearizzabile è rifiutata, quella stale risponde 1 benché ora il cluster dica x=majority.
at 16s link sa sb up
note 16s The link is back. The minority follows the majority's leader and the conflicting entry on n1 and n2 is replaced.
note[it] 16s Il collegamento torna. La minoranza segue il leader della maggioranza e la voce in conflitto su n1 e n2 viene sostituita.
at 18s link n2 sa down
note 18s Follower n2 alone is cut off. Without pre-vote it becomes a candidate over and over, each time in a higher term.
note[it] 18s Il solo follower n2 resta isolato. Senza pre-vote diventa candidato più e più volte, ogni volta in un term più alto.
at 24s link n2 sa up
note 24s n2 is back. Without pre-vote its inflated term deposes the leader; with pre-vote nothing happens.
note[it] 24s n2 è tornato. Senza pre-vote il suo term gonfiato depone il leader; con il pre-vote non succede nulla.
at 31s run n1 raft get x
note 31s The read refused at 14 s now succeeds on n1: it is forwarded to the leader, which confirms its majority first.
note[it] 31s La lettura rifiutata a 14 s ora riesce su n1: viene inoltrata al leader, che prima conferma la propria maggioranza.
duration 34s
expect n1 raft.term >= 2
expect n1 raft.truncated >= 1
expect n2 raft.truncated >= 1
expect n1 raft.read_failed == 1
expect n1 raft.stale_reads == 1
expect n1 raft.reads_ok == 1
expect n2 raft.no_futile_elections == $prevote
expect n1 raft.state_hash == 71603
expect n2 raft.state_hash == 71603
expect n3 raft.state_hash == 71603
expect n4 raft.state_hash == 71603
expect n5 raft.state_hash == 71603
expect n1 raft.applied >= 5
expect n2 raft.applied >= 5
expect n3 raft.applied >= 5
expect n4 raft.applied >= 5
expect n5 raft.applied >= 5
