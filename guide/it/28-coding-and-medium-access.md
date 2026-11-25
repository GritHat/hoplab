<!-- level: 2 -->
<!-- labs: 260-coding-bench, 261-csma-cd, 272-priority-queueing -->
# Codifica, accesso al mezzo e code a priorità

Questo capitolo riunisce tre meccanismi dei livelli bassi. Ciascuno risponde a una domanda del tipo «qualcosa non basta per tutti: chi paga?».

**I bit si corrompono.** Un codice rivelatore permette al ricevente di scartare un frame danneggiato; un **codice correttore** aggiunge abbastanza ridondanza da localizzare l'errore e ripararlo senza ritrasmissione. Il classico **codice di Hamming** protegge 4 bit di dati con 3 bit di parità disposti in modo che lo schema dei controlli falliti, la **sindrome**, sia la posizione del bit sbagliato scritta in binario. Il prezzo è il **tasso del codice** (code rate): solo 4 bit su 7 inviati sono dati.

**Un canale, molte stazioni.** Se due trasmettono insieme entrambi i frame vanno persi. Il **rilevamento della portante** (carrier sensing, CSMA) significa ascoltare prima di parlare; l'Ethernet classica aggiunge il **rilevamento delle collisioni** (CSMA/CD): continuare ad ascoltare mentre si trasmette, fermarsi subito in caso di collisione e riprovare dopo un ritardo casuale estratto da una finestra che raddoppia a ogni collisione successiva, il **backoff esponenziale binario**.

**Un collegamento di uscita, più traffico di quanto ne possa portare.** Qualcosa deve aspettare o essere scartato. Una coda FIFO tratta tutti i pacchetti allo stesso modo; l'**accodamento a priorità stretta** (strict priority queueing) mantiene più code e serve sempre la più alta non vuota, così una piccola quantità di traffico sensibile al ritardo, come la voce, non risente della congestione, e tutto il danno ricade sul resto.

<!-- detail: 2 -->
## Hamming(7,4)

Numeriamo le posizioni della parola di codice da 1 a 7. Le posizioni 1, 2 e 4 (le potenze di due) contengono i bit di parità p1, p2, p3; le posizioni 3, 5, 6, 7 contengono i bit di dati da d1 a d4. Ogni bit di parità copre le posizioni il cui numero ha la corrispondente cifra binaria a 1:

- p1 copre le posizioni 1, 3, 5, 7: p1 = d1 ⊕ d2 ⊕ d4
- p2 copre le posizioni 2, 3, 6, 7: p2 = d1 ⊕ d3 ⊕ d4
- p3 copre le posizioni 4, 5, 6, 7: p3 = d2 ⊕ d3 ⊕ d4

Il ricevente ricalcola i tre controlli: s1, s2, s3 sono lo XOR dei bit di ciascun gruppo, bit di parità compreso. La sindrome è il numero s3 s2 s1 in binario: 0 significa nessun errore rilevato, altrimenti è la posizione da invertire.

Esempio svolto: dati 1011. p1 = 1⊕0⊕1 = 0, p2 = 1⊕1⊕1 = 1, p3 = 0⊕1⊕1 = 0, quindi la parola di codice è `0110011`. Il canale inverte la posizione 5: `0110111`. Allora s1 = 0⊕1⊕1⊕1 = 1, s2 = 1⊕1⊕1⊕1 = 0, s3 = 0⊕1⊕1⊕1 = 1: sindrome 101 = 5. Si reinverte il bit 5.

Due parole di codice qualsiasi differiscono in almeno 3 posizioni (distanza minima 3), quindi un errore viene corretto. Due errori producono una sindrome non nulla che indica un terzo bit, innocente: il decodificatore lo «corregge» e consegna dati sbagliati. **Hamming(8,4)** aggiunge un bit di parità globale (distanza 4): una sindrome non nulla con parità globale giusta significa due errori, e il blocco viene scartato. È il SECDED: correzione dell'errore singolo, rilevazione del doppio.

In generale r bit di parità proteggono fino a 2^r − r − 1 bit di dati: (7,4), (15,11), (31,26). Tasso del codice = bit di dati / bit totali: 4/7 = 0,57 per Hamming(7,4), 1/2 per (8,4), 1/3 per la ripetizione tripla.

## CSMA e CSMA/CD

Sia a = ritardo di propagazione / tempo di frame. Il rilevamento della portante fallisce quando due stazioni iniziano a meno di un ritardo di propagazione l'una dall'altra, quindi funziona bene solo se a è piccolo. Varianti quando il canale è occupato: **1-persistente** attende e trasmette appena è libero (tutte le stazioni in attesa allora collidono); **non persistente** si allontana per un tempo casuale.

CSMA/CD come in Ethernet:

