<!-- level: 4 -->
<!-- labs: 480-raft-partition, 481-raft-split-votes -->
# Il consenso in presenza di partizioni

Un protocollo di consenso come Raft promette due cose di natura diversa. **Safety**: le repliche non sono mai in disaccordo su una voce confermata (committed), qualunque cosa faccia la rete. **Liveness**: i comandi vengono prima o poi confermati, purché una maggioranza possa comunicare e la rete si comporti abbastanza bene abbastanza a lungo. Il risultato FLP afferma che la seconda promessa non può essere incondizionata in un sistema asincrono, e il teorema CAP che durante una partizione alcune repliche devono rifiutare il servizio se le risposte devono restare coerenti. Nessuno dei due risultati dice per quanto tempo il servizio è indisponibile, quali repliche ne soffrono, o che cosa osservano nel frattempo i client. **Sono queste grandezze, non i teoremi di impossibilità, ciò che un ingegnere sceglie, e le determinano i timeout e alcuni dettagli che l'algoritmo di base lascia aperti.**

L'argomento è ancora vivo perché i guasti in produzione sono raramente netti. Le partizioni sono parziali e asimmetriche, i nodi sono lenti più che morti, gli orologi derivano, e diversi rapporti post-incidente descrivono cluster sicuri eppure indisponibili per ore con una maggioranza connessa. Estensioni come pre-vote, controlli di quorum e lease riparano specifici problemi di liveness e di coerenza delle letture, ciascuna con le proprie ipotesi.

I due laboratori separano le questioni: uno partiziona un cluster di cinque nodi e mostra che due "leader" non fanno uno split brain; l'altro misura per quanto tempo un cluster resta senza leader in funzione del timeout di elezione.

<!-- detail: 2 -->
## CAP, enunciato con cura

Il teorema di Gilbert e Lynch riguarda un singolo registro di lettura e scrittura in una rete asincrona che può perdere un numero arbitrario di messaggi. È impossibile garantire insieme la **coerenza atomica** (linearizzabilità) e la **disponibilità** (ogni richiesta ricevuta da un nodo non guasto ottiene prima o poi una risposta). Il diffuso "se ne scelgono due su tre" è fuorviante: le partizioni non si scelgono, quindi la scelta è fra coerenza e disponibilità, e solo finché dura la partizione. La disponibilità del teorema è inoltre molto forte: ogni nodo deve rispondere, compresi quelli del lato di minoranza. PACELC di Abadi aggiunge il compromesso che esiste anche senza partizioni: latenza contro coerenza.

Raft sceglie la coerenza. Una partizione di minoranza non può né eleggere un leader né confermare; una partizione di maggioranza prosegue. Il cluster nel suo insieme è "disponibile" solo per i client che raggiungono il lato di maggioranza.

## Perché due leader non sono uno split brain

Un leader separato dalla maggioranza non sa di essere stato sostituito. Continua ad accettare comandi e ad aggiungerli al log, ma una voce è confermata solo quando è memorizzata su una maggioranza, quindi nulla di ciò che accetta può essere dato per acquisito. La maggioranza elegge un leader in un term più alto. Quando la partizione si ricompone, il vecchio leader vede il term più alto, si dimette, e il suo suffisso non confermato viene sovrascritto. La safety poggia sull'intersezione dei quorum e sulla restrizione di elezione (il voto è concesso solo a un candidato il cui log è almeno altrettanto aggiornato), non sul fatto che un solo nodo si creda leader.

## Letture

L'argomento dell'intersezione copre le scritture. Un leader deposto che risponde a una lettura dal proprio stato locale restituisce dati obsoleti, violando la linearizzabilità. Esistono tre rimedi. Far passare le letture dal log (corretto, lento). **ReadIndex**: il leader registra il proprio commit index, conferma con un giro di heartbeat di essere ancora leader, poi risponde. **Lease**: il leader assume che nessun altro leader possa esistere fino alla scadenza di un intervallo di tempo, il che rende locali le letture ma dipende da una deriva limitata degli orologi, ipotesi esterna al modello asincrono.

## Timeout di elezione

La disponibilità di Raft dopo il guasto del leader è la somma del tempo di rilevamento e del tempo di elezione. I follower attendono un timeout di elezione casuale; se più timeout scadono a breve distanza, ciascuno vota per sé e il term va sprecato (split vote). L'articolo originale richiede broadcastTime molto minore di electionTimeout molto minore di MTBF, e raccomanda di estrarre il timeout da un intervallo ampio all'incirca quanto il suo estremo inferiore. Un timeout troppo breve rispetto all'intervallo di heartbeat o al ritardo di rete porta i follower a deporre leader sani; un intervallo troppo stretto rispetto al tempo di andata e ritorno dei messaggi rende gli split vote probabili e ripetuti.

