title  Two-phase commit: all or nothing across three databases
title[it] Two-phase commit: tutto o niente su tre database
level  3
layer  8
topic  twopc
about  A coordinator and three participants run three transactions. The first is voted YES by everybody and commits. In the second one participant votes NO: nobody commits. During the third a participant is down: its vote never arrives, the coordinator times out and aborts.
about  Watch the two phases in the log (PREPARE and votes, then the decision and the acknowledgements) and the write-ahead log in each node's panel: READY is written before voting, the decision before it is announced.
about  Try: a lossy link (loss=0.3): a lost PREPARE or vote turns into a timeout and an abort, a lost decision is repeated until everybody acknowledges (the outcome counts change, never the agreement); a timeout shorter than the round trip, which aborts transactions that could have committed.
about[it] Un coordinatore e tre partecipanti eseguono tre transazioni. Nella prima tutti votano YES e si fa il commit. Nella seconda un partecipante vota NO: nessuno fa il commit. Durante la terza un partecipante è spento: il suo voto non arriva, il coordinatore va in timeout e annulla.
about[it] Osserva le due fasi nel registro (PREPARE e voti, poi la decisione e i riscontri) e il write-ahead log nel pannello di ogni nodo: READY viene scritto prima di votare, la decisione prima di annunciarla.
about[it] Prova: un collegamento con perdite (loss=0.3): un PREPARE o un voto perso diventa un timeout e un annullamento, una decisione persa viene ripetuta finché tutti la riscontrano (cambiano i conteggi degli esiti, mai l'accordo); un timeout più corto del tempo di andata e ritorno, che annulla transazioni che avrebbero potuto concludersi.
param loss 0 Packet loss on the link of p1
param[it] loss Perdita di pacchetti sul collegamento di p1
param+ timeout 1s Timeout of coordinator and participants
param[it] timeout Timeout di coordinatore e partecipanti

node c host role=COORD
node p1 host
node p2 host
node p3 host
node s1 switch
link c s1 delay=5ms
link p1 s1 delay=10ms loss=$loss
link p2 s1 delay=10ms
link p3 s1 delay=10ms
auto all

run c  twopc coordinator participants=p1,p2,p3 timeout=$timeout
run p1 twopc participant coordinator=c timeout=$timeout
run p2 twopc participant coordinator=c timeout=$timeout
run p3 twopc participant coordinator=c timeout=$timeout

note 1s Transaction pay1: PREPARE to all, three YES votes, COMMIT.
note[it] 1s Transazione pay1: PREPARE a tutti, tre voti YES, COMMIT.
at 1s   run c twopc begin tx=pay1
note 3s Transaction pay2: p2 cannot do its part and votes NO. One NO is enough: ABORT for everybody.
note[it] 3s Transazione pay2: p2 non può fare la sua parte e vota NO. Basta un NO: ABORT per tutti.
at 2.9s run p2 twopc vote=no
at 3s   run c twopc begin tx=pay2
at 4.5s run p2 twopc vote=yes
note 5s p3 crashes. Transaction pay3: its vote never comes, the coordinator waits for the timeout and aborts.
note[it] 5s p3 si guasta. Transazione pay3: il suo voto non arriva, il coordinatore attende il timeout e annulla.
at 5s   node p3 down
at 5.5s run c twopc begin tx=pay3
note 8s p3 is back. The coordinator is still repeating the decision, so p3 learns about it and acknowledges.
note[it] 8s p3 è tornato. Il coordinatore sta ancora ripetendo la decisione, quindi p3 la apprende e la riscontra.
at 8s   node p3 up
duration 12s

expect c  twopc.committed == 1
expect c  twopc.aborted == 2
expect p1 twopc.committed == 1
expect p1 twopc.aborted == 2
expect p2 twopc.aborted == 2
expect p3 twopc.committed == 1
expect p3 twopc.aborted == 1
expect p1 twopc.blocked == 0
