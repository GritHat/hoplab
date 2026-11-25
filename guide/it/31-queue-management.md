<!-- level: 3 -->
<!-- labs: 371-bufferbloat-codel, 372-red-ecn, 373-fair-queueing, 374-token-bucket -->
# Gestione delle code: AQM, fair queueing e shaping

Ogni porta di uscita di un router ha un buffer, e lì si prendono due decisioni: quale pacchetto parte per primo (**scheduling**) e quali pacchetti vengono scartati o marcati, e quando (**gestione della coda**). La risposta di default, una FIFO drop-tail, è la più semplice possibile e fallisce in tre modi. Un buffer grande sotto carico persistente resta pieno, quindi ogni pacchetto aspetta dietro centinaia di altri e la latenza sale a secondi senza alcun guadagno di throughput (**bufferbloat**). L'unico segnale di congestione è il traboccamento, che colpisce molti flussi nello stesso istante, i quali arretrano e risalgono insieme (**sincronizzazione globale**). E un'unica FIFO non offre alcun isolamento: un flusso che non rallenta sottrae il buffer a quelli che lo fanno.

La difficoltà è che una coda ha due funzioni che dall'esterno sembrano identiche: assorbire una breve raffica è un bene, una coda stazionaria è puro ritardo. Una soglia statica sulla lunghezza non le distingue, e la lunghezza giusta dipende dalla velocità del collegamento, dai round-trip time e dal numero di flussi.

Le idee chiave sono tre. La **gestione attiva della coda** (AQM) segnala la congestione in anticipo, a pochi flussi per volta, e giudica la coda da quanto a lungo persiste (RED su una lunghezza mediata, CoDel e PIE sul ritardo). Il **fair queueing** dà a ogni flusso la propria coda e le serve a turno. I **token bucket** limitano ciò che un flusso può inviare: una velocità di lungo periodo più una raffica.

<!-- detail: 2 -->
## Algoritmi e modelli

**RED** mantiene una media esponenziale pesata della lunghezza della coda, aggiornata a ogni arrivo, e scarta (o marca) gli arrivi con una probabilità che cresce linearmente fra due soglie:

```
avg = (1 - wq)*avg + wq*q
avg < min:         p = 0
min <= avg < max:  pb = maxp*(avg - min)/(max - min);  p = pb/(1 - count*pb)
avg >= max:        p = 1   (gentle: da maxp a 1 fra max e 2*max)
```

`count` è il numero di pacchetti dall'ultimo scarto: distribuisce gli scarti in modo uniforme. La media ha una memoria di circa `1/wq` arrivi, ed è questo che lascia passare le raffiche.

**ECN** (RFC 3168): un pacchetto la cui intestazione IP dichiara ECT può essere marcato CE invece che scartato; il ricevente restituisce la marcatura (ECE) e il mittente riduce la finestra come per una perdita, poi conferma con CWR.

**CoDel** misura il tempo di permanenza (sojourn time) di ogni pacchetto (timestamp all'accodamento, letto all'estrazione) e ne controlla il minimo su una finestra:

```
se sojourn < target (5 ms) o coda < 1 MTU:  esci dallo stato di scarto
se sojourn >= target per un intero interval (100 ms):  entra in scarto,
                                                       scarta dalla testa
in stato di scarto:  next_drop = now + interval/sqrt(count);  count++
```

La frequenza degli scarti cresce come la radice quadrata del contatore perché il throughput di TCP è proporzionale a `1/sqrt(p)`: la legge di controllo fa diminuire il carico offerto in modo all'incirca lineare nel tempo. `interval` va scelto pari a circa un RTT nel caso peggiore; nulla dipende dalla velocità del collegamento.

**PIE** stima il ritardo di accodamento (lunghezza della coda divisa per la velocità di uscita misurata) e ogni `tupdate` corregge una probabilità di scarto con un controllore proporzionale-integrale:

```
p = p + alpha*(delay - target) + beta*(delay - delay_old)
```

**Deficit round robin** approssima il fair queueing con costo O(1) per pacchetto. Ogni flusso `i` ha una coda, un quanto `Q` e un contatore di deficit `D`:

```
a ogni giro, per ogni flusso con pacchetti in coda:  D += Q
    finché il pacchetto in testa ha dimensione <= D:  invialo;  D -= dimensione
    se la coda è vuota:  D = 0
```

Se `Q` è almeno un MTU, i byte serviti a due flussi qualsiasi con pacchetti in coda, su qualunque intervallo, differiscono al più di `Q + 2*MTU`. L'allocazione è **max-min fair**: i flussi che chiedono meno della quota uguale ricevono ciò che chiedono, il resto è diviso in parti uguali fra gli altri.

**Token bucket** con velocità `r` e profondità `b`: i token si accumulano a velocità `r` fino a `b`; un pacchetto di lunghezza `L` parte solo spendendo `L` token. L'uscita in qualunque intervallo di durata `t` è al più `b + r*t`. Una sorgente a velocità di picco `R > r` passa senza ritardo per `b/(R - r)` secondi.

