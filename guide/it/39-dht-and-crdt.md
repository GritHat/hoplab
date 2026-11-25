<!-- level: 3 -->
<!-- labs: 393-chord-ring, 394-crdt-counter -->
# Tabelle hash distribuite e CRDT: scalare senza coordinamento

Consenso e commit atomico comprano garanzie forti con il coordinamento: quorum, leader, round di messaggi. Due famiglie di tecniche vanno nella direzione opposta e si chiedono fin dove si arrivi senza. Rispondono a due domande diverse.

**Dove sono i dati?** Una tabella hash distribuita su migliaia di macchine che entrano ed escono ha bisogno di una regola che associ ogni chiave a una macchina. Una directory centrale è un collo di bottiglia e un singolo punto di guasto; `hash(chiave) mod N` riassegna quasi tutte le chiavi ogni volta che `N` cambia. Il **consistent hashing** dispone nodi e chiavi sullo stesso cerchio di identificatori e assegna ogni chiave al primo nodo in senso orario: un ingresso o un'uscita spostano solo le chiavi di un arco. Una **tabella hash distribuita** (DHT) come Chord aggiunge l'instradamento: ogni nodo ne conosce solo `O(log N)` altri, scelti a distanze esponenzialmente crescenti, e qualunque chiave si trova in `O(log N)` hop. La parte difficile è il churn: l'anello deve restare corretto mentre i nodi vanno e vengono, senza nessuno al comando.

**Qual è il valore?** Se le repliche accettano aggiornamenti in modo indipendente (durante una partizione, o solo per velocità), divergono, e riconciliare stati divergenti arbitrari è un problema dell'applicazione che spesso si risolve perdendo dati. Un **conflict-free replicated data type** (CRDT) è progettato in modo che ciò non possa accadere: i suoi stati formano un reticolo e la fusione è un estremo superiore, per cui repliche che hanno visto gli stessi aggiornamenti hanno lo stesso valore, qualunque siano l'ordine, le duplicazioni o i ritardi dei messaggi.

<!-- detail: 2 -->
## Algoritmi e modelli

**Consistent hashing.** Gli identificatori sono numeri di `m` bit su un cerchio modulo `2^m`; nodi e chiavi vi vengono mappati con un hash; `successor(k)` è il primo nodo il cui identificatore è uguale a `k` o lo segue. Con `N` nodi e `K` chiavi un nodo ne custodisce in media `K/N`, e un ingresso o un'uscita spostano `O(K/N)` chiavi. Con un solo punto per nodo gli archi sono disuguali (il più lungo vale circa `log N` volte la media); `v` nodi virtuali per macchina livellano il carico.

**Chord.** Il nodo `n` mantiene il successore, il predecessore, una lista di `r` successori e una finger table:

```
finger[i] = successor(n + 2^i)          i = 0 .. m-1

n.find_successor(id):
    if id in (n, successor]:  return successor
    n' = closest_preceding_node(id)     # il finger più avanzato in (n, id)
    return n'.find_successor(id)
```

Ogni hop almeno dimezza la distanza residua in senso orario verso `id`, quindi una ricerca richiede `O(log N)` hop con alta probabilità, circa `(1/2)*log2 N` in media, con stato `O(log N)` per nodo.

**Stabilizzazione.** La correttezza richiede solo i puntatori al successore; i finger sono un'ottimizzazione. Ingressi e guasti vengono assorbiti da compiti periodici:

```
n.join(n0):        predecessor = nil;  successor = n0.find_successor(n)
n.stabilize():     x = successor.predecessor
                   if x in (n, successor):  successor = x
                   successor.notify(n)
n.notify(n'):      if predecessor is nil or n' in (predecessor, n):
                       predecessor = n'
n.fix_fingers():   refresh one finger[i] = find_successor(n + 2^i)
```

Se il successore si guasta, il nodo prende la prima voce attiva della propria lista dei successori; con `r = O(log N)` l'anello sopravvive con alta probabilità al guasto di metà dei nodi. Ogni coppia esiste in `r` copie, sul proprietario e sui suoi `r - 1` successori, che sono proprio i nodi che erediteranno l'arco; quando i suoi vicini cambiano un nodo copia di nuovo le chiavi che possiede, ripristinando le `r` copie.

