<!-- level: 3 -->
<!-- labs: 391-two-phase-commit, 392-commit-blocking -->
# Commit atomico: two-phase commit, blocco e three-phase commit

Una transazione distribuita aggiorna dati custoditi da più basi di dati indipendenti, e deve essere **atomica**: o tutte ne eseguono il commit, o nessuna. Un bonifico che addebita una banca e non accredita mai l'altra è il guasto da evitare.

Il problema somiglia al consenso ma ha una particolarità che per un aspetto lo rende più difficile. Ogni partecipante ha un diritto di veto: ciascuno può non essere in grado di fare il commit (un vincolo violato, un deadlock, un disco pieno), quindi l'esito non si può decidere a maggioranza; il "commit" richiede l'unanimità. E ogni sito è autonomo finché non promette: prima di promettere può annullare da solo, dopo aver promesso deve poter andare in entrambe le direzioni, anche attraverso un crash, finché qualcuno non gli comunica l'esito.

L'idea chiave del **two-phase commit** è quella promessa, resa durevole. Nella prima fase un coordinatore chiede a tutti di prepararsi; un partecipante che vota YES scrive prima su memoria stabile tutto ciò che serve per fare il commit o per annullare in seguito, e con ciò rinuncia al diritto di decidere. Nella seconda fase il coordinatore, da solo, decide, scrive la decisione nel proprio log (quella scrittura è l'istante in cui la transazione fa il commit) e la annuncia. Il protocollo è sicuro rispetto a qualunque crash o perdita di messaggi. Il suo punto debole è la **finestra di incertezza** fra un voto YES e la decisione: se il coordinatore si guasta lì, i partecipanti devono aspettare, tenendo i propri lock. Eliminare quel blocco è possibile solo con ipotesi più forti oppure con il consenso.

<!-- detail: 2 -->
## Algoritmi e modelli

**Specifica.** Ogni processo vota YES o NO e decide COMMIT o ABORT.

- **Agreement**: due processi non decidono mai in modo diverso, e una decisione non viene mai revocata.
- **Validity**: COMMIT solo se tutti hanno votato YES; se tutti hanno votato YES e nulla si guasta, la decisione è COMMIT.
- **Termination**: se i guasti prima o poi vengono riparati, ogni processo prima o poi decide. Il protocollo è **non bloccante** se i processi corretti decidono anche mentre altri restano guasti.

**Two-phase commit con logging.** I record indicati con `force` devono essere su memoria stabile prima che parta il messaggio successivo.

```
coordinatore                       partecipante
log BEGIN(partecipanti)
invia PREPARE a tutti      ---->   se può:     force-log READY;  invia YES
                                   altrimenti: log ABORT;        invia NO
raccoglie i voti (timeout => NO)
tutti YES:  force-log COMMIT   <== punto di commit
altrimenti: log ABORT
invia la decisione a tutti ---->   force-log decisione; applica; libera i lock
                           <----   invia ACK
tutti gli ACK: log END  (fino ad allora ripete la decisione)
```

Costo nel caso senza guasti per `n` partecipanti: `4n` messaggi, tre ritardi di messaggio finché i partecipanti sanno, `2n + 1` scritture forzate sul log.

**Timeout e ripristino.** Ciò che un processo può fare dipende solo da ciò che ha scritto nel log:

```
partecipante, nulla nel log, nessun PREPARE:  annulla unilateralmente
partecipante, READY nel log, nessuna decisione:  INCERTO: chiede e aspetta
                                                 (bloccato)
partecipante, decisione nel log:              la riesegue, ACK
coordinatore, in attesa dei voti:             decide ABORT
coordinatore, riavvio, nessuna decisione:     ABORT   (presumed abort)
coordinatore, riavvio, decisione nel log:     la invia di nuovo
```

**La finestra di blocco.** Un partecipante incerto non può annullare (il coordinatore può aver scritto COMMIT e averlo detto a qualcuno) e non può fare il commit (qualcuno può aver votato NO). Chiedere agli altri partecipanti aiuta solo se uno di loro conosce la decisione o non ha ancora votato. Se il coordinatore e un partecipante sono entrambi guasti, gli altri non possono escludere che il partecipante guasto abbia già fatto il commit: nessun protocollo di terminazione può sbloccarli.

**Three-phase commit.** Si inserisce un round in modo che nessun processo possa fare il commit mentre un altro è ancora incerto:

```
PREPARE / voti  ->  PRE-COMMIT / riscontri  ->  COMMIT
il coordinatore tace: un partecipante subentra, chiede agli altri lo stato e decide
   qualcuno ha ANNULLATO o non ha votato     => ABORT
   qualcuno ha fatto il COMMIT               => COMMIT
   qualcuno in PRE-COMMIT, nessuno ha deciso => PRE-COMMIT a tutti, poi COMMIT
   tutti incerti (hanno solo votato YES)     => ABORT: nessuno può aver fatto il commit
```

