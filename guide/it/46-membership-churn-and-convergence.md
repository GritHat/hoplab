<!-- level: 4 -->
<!-- labs: 482-swim-failure-detection, 483-gossip-partition, 490-chord-churn, 491-crdt-partition -->
# Membership, churn e convergenza

Il consenso compra l'accordo al prezzo della disponibilità. Un'ampia famiglia di sistemi fa lo scambio opposto: ogni nodo continua a lavorare con ciò che sa, l'informazione si diffonde per scambi casuali a coppie, e al sistema si chiede soltanto di **convergere** quando le perturbazioni cessano. Liste dei membri, tabelle di instradamento delle reti overlay e dati replicati rientrano tutti in questo schema. Il prezzo è che, fra una perturbazione e l'altra, i nodi hanno viste diverse, e la garanzia non dice quanto diverse né per quanto tempo.

Quattro domande attraversano l'argomento. **Si può dichiarare morto un nodo senza un quorum, e quanto spesso il verdetto è sbagliato?** Un rilevatore di guasti in una rete asincrona non può essere insieme rapido e accurato, quindi il problema di progetto è la forma del compromesso. **Che cosa ripara una partizione dopo che si è ricomposta**, quando il protocollo che ha diffuso le notizie tace da tempo? **Quanto churn può assorbire un overlay strutturato** prima che la riparazione periodica resti indietro e le ricerche sbaglino? E **che cosa significa "convergere" per i dati**: ottenere repliche tutte uguali è facile buttando via aggiornamenti, e la parte interessante è quali aggiornamenti sopravvivono.

Ciascuno dei quattro laboratori isola una di queste domande su un numero di nodi compreso fra otto e undici. È abbastanza per vedere ogni meccanismo, e troppo poco per misurare una qualsiasi legge di scala.

<!-- detail: 2 -->
## SWIM e Lifeguard

SWIM separa il rilevamento dei guasti dalla disseminazione. A ogni periodo di protocollo un membro invia un ping a un pari scelto a caso. In mancanza di ack chiede ad altri k membri di sondare il bersaglio per suo conto (**sondaggio indiretto**), il che filtra un guasto del singolo percorso fra chi sonda e il bersaglio. Se fallisce anche questo, il bersaglio non è dichiarato morto ma **sospettato**, e il sospetto viene diffuso. Ogni membro porta un **numero di incarnazione** che solo lui può incrementare: venendo a sapere di essere sospettato all'incarnazione i, annuncia alive a i+1, e ciò annulla il sospetto ovunque. Solo un sospetto che sopravvive al proprio timeout diventa una notifica di morte. Gli aggiornamenti viaggiano in piggyback sui ping e sugli ack stessi, ciascuno ritrasmesso un numero di volte logaritmico nella dimensione del gruppo (**disseminazione di tipo epidemico**). Il carico per membro è costante, il tempo atteso di rilevamento non dipende dalla dimensione del gruppo, e il tasso di falsi positivi si regola con k e con il timeout di sospetto.

Il punto debole è il membro lento: un nodo sovraccarico perde gli ack di pari sani, li sospetta, ed elabora in ritardo le loro confutazioni. **Lifeguard** aggiunge la consapevolezza della propria salute: un membro che osserva indizi della propria lentezza (ack mancati, confutazioni che lo riguardano) allunga i propri timeout; i timeout di sospetto partono lunghi e si accorciano all'arrivo di conferme indipendenti; e un membro sospettato viene avvisato tempestivamente perché possa confutare.

## Gossip e partizioni

Il rumor mongering è veloce e si spegne da solo: un nodo smette di propagare un aggiornamento quando abbastanza pari lo conoscevano già. Proprio per questo non può riparare una partizione: quando il collegamento torna, nessuno sta più trasmettendo. L'**anti-entropia** (Demers et al.) è il meccanismo complementare: periodicamente si sceglie un pari a caso e si riconciliano gli stati completi, o loro sintesi. È più lenta e costa traffico per sempre, ma converge da qualunque stato. Per la membership, le due metà si sono dichiarate morte a vicenda; la ricomposizione richiede che qualcuno, di tanto in tanto, contatti ancora i "morti", che allora confutano con un'incarnazione più alta.

