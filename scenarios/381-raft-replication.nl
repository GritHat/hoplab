title  Raft: replicating the log, committing, and catching up after a crash
title[it] Raft: replicare il log, fare commit e recuperare dopo un guasto
level  3
layer  8
topic  raft
about  The leader (n1, whose first election timeout is shortened so that it wins) receives client commands, appends them to its log and replicates them with AppendEntries. An entry is committed when a majority stores it; only then is it applied to the key-value state machine and acknowledged. Commands submitted to a follower are forwarded to the leader. At 6 s follower n4 crashes: the cluster keeps committing with 4 of 5. At 12 s n4 restarts with its old log and the leader walks it forward until it holds every entry.
about  Watch: the log cells in each raft panel (blue = stored but not committed, green = applied), nextIndex / matchIndex in the leader's panel, and how n4 fills its log after the restart. Try: `n3 raft put color=red`, `n5 raft get color`; crash three nodes and submit a command: it stays blue forever.
about[it] Il leader (n1, il cui primo timeout di elezione è accorciato così da vincere) riceve i comandi dei client, li aggiunge al proprio log e li replica con AppendEntries. Una voce è committed quando è memorizzata dalla maggioranza; solo allora viene applicata alla macchina a stati chiave-valore e confermata. I comandi inviati a un follower sono inoltrati al leader. A 6 s il follower n4 cade: il cluster continua a fare commit con 4 nodi su 5. A 12 s n4 riparte con il vecchio log e il leader lo porta avanti finché non possiede ogni voce.
about[it] Da osservare: le celle del log in ogni pannello raft (blu = memorizzata ma non committed, verde = applicata), nextIndex / matchIndex nel pannello del leader, e come n4 riempie il log dopo il riavvio. Da provare: `n3 raft put color=red`, `n5 raft get color`; far cadere tre nodi e inviare un comando: resta blu per sempre.
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

run n1 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat first=1
run n2 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n3 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n4 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat
run n5 raft peers=n1,n2,n3,n4,n5 election=$election heartbeat=$heartbeat

note 0s n1 will be elected: its first election timeout is shortened.
note[it] 0s Verrà eletto n1: il suo primo timeout di elezione è accorciato.
at 2s run n1 raft put x=1
note 2s First command, sent to the leader: appended, replicated, committed after a majority answers.
note[it] 2s Primo comando, inviato al leader: aggiunto, replicato, committed quando risponde la maggioranza.
at 3500ms run n3 raft put y=2
note 3500ms A command sent to follower n3 is forwarded to the leader.
note[it] 3500ms Un comando inviato al follower n3 viene inoltrato al leader.
at 5s run n2 raft put x=3
at 6s node n4 down
note 6s Follower n4 crashes. Four members remain: still a majority, the cluster does not even slow down.
note[it] 6s Il follower n4 cade. Restano quattro membri: è ancora la maggioranza, il cluster non rallenta nemmeno.
at 7s run n1 raft put z=4
at 8s run n5 raft put y=5
at 9s run n1 raft put w=6
at 12s node n4 up
note 12s n4 restarts with the log it had saved. The leader finds the point where they agree and sends what is missing.
note[it] 12s n4 riparte con il log che aveva salvato. Il leader trova il punto in cui concordano e invia ciò che manca.
at 15s run n4 raft get y
duration 18s
expect n1 raft.is_leader == 1
expect n1 raft.commit_index == 7
expect n4 raft.reads_ok == 1
expect n1 raft.applied == 7
expect n2 raft.applied == 7
expect n3 raft.applied == 7
expect n4 raft.applied == 7
expect n5 raft.applied == 7
expect n1 raft.state_hash == 80500
expect n2 raft.state_hash == 80500
expect n3 raft.state_hash == 80500
expect n4 raft.state_hash == 80500
expect n5 raft.state_hash == 80500
