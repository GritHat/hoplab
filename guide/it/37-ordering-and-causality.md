<!-- level: 3 -->
<!-- labs: 390-causal-broadcast -->
# Ordinamento e causalità: vector clock e broadcast ordinato

In un sistema distribuito non c'è un orologio globale né una vista istantanea: ogni processo vede solo i propri eventi e i messaggi che riceve, dopo ritardi arbitrari. Eppure alle applicazioni serve un ordine. Una risposta non deve essere mostrata prima della domanda a cui risponde; le repliche devono applicare gli stessi aggiornamenti in un ordine che porti allo stesso stato. I timestamp fisici non possono fornirlo: gli orologi derivano, e secondo due orologi non sincronizzati un messaggio può arrivare "prima" di essere stato inviato.

Ciò che si può definire senza alcun orologio è la **causalità**. L'evento `a` è **accaduto prima** di `b` (happened before) se i due sono sullo stesso processo in quell'ordine, se `a` è l'invio di un messaggio e `b` la sua ricezione, oppure per transitività. Questa relazione è solo un ordine parziale: gli eventi non legati da essa sono **concorrenti**, e nessun osservatore interno al sistema può dire quale sia venuto prima. Tutto l'argomento sta nella tensione fra questo ordine parziale, che è reale, e gli ordini totali che le applicazioni vorrebbero, che vanno costruiti e pagati.

L'idea chiave è rendere calcolabile la causalità e poi imporre tanto ordine quanto serve, e non di più. Un **vector clock** riassume il passato causale di un evento in `N` interi, così che la causalità si decide confrontando vettori. Uno strato di **broadcast** può allora ritardare la consegna di un messaggio finché non sono stati consegnati i messaggi da cui dipende. Garanzie più forti costano di più: l'ordine FIFO è quasi gratuito, l'ordine causale costa un vettore per messaggio, l'ordine totale costa quanto il consenso.

<!-- detail: 2 -->
## Algoritmi e modelli

**Clock di Lamport.** Un contatore `L` per processo: incrementarlo a ogni evento; marcare i messaggi; alla ricezione porre `L = max(L, ricevuto) + 1`. Allora `a → b` implica `L(a) < L(b)`. Il viceversa non vale: i timestamp non distinguono la causalità dalla coincidenza. Ordinando per `(L, id del processo)` si ottiene un ordine totale coerente con la causalità.

**Vector clock.** Il processo `i` mantiene `V[1..N]`:

```
evento locale o invio in i:  V[i] += 1               (i messaggi portano V)
ricezione di m in i:         V[k] = max(V[k], m.V[k]) per ogni k;  V[i] += 1

V(a) <= V(b)   sse  V(a)[k] <= V(b)[k] per ogni k
a -> b         sse  V(a) <= V(b) e V(a) != V(b)
a || b         sse  né V(a) <= V(b) né V(b) <= V(a)
```

I vector clock caratterizzano esattamente la causalità: è la proprietà che manca ai clock di Lamport.

**Specifiche di broadcast.** Un processo fa il **broadcast** di un messaggio; lo strato lo **consegna** (deliver) all'applicazione, eventualmente più tardi di quando l'ha ricevuto.

- **FIFO**: se un processo fa il broadcast di `m` prima di `m'`, nessun processo consegna `m'` prima di `m`.
- **Causale**: se il broadcast di `m` è accaduto prima del broadcast di `m'`, nessun processo consegna `m'` prima di `m`. Causale implica FIFO.
- **Totale**: se due processi consegnano entrambi `m` e `m'`, li consegnano nello stesso ordine. L'ordine totale è indipendente dagli altri due e si può combinare con essi.

**Condizione di consegna causale.** Ogni processo `i` mantiene `D[1..N]`, dove `D[k]` conta i broadcast di `k` consegnati in `i`; un broadcast di `j` porta `m.V` = il `D` del mittente con `m.V[j]` incrementato. Il processo `i` può consegnare `m` proveniente da `j` quando

```
m.V[j] == D[j] + 1          (m è il prossimo broadcast di j)
m.V[k] <= D[k]  per k != j  (i ha consegnato tutto ciò che j aveva consegnato)
```

Altrimenti `m` aspetta in una **coda di attesa** (hold-back queue), riesaminata dopo ogni consegna. Il broadcast FIFO è la sola prima riga, con un unico numero di sequenza per mittente.

**Ordine totale.** Tre costruzioni: un sequenziatore fisso che numera tutti i messaggi (semplice, un singolo punto di guasto); l'algoritmo di Lamport (ogni broadcast viene accodato per `(timestamp, id)`, riscontrato da tutti, e consegnato quando è in testa alla coda e da ogni processo si è ricevuto un timestamp maggiore); il consenso sul prossimo messaggio, posizione per posizione.

