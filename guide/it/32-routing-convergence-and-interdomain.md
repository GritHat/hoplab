<!-- level: 3 -->
<!-- labs: 330-ospf-vs-rip, 331-bgp-basics -->
# Convergenza del routing e routing interdominio

Un protocollo di routing è un algoritmo distribuito che deve raggiungere un insieme coerente di tabelle di inoltro, e raggiungerlo di nuovo dopo ogni cambiamento. Fra un guasto e il momento in cui tutti i router sono d'accordo, le tabelle sono incoerenti fra loro: i pacchetti vengono scartati da un router che non ha una rotta (un buco nero) oppure circolano fra router ciascuno dei quali crede che l'altro sia più vicino (un ciclo transitorio). Il **tempo di convergenza** è quindi la grandezza che conta nell'esercizio di una rete, molto più dell'ottimalità dei percorsi.

È difficile perché nessun router vede la rete: ciascuno agisce su informazioni parziali, ritardate e magari già false. Un router link-state diffonde fatti (i miei collegamenti, i loro costi) e tutti calcolano sulla stessa mappa; le cattive notizie viaggiano veloci quanto le buone. Un router distance-vector scambia conclusioni (la mia distanza da X), e una conclusione può dipendere, senza che lo si sappia, proprio dal router che la riceve: da qui il recupero lento e il conteggio all'infinito.

Fra sistemi autonomi il problema cambia natura. Non c'è una metrica comune, nessun AS rivela la propria topologia e ciascuno vuole applicare la propria politica commerciale. BGP risponde con un **path vector**: ogni rotta porta con sé l'elenco degli AS attraversati, il che garantisce l'assenza di cicli senza una mappa globale e offre alla politica qualcosa su cui agire. Il prezzo è che la convergenza è lenta, e in generale nemmeno garantita.

<!-- detail: 2 -->
## Algoritmi e modelli

**Anatomia della convergenza.** Per qualunque protocollo:

```
T_conv = T_detect + T_propagate + T_compute + T_install
```

Il rilevamento (`T_detect`) è fisico (perdita della portante, millisecondi) oppure per timeout (dead interval di OSPF = 4 hello; hold time di BGP = 3 keepalive; timeout delle rotte RIP = 6 aggiornamenti).

**Link state.** Ogni router origina un LSA (i suoi vicini e i relativi costi) con un numero di sequenza; il flooding è affidabile: un LSA più recente viene memorizzato, confermato e inoltrato su tutte le altre interfacce. Dopo un'attesa (hold-down) che raggruppa i cambiamenti correlati, ogni router esegue Dijkstra sul database, in `O((V + E) log V)`. Quindi `T_propagate` vale pochi ritardi di collegamento e `T_compute` è dominato dall'hold-down di SPF.

**Distance vector.** Ogni router mantiene, per destinazione, una distanza e un next hop, e applica Bellman-Ford a ciò che i vicini annunciano:

```
D_x(d) = min sui vicini v di ( c(x,v) + D_v(d) )
```

RIP ricorda solo l'offerta migliore. Quando quella rotta muore, un'alternativa che era stata scartata non è più in memoria e ritorna solo con il successivo aggiornamento periodico del vicino: `T_propagate` è uniforme in `(0, update)` per hop. Senza split horizon un router può riaccettare da un vicino la propria stessa rotta e contare verso l'alto fino a 16. Split horizon, poisoned reverse e triggered update eliminano solo il caso a due nodi.

**Path vector.** Una rotta BGP è un prefisso più degli attributi; AS_PATH viene esteso a ogni esportazione, e un router scarta una rotta il cui AS_PATH contiene il proprio AS. Fra più rotte per uno stesso prefisso il processo di decisione è un confronto lessicografico:

```
1. LOCAL_PREF più alto           (politica: cliente > peer > provider)
2. AS_PATH più corto
3. ORIGIN più basso
4. MED più basso                 (solo fra rotte dallo stesso AS vicino)
5. eBGP preferito a iBGP
6. costo IGP minimo verso NEXT_HOP   (hot-potato routing)
7. router ID più basso           (spareggio)
```

Si esporta solo la rotta migliore, nel rispetto della politica di esportazione (le rotte dei clienti a tutti; le rotte di peer e provider solo ai clienti). Una rotta che scompare viene annunciata con un **withdrawal** esplicito.

**Esplorazione dei percorsi (path exploration).** Dopo un withdrawal un router non sa che la destinazione è scomparsa: seleziona la migliore fra le rotte che ancora possiede, che può essere obsoleta e passare proprio per il guasto, e la annuncia. I vicini fanno lo stesso. Il sistema attraversa percorsi obsoleti via via più lunghi prima che siano tutti eliminati; ogni passo è ritardato dal minimum route advertisement interval (MRAI, 30 s nella pratica). In una clique di `n` AS il caso peggiore esplora un numero di percorsi fattoriale in `n`.