- Si ascolta il canale; quando è libero si trasmette (1-persistente).
- Mentre si trasmette si ascolta. In caso di collisione ci si ferma e si invia un breve segnale di **jam** (32 bit) perché tutti se ne accorgano.
- Dopo la n-esima collisione consecutiva di questo frame si sceglie K uniformemente in {0, 1, ..., 2^min(n,10) − 1} e si attendono K **slot**; poi si ricomincia. Dopo 16 tentativi si rinuncia.

Uno slot è il doppio del ritardo di propagazione massimo (51,2 µs, 512 tempi di bit, a 10 Mbit/s): trascorso uno slot senza collisioni la stazione ha conquistato il canale. Per la stessa ragione un frame deve durare almeno uno slot, altrimenti il mittente potrebbe finire prima che la collisione torni indietro: da qui il minimo di 64 byte di Ethernet. Dopo la prima collisione K ∈ {0,1}; dopo la seconda {0,1,2,3}; dopo la terza da 0 a 7. Due stazioni che collidono per la prima volta collidono di nuovo con probabilità 1/2, poi 1/4: la finestra si adatta al numero ignoto di contendenti.

L'efficienza è circa 1 / (1 + 5a): con a = 0,01, come nel laboratorio 261, circa il 95%; con a = 0,1, il 67%.

## Accodamento a priorità stretta

Il classificatore assegna ogni pacchetto a una banda, nel laboratorio 272 in base al campo DSCP dell'intestazione IP: banda 0 per Expedited Forwarding (DSCP 46, scritto `tos=184` perché il DSCP occupa i sei bit alti del vecchio byte TOS: 46 × 4 = 184), banda 2 per il best effort. Lo scheduler trasmette sempre dalla banda non vuota di numero più basso. Non è preemptive: un pacchetto in corso di trasmissione viene prima terminato.

I numeri del laboratorio: il collegamento porta 500 kbit/s. La voce invia pacchetti da 160 byte a 64 kbit/s, 50 al secondo; il traffico di massa offre 700 kbit/s in pacchetti da 1000 byte. Un pacchetto voce attende al massimo un pacchetto di massa, circa 1000 × 8 / 500000 = 16 ms, e nessuno va perso. Al traffico di massa resta ciò che avanza, circa 430 kbit/s, e perde circa il 40% dei pacchetti. Con un'unica FIFO da 30 pacchetti la coda resta piena: fino a 30 × 16 ms ≈ 0,5 s di ritardo per tutti, ed entrambi i flussi perdono pacchetti.

<!-- detail: 3 -->
## In profondità: codifica

Un codice di Hamming è *perfetto*: le 2^r sindromi sono esattamente «nessun errore» più una per posizione, senza nulla d'avanzo; ecco perché due errori non possono essere segnalati e vengono corretti male. I codici di Hamming si adattano a canali con errori rari e indipendenti: codici SECDED come il (72,64) proteggono la memoria dei server. Le reti vedono raffiche, che mettono più errori in un blocco; un interleaver distribuisce una raffica su molti blocchi, e i codici di Reed-Solomon correggono byte interi. La correzione in avanti (FEC) paga la sua ridondanza sempre, l'ARQ (rilevare e ritrasmettere) solo in caso di errore, al costo di un giro di andata e ritorno: i collegamenti cablati usano rilevazione e ritrasmissione ai livelli superiori, mentre radio, satellite e fibra combinano FEC e ARQ. Shannon (1948) dimostrò che una comunicazione affidabile è possibile a qualsiasi tasso inferiore alla capacità del canale; «Error Detecting and Error Correcting Codes» di Hamming (1950) ne diede la prima costruzione pratica. In hoplab i frame sono codificati per intero e Reed-Solomon usa blocchi fissi (255,223).

## In profondità: accesso al mezzo

Il backoff è iniquo nel breve periodo: la stazione che vince azzera la propria finestra mentre quella di chi perde continua a raddoppiare (*effetto cattura*). CSMA/CD limita il diametro della rete: a 100 Mbit/s lo slot scende a 5,12 µs e il dominio di collisione a circa 200 m; Gigabit Ethernet ebbe bisogno della carrier extension. Tutto questo è storia: con gli switch e i collegamenti full-duplex non ci sono collisioni, e CSMA/CD è disattivato. Le radio non possono ascoltare mentre trasmettono, perciò il Wi-Fi usa la *prevenzione* delle collisioni (collision avoidance). Riferimenti: Metcalfe e Boggs, «Ethernet: Distributed Packet Switching for Local Computer Networks» (1976); l'analisi del CSMA di Kleinrock e Tobagi (1975); IEEE 802.3. Nel modello di hoplab il tempo avanza a passi di un ritardo di propagazione, ogni stazione dista un passo da ogni altra, lo slot vale due passi, il jam tre, e ogni stazione accoda al massimo 16 frame.

## In profondità: priorità

