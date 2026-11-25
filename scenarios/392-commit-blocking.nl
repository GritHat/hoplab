title  The blocking problem: the coordinator crashes after PREPARE (2PC vs 3PC)
title[it] Il problema del blocco: il coordinatore si guasta dopo PREPARE (2PC e 3PC)
level  3
layer  8
topic  twopc
about  The coordinator crashes right after sending PREPARE, twice. The first time the transaction uses two-phase commit: the participants have voted YES, so they can neither commit nor abort; they stay blocked, locks held, until the coordinator restarts, reads its log, finds no decision and aborts.
about  The second time the transaction uses three-phase commit: a participant that has seen no PRE-COMMIT knows that nobody can have committed, and aborts on its own after a timeout. No blocking, but only because timeouts can be trusted here: with a partition instead of a crash the same rule can split the decision.
about  Try: a longer outage of the coordinator and watch how long the locks are held; crash the coordinator a little later (crash=60ms) so that it has already decided.
about[it] Il coordinatore si guasta subito dopo aver inviato PREPARE, due volte. La prima volta la transazione usa il two-phase commit: i partecipanti hanno votato YES, quindi non possono né concludere né annullare; restano bloccati, con i lock presi, finché il coordinatore riparte, legge il suo log, non trova una decisione e annulla.
about[it] La seconda volta la transazione usa il three-phase commit: un partecipante che non ha visto PRE-COMMIT sa che nessuno può aver fatto il commit, e dopo un timeout annulla da solo. Nessun blocco, ma solo perché qui ci si può fidare dei timeout: con una partizione al posto di un guasto la stessa regola può dividere la decisione.
about[it] Prova: un fermo più lungo del coordinatore per vedere quanto a lungo restano presi i lock; un guasto un po' più tardivo (crash=60ms), quando il coordinatore ha già deciso.
param outage 5s How long the coordinator stays down
param[it] outage Per quanto tempo il coordinatore resta spento
param+ crash 5ms Delay between PREPARE and the crash
param[it] crash Ritardo fra PREPARE e il guasto

node c host role=COORD
node p1 host
node p2 host
node p3 host
node s1 switch
link c s1 delay=5ms
link p1 s1 delay=10ms
link p2 s1 delay=10ms
link p3 s1 delay=10ms
auto all

run c  twopc coordinator participants=p1,p2,p3
run p1 twopc participant coordinator=c
run p2 twopc participant coordinator=c
run p3 twopc participant coordinator=c

note 1s A normal two-phase commit, to compare with what follows.
note[it] 1s Un two-phase commit normale, da confrontare con quello che segue.
at 1s   run c twopc begin tx=t1
note 3s Transaction t2 (2PC): PREPARE leaves and the coordinator crashes at once. The participants vote YES into the void.
note[it] 3s Transazione t2 (2PC): PREPARE parte e il coordinatore si guasta subito. I partecipanti votano YES nel vuoto.
at 3s   run c twopc begin tx=t2
at 3s   at $crash node c down
note 5.1s The participants are BLOCKED: prepared, uncertain, holding locks, asking a coordinator that does not answer.
note[it] 5.1s I partecipanti sono BLOCCATI: pronti, incerti, con i lock presi, a interrogare un coordinatore che non risponde.
at 3s   at $outage node c up
note 9s With the coordinator back, its log has no decision for t2: presumed abort, and the participants are released.
note[it] 9s Tornato il coordinatore, nel suo log non c'è una decisione per t2: presumed abort, e i partecipanti sono liberati.
note 12s A normal three-phase commit: PREPARE, PRE-COMMIT, COMMIT.
note[it] 12s Un three-phase commit normale: PREPARE, PRE-COMMIT, COMMIT.
at 12s  run c twopc begin tx=t3 three=1
note 14s Transaction t4 (3PC) with the same crash: after the timeout the participants abort on their own.
note[it] 14s Transazione t4 (3PC) con lo stesso guasto: dopo il timeout i partecipanti annullano da soli.
at 14s  run c twopc begin tx=t4 three=1
at 14s  at $crash node c down
at 14s  at $outage node c up
duration 22s

expect p1 twopc.blocked == 1
expect p2 twopc.blocked == 1
expect p1 twopc.committed == 2
expect p1 twopc.aborted == 2
expect p3 twopc.aborted == 2
expect c  twopc.committed == 2
expect c  twopc.aborted == 2