**Esempio svolto (laboratorio 330).** 50 datagrammi al secondo attraversano r1–r2–r3; il collegamento r2–r3 si guasta a 20,000 s. Con OSPF, r2 vede cadere la portante a 20,044, il suo LSA raggiunge ogni router entro 20,056, SPF viene eseguito dopo l'hold-down di 200 ms e la deviazione è installata a 20,22: circa 11 datagrammi persi. Con RIP, r1 sente "irraggiungibile" entro 0,2 s, ma la deviazione dipende dal successivo aggiornamento periodico di r4, uniforme in 0–5 s: in media 2,5 s, circa 125 datagrammi, con un massimo vicino a 250.

<!-- detail: 3 -->
## In profondità

**Correttezza.** Il link state è corretto una volta che i database sono uguali: ogni router esegue lo stesso calcolo deterministico sullo stesso grafo, quindi i next hop sono coerenti. Durante il flooding non lo sono, e sono possibili micro-cicli; i rimedi sono l'aggiornamento ordinato delle FIB e le alternative loop-free precalcolate (RFC 5286). Il distance vector converge per costi statici (Bellman-Ford asincrono), ma dopo un aumento di costo distanze obsolete possono propagarsi lungo un ciclo. DUAL (EIGRP) mantiene l'invariante di fattibilità "accetta un vicino solo se la distanza che dichiara è minore della mia migliore distanza di sempre" ed è privo di cicli in ogni istante, al prezzo di una computazione diffusa.

**BGP può non convergere.** Ogni AS ordina i percorsi secondo preferenze private; lo Stable Paths Problem che ne risulta può non avere soluzione (il BAD GADGET oscilla per sempre), averne più d'una, o averne raggiungibili solo per caso, e decidere quale caso si presenta è NP-completo. Gao e Rexford hanno mostrato che, se ogni AS preferisce le rotte dei clienti ed esporta in modo valley-free, senza cicli nelle relazioni di provider, esiste un unico stato stabile e viene sempre raggiunto: è l'economia, non il protocollo, a mantenere stabile Internet. Il rilevamento dei cicli del path vector evita il conteggio all'infinito ma non l'esplorazione dei percorsi: Labovitz et al. hanno misurato minuti di convergenza dopo un withdrawal.

**Compromessi.** Timer brevi rilevano prima ma trasformano la congestione in falsi guasti e in instabilità (flapping); i ritardi di SPF e MRAI smorzano il rumore al prezzo di tempo di interruzione. Le reti reali rilevano con BFD (RFC 5880) in decine di millisecondi, regolano SPF con un back-off esponenziale e proteggono con il fast reroute; BGP aggiunge il route-flap damping (RFC 2439), che può a sua volta ritardare la convergenza, e i route reflector e iBGP all'interno di ogni AS.

**In netlab.** `ospf`: una sola area, nessun designated router, una nuova adiacenza invia l'intero database invece dello scambio di database description, gli LSA non invecchiano, i formati dei pacchetti non sono esatti al bit. `rip`: formato RIPv2 su UDP 520, senza autenticazione. `bgp`: sessioni su TCP, porta 179, con i messaggi e la macchina a stati della RFC 4271, solo eBGP, un router per AS, niente MED, community o aggregazione; il processo di decisione è LOCAL_PREF dalla relazione, lunghezza di AS_PATH, router ID; MRAI vale 500 ms di default.

**Letteratura primaria.**

- R. Bellman, "On a Routing Problem", 1958; E. W. Dijkstra, "A Note on Two Problems in Connexion with Graphs", 1959.
- J. McQuillan, I. Richer, E. Rosen, "The New Routing Algorithm for the ARPANET", 1980.
- J. J. Garcia-Luna-Aceves, "Loop-Free Routing Using Diffusing Computations", 1993.
- C. Labovitz, A. Ahuja, A. Bose, F. Jahanian, "Delayed Internet Routing Convergence", SIGCOMM 2000.
- L. Gao, J. Rexford, "Stable Internet Routing Without Global Coordination", 2001.
- T. Griffin, F. B. Shepherd, G. Wilfong, "The Stable Paths Problem and Interdomain Routing", 2002.
- RFC 2453 (RIPv2), RFC 2328 (OSPFv2), RFC 4271 (BGP-4).

<!-- detail: 1 -->
## In laboratorio

