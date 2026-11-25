<!-- level: 3 -->
<!-- labs: 380-raft-election, 381-raft-replication -->
# Consenso e Raft

Un servizio replicato resta disponibile quando le macchine si guastano solo se le sue repliche eseguono gli stessi comandi nello stesso ordine: allora sono copie di un'unica macchina a stati deterministica. Accordarsi su quell'ordine è il problema del **consenso**. Ogni processo può proporre un valore, e il protocollo deve garantire **agreement** (accordo: due processi non decidono mai valori diversi), **validity** (validità: il valore deciso è stato proposto) e **termination** (terminazione: ogni processo corretto prima o poi decide).

È difficile perché, in una rete asincrona, un processo guasto non si distingue da uno lento. Un protocollo che aspetta tutti si blocca al primo guasto; uno che smette di aspettare può agire su un sospetto falso, e due gruppi possono allora decidere in modo diverso. Le proprietà si dividono di conseguenza: l'accordo è una proprietà di **safety** (nulla di male accade mai, qualunque siano i tempi), la terminazione è una proprietà di **liveness** (qualcosa di buono prima o poi accade), e un risultato fondamentale dice che le due non si possono garantire insieme in condizioni di completa asincronia.

Le idee chiave sono due. I **quorum a maggioranza**: due maggioranze qualsiasi dello stesso gruppo hanno almeno un membro in comune, quindi un fatto memorizzato da una maggioranza non può essere ignorato da una maggioranza successiva. E i **term**: il tempo è diviso in epoche numerate, ciascuna con al più un leader, così ogni messaggio può essere giudicato attuale o superato. Raft mantiene la safety incondizionatamente e rinuncia solo alla liveness, e solo finché la rete si comporta male.

<!-- detail: 2 -->
## Algoritmi e modelli

**Modello.** `N = 2f + 1` processi tollerano `f` guasti per crash (non bizantini); i messaggi possono essere persi, ritardati, duplicati, riordinati; i processi hanno memoria stabile.

**FLP in un paragrafo.** Fischer, Lynch e Paterson (1985) hanno dimostrato che nessun protocollo deterministico risolve il consenso in un sistema asincrono se anche un solo processo può guastarsi. La dimostrazione mostra che da una configurazione indecisa ("bivalente") uno scheduler avversario può sempre ritardare un messaggio in modo che il sistema resti indeciso per sempre. Il risultato non dice che il consenso sia impossibile in pratica; dice che la terminazione richiede un'ipotesi in più: sincronia parziale (i limiti di tempo prima o poi valgono), failure detector, oppure randomizzazione. Raft usa i timeout, quindi è live solo quando i messaggi arrivano entro il timeout di elezione.