## Chord sotto churn

La correttezza di Chord dipende soltanto dai puntatori al successore; le finger servono solo a rendere veloci le ricerche. Ingressi e guasti sono riparati dalla **stabilizzazione** periodica, e ai guasti si sopravvive con una **lista di successori** di lunghezza r: l'anello resta connesso a meno che r nodi consecutivi si guastino entro un intervallo di riparazione, e le chiavi sopravvivono se sono replicate su quei successori. Con churn continuo la questione è se la riparazione tenga il passo. Liben-Nowell, Balakrishnan e Karger definiscono il tempo di dimezzamento (half-life) del sistema e mostrano che ogni nodo deve eseguire un numero di azioni di riparazione dell'ordine di log N per half-life. Rhea e colleghi hanno mostrato sperimentalmente che il comportamento sotto churn è dominato da scelte ingegneristiche: come si calcolano i timeout, recupero reattivo o periodico, come si scelgono i vicini.

## CRDT dopo una partizione

I CRDT basati sullo stato convergono perché il merge è un join su un semireticolo: commutativo, associativo, idempotente. È la **strong eventual consistency**: repliche che hanno ricevuto gli stessi aggiornamenti sono nello stesso stato, senza rollback. Ciò che distingue i tipi è la semantica della concorrenza. L'**OR-Set** etichetta ogni add; una remove elimina solo le etichette che ha osservato, quindi una add concorrente a una remove sopravvive. Il **registro last-writer-wins** conserva la scrittura con il timestamp maggiore e scarta l'altra. Converge, ma "ultima" è deciso da orologi non sincronizzati: una replica con l'orologio in ritardo perde contro scritture precedenti, e l'aggiornamento perduto è silenzioso.

<!-- detail: 3 -->
## Problemi aperti

**Guasti grigi.** I rilevatori in stile SWIM assumono membri che si arrestano per crash e perdite di pacchetti indipendenti. Membri lenti, parzialmente connessi o irraggiungibili solo da alcuni pari producono oscillazioni, e una vista dei membri che non si assesta mai. Lifeguard tratta il caso dell'osservatore lento; la raggiungibilità asimmetrica è in gran parte aperta, e per questa ragione sono ricomparsi servizi di membership che cercano deliberatamente un accordo sulla vista.

**Stato formale degli overlay.** Zave ha mostrato, con una modellazione leggera, che il protocollo di manutenzione di Chord così come pubblicato non preserva il proprio invariante di anello per alcuni interlacciamenti di ingressi e guasti, a un decennio dalla pubblicazione. Quanti protocolli "prima o poi corretti" in esercizio reggerebbero allo stesso esame non è noto.

**Metadati.** I CRDT pagano l'assenza di coordinamento con metadati che crescono con la storia: tombstone, etichette, vettori di versione. I delta-state CRDT riducono ciò che si trasmette, non ciò che si conserva; una garbage collection sicura richiede di sapere che cosa ogni replica ha visto, il che è un problema di membership e reintroduce coordinamento.

**Semantica.** La convergenza non implica che lo stato raggiunto sia ciò che un qualche utente intendeva. Quali invarianti si possano mantenere senza coordinamento, e come esporre alle applicazioni i conflitti residui, è una linea di ricerca attiva.

## Il modello del laboratorio e la realtà

Il modulo `gossip` implementa di SWIM sondaggio, sondaggio indiretto, sospetto, incarnazione e piggyback, con timeout fissi: **niente Lifeguard**, e nessun membro lento, perché un nodo simulato elabora ogni messaggio in tempo nullo. Il contatore `swim.false_positives` deriva dalla conoscenza globale che il simulatore ha di chi è davvero attivo; si noti che durante una partizione conta come falso positivo ogni membro vivo ma irraggiungibile: è il punto di vista di un oracolo, non un errore significativo del rilevatore. La perdita su un collegamento è indipendente per pacchetto, a meno di impostare una lunghezza di burst. Nel laboratorio sulla partizione ogni rumor è una breve stringa e l'"anti-entropia" scambia l'intero insieme dei rumor: dimensione delle sintesi e costo di riconciliazione, le vere preoccupazioni su larga scala, sono invisibili.

