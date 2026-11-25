<!-- level: 4 -->
<!-- labs: 420-tcp-fairness, 421-bbr-anatomy -->
# Equità e controllo di congestione basato su modello

Il controllo di congestione è stato progettato per una rete in cui tutti i mittenti eseguono lo stesso algoritmo. Sotto questa ipotesi AIMD ha una proprietà limpida: flussi che osservano lo stesso segnale di perdita convergono a finestre uguali. L'Internet di oggi viola l'ipotesi in ogni direzione. Reno, CUBIC e BBR convivono sugli stessi colli di bottiglia; i tempi di andata e ritorno differiscono di due ordini di grandezza; i buffer vanno da una frazione del prodotto banda-ritardo a secondi di coda. La domanda di questo argomento è: **quando controllori eterogenei condividono un collo di bottiglia, chi ottiene che cosa, e secondo quale definizione l'esito è "equo"?**

La domanda resta aperta per tre ragioni. Non esiste un obiettivo di equità condiviso (tassi uguali, max-min, proporzionale, oppure "non nuocere più di quanto faccia l'algoritmo già diffuso"). L'esito dipende da parametri che gli host non possono osservare, in particolare la dimensione del buffer e la disciplina di coda. E i controllori basati su modello, come BBR, sostituiscono il segnale di perdita con un modello esplicito del percorso: la dinamica non è più un unico anello di retroazione condiviso, ma un insieme di stimatori che si perturbano a vicenda.

Il laboratorio mette due flussi su un collo di bottiglia e permette di scegliere l'algoritmo di ciascuno, il buffer e l'RTT. È un esperimento piccolo e deterministico: utile per formulare ipotesi, non per chiuderle.

<!-- detail: 2 -->
## Definizioni di equità