## Le perturbazioni e i loro rimedi

Un follower isolato dal leader continua ad andare in timeout e a incrementare il proprio term. Quando si riconnette, il suo term più alto costringe il leader in carica a dimettersi, benché il nodo rientrante non possa vincere (il suo log è arretrato). Il **pre-vote** aggiunge un turno di prova: un nodo incrementa il term solo se una maggioranza dichiara che gli concederebbe il voto, e i nodi che hanno sentito di recente un leader rifiutano. **CheckQuorum** fa dimettere un leader che non riceve notizie da una maggioranza, limitando il tempo in cui un leader obsoleto serve richieste. Con partizioni parziali (A raggiunge B, B raggiunge C, A non raggiunge C) il Raft di base può perdere del tutto la liveness, perché la leadership rimbalza fra nodi che non possono tenere entrambi un quorum.

## Paxos e Viewstamped Replication

Multi-Paxos, Viewstamped Replication e Raft risolvono lo stesso problema con la stessa struttura di quorum. Howard e Mortier individuano la differenza essenziale nell'elezione del leader: Raft elegge soltanto un nodo il cui log è già aggiornato, per cui le voci fluiscono solo dal leader; Paxos lascia vincere qualsiasi nodo, che poi recupera le voci mancanti dal proprio quorum; VR ruota il primario in modo deterministico secondo il numero di vista. Le conseguenze riguardano la liveness e il costo del recupero, non la safety.

<!-- detail: 3 -->
## Problemi aperti

**Liveness con guasti realistici.** Le dimostrazioni di safety sono mature e meccanizzate; la liveness con partizioni parziali, collegamenti instabili, dischi lenti e guasti "grigi" si argomenta caso per caso. Jensen, Howard e Mortier mostrano che pre-vote e CheckQuorum, ciascuno sensato da solo, interagiscono male in alcune partizioni parziali. Alquraan e colleghi hanno riscontrato che la maggior parte dei malfunzionamenti da partizione nei sistemi di produzione richiedeva soltanto una partizione semplice e pochi eventi, e che molti erano catastrofici.

**Timeout.** Il timeout di elezione è una stima statica di una rete dinamica. Timeout adattivi o guidati dalle misure, e la loro interazione con le estensioni neutre rispetto alla safety, non hanno un progetto comunemente accettato.

**Letture e orologi.** I lease comprano latenza con un'ipotesi di sincronia. Come enunciare, sorvegliare e verificare tale ipotesi (come fa Spanner con TrueTime) dipende dall'installazione.

**Riconfigurazione e geo-replicazione** aggiungono ulteriori compromessi di disponibilità (quorum flessibili, varianti senza leader come EPaxos) che l'algoritmo di base non affronta.

## Il modello del laboratorio e la realtà

Il modulo `raft` implementa elezioni, replicazione, restrizione di elezione, no-op a inizio term, persistenza di term, voto e log, e il pre-vote facoltativo. Il suo testo di aiuto elenca ciò che i sistemi reali aggiungono e che qui manca: **compattazione del log, cambio di configurazione, ReadIndex e lease**. `raft get` legge la macchina a stati locale, e avverte che il valore può essere obsoleto solo quando il nodo sa di non essere il leader; il laboratorio può quindi mostrare una lettura obsoleta ma non il suo rimedio. Manca CheckQuorum: un leader isolato resta leader indefinitamente.

I collegamenti sono simmetrici, e i membri si raggiungono attraverso switch, quindi una partizione è sempre netta: due membri comunicano in entrambe le direzioni oppure in nessuna. Le **partizioni asimmetriche e parziali**, i casi in cui la liveness è davvero in gioco, richiedono un'altra topologia (collegamenti punto-punto fra i membri), e la perdita in una sola direzione non è esprimibile affatto. Gli orologi sono perfetti e non c'è ritardo di elaborazione né di disco: un follower risponde in tempo nullo, quindi broadcastTime è puro ritardo di collegamento e i timeout che qui funzionano sono ottimistici per un'installazione reale. I client sono comandi inviati una sola volta, senza ritentativi né sessione: "ciò che vede il client" va ragionato, non misurato.

