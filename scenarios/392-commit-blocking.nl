title  The blocking problem: the coordinator crashes after PREPARE (2PC vs 3PC)
title[it] Il problema del blocco: il coordinatore si guasta dopo PREPARE (2PC e 3PC)
level  3
layer  8
topic  twopc
about  The coordinator crashes right after sending PREPARE, twice. The first time (5 s) the transaction uses three-phase commit. The participants have voted YES and hear nothing more; after a timeout the first of them takes over (the termination protocol), asks the others for their state, finds everybody still uncertain, concludes that nobody can have committed, and aborts for all: no blocking. When the coordinator restarts it does not trust its own log: it asks the participants what happened and follows them.
about
about  The second time (14 s) the transaction uses two-phase commit, which has no such rule: a participant that voted YES cannot know whether the coordinator decided, so it can neither commit nor abort. All three stay BLOCKED, locks held, for as long as the coordinator is down (the outage parameter, up to about 40 s in this lab), until it restarts, reads its log, finds no decision and aborts.
about
about  Try: a longer outage and read "locks were held for" in the log; crash the coordinator later (crash=60ms): by then it has the votes, so under 2PC it has already decided and nobody blocks, and under 3PC the participants are pre-committed and the takeover commits instead of aborting. The escape of 3PC is paid with an assumption: timeouts must mean crashes. Run with crash=45ms cable=down: 40 ms after the PREPARE of t3 the cable of p3 is cut, just before PRE-COMMIT reaches it. p1 and p2 are pre-committed and commit; p3, alone and uncertain, aborts. When the cable returns at 12.5 s p3 is told to commit what it has aborted: atomicity is broken, and twopc.contradicted on p3 counts it.
about[it] Il coordinatore si guasta subito dopo aver inviato PREPARE, due volte. La prima volta (5 s) la transazione usa il three-phase commit. I partecipanti hanno votato YES e non sentono più nulla; dopo un timeout il primo di loro subentra (il protocollo di terminazione), chiede agli altri il loro stato, li trova tutti ancora incerti, conclude che nessuno può aver fatto il commit, e annulla per tutti: nessun blocco. Quando il coordinatore riparte non si fida del proprio log: chiede ai partecipanti che cosa è successo e li segue.
about[it]
about[it] La seconda volta (14 s) la transazione usa il two-phase commit, che non ha una regola simile: un partecipante che ha votato YES non può sapere se il coordinatore ha deciso, quindi non può né concludere né annullare. Tutti e tre restano BLOCCATI, con i lock presi, finché il coordinatore è spento (il parametro outage, fino a circa 40 s in questo laboratorio), finché riparte, legge il suo log, non trova una decisione e annulla.
about[it]
about[it] Prova: un fermo più lungo e leggi "locks were held for" nel registro; un guasto più tardivo (crash=60ms): a quel punto il coordinatore ha i voti, quindi con 2PC ha già deciso e nessuno si blocca, e con 3PC i partecipanti sono in pre-commit e chi subentra conclude invece di annullare. La via d'uscita di 3PC si paga con un'ipotesi: un timeout deve significare un guasto. Esegui con crash=45ms cable=down: 40 ms dopo il PREPARE di t3 il cavo di p3 viene tagliato, appena prima che gli arrivi PRE-COMMIT. p1 e p2 sono in pre-commit e concludono; p3, solo e incerto, annulla. Quando il cavo torna a 12,5 s a p3 viene detto di concludere ciò che ha annullato: l'atomicità è violata, e twopc.contradicted su p3 lo conta.
param outage 5s How long the coordinator stays down in the 2PC case
param[it] outage Per quanto tempo il coordinatore resta spento nel caso 2PC
param+ crash 5ms Delay between PREPARE and the crash
param+[it] crash Ritardo fra PREPARE e il guasto
param+ cable up 3PC case: cable of p3 40 ms after PREPARE (up or down)
param+[it] cable Caso 3PC: cavo di p3 40 ms dopo PREPARE (up o down)

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

note 1s A normal two-phase commit: PREPARE, three YES votes, COMMIT, acknowledgements.
note[it] 1s Un normale two-phase commit: PREPARE, tre voti YES, COMMIT, riscontri.
at 1s   run c twopc begin tx=t1
note 3s A normal three-phase commit: PREPARE, PRE-COMMIT, COMMIT.
note[it] 3s Un normale three-phase commit: PREPARE, PRE-COMMIT, COMMIT.
at 3s   run c twopc begin tx=t2 three=1
note 5s Transaction t3 (3PC): PREPARE leaves and the coordinator crashes. Watch a participant take over and terminate the transaction.
note[it] 5s Transazione t3 (3PC): parte PREPARE e il coordinatore si guasta. Osserva un partecipante subentrare e terminare la transazione.
at 5s   run c twopc begin tx=t3 three=1
at 5s   at $crash node c down
at 5s   at 40ms link p3 s1 $cable
at 12500ms link p3 s1 up
at 11s  node c up
note 11s The coordinator is back: it asks the participants what became of t3 and adopts their decision.
note[it] 11s Il coordinatore è tornato: chiede ai partecipanti che ne è stato di t3 e adotta la loro decisione.
note 14s Transaction t4 (2PC) with the same crash: the participants vote YES into the void and are BLOCKED until the coordinator returns, $outage later.
note[it] 14s Transazione t4 (2PC) con lo stesso guasto: i partecipanti votano YES nel vuoto e restano BLOCCATI finché il coordinatore non torna, dopo $outage.
at 14s  run c twopc begin tx=t4
at 14s  at $crash node c down
at 14s  at $outage node c up
duration 62s

expect c  twopc.ended == 4
expect c  twopc.decided == 4
expect p1 twopc.decided == 4
expect p2 twopc.decided == 4
expect p3 twopc.decided == 4
expect p1 twopc.contradicted == 0
expect p2 twopc.contradicted == 0
expect p3 twopc.contradicted >= 0
expect p1 twopc.blocked <= 1
expect p1 twopc.took_over >= 0