**Stato.** Persistente su ogni server: `currentTerm`, `votedFor`, `log[]` (ogni voce contiene un comando e il term in cui un leader l'ha creata). Volatile: `commitIndex`, `lastApplied`; sul leader `nextIndex[]`, `matchIndex[]`. Un messaggio con term più alto fa sì che chi lo riceve lo adotti e diventi follower; un messaggio con term più basso viene rifiutato.

**Elezione del leader.** Un follower che non sente un leader per un tempo casuale in `[T, 2T]` incrementa il proprio term, vota per sé e invia `RequestVote(term, lastLogIndex, lastLogTerm)`. Un server concede il suo unico voto del term solo se il log del candidato è **almeno aggiornato quanto** il proprio:

```
concedi  sse  term >= currentTerm  e  votedFor in {null, candidato}
              e (lastLogTerm, lastLogIndex) >= (myLastTerm, myLastIndex)
              (coppie confrontate lessicograficamente)
```

Una maggioranza di voti fa un leader. Due candidati possono dividersi i voti (split vote): nessuno vince, e i timer scadono di nuovo in istanti casuali diversi.

**Replica.** Il leader aggiunge i comandi al log e invia

```
AppendEntries(term, prevLogIndex, prevLogTerm, entries[], leaderCommit)
follower: rifiuta se term < currentTerm
                  o log[prevLogIndex].term != prevLogTerm
          altrimenti elimina l'eventuale suffisso in conflitto, aggiunge
          le voci, commitIndex = min(leaderCommit, indice dell'ultima nuova)
leader:   al rifiuto, decrementa nextIndex[follower] e riprova
```

Questo controllo di coerenza dà il **log matching**: se due log contengono una voce con lo stesso indice e lo stesso term, sono identici fino a quell'indice.

**Regola di commit.**

```
il leader pone commitIndex = n   sse   matchIndex[i] >= n su una maggioranza
                                       e log[n].term == currentTerm
```

**Perché solo le voci del term corrente.** Cinque server. S1, leader del term 2, memorizza la voce `(indice 2, term 2)` su S1 e S2 e si guasta. S5 vince il term 3 con i voti di S3 e S4, aggiunge la propria `(2, term 3)` e si guasta. S1 vince il term 4 e copia `(2, term 2)` su S3: ora la voce è su una maggioranza. Se S1 contasse le repliche e la dichiarasse committed, e poi si guastasse, S5 potrebbe ancora vincere il term 5 con i voti di S2, S3, S4 (il suo ultimo term, 3, batte il loro 2) e sovrascrivere l'indice 2: una voce committed andrebbe persa. Se invece S1 replica prima su una maggioranza una voce del term 4, S5 non può più vincere, e la vecchia voce risulta committed indirettamente. Da qui il no-op che un nuovo leader aggiunge subito.

**Tempi.** La liveness richiede `tempo di broadcast << timeout di elezione << tempo medio fra i guasti`.

<!-- detail: 3 -->
## In profondità

**L'argomento di safety.** Raft mantiene cinque invarianti: election safety (al più un leader per term: un voto per server per term, e le maggioranze si intersecano), leader append-only, log matching (per induzione sul controllo di AppendEntries), **leader completeness** (una voce committed nel term `t` è nel log di ogni leader di un term superiore a `t`) e state-machine safety (due server non applicano mai comandi diversi allo stesso indice). La leader completeness è il nucleo: una voce committed è su una maggioranza; un candidato successivo ha bisogno di una maggioranza di voti; qualche votante possiede la voce; vota solo per un log almeno aggiornato quanto il proprio; per induzione sui term intermedi il candidato possiede la voce. La restrizione al term corrente è ciò che rende vero il primo passo di quell'induzione.

**Persistenza.** `currentTerm`, `votedFor` e il log devono raggiungere la memoria stabile prima che parta una risposta. Un server che dimentica il proprio voto può votare due volte in un term e creare due leader; uno che dimentica una voce di cui ha dato riscontro rompe l'aritmetica dei quorum. L'indice di commit è volatile: dopo un riavvio lo si apprende di nuovo dal leader.

**Modi di guasto e rimedi.** Split vote ed elezioni ripetute con timeout stretti o mal randomizzati. Un server isolato da una partizione gonfia il proprio term e, al rientro, depone un leader sano: PreVote e CheckQuorum lo impediscono. Un leader deposto può ancora rispondere alle letture con uno stato superato: le letture linearizzabili richiedono un giro di heartbeat (ReadIndex) oppure un lease con deriva degli orologi limitata. Un client che ritenta un comando dopo un cambio di leader può vederlo applicato due volte, a meno che i comandi portino identificatori di sessione. I cambi di configurazione non devono mai permettere due maggioranze disgiunte: joint consensus, oppure un server alla volta. I log crescono senza limite: snapshot e InstallSnapshot.

**Parenti.** Viewstamped Replication (1988) e Multi-Paxos raggiungono le stesse garanzie; Raft si distingue perché richiede che il nuovo leader possieda già tutte le voci committed, così i dati fluiscono solo dal leader ai follower. Zab (ZooKeeper) è simile. I guasti bizantini richiedono `3f + 1` repliche (PBFT). Raft in produzione: etcd, Consul, CockroachDB, TiKV, KRaft di Kafka.

**In netlab.** Il modulo `raft` rende persistenti term, voto e log attraverso `node N down/up`, aggiunge un no-op all'inizio di ogni term, inoltra al leader i comandi ricevuti dai follower e offre `prevote=1`. Non ha snapshot né cambi di configurazione, e `raft get` legge la copia locale della macchina a stati, che può essere superata: non è una lettura linearizzabile. I follower apprendono un nuovo indice di commit dal successivo AppendEntries, quindi applicano con un heartbeat di ritardo rispetto al leader.

**Letteratura primaria.**

- M. Fischer, N. Lynch, M. Paterson, "Impossibility of Distributed Consensus with One Faulty Process", 1985.
- B. Oki, B. Liskov, "Viewstamped Replication: A New Primary Copy Method to Support Highly-Available Distributed Systems", 1988.
- C. Dwork, N. Lynch, L. Stockmeyer, "Consensus in the Presence of Partial Synchrony", 1988.
- F. Schneider, "Implementing Fault-Tolerant Services Using the State Machine Approach: A Tutorial", 1990.
- T. Chandra, S. Toueg, "Unreliable Failure Detectors for Reliable Distributed Systems", 1996.
- L. Lamport, "The Part-Time Parliament", 1998; "Paxos Made Simple", 2001.
- D. Ongaro, J. Ousterhout, "In Search of an Understandable Consensus Algorithm", USENIX ATC 2014; D. Ongaro, "Consensus: Bridging Theory and Practice", tesi di dottorato, 2014.

<!-- detail: 1 -->
## In laboratorio

**380-raft-election.** `./netlab run 380-raft-election`. Cinque server attorno a uno switch; n1 ha un timeout più breve (400–500 ms) in modo da vincere il term 1. Selezionare un nodo (`n`) e aprire la scheda `raft`: ruolo, term, la barra del timer di elezione, il log. Premere `s` per la vista di sequenza e seguire RequestVote, i voti, poi il primo AppendEntries che porta il no-op. A 8 s n1 si guasta: misurare il tempo fino a "eletto LEADER per il term 2" (atteso poco più di 1 s: il più piccolo di quattro timeout estratti in 1–2 s). A 16 s n1 riparte "dalla memoria stabile: term=1", riceve un AppendEntries del term 2 e diventa follower.

- `election=1000-1050ms`: i timeout sono quasi uguali. Prevedere gli split vote: più righe "candidato per il term …" a pochi millisecondi di distanza, talvolta un term senza leader. Contare le elezioni (contatore `raft.elections`).
- Isolare un follower: `:` poi `link n5 s1 down`; osservare il term di n5 crescere a ogni timeout. Dopo qualche secondo `link n5 s1 up`. Previsione: il leader sano scrive "si dimette: esiste un term più alto" e segue un'elezione benché nulla si fosse guastato. È il disturbo che PreVote elimina.
- Perdere il quorum: guastare tre server (`node n2 down`, `node n3 down`, `node n4 down`, oppure `k` sul nodo selezionato). Prevedere che cosa possono fare i due superstiti. Se il leader è fra i guasti, i term salgono per sempre e nessuno viene eletto; se sopravvive, verificare se mantiene il ruolo e se qualcosa può ancora diventare committed.

**381-raft-replication.** `./netlab run 381-raft-replication`. Aprire la scheda `raft` del leader n1 (`nextIndex`/`matchIndex` per follower) e di un follower. Le celle del log sono blu quando la voce è memorizzata e verdi quando è applicata. Seguire un comando nel log a dettaglio `2`: "aggiunto all'indice 2", quattro AppendEntries con `prev=1/1`, "voce committed 2 … memorizzata dalla maggioranza", e i follower che la applicano circa un heartbeat dopo. A 6 s n4 si guasta e i commit proseguono con quattro server; a 12 s n4 riparte con un log di lunghezza 4 e un solo AppendEntries con `prev=4/1 entries=3` lo porta a 7.

- `:` poi `n3 raft put color=red` e `n5 raft get color`. Spiegare perché il get può non restituire nulla se eseguito subito.
- Guastare tre server e inviare un comando: la cella resta blu. Riattivare un server con `node N up` e prevedere che cosa accade alla voce in sospeso.
- Mettere in pausa (spazio), inviare `n1 raft put a=1`, avanzare con `.` finché gli AppendEntries sono partiti, poi guastare n1 con `k`. Prevedere, dalla regola del log più aggiornato, quali server possono diventare leader e se `a=1` sopravvive; verificare con `raft.truncated` e con i log.

## Verifica

- Enunciare agreement, validity e termination, classificare ciascuna come safety o liveness, e dire con precisione che cosa vieta FLP e quale ipotesi aggiunge Raft per sottrarvisi.
- Dimostrare che non possono esistere due leader nello stesso term. Su quale variabile persistente si fonda la dimostrazione, e che cosa va storto se viene scritta su disco dopo aver risposto?
- Ricostruire lo scenario in cui una voce memorizzata su una maggioranza viene sovrascritta, e mostrare come la regola di commit ne impedisce la perdita.
- Il log di un follower è più lungo di quello del leader e ne differisce dall'indice 5 in poi. Descrivere i messaggi scambiati finché i log coincidono, e argomentare che nessuna voce committed viene cancellata.
- Perché leggere lo stato locale del leader non è linearizzabile? Dare due meccanismi che rendono linearizzabili le letture e l'ipotesi di cui ciascuno ha bisogno.