Un'ulteriore cautela. Con `prevote=1` il nodo configurato con il timeout breve non è più in modo affidabile il primo leader, perché il suo pre-vote iniziale viene rifiutato dai pari i cui timer non sono ancora scaduti. Nel laboratorio 480 guida allora un membro del lato di maggioranza, la minoranza non aggiunge mai nulla al log, e le aspettative dello scenario sul troncamento falliscono: un esperimento diverso da quello descritto.

## Letteratura primaria

- M. Fischer, N. Lynch, M. Paterson, "Impossibility of Distributed Consensus with One Faulty Process", Journal of the ACM, 1985.
- C. Dwork, N. Lynch, L. Stockmeyer, "Consensus in the Presence of Partial Synchrony", Journal of the ACM, 1988.
- B. Oki, B. Liskov, "Viewstamped Replication: A New Primary Copy Method to Support Highly-Available Distributed Systems", ACM PODC, 1988.
- C. Gray, D. Cheriton, "Leases: An Efficient Fault-Tolerant Mechanism for Distributed File Cache Consistency", ACM SOSP, 1989.
- M. Herlihy, J. Wing, "Linearizability: A Correctness Condition for Concurrent Objects", ACM TOPLAS, 1990.
- L. Lamport, "The Part-Time Parliament", ACM Transactions on Computer Systems, 1998; "Paxos Made Simple", ACM SIGACT News, 2001.
- S. Gilbert, N. Lynch, "Brewer's Conjecture and the Feasibility of Consistent, Available, Partition-Tolerant Web Services", ACM SIGACT News, 2002.
- T. Chandra, R. Griesemer, J. Redstone, "Paxos Made Live: An Engineering Perspective", ACM PODC, 2007.
- E. Brewer, "CAP Twelve Years Later: How the 'Rules' Have Changed", IEEE Computer, 2012; D. Abadi, "Consistency Tradeoffs in Modern Distributed Database System Design", IEEE Computer, 2012.
- J. Corbett et al., "Spanner: Google's Globally-Distributed Database", USENIX OSDI, 2012.
- D. Ongaro, J. Ousterhout, "In Search of an Understandable Consensus Algorithm", USENIX ATC, 2014; D. Ongaro, "Consensus: Bridging Theory and Practice", tesi di dottorato, Stanford University, 2014 (pre-vote, lease, ReadIndex).
- P. Bailis, K. Kingsbury, "The Network is Reliable", ACM Queue, 2014.
- A. Alquraan, H. Takruri, M. Alfatafta, S. Al-Kiswany, "An Analysis of Network-Partitioning Failures in Cloud Systems", USENIX OSDI, 2018.
- H. Howard, R. Mortier, "Paxos vs Raft: Have We Reached Consensus on Distributed Consensus?", ACM PaPoC, 2020.
- C. Jensen, H. Howard, R. Mortier, "Examining Raft's Behaviour During Partial Network Failures", ACM HAOC, 2021.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 480-raft-partition.** Eseguire `./netlab run 480-raft-partition`. n1 e n2 sono collegati allo switch sa; n3, n4, n5 a sb. n1 diventa leader, il collegamento sa — sb si guasta a 5 s e torna a 14 s. Parametri: `election`, `heartbeat`, `delay`, `prevote`. Contatori su ogni membro: `raft.term`, `raft.max_term`, `raft.is_leader`, `raft.leaders`, `raft.elections`, `raft.truncated`, `raft.commit_index`, `raft.applied`, `raft.agree`, `raft.state_hash`. Si osservino le etichette di term e ruolo e la cella non confermata su n1 e n2.