**Esempi svolti (i laboratori).** Bufferbloat: 200 pacchetti di circa 540 byte su un collegamento a 1 Mbit/s richiedono `200*540*8/1e6`, circa 0,87 s, per essere smaltiti: è il ritardo di ogni pacchetto una volta che la FIFO è piena. Fair queueing: su 1 Mbit/s con richieste di 3 Mbit/s e 100 kbit/s la quota uguale è 500 kbit/s; il mouse ottiene i suoi 100, lo hog i restanti 900. In una FIFO entrambi perderebbero la stessa frazione, `(3.1 - 1)/3.1`, circa il 68%. Token bucket: con `b` = 10000 byte, `r` = 500 kbit/s e una sorgente a 2 Mbit/s la raffica dura `80000/(1.5e6)`, circa 53 ms; poi l'uscita è 500 kbit/s e la coda da 100 pacchetti aggiunge fino a 0,87 s.

<!-- detail: 3 -->
## In profondità

**Perché la lunghezza è la variabile sbagliata.** Una coda buona è una coda che si svuota entro un RTT; una coda cattiva è una coda il cui minimo su un RTT resta sopra lo zero. CoDel segue esattamente quel minimo, in unità di tempo, per cui le sue due costanti funzionano dai kbit/s ai Gbit/s. Le soglie di RED sono in pacchetti e il suo comportamento dipende da `wq`, `maxp` e dal carico: con poca media reagisce alle raffiche, con troppa oscilla. Questa sensibilità, documentata da molti lavori sperimentali, è il motivo per cui RED è stato standardizzato e poi raramente attivato.

**L'AQM ha bisogno di un mittente che reagisce.** L'anello di controllo si chiude attraverso il trasporto. Contro una sorgente a velocità costante la coda eccede comunque le intenzioni dell'AQM: la legge a radice quadrata di CoDel accelera lentamente e il ritardo si assesta ben sopra il target; la media di RED supera `max` e tutto viene scartato. I laboratori qui usano di proposito UDP a velocità costante, per mostrare che l'AQM limita il ritardo ma non può eliminare una perdita che è aritmetica. L'isolamento dai flussi che non reagiscono richiede lo scheduling, non l'AQM.

**Compromessi.** Lo scarto dalla testa (CoDel) consegna il segnale con un tempo di coda di anticipo rispetto allo scarto in coda. ECN elimina la perdita ma sotto forte sovraccarico deve tornare a scartare, altrimenti un flusso ECT che non reagisce sarebbe favorito. Il fair queueing costa stato per flusso; il fair queueing stocastico distribuisce i flussi con un hash su un numero fisso di code. L'equità stretta fra flussi non è equità fra utenti, che possono aprire molti flussi. Uno shaper ritarda l'eccesso, un policer lo scarta: lo shaping richiede un buffer e aggiunge ritardo, il policing interagisce male con le raffiche di TCP.

**Dimensionamento dei buffer.** La regola classica è un prodotto banda-ritardo, `C*RTT`; con `n` flussi lunghi non sincronizzati basta `C*RTT/sqrt(n)` (Appenzeller et al.).

**Che cosa fanno i sistemi reali.** `fq_codel` (RFC 8290), il default di Linux, esegue CoDel dentro ogni coda DRR e dà priorità ai flussi nuovi; CAKE aggiunge lo shaping e l'equità per host; i modem DOCSIS 3.1 usano PIE; i data center usano DCTCP con un'unica soglia di marcatura istantanea; L4S (RFC 9330) definisce una doppia coda per i controlli di congestione scalabili.

**In netlab.** `fq` ricava i flussi dalla quintupla IP con un unico buffer condiviso e, quando è pieno, scarta dalla coda più lunga; `tbf` opera sopra la velocità del collegamento; le soglie di RED sono in pacchetti. Le sorgenti di questi quattro laboratori non reagiscono né agli scarti né alle marcature.

**Letteratura primaria.**