- **Indice di Jain** per i tassi x1..xn: J = (somma degli xi)^2 / (n per la somma degli xi^2). Vale 1 quando tutti i tassi sono uguali e 1/n quando un solo flusso prende tutto. Misura l'uguaglianza, non l'efficienza, ed è cieco rispetto al motivo per cui un flusso è lento (un flusso limitato altrove abbassa l'indice).
- **Equità max-min**: un'allocazione in cui nessun tasso può crescere senza far diminuire un tasso già minore o uguale. È ciò che il fair queueing per flusso impone su un singolo collo di bottiglia.
- **Equità proporzionale** (Kelly): l'allocazione che massimizza la somma dei log(xi). Su percorsi con più colli di bottiglia assegna meno ai flussi che consumano più risorse; un AIMD idealizzato approssima un obiettivo pesato affine.
- **TCP-friendliness**: un flusso non dovrebbe prendere più di quanto prenderebbe un flusso Reno nelle stesse condizioni. Il tasso di Reno segue la legge della radice quadrata, tasso ≈ (MSS/RTT) · sqrt(3/2) / sqrt(p), quindi "friendly" è a sua volta sbilanciato rispetto all'RTT.

## Controllori basati sulle perdite

Reno aumenta la finestra di un segmento per RTT e la dimezza a ogni perdita. Poiché l'incremento è per RTT, due flussi Reno con perdite sincronizzate ottengono tassi in un rapporto compreso fra RTT2/RTT1 e (RTT2/RTT1)^2: vince il percorso breve. CUBIC rende la finestra una funzione cubica del tempo reale trascorso dall'ultima perdita, W(t) = C(t − K)^3 + Wmax con C = 0,4 e fattore di riduzione 0,7; la crescita non dipende dall'RTT, il che riduce (senza eliminarla) l'iniquità da RTT e rende CUBIC più aggressivo di Reno quando il prodotto banda-ritardo è grande. Con finestre piccole CUBIC ricade in una "regione TCP-friendly" in cui emula Reno: su percorsi lenti e corti i due si comportano allo stesso modo.

## Il modello di BBR

BBR (v1) non tratta la perdita come un segnale. Stima due grandezze: **BtlBw**, un massimo su finestra temporale del tasso di consegna misurato, e **RTprop**, un minimo su finestra dell'RTT. Trasmette con pacing a guadagno × BtlBw e limita i dati in volo a circa 2 × BtlBw × RTprop. Una fase di startup con guadagno 2,89 trova la banda del collo di bottiglia, una fase di drain svuota la coda così creata, poi un ciclo di guadagni in otto fasi (1,25, 0,75, 1, 1, 1, 1, 1, 1) sonda la disponibilità di altra banda; periodicamente PROBE_RTT riduce la finestra per aggiornare RTprop.

I problemi di coesistenza discendono dal modello. In un buffer poco profondo BBR ignora le perdite causate dal proprio sondaggio mentre i flussi basati sulle perdite continuano a dimezzare: BBR prende gran parte del collegamento e il tasso di ritrasmissione sale. In un buffer profondo un flusso basato sulle perdite riempie la coda, il limite sui dati in volo di BBR (calcolato da un RTprop ormai obsoleto) diventa vincolante, e la quota di BBR è determinata da quel limite e non da una regola di equità. Nemmeno più flussi BBR con RTT diversi convergono a quote uguali. BBRv2 e v3 aggiungono perdita ed ECN come vincoli sul modello per attenuare questi effetti.

## Che cosa cambiano ECN e L4S

L'ECN classico (RFC 3168) permette a un AQM di marcare invece di scartare, e una marca è trattata come una perdita. L4S (RFC 9330–9332) ridefinisce il segnale: una marcatura a soglia bassa la cui frequenza, non la semplice presenza, codifica la congestione, con controllori scalabili (del tipo DCTCP, TCP Prague) e un AQM a doppia coda accoppiata che li isola dai flussi classici ma accoppia le probabilità di marcatura in modo che i tassi restino grosso modo confrontabili. L'equità si sposta così dagli host a un accordo fra gli host e il collo di bottiglia.

<!-- detail: 3 -->
## Problemi aperti

**Qual è la soglia per il dispiegamento?** Ware e colleghi sostengono che l'indice di Jain e la TCP-friendliness siano i test sbagliati per un nuovo algoritmo e propongono un criterio basato sul danno: un nuovo arrivato non dovrebbe danneggiare l'algoritmo esistente più di quanto farebbe un altro flusso dello stesso tipo. Non c'è consenso, né una batteria di test accettata da tutti.

**La starvation è inevitabile?** Arun, Alizadeh e Balakrishnan (2022) mostrano che ogni controllore end-to-end che limita il ritardo, in un'ampia classe, può affamare qualche flusso quando i percorsi presentano variazioni di ritardo non dovute alla congestione. Se il supporto della rete (fair queueing, L4S) sia necessario e non solo comodo è una questione di progetto aperta.

**Fedeltà del modello.** Il comportamento di BBR contro i flussi basati sulle perdite è governato dal limite sui dati in volo, come mostra il modello di Ware e colleghi (2019), e dalla dinamica degli stimatori (aggregazione, PROBE_RTT, campioni limitati dall'applicazione) che i modelli ridotti tendono a perdere.

## Il modello del laboratorio e la realtà

Prima di trarre conclusioni si legga `./netlab help tcp`. Questo TCP ha SACK con il recupero della RFC 6675 (e un controllo in stile RACK per le ritrasmissioni perse) e gli ACK ritardati, ma **non ha timestamp, né tail-loss probe, né proportional rate reduction, né HyStart**: lo slow start eccede fin dove il buffer glielo permette, e un flusso schiacciato a pochi segmenti recupera le sue perdite a forza di timeout (si contino i timeout di CUBIC contro BBR con `queue=10`). Il `bbr` è BBRv1 come nell'articolo del 2016 e in draft-cardwell-iccrg-bbr-congestion-control-00: campioni di velocità di consegna a ogni ACK con marcatura dei periodi limitati dall'applicazione, massimo su dieci giri per BtlBw, minimo su dieci secondi per RTprop, STARTUP, DRAIN, il ciclo in otto fasi di PROBE_BW, PROBE_RTT, pacing, un tetto ai dati in volo pari al doppio del BDP stimato e conservazione dei pacchetti durante il recupero; manca solo il campionamento a lungo termine (policer). Il laboratorio `421-bbr-anatomy` mostra la macchina a stati su un solo flusso. Le misure pubblicate trovano che BBRv1 favorisce il flusso con RTT più lungo: qui `cc1=bbr cc2=bbr delay2=80ms` dà al flusso con RTT lungo circa quattro volte la velocità dell'altro, lo stesso segno. È BBRv1, non v2 né v3: nessuno dei limiti basati su perdite ed ECN aggiunti dalle versioni successive.

Altre due minacce alla validità. Primo, gli effetti di fase: i due flussi partono entro mezzo secondo casuale l'uno dall'altro (`jitter=500ms`), quindi semi diversi danno esecuzioni diverse, ma una volta sincronizzati da una coda drop-tail lo schema di chi perde a ogni traboccamento può persistere, come descritto da Floyd e Jacobson, e una piccola variazione del ritardo può rovesciare l'esito: la scansione dell'RTT dell'esperimento 1 non è monotona. Secondo, la scala: le velocità a regime sono misurate sui trenta secondi in cui entrambi i flussi hanno superato l'avvio, il che elimina la confusione dovuta al vantaggio iniziale, ma due flussi e un solo collo di bottiglia sono lontani dal regime a molti flussi in cui di solito si enunciano i risultati sull'equità.

## Letteratura primaria

- D.-M. Chiu, R. Jain, "Analysis of the Increase and Decrease Algorithms for Congestion Avoidance in Computer Networks", Computer Networks and ISDN Systems, 1989.
- R. Jain, D.-M. Chiu, W. Hawe, "A Quantitative Measure of Fairness and Discrimination for Resource Allocation in Shared Computer Systems", DEC Research Report TR-301, 1984.
- F. Kelly, A. Maulloo, D. Tan, "Rate Control for Communication Networks: Shadow Prices, Proportional Fairness and Stability", Journal of the Operational Research Society, 1998.
- M. Mathis, J. Semke, J. Mahdavi, T. Ott, "The Macroscopic Behavior of the TCP Congestion Avoidance Algorithm", ACM CCR, 1997.
- S. Floyd, V. Jacobson, "On Traffic Phase Effects in Packet-Switched Gateways", Internetworking: Research and Experience, 1992.
- S. Ha, I. Rhee, L. Xu, "CUBIC: A New TCP-Friendly High-Speed TCP Variant", ACM SIGOPS Operating Systems Review, 2008; RFC 9438 (2023).
- N. Cardwell, Y. Cheng, C. S. Gunn, S. H. Yeganeh, V. Jacobson, "BBR: Congestion-Based Congestion Control", ACM Queue, 2016.
- M. Hock, R. Bless, M. Zitterbart, "Experimental Evaluation of BBR Congestion Control", IEEE ICNP, 2017.
- R. Ware, M. Mukerjee, S. Seshan, J. Sherry, "Modeling BBR's Interactions with Loss-Based Congestion Control", ACM IMC, 2019.
- R. Ware, M. Mukerjee, S. Seshan, J. Sherry, "Beyond Jain's Fairness Index: Setting the Bar for the Deployment of Congestion Control Algorithms", ACM HotNets, 2019.
- V. Arun, M. Alizadeh, H. Balakrishnan, "Starvation in End-to-End Congestion Control", ACM SIGCOMM, 2022.
- M. Alizadeh et al., "Data Center TCP (DCTCP)", ACM SIGCOMM, 2010.
- RFC 5681 (controllo di congestione di TCP), RFC 3168 (ECN), RFC 9330, 9331, 9332 (architettura L4S, segnalazione, AQM DualQ accoppiato).

<!-- detail: 1 -->
## In laboratorio

Eseguire `./netlab run 420-tcp-fairness`. Topologia: h1 e h2 trasmettono attraverso r1 — r2 (4 Mbit/s, 15 ms, coda drop-tail da 40 pacchetti) verso d1 e d2. Entrambi i flussi partono a 1 s più uno scarto casuale fino a mezzo secondo e trasmettono per 40 s. Parametri: `cc1`, `cc2` (reno, tahoe, newreno, cubic, bbr), `queue`, `bw`, `delay`, `delay2`. L'RTT di base del flusso 1 è di circa 38 ms, quindi un prodotto banda-ritardo vale circa 13 pacchetti.

Osservare i grafici di cwnd di h1 e h2 e le righe finali: ogni riga `completato` dà il goodput sull'intero trasferimento e la velocità **a regime** (byte riscontrati dopo i primi dieci secondi, divisi per il tempo), poi una riga dà l'indice di Jain sulle due velocità a regime. Per il lavoro in serie eseguire senza interfaccia e tenere solo quelle righe:

```
./netlab run 420-tcp-fairness cc1=bbr cc2=cubic queue=10 --headless --detail 1 --lang en | grep -E 'done|Jain'
```

Su h1 e h2 esistono i contatori `tcp.steady_kbps`, `tcp.share_pct`, `tcp.jain_pct`, `tcp.goodput_kbps`, `tcp.retransmits` e `tcp.timeouts`. Per le quote si usi la velocità a regime, non il goodput; si ripeta ogni punto con almeno cinque semi (`--seed 1` … `--seed 5`) e si riporti la dispersione.

**Esperimento 1 — iniquità da RTT.** Ipotesi: a parità di algoritmo basato sulle perdite, la quota di un flusso diminuisce al crescere del suo RTT, con un esponente fra 1 e 2. Manipolazione: `cc1=reno cc2=reno` con `delay2=2ms`, `10ms`, `20ms`, `40ms`, `80ms`; ripetere con `cc1=cubic cc2=cubic` e con `cc1=bbr cc2=bbr`. Metrica: rapporto fra le due velocità a regime in funzione del rapporto fra gli RTT. Atteso: per Reno una curva rumorosa e non monotona (nelle nostre esecuzioni il flusso 2 aveva 0,6 volte la velocità del flusso 1 a 10 ms, 1,4 a 20 ms, 0,3 a 40 ms e 0,7 a 80 ms): con due flussi su una coda drop-tail si misura la fase tanto quanto l'RTT, quindi si faccia la media su più semi e su ritardi vicini prima di stimare un esponente, oppure si rendano casuali gli scarti con `link r1 r2 qdisc=red` al prompt `:`. Per BBR il segno è rovesciato: vince il flusso con RTT lungo (circa 3,1 contro 0,7 Mbit/s a 80 ms), perché la sua stima più grande del BDP gli lascia tenere più dati in coda.

**Esperimento 2 — la profondità del buffer decide fra BBR e CUBIC.** Ipotesi: BBR domina con un buffer piccolo ed è dominato con uno profondo. Manipolazione: `cc1=bbr cc2=cubic` e l'inverso `cc1=cubic cc2=bbr`, ciascuno con `queue=10`, `40`, `200`. Metrica: velocità a regime, indice di Jain, ritrasmissioni e timeout di ciascun flusso. Atteso: con `queue=10` (meno di un BDP) BBR si prende circa 3,6 dei 3,9 Mbit/s e CUBIC, tagliato a ognuno dei traboccamenti che BBR ignora, vive di timeout (una settantina), J circa 0,6; con `queue=40` le quote stanno entro un fattore due, CUBIC in vantaggio (J circa 0,95); con `queue=200` è CUBIC a prendersi 3,4 Mbit/s, perché riempie il buffer e il tetto di BBR, due volte la sua stima del BDP, gli lascia una piccola parte di quella coda. Scambiare `cc1` e `cc2` non cambia nulla oltre il rumore, ora che partono insieme.

**Esperimento 3 — l'equità fornita dalla rete.** Ipotesi: il fair queueing per flusso rende la ripartizione indipendente dagli algoritmi. Manipolazione: ripetere l'esperimento 2 con `queue=200` e, appena il laboratorio si apre (mettendolo prima in pausa con la barra spaziatrice), digitare `link r1 r2 qdisc=fq`; poi provare `qdisc=codel`. Metrica: come sopra, più i grafici dell'RTT. Esito atteso: con `fq` ripartizione quasi uguale (J circa 1,0) quali che siano i due algoritmi, anche se CUBIC continua a far traboccare la propria coda; con `codel` coda corta, ma quote che possono ancora dipendere dall'algoritmo e da chi è partito per primo.

**Esperimento 4 — il modello, un flusso alla volta.** Prima di fidarsi di ciò che BBR fa contro un altro flusso, lo si osservi da solo: `./netlab run 421-bbr-anatomy`. Ipotesi: BtlBw segue un calo della velocità del collo di bottiglia solo dopo dieci giri, e una risalita al 25% per ciclo di sonda. Manipolazione: il laboratorio dimezza il collo di bottiglia a 14 s e lo ripristina a 22 s; si leggano le serie `bbr btlbw`, `pacing` e `rtt` nella vista Grafici di h1 e le transizioni di stato nel registro a dettaglio 2. Metrica: tempo da ogni cambiamento al nuovo BtlBw, e la coda di r1 nel frattempo. Atteso: STARTUP termina dopo circa 0,6 s, PROBE_RTT compare ogni dieci secondi, la stima impiega circa mezzo secondo (dieci giri) a scendere mentre la coda cresce, e qualche secondo di sonde a 1,25 per risalire. Con `queue=5` STARTUP perde pacchetti e BBR non rallenta per questo.

## Domande per la discussione

- Indice di Jain su quale intervallo, e su quale popolazione di flussi? Si costruiscano due finestre di misura della stessa esecuzione che portino a conclusioni opposte.
- Se l'accodamento per flusso al collo di bottiglia rende irrilevante l'equità fra host, perché non è stato adottato ovunque, e che cosa concede a questo argomento la doppia coda di L4S?
- Il limite sui dati in volo vincola la quota di BBR nei buffer profondi. È un meccanismo di equità o un accidente? Come sarebbe fatto un meccanismo esplicito?
- Il TCP del laboratorio ha SACK ma non il tail-loss probe, usa il pacing solo per BBR e non ha HyStart. Per ciascuna mancanza si dica quale conclusione potrebbe cambiare, e si progetti l'esperimento che lo direbbe.
- "Non nuocere più di CUBIC" è un criterio di dispiegamento stabile quando CUBIC non sarà più maggioritario?