Le regole sono sicure solo se un timeout dimostra un crash: rete sincrona, guasti per crash, nessuna partizione. Costo: `6n` messaggi, cinque ritardi.

**Esempio svolto (i laboratori).** I collegamenti sono da 5 ms e 10 ms, quindi un hop coordinatore–partecipante vale 15 ms. PREPARE a `t`, voti di ritorno a `t + 30`, COMMIT ricevuto a `t + 45`, ACK a `t + 60` ms: tre ritardi per il commit. Nel laboratorio 392 il coordinatore si guasta 5 ms dopo PREPARE e torna dopo 5 s: i partecipanti votano a `t + 15 ms`, si dichiarano bloccati dopo due timeout (2 s) e rilasciano i lock solo a `t + 5,045 s`. Il tempo di lock è pari alla durata del guasto, per quanto lunga.

<!-- detail: 3 -->
## In profondità

**Perché 2PC è sicuro.** La decisione è un unico fatto durevole in un unico sito, il record COMMIT del coordinatore; ogni altra azione è funzione dello stato nel log e può essere ripetuta. I messaggi sono idempotenti e vengono ritrasmessi. Un partecipante non decide mai COMMIT senza la parola del coordinatore, e il coordinatore non scrive mai COMMIT senza tutti i voti YES, ciascuno sostenuto da un record READY che sopravvive a un crash.

**Perché il blocco è fondamentale.** Teorema di Skeen: un protocollo di commit è non bloccante (in presenza di soli guasti di sito) se e solo se non esiste uno stato in cui un sito possa trovarsi mentre un altro ha fatto il commit e un terzo è ancora incerto. 2PC viola la condizione; 3PC la soddisfa aggiungendo lo stato cuscinetto. Ma con perdite di messaggi o partizioni il commit atomico non bloccante è impossibile, e in un sistema puramente asincrono è irrisolvibile anche con un solo crash, come il consenso. 3PC mostra il guasto concretamente: se PRE-COMMIT raggiunge alcuni partecipanti e una partizione isola gli altri, il primo gruppo va in timeout e fa il commit, il secondo va in timeout e annulla.

**Relazione con il consenso.** Il commit atomico non è il consenso (la sua regola di validità dipende da tutti i voti e i guasti lo sbilanciano verso ABORT), ma il rimedio è il consenso: rendere tollerante ai guasti la decisione stessa. In Paxos Commit (Gray e Lamport) il voto di ogni partecipante è registrato da un'istanza di Paxos su `2F + 1` acceptor; la transazione fa il commit quando ogni istanza ha scelto "prepared"; si tollerano `F` guasti qualsiasi e il 2PC classico è il caso particolare `F = 0`. In modo equivalente, si replica il log del coordinatore con Raft.

**Che cosa fanno i sistemi reali.** I transaction manager XA realizzano 2PC con presumed abort (nessun record di abort forzato, nessun ACK per gli abort) o presumed commit, e un'ottimizzazione read-only che lascia uscire dopo la prima fase i partecipanti senza aggiornamenti; gli operatori risolvono a mano le transazioni in dubbio (decisioni "euristiche", che possono violare l'atomicità). Spanner e CockroachDB eseguono 2PC fra shard i cui partecipanti e coordinatore sono ciascuno un gruppo Paxos o Raft, così che il guasto di una singola macchina non blocca. Altri progetti evitano il commit distribuito: ordinamento deterministico prima dell'esecuzione (Calvin), oppure saghe con azioni di compensazione quando l'isolamento si può allentare. 3PC non è praticamente mai usato in produzione.

**In netlab.** Il modulo `twopc` scrive READY nel log prima di votare e la decisione prima di annunciarla, ripete PREPARE alcune volte entro il timeout, rinvia la decisione finché non è riscontrata e risponde alle richieste dei partecipanti in dubbio. Un partecipante 2PC si dichiara bloccato dopo due timeout. Con `three=1` i partecipanti eseguono il protocollo di terminazione: il primo a cui scade il timer subentra, raccoglie gli stati e applica le regole qui sopra; un coordinatore riavviato compie la stessa indagine invece di fidarsi del proprio log. Il contatore `twopc.contradicted` rivela un partecipante a cui è stato detto il contrario di ciò che ha deciso.

**Letteratura primaria.**

- J. Gray, "Notes on Data Base Operating Systems", 1978 (two-phase commit).
- B. Lampson, H. Sturgis, "Crash Recovery in a Distributed Data Storage System", 1979.
- D. Skeen, "Nonblocking Commit Protocols", SIGMOD 1981.
- D. Skeen, M. Stonebraker, "A Formal Model of Crash Recovery in a Distributed System", 1983.
- C. Mohan, B. Lindsay, R. Obermarck, "Transaction Management in the R* Distributed Database Management System", 1986 (presumed abort e presumed commit).
- P. Bernstein, V. Hadzilacos, N. Goodman, "Concurrency Control and Recovery in Database Systems", 1987, capitolo 7.
- J. Gray, L. Lamport, "Consensus on Transaction Commit", 2006.
- J. Corbett et al., "Spanner: Google's Globally-Distributed Database", OSDI 2012.

