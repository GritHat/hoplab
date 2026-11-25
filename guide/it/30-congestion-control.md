<!-- level: 3 -->
<!-- labs: 320-tcp-congestion, 321-tcp-sack -->
# Controllo di congestione: AIMD, CUBIC e i modelli che li spiegano

Una rete a pacchetti non ha controllo di ammissione: qualunque host può trasmettere a qualunque velocità, e un router che riceve più di quanto il suo collegamento di uscita possa trasportare può solo accodare e poi scartare. Se i mittenti rispondono alle perdite ritrasmettendo alla stessa velocità, la rete entra in **collasso da congestione**: i collegamenti restano occupati da pacchetti che verranno scartati più avanti o che sono duplicati inutili, e il throughput utile crolla di ordini di grandezza (nell'ottobre 1986 la dorsale NSFNET passò da 32 kbit/s a 40 bit/s).

Il problema è difficile perché è un problema distribuito di allocazione di risorse, da risolvere quasi senza informazione. Il mittente non conosce la capacità del collo di bottiglia, né quanti flussi lo condividono, né come le due cose cambiano; i soli segnali sono impliciti e tardivi: un pacchetto che non è arrivato, un ACK arrivato più tardi del solito.

L'idea chiave (Jacobson, 1988) è la **conservazione dei pacchetti**: all'equilibrio un nuovo pacchetto entra in rete solo quando uno vecchio ne esce, quindi sono gli ACK a dare il ritmo al mittente. Su questa temporizzazione implicita ogni mittente mantiene una finestra di congestione `cwnd` e sonda la rete: la aumenta con cautela finché tutto viene confermato e la riduce bruscamente a ogni segnale di congestione. L'incremento additivo con decremento moltiplicativo (**AIMD**) è la regola che fa convergere mittenti indipendenti verso una ripartizione insieme efficiente ed equa.

<!-- detail: 2 -->
## Algoritmi e modelli

Il mittente può avere in volo al più `min(cwnd, rwnd)` byte. Due variabili di stato, `cwnd` e `ssthresh`, e quattro regole (RFC 5681):

```
slow start (cwnd < ssthresh):    cwnd += MSS           per ACK  (x2 per RTT)
cong. avoid. (cwnd >= ssthresh): cwnd += MSS*MSS/cwnd  per ACK  (+1 MSS/RTT)
3 ACK duplicati: ssthresh = max(flight/2, 2*MSS); fast retransmit del buco
scadenza RTO:    ssthresh = max(flight/2, 2*MSS); cwnd = 1 MSS; RTO = 2*RTO
```

Le varianti differiscono in ciò che segue i tre ACK duplicati:

- **Tahoe**: `cwnd = 1 MSS` e di nuovo slow start fino al nuovo `ssthresh`.
- **Reno**: fast recovery. `cwnd = ssthresh + 3 MSS`, più un MSS per ogni ulteriore ACK duplicato (ciascuno segnala che un pacchetto ha lasciato la rete); al primo ACK nuovo `cwnd = ssthresh`. Una perdita per finestra viene riparata senza svuotare il canale; più perdite finiscono di solito in un timeout.
- **NewReno** (RFC 6582): ricorda il numero di sequenza più alto inviato all'inizio del recupero (`recover`). Un ACK inferiore a quel valore è un **ACK parziale**: il buco successivo viene ritrasmesso subito e il recupero continua. Si ripara un buco per RTT: è ciò che fa un mittente senza SACK (in hoplab, `sack=0`).

**Perché AIMD è equo.** Si considerino due flussi con velocità `x1`, `x2` su un collegamento di capacità `C`. L'incremento additivo aggiunge la stessa quantità a entrambi, quindi la differenza `x1 - x2` resta invariata; il decremento moltiplicativo moltiplica entrambi per `b < 1`, quindi la differenza si riduce di un fattore `b`. A ogni ciclo lo stato si avvicina alla retta `x1 = x2`, oscillando attorno alla retta di efficienza `x1 + x2 = C`. Un decremento additivo o un incremento moltiplicativo conserverebbero o amplificherebbero l'iniquità.

**CUBIC** (RFC 9438) sostituisce la sonda lineare con una funzione del tempo `t` trascorso dall'ultima riduzione, indipendente dall'RTT:

```
W(t) = C*(t - K)^3 + Wmax        K = cbrt(Wmax*(1 - beta)/C)
C = 0.4, beta = 0.7  (a ogni perdita: Wmax = cwnd; cwnd = beta*cwnd)
```

La finestra sale rapidamente, si appiattisce vicino al massimo precedente `Wmax` (dove la perdita è probabile) e poi accelera di nuovo per cercare nuova capacità. Un limite inferiore "TCP-friendly" impedisce che sia più lento di Reno sui percorsi brevi e lenti.

**La legge della radice quadrata.** A regime, con probabilità di perdita `p`, la finestra di Reno oscilla fra `W/2` e `W`. Un ciclo dura `W/2` round trip e trasporta `3W^2/8` pacchetti, dei quali esattamente uno viene perso:

```
3*W^2/8 = 1/p   =>   W = sqrt(8/(3p))
rate = (3W/4)*MSS/RTT = (MSS/RTT)*sqrt(3/(2p)) ~ 1.22*MSS/(RTT*sqrt(p))
```

**Esempio svolto (i valori di default del laboratorio).** Collo di bottiglia a 2 Mbit/s, RTT di base di circa 44 ms, pacchetti da 1500 byte: il prodotto banda-ritardo (BDP) è `2e6*0.044/(8*1500)`, circa 7 pacchetti. La coda ne contiene 25, quindi la perdita avviene a `Wmax` vicino a 7 + 25 + 2, e in effetti il log del laboratorio mostra `cwnd 35.0 → ssthresh 17.0` a ogni perdita. Fra due perdite il log mostra circa 500 segmenti, quindi `p` vale 0,002 e la formula dà `W = sqrt(8/0.006)`, circa 36. Poiché la finestra minima (17) supera il BDP (7), la coda non si svuota mai e il collegamento resta pienamente utilizzato: a un flusso Reno serve, per questo, un buffer di almeno un BDP.

<!-- detail: 3 -->
## In profondità

**Invarianti e stabilità.** Chiu e Jain hanno dimostrato che, fra i controlli lineari guidati da una retroazione binaria, l'incremento additivo con decremento moltiplicativo è l'unica combinazione che converge sia all'efficienza sia all'equità. L'equità raggiunta è sulle finestre, non sulle velocità: poiché la velocità è `W/RTT`, i flussi con RTT breve ottengono di più (la legge precedente dice velocità ∝ 1/RTT). La crescita basata sul tempo di CUBIC elimina gran parte di questa distorsione.

**Qual è il segnale?** Il controllo basato sulle perdite (Reno, CUBIC) deve riempire il buffer per trovare il limite, quindi la coda stazionaria e il ritardo crescono con la dimensione del buffer; una perdita casuale non dovuta a congestione viene scambiata per congestione. Il controllo basato sul ritardo (Vegas, poi FAST, Copa) arretra quando l'RTT supera il suo minimo e mantiene corte le code, ma soccombe ai flussi basati sulle perdite che condividono la stessa coda, ed è per questo che non è mai diventato il default. Il controllo basato su un modello (BBR) stima la banda del collo di bottiglia (massimo su finestra della velocità di consegna) e l'RTT di propagazione (minimo su finestra), trasmette a quel ritmo (pacing) e sonda periodicamente con guadagni 1,25 e 0,75. Ignora le perdite isolate, ma la versione 1 può essere iniqua verso i flussi basati sulle perdite e provocare molte perdite con buffer poco profondi; le versioni successive reagiscono anche a perdite ed ECN. ECN (RFC 3168) e DCTCP (RFC 8257) trasformano il segnale in marcature esplicite.

**Modi di guasto.** Lo slow start eccede: raddoppia finché la coda trabocca e perde fino a metà finestra in un colpo. Senza SACK quella raffica costa un RTT per buco (NewReno) o un timeout (Reno); con SACK (RFC 6675) circa un giro. La legge della radice quadrata ignora i timeout; il modello di Padhye li include e prevede velocità molto inferiori per `p` elevato.

**Che cosa fanno di diverso gli stack reali.** SACK (RFC 2018) e proportional rate reduction (RFC 6937), rilevamento delle perdite basato sul tempo con RACK-TLP (RFC 8985), finestra iniziale di 10 segmenti (RFC 6928), HyStart++ per uscire dallo slow start prima dell'eccesso (RFC 9406), pacing, e CUBIC come default in Linux, Windows e nella maggior parte degli stack QUIC.

**In hoplab.** Il modulo `tcp` implementa SACK con il recupero della RFC 6675 (più un controllo in stile RACK per le ritrasmissioni perse), gli ACK ritardati e BBR versione 1 con i suoi quattro stati e un vero timer di pacing. Non ha timestamp, né tail-loss probe, né HyStart, né proportional rate reduction: l'eccesso dello slow start perde comunque una raffica, una coda persa aspetta comunque il timer, e dopo un recupero la finestra riparte subito da ssthresh. `sack=0` dà il recupero NewReno (Reno classico con `cc=reno`), per vedere che cosa si guadagna con SACK.

**Letteratura primaria.**

- J. Nagle, "Congestion Control in IP/TCP Internetworks", RFC 896, 1984.
- V. Jacobson, "Congestion Avoidance and Control", SIGCOMM 1988.
- D.-M. Chiu, R. Jain, "Analysis of the Increase and Decrease Algorithms for Congestion Avoidance in Computer Networks", 1989.
- L. Brakmo, L. Peterson, "TCP Vegas: End to End Congestion Avoidance on a Global Internet", 1995.
- M. Mathis, J. Semke, J. Mahdavi, T. Ott, "The Macroscopic Behavior of the TCP Congestion Avoidance Algorithm", 1997.
- J. Padhye, V. Firoiu, D. Towsley, J. Kurose, "Modeling TCP Throughput: A Simple Model and its Empirical Validation", SIGCOMM 1998.
- S. Ha, I. Rhee, L. Xu, "CUBIC: A New TCP-Friendly High-Speed TCP Variant", 2008.
- N. Cardwell, Y. Cheng, C. S. Gunn, S. H. Yeganeh, V. Jacobson, "BBR: Congestion-Based Congestion Control", 2016.
- RFC 5681 (controllo di congestione di TCP), RFC 6582 (NewReno), RFC 9438 (CUBIC).

<!-- detail: 1 -->
## In laboratorio

Eseguire `./hoplab run 320-tcp-congestion`. L'host h1 invia 5 MB a h2 attraverso il collo di bottiglia r1–r2 (2 Mbit/s, 20 ms, 25 pacchetti). Selezionare h1 (`n`), aprire la scheda `tcp` dell'ispettore e osservare il grafico `cwnd (segments)`, con `ssthresh` e `rtt (ms)`; premere `g` per la vista dei grafici e `l` per il log. Su r1 guardare la coda verso r2.

**Riferimento (reno).** Prima di eseguire, calcolare il BDP in pacchetti e prevedere la finestra alla quale la coda trabocca. Nel log, le righe `SACK rivela un buco: fast retransmit … (reno+sack, cwnd X → Y)` danno la cima e il fondo di ogni dente (35 → 17 segmenti, un dente ogni 2,8 s): misurare X, Y e il periodo, e confrontarli con l'esempio svolto. L'ultima riga (`completato …`) riporta il goodput, la velocità a regime dopo il primo quarto del trasferimento, e il numero di ritrasmissioni, di fast retransmit e di timeout. Si noti il primo secondo: l'eccesso dello slow start fa scartare a r1 un'intera raffica (cwnd 67 → 33,5), e SACK la ripara in un solo recupero senza timeout. Rieseguire con `sack=0`: la stessa raffica ora costa un timeout.

**Esperimento 1: l'algoritmo.** Rieseguire con `cc=tahoe`, `cc=newreno`, `cc=cubic`, `cc=bbr`. Prevedere prima la forma di ciascun grafico di cwnd. Atteso: Tahoe scende a 1 a ogni perdita; CUBIC mostra la curva prima concava e poi convessa e riduzioni più piccole (fattore 0,7); `bbr` lascia STARTUP dopo circa 0,6 s, resta vicino al BDP, mantiene la coda quasi vuota e termina con zero ritrasmissioni. Il goodput è circa 1,9 Mbit/s per Reno, CUBIC e BBR allo stesso modo (il collegamento trasporta 1,92 Mbit/s di dati utili): con un solo flusso e un buffer da 25 pacchetti gli algoritmi differiscono nella coda e nelle perdite, non nella velocità. Tahoe è l'eccezione (circa 1,65 Mbit/s e centinaia di ritrasmissioni): riparte da un segmento e rimanda ciò che il ricevente ha già.

**Esperimento 2: il buffer.** `./hoplab run 320-tcp-congestion queue=5`. Previsione: ora il buffer è più piccolo del BDP, quindi dopo ogni dimezzamento la finestra di Reno è sotto il BDP e il collegamento resta inattivo; il goodput scende (circa 1,83 Mbit/s) e i fast retransmit diventano frequenti (38 invece di 8). CUBIC, che taglia solo al 70%, dovrebbe perdere meno (resta a 1,91). Poi `queue=200`: ora lo slow start eccede di oltre cento segmenti prima della prima perdita, il recupero che segue ne rimanda diverse centinaia, e da lì in poi la serie `rtt (ms)` sale a diverse volte l'RTT di base a ogni dente. È bufferbloat, pagato senza alcun throughput in più. `cc=bbr` sullo stesso buffer profondo non lo riempie mai.

**Esperimento 3: perdite non dovute a congestione.** `./hoplab run 320-tcp-congestion loss=0.05`, con `cc=reno` e poi `cc=bbr`. Usare la legge della radice quadrata per prevedere la velocità di Reno (W di circa 5,5 segmenti, sotto il BDP), poi confrontare: Reno misura circa 1,1 Mbit/s e CUBIC 1,2, mentre `bbr`, che non legge la perdita come congestione, mantiene circa 1,8 Mbit/s con un numero simile di ritrasmissioni. Si può anche introdurre la perdita in un laboratorio in esecuzione con `:` e `link r1 r2 loss=0.05`.

**Esperimento 4: che cosa si guadagna con SACK.** `./hoplab run 321-tcp-sack`, poi `./hoplab run 321-tcp-sack sack=0`. L'eccesso dello slow start perde circa due dozzine di segmenti di una stessa finestra su un percorso con RTT di 84 ms. Prevedere il costo di ripararli uno per giro. Atteso: con SACK un solo recupero, circa 24 ritrasmissioni, nessun timeout, 1,0 s in tutto; con i soli ACK cumulativi NewReno parte bene, il suo timer scade due volte, gran parte della finestra viene rimandata (circa 75 ritrasmissioni) e il trasferimento dura 2,4 s. Aprire un ACK duplicato nella vista Pacchetti e leggerne i blocchi SACK; a dettaglio 2 il pannello tcp di h1 conta i segmenti in volo, riscontrati selettivamente e persi.

## Verifica

- Ricavare la relazione velocità ≈ 1,22·MSS/(RTT·√p) dal dente di sega, enunciando ogni ipotesi. Quale ipotesi cade per prima al crescere di p, e in quale direzione sbaglia allora la formula?
- Due flussi Reno con RTT di 20 ms e 200 ms condividono un collo di bottiglia drop-tail e vedono la stessa probabilità di perdita. Qual è il rapporto fra i loro throughput? Come cambia la risposta con CUBIC, e perché?
- Dato un collo di bottiglia di capacità C, un round-trip time di base T e un buffer drop-tail di B pacchetti, dare la condizione su B perché un singolo flusso Reno mantenga il collegamento pienamente utilizzato. Ripetere per un fattore di decremento moltiplicativo pari a 0,7.
- Mostrare con il diagramma vettoriale a due flussi perché incremento additivo/decremento additivo non converge all'equità, e perché non vi converge nemmeno incremento moltiplicativo/decremento moltiplicativo.
- Un flusso basato sul ritardo e uno basato sulle perdite condividono un buffer drop-tail profondo. Descrivere l'equilibrio e spiegare che cosa fa BBR di diverso da Vegas per non essere affamato.