**Esempio svolto (laboratorio 393, 8 bit).** I dieci nodi hanno hash 0 (n7), 1 (n1), 129 (n3), 141 (n5), 145 (n6), 172 (n8), 198 (n10), 208 (n4), 239 (n9), 247 (n2). La chiave `apple` ha hash 248: il suo proprietario è `successor(248)` = n7 in 0, oltre il punto di chiusura del cerchio. Da n3 (129) i finger puntano ai successori di 130, 131, 133, 137, 145, 161, 193 e 1; il più avanzato prima di 248 è n10 (198). n10 inoltra a n2 (247), il cui successore n7 copre 248: due hop. Si noti l'arco (1, 129]: metà dell'anello appartiene a n3, ed è per questo che `cherry` (90) e `plum` (28) finiscono entrambe lì.

**CRDT basati sullo stato.** Un insieme di stati `S` con una fusione `⊔` che è

```
commutativa:  a ⊔ b = b ⊔ a
associativa:  (a ⊔ b) ⊔ c = a ⊔ (b ⊔ c)
idempotente:  a ⊔ a = a
```

è un **semireticolo superiore** (join semilattice); `a <= b` significa `a ⊔ b = b`. Ogni aggiornamento deve essere un'inflazione (`s <= update(s)`). Le repliche inviano il proprio stato alle altre in qualunque momento; chi riceve fonde. **Strong eventual consistency**: due repliche qualsiasi che hanno ricevuto lo stesso insieme di aggiornamenti sono nello stesso stato, senza rollback e senza consenso.

```
G-Counter:   stato P[1..N];   inc sulla replica i: P[i] += 1
             valore = sum(P);  fusione = massimo componente per componente
PN-Counter:  due G-Counter P, N;  dec in i: N[i] += 1
             valore = sum(P) - sum(N)
```

**Esempio svolto (laboratorio 394).** In concorrenza n1 aggiunge 2, n2 aggiunge 1, n3 aggiunge 3: `P` vale `[2,0,0]`, `[0,1,0]`, `[0,0,3]`. Quando n3 fonde lo stato di n2 ottiene `[0,1,3]` = 4; dopo tutte le fusioni `[2,1,3]` = 6 ovunque. Un decremento su n2 dà `N = [0,1,0]` e il valore 5. Un unico intero condiviso fuso con il massimo darebbe 3; fuso con la somma conterebbe due volte a ogni riconsegna.

<!-- detail: 3 -->
## In profondità

**Chord: gli invarianti e la loro fragilità.** Lo stato ideale è un unico ciclo di puntatori al successore nell'ordine degli identificatori. Le ricerche sono corrette se i successori sono corretti, e soltanto lente se i finger sono obsoleti. L'articolo originale sosteneva che la stabilizzazione mantiene l'anello in presenza di ingressi e guasti; l'analisi formale di Zave (2012) ha mostrato che nessuno degli invarianti dichiarati vale per il protocollo pubblicato, con controesempi di pochi nodi, e in seguito ne ha dato una versione corretta. In pratica i modi di guasto sono: ricerche che restituiscono il proprietario sbagliato mentre un ingresso viene assorbito (il dato allora risulta "non trovato" pur esistendo); perdita di un arco quando un nodo e tutta la sua lista dei successori si guastano insieme; anelli che si dividono dopo una partizione e non si ricongiungono da soli; churn elevato, in cui i timeout sui finger morti dominano la latenza. L'adesione aperta aggiunge gli attacchi Sybil ed eclipse. Gli hop dell'overlay ignorano la distanza di rete, quindi `O(log N)` hop possono significare secondi.

**Che cosa fanno i sistemi reali.** La DHT sopravvissuta sull'Internet aperta è Kademlia (BitTorrent, IPFS, discovery di Ethereum): la distanza XOR rende simmetriche le tabelle di instradamento, così le ricerche le aggiornano gratis, e le ricerche sono iterative e parallele. All'interno di un data center l'insieme dei membri è piccolo e fidato, quindi Dynamo, Cassandra e Riak tengono l'intero anello su ogni nodo (una DHT a zero hop, diffusa per gossip) con nodi virtuali e replica sulle successive `r` macchine distinte.