<!-- detail: 1 -->
## In laboratorio

**391-two-phase-commit.** `./netlab run 391-two-phase-commit`. Un coordinatore c e i partecipanti p1, p2, p3. Aprire la scheda `twopc` di c e di un partecipante: ciascuna mostra il proprio write-ahead log. Usare la vista di sequenza (`s`) per le due fasi. Tre transazioni: `pay1` fa il commit (misurare i ritardi: voti 30 ms dopo PREPARE, commit noto 45 ms dopo); `pay2` viene annullata perché p2 vota NO; `pay3` parte mentre p3 è spento, quindi il coordinatore aspetta un timeout (1 s) e annulla, poi ripete la decisione ogni secondo finché p3, riavviato a 8 s, la riscontra. In ogni log controllare l'ordine: READY prima del voto, la decisione prima del suo annuncio.

- `loss=0.3` (sul collegamento di p1, da 0,5 s in poi). Prevedere gli esiti possibili per ogni transazione: un PREPARE o un voto perso viene richiesto di nuovo entro il timeout e diventa un ABORT solo se continua a mancare; una decisione o un ACK perso provoca soltanto "sending it again". Il numero di commit può cambiare da un'esecuzione all'altra; i partecipanti non sono mai in disaccordo: le verifiche chiedono `twopc.ended == 3` su c e `twopc.contradicted == 0` ovunque.
- `timeout=20ms`: più breve dei 30 ms di andata e ritorno. Ogni transazione viene annullata benché nessuno si sia guastato. Enunciare la regola per scegliere il timeout.
- In un laboratorio in esecuzione: `:` poi `p1 twopc vote=no` e `c twopc begin tx=x1`; poi guastare un partecipante dopo che ha votato (pausa con spazio, avanzamento con `.`, selezionarlo e premere `k`) e leggere che cosa fa al riavvio.

**392-commit-blocking.** `./netlab run 392-commit-blocking`. Dopo un normale 2PC (`t1`) e un normale 3PC (`t2`), la transazione `t3` (3PC) perde il coordinatore 5 ms dopo PREPARE: due secondi più tardi p1 annuncia nel registro la terminazione 3PC ("the coordinator is silent, I take over"), trova tutti incerti e annulla per tutti; a 11 s il coordinatore riparte, interroga i partecipanti e adotta la loro decisione. La transazione `t4` (2PC, 14 s) ripete il guasto: i partecipanti stampano "BLOCKED — voted yes and heard nothing" e interrogano il coordinatore ogni secondo finché non riparte dopo `outage`, non trova una decisione e annulla; il registro riporta "unblocked; locks were held for 5.0 s" (contatore `twopc.blocked`).

- `outage=20s` (fino a circa 40 s): prevedere per quanto tempo restano presi i lock della transazione 2PC (quanto il fermo) e osservare che quella 3PC si è chiusa in circa tre secondi qualunque sia il fermo.
- `crash=60ms`: ora il coordinatore si guasta dopo aver ricevuto i voti. Previsione: `t4` si conclude con il commit senza blocco (la decisione era già stata annunciata); in `t3` i partecipanti sono in pre-commit e chi subentra conclude con il commit invece di annullare. Poi `crash=20ms`: i voti arrivano a un coordinatore morto; 2PC si blocca come prima.
- Rompere 3PC con una partizione: `crash=45ms cable=down`. 40 ms dopo il PREPARE di `t3` il cavo di p3 viene tagliato, appena prima che gli arrivi PRE-COMMIT. Previsione: p1 e p2 sono in pre-commit e concludono; p3, solo e incerto, annulla; quando il cavo torna p3 registra "ATOMICITY VIOLATED" e `twopc.contradicted` vale 1 su p3. Una partizione non è un guasto.

## Verifica

- Per ogni stato di un partecipante 2PC e del coordinatore, dare l'azione al timeout e al riavvio, e giustificarla a partire dal contenuto del log. Quali scritture sul log devono essere forzate, e che cosa si rompe se il record READY viene scritto in modo pigro?
- Descrivere con precisione la combinazione di guasti in cui i partecipanti superstiti di 2PC non possono terminare nemmeno consultandosi fra loro.
- Enunciare la condizione di Skeen per il commit non bloccante e mostrare che 3PC la soddisfa mentre 2PC no. Poi dare un'esecuzione in cui 3PC viola l'agreement, e indicare l'ipotesi che è stata infranta.
- Confrontare commit atomico e consenso: specifica, significato del voto di un processo guasto, risolvibilità nei sistemi asincroni. Come usa il consenso Paxos Commit, e perché 2PC ne è il caso F = 0?
- Un sistema esegue 2PC su shard replicati ciascuno con Raft. Quali guasti bloccano ancora una transazione, e per quanto tempo?