La priorità stretta può **affamare** le bande inferiori (starvation): se la sola banda 0 supera la capacità del collegamento non viene più trasmesso nient'altro. I router reali limitano perciò la classe prioritaria a un tasso fisso con un policer (low-latency queueing) e vi ammettono solo il traffico che lo merita. Le marcature sono considerate attendibili solo all'interno di un dominio amministrativo e di solito vengono riscritte al confine. L'accodamento equo (fair queueing), al contrario, isola i flussi senza fidarsi di alcuna marcatura. Il limite sul ritardo dipende dal più grande pacchetto di priorità inferiore: sui collegamenti lenti un pacchetto da 1500 byte ritarda la voce di decine di millisecondi, ed è per questo che su tali collegamenti si frammenta e si intercala. Riferimenti: RFC 2474 e RFC 2475 (Differentiated Services), RFC 3246 (Expedited Forwarding). `prio` di hoplab ha tre bande: DSCP ≥ 40 e ARP, altri DSCP non nulli, best effort.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 260: `./hoplab run 260-coding-bench`**

- Seleziona il nodo lab e apri la sua scheda `coding`: bit di dati blu, ridondanza gialla, bit invertiti rossi. Ogni passo è spiegato nel registro.
- 9 s: Hamming(7,4), bit 9 invertito (i bit sono numerati da 0, quindi è il terzo bit del secondo blocco): `sindrome 3`, corretto.
- 12 s: bit 7 e 8, posizioni 1 e 2 di quel blocco: sindrome 1 ⊕ 2 = 3, viene invertito l'innocente bit 3 e il testo diventa «@ELLO». 15 s: lo stesso errore contro Hamming(8,4) viene rilevato e il frame scartato.
- 24 s: la scansione. Per ogni tasso di errore sul bit leggi le percentuali di frame consegnati, scartati e sbagliati in silenzio.
- Prevedi, poi prova: `:lab coding show text=HELLO code=hamming74 pos=0,7,14`. Tre bit invertiti: il testo è giusto? Perché? Poi `pos=0,1,2`.
- Prevedi, poi prova: `./hoplab run 260-coding-bench code=hamming74` e confronta la colonna «sbagliati in silenzio» e il costo per bit di dati con il default hamming84. Quale sceglieresti con un tasso di errore di 1e-2?

**Laboratorio 261: `./hoplab run 261-csma-cd`**

- Tre nodi eseguono lo stesso carico con tre discipline. Apri a turno la scheda `mac` di `nonpers`, `pers` ed `ethernet`: il verde è un successo, il rosso una collisione. Confronta la larghezza delle zone rosse.
- Il registro stampa S (throughput) e G (tentativi) ogni 20 tempi di frame. Valori finali tipici: circa 0,8 per il non persistente, 0,4 per l'1-persistente, 0,88 per CSMA/CD, con un carico offerto di 0,9.
- Prevedi, poi prova: `./hoplab run 261-csma-cd prop=10ms` (a = 0,1). Quale protocollo perde di più? Confronta CSMA/CD con 1 / (1 + 5a).
- Prevedi, poi prova: `./hoplab run 261-csma-cd load=2` e `load=0.3`. Quale disciplina degrada in sovraccarico, e quale è limitata solo dal carico offerto?

**Laboratorio 272: `./hoplab run 272-priority-queueing`**

- Seleziona r1 e apri la scheda `qdisc`: tre bande, con lunghezze e scarti. Seleziona v2 e b2 per perdita, ritardo e jitter dei due flussi. I Grafici (`g`) mostrano il ritardo nel tempo.
- Fino a 3 s la voce è sola; poi parte il flusso di massa e r1 scarta solo pacchetti di massa (`✗ prio` nel registro).
- Prevedi, poi prova: `./hoplab run 272-priority-queueing qdisc=droptail`. Stima il ritardo della voce da lunghezza della coda e velocità del collegamento prima di guardare.
- Prevedi, poi prova: `./hoplab run 272-priority-queueing bulk=2M`. Cambia qualcosa per la voce? E per il traffico di massa? Poi `qdisc=fq`.

## Verifica

- Codifica i bit di dati 0110 con Hamming(7,4). Poi decodifica la parola ricevuta 1011011: calcola la sindrome e indica i bit di dati.
- Perché Hamming(7,4) consegna dati sbagliati quando in un blocco vengono invertiti due bit, e come lo evita l'ottavo bit di Hamming(8,4)?
- Una stazione ha appena subito la terza collisione consecutiva. Da quale insieme estrae il backoff, qual è l'attesa media in slot, e perché lo slot vale il doppio del ritardo di propagazione?
- Perché a Ethernet serve una lunghezza minima del frame, e come è legata alla lunghezza del cavo?
- Un collegamento da 256 kbit/s serve una coda prioritaria per la voce (80 kbit/s, pacchetti da 200 byte) e una coda best effort con pacchetti da 1500 byte. Qual è il ritardo di accodamento nel caso peggiore per un pacchetto voce? Che cosa succede al traffico best effort se la classe voce invia 300 kbit/s?