**CRDT: perché la convergenza vale e quanto costa.** Lo stato di una replica è l'estremo superiore degli aggiornamenti che ha ricevuto; l'estremo superiore è indipendente da ordine e molteplicità, quindi insiemi uguali di aggiornamenti danno stati uguali. Il trasporto può perdere, duplicare e riordinare, purché prima o poi gli aggiornamenti passino. I CRDT basati sulle operazioni inviano operazioni invece di stati e richiedono che le operazioni concorrenti commutino, ma allora serve una consegna affidabile, esattamente una volta, in ordine causale; i delta CRDT spediscono solo la parte recente dello stato. I costi sono metadati che crescono con il numero di repliche mai viste e, per insiemi e sequenze, tombstone la cui garbage collection richiede coordinamento. Il limite fondamentale è l'espressività: un CRDT non può imporre un invariante trasversale alle repliche, come un contatore che non scende mai sotto zero o un nome utente unico. Il teorema CALM lo rende preciso: esattamente i programmi monotoni hanno realizzazioni coerenti senza coordinamento. E convergenza non è intenzione: un registro last-writer-wins converge scartando una scrittura concorrente.

**In hoplab.** `chord`: le ricerche sono ricorsive, l'attività dei nodi è controllata con ping a ogni round di stabilizzazione, gli identificatori sono un hash del nome del nodo (con pochi bit due nomi collidono: la collisione viene rilevata e un nodo si sposta sull'identificatore successivo), un nodo riavviato ha perso lo stato e rientra come nuovo, `succlist` vale 3 di default. `crdt`: basato sullo stato; ogni replica invia l'intero stato a un peer casuale a ogni `sync` e risponde a un peer il cui stato differisce; `type` sceglie i tipi di dato che una replica ospita, e `crdt.converged` dice quando l'ultimo stato ricevuto da ogni peer era uguale al proprio.

**Letteratura primaria.**

- D. Karger et al., "Consistent Hashing and Random Trees", STOC 1997.
- I. Stoica, R. Morris, D. Karger, M. F. Kaashoek, H. Balakrishnan, "Chord: A Scalable Peer-to-Peer Lookup Service for Internet Applications", SIGCOMM 2001.
- P. Maymounkov, D. Mazières, "Kademlia: A Peer-to-Peer Information System Based on the XOR Metric", 2002.
- S. Rhea, D. Geels, T. Roscoe, J. Kubiatowicz, "Handling Churn in a DHT", 2004.
- G. DeCandia et al., "Dynamo: Amazon's Highly Available Key-Value Store", SOSP 2007.
- P. Zave, "Using Lightweight Modeling to Understand Chord", 2012.
- D. Terry et al., "Managing Update Conflicts in Bayou, a Weakly Connected Replicated Storage System", SOSP 1995.
- M. Shapiro, N. Preguiça, C. Baquero, M. Zawirski, "Conflict-Free Replicated Data Types", SSS 2011.
- P. Almeida, A. Shoker, C. Baquero, "Delta State Replicated Data Types", 2018.
- J. Hellerstein, P. Alvaro, "Keeping CALM: When Distributed Consistency Is Easy", 2020.

<!-- detail: 1 -->
## In laboratorio

**393-chord-ring.** `./hoplab run 393-chord-ring`. n1 crea l'anello e nove nodi vi entrano contemporaneamente passando da lui. Selezionare un nodo e aprire la scheda `chord`: identificatore, successore, predecessore, finger, chiavi memorizzate. Nel primo mezzo secondo ogni nodo ha n1 come successore; osservare le righe "nuovo successore" e "nuovo predecessore" mentre la stabilizzazione (ogni 400 ms) ordina l'anello. Prima di 12 s disegnare l'anello a partire dagli identificatori e calcolare a mano il proprietario di 248, 173, 90 e 28. A 12 s n3 memorizza quattro chiavi: il log mostra ogni `find_successor(…) → inoltrato a …`, il proprietario, il numero di salti, e tre righe "memorizzata una copia" sui successori del proprietario. A 15 s altri nodi le leggono; `mango` non viene trovata. Il grafico `lookup hops` e il contatore `chord.hops` danno le lunghezze dei percorsi: confrontarle con `(1/2)*log2 10`, circa 1,7.