**Esempio svolto (il laboratorio).** Tre processi. p1 fa il broadcast della domanda `q` con `V = [1,0,0]`. p2 la consegna e fa il broadcast della risposta `a` con `V = [1,1,0]`. Il collegamento p1–p3 è lento, quindi `a` raggiunge p3 per prima, dove `D = [0,0,0]`. Controllo: `a.V[2] = 1 = D[2] + 1`, va bene; ma `a.V[1] = 1 > D[1] = 0`: p3 non ha consegnato qualcosa che il mittente aveva. `a` viene trattenuta. Quando arriva `q`, `q.V[1] = 1 = D[1] + 1`: consegnata, `D = [1,0,0]`; ora `a` supera il controllo ed è consegnata subito. L'ordine FIFO non sarebbe servito: `q` e `a` provengono da mittenti diversi.

**Costo.**

```
FIFO     1 intero per messaggio, attesa per singolo mittente
causale  N interi per messaggio, attesa trasversale ai mittenti
totale   round aggiuntivi o un leader; equivalente al consenso
```

<!-- detail: 3 -->
## In profondità

**Correttezza della consegna causale.** Safety: se `m → m'` allora `m.V <= m'.V`, e la condizione vieta di consegnare `m'` a un processo il cui `D` non domina ancora le dipendenze di `m'`, fra le quali c'è `m`. Liveness: ogni messaggio trattenuto prima o poi viene consegnato, purché prima o poi arrivi ogni messaggio che il suo mittente aveva consegnato. Questa clausola è essenziale: l'ordine causale deve poggiare su un broadcast **affidabile** (se un processo corretto consegna `m`, lo fanno tutti). Con un trasporto best-effort un solo messaggio perso blocca, per sempre, tutto ciò che lo segue causalmente.

**Quanti metadati sono necessari.** Charron-Bost ha dimostrato che per caratterizzare la causalità fra `N` processi servono, in generale, vettori di `N` componenti: per il caso generale non ci sono scorciatoie. I sistemi reali riducono il costo inviando solo le componenti cambiate, sfruttando la topologia (un albero o un unico hub richiedono meno), oppure tracciando la causalità dei dati anziché dei messaggi: i version vector e i dotted version vector degli archivi in stile Dynamo hanno una componente per replica, non per client. Gli hybrid logical clock combinano il tempo fisico con un contatore di Lamport in 64 bit e danno un ordine coerente con la causalità e vicino al tempo reale; TrueTime di Spanner limita invece l'incertezza degli orologi e la aspetta.

**L'ordine totale è consenso.** Chandra e Toueg hanno mostrato che il broadcast a ordine totale (atomic broadcast) e il consenso sono equivalenti: ciascuno si costruisce dall'altro. L'ordine totale eredita quindi l'impossibilità FLP e la necessità di un quorum e di ipotesi sui tempi. La costruzione di Lamport del 1978 lo rende concreto dall'altro lato: non tollera guasti, perché la consegna aspetta ogni processo, e un solo crash blocca tutto.

**Limiti dell'astrazione.** Cheriton e Skeen hanno sostenuto che la causalità a livello di messaggi non cattura né le dipendenze semantiche che passano fuori dal sistema (una base di dati condivisa, una persona), né vincoli del tipo "questi due aggiornamenti insieme", e che code di attesa e vettori non scalano; spesso l'applicazione può ordinare ciò che conta con numeri di versione propri. La replica (Birman) è che farlo bene una volta in uno strato è più sicuro. Entrambe le posizioni sopravvivono: i toolkit di comunicazione di gruppo (Isis, Horus, Spread) offrivano la virtual synchrony con ordine causale e totale; i sistemi moderni prendono per lo più l'ordine totale da un log replicato (Raft, Zab, le partizioni di Kafka) e la causalità da version vector a livello di dati o dai CRDT.

**In netlab.** I vettori del modulo `clock` contano ogni evento, ricezioni comprese, quindi i vettori a schermo sono più grandi dei vettori dei consegnati `D` usati sopra; il controllo delle dipendenze è lo stesso. `order=total` è la costruzione di Lamport: si fonda su canali FIFO e sul fatto che ogni processo sia attivo, e non gestisce i guasti. `order=none` consegna all'arrivo e conta a posteriori le violazioni di causalità.

**Letteratura primaria.**

