title  Raft under a network partition: no split brain
title[it] Raft durante una partizione di rete: nessuno split brain
level  4
layer  8
topic  raft
about  Two racks joined by one link: n1 and n2 on switch sa, n3 n4 n5 on switch sb. n1 is elected (shorter timeout). At 5 s the inter-rack link fails. The old leader n1 keeps believing it leads, accepts a command, but with only 2 of 5 members it can never commit it. The majority side elects a new leader in a higher term and keeps serving. For a while two nodes call themselves leader, yet only one can commit: this is why Raft has no split brain. At 14 s the link is repaired: n1 sees the higher term and steps down, and the uncommitted entry of n1 and n2 is overwritten by the new leader's log. In the end all five state machines are identical.
about  Watch: the term and role chips on both sides, the blue (never committed) cell on n1 and n2, the "discarding uncommitted entries" line after the heal. Try prevote=1 and move the old leader to the majority side (crash it instead): a lone follower no longer inflates its term. Question: a client was told nothing about `x=minority`; what would go wrong if n1 had answered "ok" before committing?
about[it] Due rack uniti da un solo collegamento: n1 e n2 sullo switch sa, n3 n4 n5 sullo switch sb. Viene eletto n1 (timeout più corto). A 5 s il collegamento tra i rack si guasta. Il vecchio leader n1 continua a credersi leader, accetta un comando, ma con soli 2 membri su 5 non potrà mai farne il commit. Il lato di maggioranza elegge un nuovo leader in un term più alto e continua a servire. Per un po' due nodi si dichiarano leader, ma solo uno può fare commit: ecco perché Raft non ha split brain. A 14 s il collegamento viene riparato: n1 vede il term più alto e si dimette, e la voce non committed di n1 e n2 viene sovrascritta dal log del nuovo leader. Alla fine le cinque macchine a stati sono identiche.
about[it] Da osservare: term e ruolo sui due lati, la cella blu (mai committed) su n1 e n2, la riga "discarding uncommitted entries" dopo la riparazione. Da provare prevote=1 facendo cadere il vecchio leader invece di isolarlo: un follower isolato non gonfia più il proprio term. Domanda: al client non è stato confermato `x=minority`; cosa andrebbe storto se n1 avesse risposto "ok" prima del commit?
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

run n1 raft peers=n1,n2,n3,n4,n5 election=400-500ms heartbeat=$heartbeat prevote=$prevote
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
at 6s run n1 raft put x=minority
note 6s n1 still thinks it leads and appends x=minority; only n2 can store it: 2 of 5 is not a majority, it stays uncommitted.
note[it] 6s n1 si crede ancora leader e aggiunge x=minority; solo n2 può memorizzarlo: 2 su 5 non è maggioranza, resta non committed.
note 8s The majority side has elected its own leader in a higher term. Two "leaders" exist, but only one can commit.
note[it] 8s Il lato di maggioranza ha eletto un proprio leader in un term più alto. Esistono due "leader", ma solo uno può fare commit.
at 10s run n3 raft put y=2
at 11s run n4 raft put x=majority
at 14s link sa sb up
note 14s The link is back. n1 hears a higher term, steps down, and the conflicting entry on n1 and n2 is replaced.
note[it] 14s Il collegamento torna. n1 sente un term più alto, si dimette, e la voce in conflitto su n1 e n2 viene sostituita.
at 19s run n1 raft get x
duration 22s
expect n1 raft.is_leader == 0
expect n1 raft.truncated >= 1
expect n2 raft.truncated >= 1
expect n3 raft.leaders == 1
expect n3 raft.agree == 1
expect n1 raft.applied >= 4