Il modulo `chord` usa per default uno **spazio di identificatori a 8 bit**, ricerche ricorsive, controllo di vitalità con ping a ogni turno di stabilizzazione e replicazione sulla lista dei successori. Dieci nodi e cinque chiavi non possono esibire conteggi di salti logaritmici né misurare tassi di fallimento delle ricerche; il churn consiste in due crash e due ingressi programmati, non in un processo con un tasso. Un nodo riavviato ha perso la memoria per scelta di progetto.

Il modulo `crdt` invia l'intero stato a un pari casuale per turno, conserva le tombstone per sempre e contiene al più poche decine di elementi. L'errore di orologio è uno scostamento costante (`skew`) su una replica: nessuna deriva, nessun orologio logico ibrido, nessuna causalità per scrittura. L'anomalia del registro che mostra è reale; la sua frequenza in un sistema reale dipende dalla qualità degli orologi e dalla concorrenza delle scritture, che il laboratorio non modella.

## Letteratura primaria

- A. Demers et al., "Epidemic Algorithms for Replicated Database Maintenance", ACM PODC, 1987.
- T. Chandra, S. Toueg, "Unreliable Failure Detectors for Reliable Distributed Systems", Journal of the ACM, 1996.
- R. van Renesse, Y. Minsky, M. Hayden, "A Gossip-Style Failure Detection Service", Middleware, 1998.
- R. Karp, C. Schindelhauer, S. Shenker, B. Vöcking, "Randomized Rumor Spreading", IEEE FOCS, 2000.
- I. Stoica, R. Morris, D. Karger, M. F. Kaashoek, H. Balakrishnan, "Chord: A Scalable Peer-to-peer Lookup Service for Internet Applications", ACM SIGCOMM, 2001.
- A. Das, I. Gupta, A. Motivala, "SWIM: Scalable Weakly-consistent Infection-style Process Group Membership Protocol", IEEE DSN, 2002.
- D. Liben-Nowell, H. Balakrishnan, D. Karger, "Analysis of the Evolution of Peer-to-Peer Systems", ACM PODC, 2002.
- S. Rhea, D. Geels, T. Roscoe, J. Kubiatowicz, "Handling Churn in a DHT", USENIX Annual Technical Conference, 2004.
- N. Hayashibara, X. Défago, R. Yared, T. Katayama, "The φ Accrual Failure Detector", IEEE SRDS, 2004.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-value Store", ACM SOSP, 2007.
- M. Shapiro, N. Preguiça, C. Baquero, M. Zawirski, "Conflict-free Replicated Data Types", SSS, 2011.
- P. Zave, "Using Lightweight Modeling to Understand Chord", ACM SIGCOMM Computer Communication Review, 2012.
- S. Kulkarni, M. Demirbas, D. Madappa, B. Avva, M. Leone, "Logical Physical Clocks", OPODIS, 2014.
- P. S. Almeida, A. Shoker, C. Baquero, "Delta State Replicated Data Types", Journal of Parallel and Distributed Computing, 2018.
- A. Dadgar, J. Phillips, J. Currey, "Lifeguard: Local Health Awareness for More Accurate Failure Detection", IEEE/IFIP DSN Workshops, 2018.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 482-swim-failure-detection.** `./netlab run 482-swim-failure-detection`: otto membri, n8 dietro un collegamento con perdite, n3 va in crash a 8 s e rientra a 22 s. Parametri: `loss`, `period`, `suspect`, `helpers`. Contatori: `swim.detected`, `swim.false_positives`, `swim.refutations`, `swim.incarnation`, `swim.alive`, `gossip.messages`. Si usino più semi.