- In un laboratorio in esecuzione, prima che l'anello sia in ordine (pausa a circa 1 s), scrivere `n3 chord put key=early value=1` e `n6 chord get key=early`. Previsione: la chiave viene memorizzata su un nodo che non è il suo proprietario definitivo e una lettura precoce può restituire "not found", benché nessun nodo si sia guastato; pochi secondi dopo la stessa lettura riesce, perché la stabilizzazione consegna la chiave al suo proprietario. Spiegare quale proprietà di Chord mancava al momento del put. (Le chiavi dello script sono memorizzate a 12 s: tenere `stabilize` a 1 s o meno, altrimenti l'anello non è ancora pronto.)
- `:` poi `node n7 down`, attendere due round di stabilizzazione, poi `n6 chord get key=apple`. Previsione: n2 scrive "il mio successore ora è n1" e al get risponde n1 dalla propria copia.
- `bits=5` o `bits=4`: un anello abbastanza piccolo da verificare a mano. Due nomi finiscono sullo stesso identificatore: leggere le righe che segnalano la collisione e il contatore `chord.collisions`, e dire che cosa fa un sistema reale per le collisioni (identificatori a 160 bit le rendono trascurabili).
- `:` poi `n4 chord lookup key=banana` da più nodi: annotare i salti e individuare quale finger è stato usato a ogni passo.

**394-crdt-counter.** `./hoplab run 394-crdt-counter`. Tre repliche di un PN-Counter, che ogni 500 ms inviano per gossip l'intero stato a un peer casuale su collegamenti che perdono il 20% dei pacchetti. Aprire la scheda `crdt` di ogni replica: lo stato è una coppia (incrementi, decrementi) per replica; il valore è la somma. A 2 s i tre incrementi concorrenti mettono le repliche in disaccordo (2, 1, 3); seguire le righe "fusione con …: contatore a → b" finché tutte mostrano 6, poi il decremento a 6 s finché tutte mostrano 5. Contare le fusioni necessarie (`crdt.merges`) e i messaggi di stato persi.

- `loss=0.6`: prevedere l'esito (lo stesso valore, più tardi). `sync=2s`: il tempo di convergenza scala con il periodo.
- Partizione: `:` poi `link n3 s1 down`; `n3 crdt inc by=10`; `n1 crdt dec by=4`. Leggere i tre valori, prevedere quello finale, poi `link n3 s1 up`.
- Esperimento mentale: con `type=gcounter` quale delle operazioni del laboratorio diventa impossibile, e perché "fusione = massimo dei valori" è sbagliata per un contatore ma giusta per ciascuna componente?

## Verifica

- Dimostrare che una ricerca in Chord richiede O(log N) hop con alta probabilità, e dire quanto costa la ricerca, e se è ancora corretta, quando tutti i finger sono obsoleti ma i successori sono giusti.
- Seguire stabilize e notify per un nodo che entra fra due nodi adiacenti, e mostrare un istante in cui la ricerca di una chiave nell'arco del nuovo nodo restituisce il proprietario sbagliato.
- Perché il consistent hashing con un punto per nodo dà un carico disuguale, e come cambiano con i nodi virtuali la media e la varianza dell'arco posseduto da una macchina?
- Mostrare che la fusione del G-Counter è un estremo superiore, e che uno stato di contatore fatto di un solo intero non ammette alcuna fusione che sia insieme idempotente e corretta in presenza di incrementi concorrenti.
- Dare un invariante che nessun CRDT può mantenere senza coordinamento e metterlo in relazione con la monotonia. Perché un registro last-writer-wins soddisfa la strong eventual consistency e tuttavia perde aggiornamenti?