- J. Turner, "New Directions in Communications (or Which Way to the Information Age?)", 1986 (il leaky bucket).
- A. Demers, S. Keshav, S. Shenker, "Analysis and Simulation of a Fair Queueing Algorithm", SIGCOMM 1989.
- A. Parekh, R. Gallager, "A Generalized Processor Sharing Approach to Flow Control in Integrated Services Networks", 1993.
- S. Floyd, V. Jacobson, "Random Early Detection Gateways for Congestion Avoidance", 1993.
- M. Shreedhar, G. Varghese, "Efficient Fair Queuing Using Deficit Round Robin", SIGCOMM 1995.
- G. Appenzeller, I. Keslassy, N. McKeown, "Sizing Router Buffers", SIGCOMM 2004.
- J. Gettys, K. Nichols, "Bufferbloat: Dark Buffers in the Internet", 2011.
- K. Nichols, V. Jacobson, "Controlling Queue Delay", 2012.
- R. Pan et al., "PIE: A Lightweight Control Scheme to Address the Bufferbloat Problem", 2013.
- RFC 3168 (ECN), RFC 7567 (raccomandazioni sull'AQM), RFC 8289 (CoDel), RFC 8033 (PIE), RFC 8290 (FQ-CoDel).

<!-- detail: 1 -->
## In laboratorio

**371-bufferbloat-codel.** `./netlab run 371-bufferbloat-codel`. Due percorsi identici con 1,3 Mbit/s offerti a un collegamento da 1 Mbit/s: a1–ra–a2 con una FIFO da 200 pacchetti, b1–rb–b2 con CoDel. Aprire la scheda `ping` di a1 e di b1 (grafico `ping rtt (ms)`), la scheda `qdisc` di ra e rb (grafici di coda e di attesa) e, alla fine, la scheda `sink` di a2 e b2. Misurare: il ping su A sale verso 0,9 s, su B resta intorno a 0,1 s; i due sink ricevono all'incirca lo stesso numero di datagrammi. Prevedere prima di provare:

- `buffer=20`: il ritardo della FIFO è limitato da `20*540*8/1e6`, circa 90 ms. Un buffer piccolo nasconde il problema.
- `load=900k`: nessun sovraccarico, CoDel non scarta mai e i due percorsi sono identici (circa 12 ms).
- `aqm=pie`, poi `aqm=red`: contro questa sorgente che non reagisce PIE tiene il ritardo più basso di CoDel (decine di ms); RED con le soglie di default su un buffer da 200 pacchetti fa molto peggio. Spiegare entrambi i risultati a partire dalle leggi di controllo.

**372-red-ecn.** `./netlab run 372-red-ecn`. Su r1 aprire la scheda `qdisc`: grafici `queue` e `avg queue`, la probabilità di scarto, i contatori `red.early`, `red.forced`, `red.marks`. Nella vista dei pacchetti (`p`) trovare un datagramma di h1 dopo r1 e leggere il campo ECN (CE). Esperimenti: `wq=0.002` (alla media servono circa 500 arrivi, più di un secondo, per seguire la coda: prevedere più marcature, perché indugia fra `min` e `max`, e una reazione più tardiva); `wq=0.2` (la media è quasi la coda istantanea); `ecn=0` (le marcature diventano scarti: `red.marks` vale 0); `gentle=0` (sopra `max` ogni arrivo è uno scarto forzato: `red.forced` cresce, `red.early` diminuisce).

**373-fair-queueing.** `./netlab run 373-fair-queueing`. Su r1 la scheda `qdisc` mostra l'arretrato per flusso; i sink d1 (hog) e d2 (mouse) mostrano perdita e ritardo. Atteso: il mouse non perde nulla e aspetta circa 15 ms. Poi `qdisc=droptail` (previsione: il mouse perde circa il 68%, come lo hog, e ne eredita il ritardo), `mouse=600k` (sopra la quota equa: entrambi sono tenuti a circa 500 kbit/s ed entrambi perdono), `quantum=300` (alternanza più fine, ritardo del mouse leggermente inferiore).

**374-token-bucket.** `./netlab run 374-token-bucket`. Su r1 osservare `tokens (B)` e `queue`; su h2 la scheda `sink`, grafico `rx (kbit/s)`. Tre fasi: 300 kbit/s (secchio pieno, nessuna attesa), 2 Mbit/s per 4 s (raffica, poi limitazione a 500 kbit/s, coda piena, perdite), di nuovo 300 kbit/s. Calcolare prima quanti datagrammi passano nella seconda fase (raffica + 4 s alla velocità di shaping + una coda). Esperimenti: `bucket=100000` (prevedere i pacchetti consegnati in più: circa 90000 byte di raffica in più), `shape=1M`, `queue=500` (meno perdite, ma un ritardo di `500*540*8/5e5`, oltre 4 s).

## Verifica

- Lo scarto successivo di CoDel è programmato a `interval/sqrt(count)`. Ricavare perché proprio una radice quadrata, partendo dalla relazione sul throughput di TCP, e dire che cosa resta di quell'argomento quando il traffico è UDP a velocità costante.
- RED calcola `p = pb/(1 - count*pb)`. Quale distribuzione dell'intervallo fra scarti ne risulta, rispetto all'uso diretto di `pb`, e perché conta per la sincronizzazione?
- Dare l'allocazione max-min fair per richieste di 0,1, 0,4, 0,8 e 2 Mbit/s su un collegamento da 2 Mbit/s, e il limite di equità di DRR in byte per un quanto di 1514.
- Un flusso è vincolato da un token bucket (r, b) e attraversa un collegamento di velocità R ≥ r servito in FIFO senza altro traffico. Limitare il suo arretrato e il suo ritardo. Che cosa cambia se il flusso passa prima da uno shaper a velocità di picco P?
- Perché un AQM che marca con ECN deve cominciare a scartare i pacchetti ECT oltre un certo livello di congestione? Costruire l'attacco che l'alternativa renderebbe possibile.