- **Esperimento 1.** Ipotesi: i sondaggi indiretti riducono i sospetti, e il timeout di sospetto trasforma quelli residui in confutazioni anziché in morti. Manipolazione: `helpers=3` contro `helpers=0` con la perdita di default. Metrica: `swim.refutations` e `swim.incarnation` su n8, `swim.false_positives` ovunque. Esito atteso: nessun falso positivo in entrambi i casi, ma senza helper n8 deve confutare più volte, con gli helper quasi mai.
- **Esperimento 2.** Ipotesi: i falsi positivi compaiono quando il timeout di sospetto è breve rispetto al tempo che una confutazione richiede in presenza di perdite. Manipolazione: `suspect=1s loss=40%` con `helpers=3` e con `helpers=0`. Metrica: media di `swim.false_positives` per membro. Esito atteso: alcune unità per membro con gli helper, circa il triplo senza.
- **Esperimento 3.** Ipotesi: il tempo di rilevamento scala con il periodo, il traffico con il suo inverso. Manipolazione: `period=200ms` contro `1s`. Metrica: tempo dal crash a 8 s alla prima riga "declares DEAD" ("dichiara MORTO") per n3, e `gossip.messages` su n1. Esito atteso: circa cinque volte i messaggi; tempo di rilevamento dominato da `suspect`, a meno di scalare anche quello.

**Laboratorio 483-gossip-partition.** `./netlab run 483-gossip-partition`: due rack da cinque, separati da 2 s a 14 s; il rumor A nasce da un lato, B dall'altro. Parametri: `mode`, `swim`, `interval`, `suspect`. Contatori: `gossip.infected`, `gossip.messages`, `swim.dead`, `swim.alive`.

- **Esperimento 1.** Ipotesi: il solo rumor mongering non ricompone una partizione. Manipolazione: `mode=push`, `mode=pull`, `mode=pushpull`. Metrica: `gossip.infected` su ogni nodo alla fine. Esito atteso: 1 con push (lo scenario fallisce: è proprio il risultato), 2 con gli altri.
- **Esperimento 2.** Ipotesi: il tempo di riconvergenza dopo la ricomposizione è di pochi turni di anti-entropia, a fronte di un costo di traffico permanente. Manipolazione: `interval=500ms` contro `2s`. Metrica: tempo dai 14 s alla riga "everybody knows" ("tutti sanno") di ciascun rumor; `gossip.messages`. Esito atteso: circa tre turni in entrambi i casi, quindi un tempo quattro volte maggiore con l'intervallo più lento e circa la metà dei messaggi.
- **Esperimento 3.** Ipotesi: la riparazione dei dati e quella della membership sono meccanismi indipendenti. Manipolazione: `swim=0` contro `swim=1`; poi una partizione più lunga, copiando lo scenario. Metrica: `gossip.infected`, `swim.alive` alla fine, e `swim.false_positives` durante la partizione. Esito atteso: i rumor convergono in ogni caso; con SWIM ciascun lato dichiara morti i cinque dell'altro e più tardi li riammette; il contatore dei falsi positivi va interpretato con cautela.

**Laboratorio 490-chord-churn.** `./netlab run 490-chord-churn`: dieci nodi, cinque chiavi, n3 e n10 vanno in crash a 14 s, n11 entra a 21 s, n3 rientra vuoto a 23 s. Parametri: `succlist`, `stabilize`. Contatori: `chord.found`, `chord.notfound`, `chord.failed`, `chord.hops`, `chord.keys`.