**330-ospf-vs-rip.** `./netlab run 330-ospf-vs-rip`, poi `./netlab run 330-ospf-vs-rip proto=rip`. Da 12 s un flusso di 50 datagrammi/s va da h1 a h3; a 20 s il collegamento r2–r3 cade. Selezionare h3 e aprire la scheda `sink`: il contatore `lost` diviso per 50 è l'interruzione in secondi (le ultime righe del log danno lo stesso numero come `check lost`). Nel log (`l`, dettaglio `2`) seguire, per OSPF, "interfaccia non attiva", "genero il router-LSA", "più recente: memorizzato e inoltrato agli altri", "SPF #2"; per RIP, "ora è irraggiungibile", "aggiornamento immediato", e poi l'attesa. Il traceroute a 58 s deve mostrare 5 hop.

- Ripetere RIP con più semi (`--seed 1`, `--seed 2`, …). Previsione: la perdita è distribuita fra pochi e circa 250 datagrammi, perché dipende dalla fase del timer di r4; OSPF resta vicino a 11. La verifica `blackout` del laboratorio enuncia il limite atteso per ogni impostazione: al più hello + spf per OSPF, fra tre e quattro hello più spf con `carrier=0`, fino a circa un periodo di aggiornamento per RIP.
- `proto=rip update=15s`: prevedere la perdita media e quella massima (375 e 750).
- `proto=ospf carrier=0`: ora il rilevamento aspetta il dead interval, 4 hello da 1 s. Prevedere circa 160 persi. Poi `spf=2s`: circa 100.
- In un laboratorio in esecuzione, `:` poi `link r2 r3 up`, attendere, e di nuovo `link r2 r3 down`: l'esperimento si ripete con una fase diversa dei timer.

**331-bgp-basics.** `./netlab run 331-bgp-basics`. Tre AS a triangolo. Nella vista dei pacchetti (`p`) seguire una sessione dall'handshake TCP verso la porta 179 fino a OPEN e KEEPALIVE, poi aprire un UPDATE e leggere l'AS_PATH. Selezionare r1 e aprire la scheda `bgp`: 10.3.0.0/24 è noto con percorso `65003` e con `65002 65003`, e il log dichiara il motivo di ogni scelta (per esempio "unico percorso", "router id più basso"). A 25 s il collegamento r1–r3 si guasta: la sessione cade, r1 passa al percorso memorizzato via AS 65002 e il traceroute a 37 s mostra 4 hop. A 40 s r3 ritira 10.3.0.0/24 e all'ultimo ping risponde "network unreachable" r1.

- Esplorazione dei percorsi in miniatura (da fare prima del guasto a 25 s). `:` poi `r3 bgp announce 10.9.0.0/24`; attendere qualche secondo; `r3 bgp withdraw 10.9.0.0/24`. Previsione: r2 non perde subito la rotta; per un MRAI installa il percorso obsoleto `65001 65003` appreso da r1 e offre perfino `65002 65001 65003` a r3, che lo rifiuta ("il nostro AS è nel path: ciclo, rifiutata"); solo dopo il withdrawal di r1 il prefisso scompare. Il ritiro di 10.3.0.0/24 previsto a 40 s non mostra esplorazione: a quel punto il triangolo è rotto e r2 non ha alternative su cui ripiegare.
- `prepend=2`: r3 esporta `65003 65003 65003` solo verso r1 (`prepend.r1=`), e il semplice `65003` verso r2. Prevedere la scelta di r1 per 10.3.0.0/24 e il primo traceroute. r1 ora preferisce `65002 65003` (due AS contro tre) e il traceroute a 15 s mostra 4 hop invece di 3: ingegneria del traffico in ingresso. Con `prepend=1` i due percorsi sono pari e decide il router id più basso. Spiegare perché un prepending verso tutti i vicini non cambierebbe nulla.
- `keepalive=1s`: l'hold time diventa 3 s. Qui il guasto è rilevato dalla caduta dell'interfaccia, quindi l'interruzione cambia appena; dire quando l'hold time conterebbe.

## Verifica

- Scomporre il tempo di convergenza di OSPF e di RIP dopo il guasto di un collegamento sul percorso migliore, e individuare per ciascuno il termine dominante e il parametro che lo controlla. Perché il termine di RIP è una variabile aleatoria?
- Dare una topologia di tre router e una sequenza di eventi in cui lo split horizon con poisoned reverse non impedisce il conteggio all'infinito.
- Dimostrare che il controllo su AS_PATH rende BGP privo di cicli a regime, e spiegare perché non limita il numero di stati intermedi dopo un withdrawal.
- Due rotte verso lo stesso prefisso differiscono così: A ha LOCAL_PREF 100 e AS_PATH di lunghezza 2; B ha LOCAL_PREF 200 e AS_PATH di lunghezza 5. Quale viene scelta, e che cosa implica per l'AS-path prepending come strumento di ingegneria del traffico?
- Enunciare le condizioni di Gao–Rexford e spiegare informalmente perché escludono un BAD GADGET.