- **Esperimento 1 — safety.** Ipotesi: nessuna voce data per confermata va mai perduta, e la voce della minoranza non viene mai confermata. Manipolazione: esecuzione di default; poi, durante la partizione, digitare più volte `n1 raft put z=1` e `n2 raft get x`. Metrica: `raft.commit_index` su n1 fra 5 s e 14 s, `raft.truncated` dopo la ricomposizione, uguaglianza di `raft.state_hash` su tutti e cinque alla fine. Esito atteso: il commit index di n1 è fermo, ogni voce della minoranza è scartata dopo i 14 s, gli hash coincidono.
- **Esperimento 2 — letture obsolete.** Ipotesi: un leader deposto serve dati obsoleti. Manipolazione: esecuzione di default; intorno ai 12 s digitare `n1 raft get x` e `n4 raft get x`. Metrica: i due valori. Esito atteso: n1 risponde 1 mentre la maggioranza ha confermato `majority`: non linearizzabile. Si noti quale risposta porta l'avviso "may be stale" ("può essere vecchio"): quella sbagliata no, perché n1 crede ancora di essere leader. Si discuta che cosa avrebbero fatto in quell'istante ReadIndex e un lease.
- **Esperimento 3 — inflazione del term e pre-vote.** Ipotesi: un follower isolato, al rientro, perturba un cluster sano, e il pre-vote lo impedisce. Manipolazione: copiare lo scenario, eliminarne le righe `at` ed `expect` e aggiungere `at 5s link n3 sb down` e `at 15s link n3 sb up`; eseguire con `prevote=0` e `prevote=1` (si verifichi nel log quale nodo è leader e si isoli un follower). Metrica: `raft.max_term`, `raft.elections` su n3, e l'intervallo senza leader dopo i 15 s. Esito atteso: senza pre-vote n3 arriva al term 7 o 8, alla ricomposizione il leader si dimette e una nuova elezione costa circa mezzo secondo; con il pre-vote il term non si muove e nulla viene perturbato.

**Laboratorio 481-raft-split-votes.** Eseguire `./netlab run 481-raft-split-votes`. Il leader n1 va in crash a 5 s; i quattro superstiti estraggono il timeout da `election` (default 1000-1060ms, volutamente stretto). Parametri: `election`, `heartbeat`, `delay`. Metrica per tutti gli esperimenti: tempo senza leader, da 5 s alla prima riga `elected LEADER` (`eletto LEADER`), e numero di term consumati (`raft.max_term` meno 1). Si usino almeno dieci semi per punto, per esempio:

```
for s in 1 2 3 4 5 6 7 8 9 10; do ./netlab run 481-raft-split-votes election=1000-1010ms --seed $s --headless --detail 1 --lang en | grep LEADER; done
```

- **Esperimento 1 — ampiezza dell'intervallo.** Ipotesi: il numero atteso di term sprecati diminuisce all'allargarsi dell'intervallo rispetto al tempo di andata e ritorno dei messaggi. Manipolazione: `election=1000-1010ms`, `1000-1060ms`, `1000-2000ms`. Metrica: distribuzione del tempo senza leader. Esito atteso: più turni (da 2 a 4 s) per il più stretto, uno o due per il default, un solo turno di circa un secondo per il più ampio, il cui tempo medio di rilevamento è però maggiore.
- **Esperimento 2 — ritardo di rete.** Ipotesi: gli split vote dipendono dal rapporto fra ampiezza dell'intervallo e ritardo, non dalla sola ampiezza. Manipolazione: intervallo di default con `delay=5ms`, `20ms`, `50ms`. Metrica: come sopra, e se l'esecuzione supera le aspettative. Esito atteso: a 50 ms circa dieci term sprecati e un periodo senza leader di dieci secondi o più; con alcuni semi nessuno viene eletto prima della fine dello scenario.
- **Esperimento 3 — timeout inferiore all'heartbeat.** Ipotesi: quando electionTimeout non è ben al di sopra dell'intervallo di heartbeat non esiste un leader stabile, nemmeno senza guasti. Manipolazione: `election=150-300ms` (l'heartbeat è di 300 ms). Metrica: `raft.max_term` alla fine, `raft.agree`. Esito atteso: più di cento term in 25 s e continui cambi di leader; i comandi vengono comunque confermati fra un'elezione e l'altra, cosa che merita una spiegazione.

## Domande per la discussione

- Si dica con precisione quali client osservano indisponibilità nel laboratorio 480, per quanto tempo, e se qualcuno di essi potrebbe osservare un'incoerenza. A quale "lettera" di CAP corrisponde ciascuna osservazione?
- Si supponga che n1 avesse risposto "ok" a `x=minority` prima della conferma. Si costruisca l'anomalia visibile al client dopo la ricomposizione.
- Un lease consente al leader di leggere localmente per la durata del lease. Che cosa deve valere esattamente per gli orologi, e come lo si verificherebbe in un emulatore con orologi perfetti?
- Nell'esperimento 2 del laboratorio 481 il cluster è sicuro e indisponibile. Si proponga una modifica della regola dei timeout che lo avrebbe evitato, e si dica che cosa costa quando la rete è sana.
- Paxos consente a un nodo con log arretrato di diventare leader; Raft no. In quale schema di guasto questa differenza modifica la disponibilità?