- **Esperimento 1.** Ipotesi: i dati si perdono solo quando un proprietario e tutti i detentori delle sue repliche si guastano entro un intervallo di riparazione. Manipolazione: `succlist=1`, `2`, `3` con lo scenario originale; poi copiarlo e cambiare `node n10 down` in `node n5 down`, in modo che i due nodi in crash siano adiacenti sull'anello. Metrica: `chord.notfound`. Esito atteso: lo scenario originale non perde nulla nemmeno con `succlist=1` (i nodi in crash non sono vicini, a dispetto di quanto suggerisce la descrizione); con crash adiacenti `succlist=1` perde tre chiavi e `succlist=2` nessuna.
- **Esperimento 2.** Ipotesi: le ricerche emesse dentro la finestra di riparazione sono ritardate di circa un periodo di stabilizzazione. Manipolazione: in una copia aggiungere `at 14.05s run n6 chord get key=cherry`; eseguire con `stabilize=100ms`, `400ms`, `2s`. Metrica: ritardo fra la richiesta e la riga di risposta, richieste senza risposta. Esito atteso: circa un secondo a 400 ms; a 2 s alcune letture precoci non ricevono mai risposta.
- **Esperimento 3.** Ipotesi: un ingresso sposta solo le chiavi di un arco. Manipolazione: esecuzione di default. Metrica: `chord.keys` su n3 e sul suo successore prima dei 14 s e dopo i 30 s; righe "stored a copy" ("memorizzata una copia") a dettaglio 2. Esito atteso: a n3 vengono riconsegnate le chiavi del suo arco e nient'altro si sposta.

**Laboratorio 491-crdt-partition.** `./netlab run 491-crdt-partition`: tre repliche con un OR-Set e un registro LWW; n3 resta isolato da 3 s fino a `heal`. Parametri: `skew`, `heal`. Contatori: `crdt.size`, `crdt.register`, `crdt.lost`, `crdt.merges`.

- **Esperimento 1.** Ipotesi: l'OR-Set preserva una add concorrente a una remove; il registro scarta in silenzio una scrittura. Manipolazione: esecuzione di default. Metrica: insieme e registro finali su tutte e tre le repliche, `crdt.lost`. Esito atteso: {bread, milk, eggs} ovunque, registro a 30, un aggiornamento perduto segnalato da n1.
- **Esperimento 2.** Ipotesi: quale scrittura sia l'"ultima" è deciso dall'errore di orologio non appena questo supera l'intervallo reale fra le scritture (qui 500 ms). Manipolazione: `skew=-400ms`, `-600ms`, `-3s`. Metrica: registro finale, e quale replica segnala una perdita. Esito atteso: a −400 ms vince 30; da −600 ms la scrittura precedente, 20, vince ovunque e nessuno segnala perdite, perché a perdere è stata la scrittura successiva: le aspettative dello scenario falliscono, e l'anomalia è invisibile al contatore.
- **Esperimento 3.** Ipotesi: la disponibilità durante la partizione è incondizionata, e il tempo di convergenza successivo dipende solo dal periodo di gossip. Manipolazione: `heal=8s` contro `heal=12s`; durante la partizione digitare ulteriori comandi `n3 crdt add X`. Metrica: tempo dalla ricomposizione all'ultima riga di merge ("fusione"); stati finali. Esito atteso: esiti identici e circa un secondo per convergere, qualunque sia la durata della partizione.

## Domande per la discussione

- `swim.false_positives` è calcolato da un oracolo. Si definisca una metrica di falso positivo misurabile in un'installazione reale, e si dica che cosa le sfugge.
- Dopo la ricomposizione di una partizione ciascun lato detiene notifiche di morte per l'altro. Quale proprietà dei numeri di incarnazione garantisce che il gruppo si riunisca, e che cosa si rompe se un membro riavviato riusa una vecchia incarnazione?
- L'invariante di anello di Chord si è rivelato non valido a un decennio dalla pubblicazione. Che cosa si sottoporrebbe a model checking nell'implementazione di questo emulatore, e rispetto a quali interlacciamenti?
- L'esperimento 2 del laboratorio sui CRDT produce un aggiornamento perduto che nessun contatore segnala. Che cosa dovrebbe memorizzare una replica per rilevarlo, e come si chiama quella struttura?
- Add-wins è una scelta. Si indichi un'applicazione per cui remove-wins è la semantica giusta, e una per cui nessuna delle due è accettabile senza coordinamento.