- L. Lamport, "Time, Clocks, and the Ordering of Events in a Distributed System", 1978.
- K. Birman, T. Joseph, "Reliable Communication in the Presence of Failures", 1987.
- C. Fidge, "Timestamps in Message-Passing Systems That Preserve the Partial Ordering", 1988.
- F. Mattern, "Virtual Time and Global States of Distributed Systems", 1989.
- K. Birman, A. Schiper, P. Stephenson, "Lightweight Causal and Atomic Group Multicast", 1991.
- B. Charron-Bost, "Concerning the Size of Logical Clocks in Distributed Systems", 1991.
- D. Cheriton, D. Skeen, "Understanding the Limitations of Causally and Totally Ordered Communication", SOSP 1993.
- V. Hadzilacos, S. Toueg, "A Modular Approach to Fault-Tolerant Broadcasts and Related Problems", 1994.
- X. Défago, A. Schiper, P. Urbán, "Total Order Broadcast and Multicast Algorithms: Taxonomy and Survey", 2004.

<!-- detail: 1 -->
## In laboratorio

`./netlab run 390-causal-broadcast`. Tre processi a triangolo; il collegamento n1–n3 è lento (150 ms). La stessa scena viene recitata due volte. Selezionare n3 (`n`), aprire la scheda `clock` e premere `s` per il diagramma di sequenza: le frecce che si incrociano raccontano tutto. Usare il log a dettaglio `2` per leggere i vettori.

**Primo round (1 s), nessun ordinamento.** n1 fa il broadcast di "what time is it?", n2 lo consegna e fa il broadcast di "half past nine". In n3 la risposta arriva per prima e viene consegnata subito: il log stampa "causalità violata … dipende da un messaggio non ancora consegnato, di n1", e `clock.violations` diventa 1. Prima di guardare, scrivere il vettore portato da ciascun messaggio e stabilire per confronto che la domanda è accaduta prima della risposta.

**Secondo round (4 s), ordine causale.** Stesso scambio. In n3 il log mostra "trattenuto «ten o'clock» da n2: un messaggio da cui dipende non è ancora arrivato", poi la domanda viene consegnata e segue subito "esce dalla coda di attesa". Misurare il tempo che la risposta trascorre in coda (circa 40 ms: la differenza fra i due percorsi) e notare `clock.heldback` = 1.

**Esperimento 1: FIFO non basta.** `./netlab run 390-causal-broadcast order2=fifo`. Prevedere l'esito prima di eseguire: i due messaggi hanno mittenti diversi, quindi FIFO non impone nulla fra loro e n3 viola di nuovo la causalità (`clock.violations` = 2: la verifica `causality` del laboratorio si aspetta proprio questo per `none` e `fifo`, e 1 per `causal` e `total`).

**Esperimento 2: il prezzo dell'ordine totale.** `order2=total`. Ora ogni processo accoda ciascun messaggio ("in coda … in attesa del riscontro di tutti i processi") e lo consegna solo quando tutti l'hanno riscontrato. Misurare quando n1 consegna la propria domanda: circa 300 ms dopo l'invio, un'andata e ritorno sul collegamento lento, contro 0 ms di prima. Tutti e tre consegnano nello stesso ordine. Poi aumentare `slow=500ms` e prevedere i nuovi tempi di consegna per l'ordine causale e per quello totale.

**Esperimento 3: concorrenza e un guasto.** In un laboratorio in esecuzione con `order2=causal`, mettere in pausa (spazio), inserire `:` `n1 clock bcast text=A` e `:` `n3 clock bcast text=B`, e riprendere: i due sono concorrenti; verificare che i loro vettori siano inconfrontabili e se tutti i processi li consegnano nello stesso ordine (non sono tenuti a farlo). Ripetere con `order2=total`: ora devono. Infine, con l'ordine totale, `node n3 down` e un broadcast da n1: prevedere che cosa accade al messaggio, e dire perché un ordine totale basato sul consenso si comporterebbe diversamente.

## Verifica

- Dimostrare che V(a) < V(b) se e solo se a è accaduto prima di b, e dare due eventi per i quali i timestamp di Lamport sono ordinati ma gli eventi sono concorrenti.
- Enunciare la condizione di consegna causale e dimostrare che non consegna mai un messaggio prima di uno che lo precede causalmente. Che cosa deve garantire il trasporto sottostante perché i messaggi trattenuti vengano prima o poi consegnati?
- Dare un'esecuzione che soddisfa l'ordine FIFO e l'ordine totale ma viola l'ordine causale.
- Spiegare perché il broadcast a ordine totale è equivalente al consenso, abbozzando entrambe le riduzioni. Che cosa implica per un broadcast a ordine totale asincrono e tollerante ai guasti?
- Un servizio di chat con un milione di client vuole che le risposte non compaiano mai prima dei messaggi a cui rispondono. Perché i vector clock con una componente per client sono il progetto sbagliato, e che cosa fareste invece?
