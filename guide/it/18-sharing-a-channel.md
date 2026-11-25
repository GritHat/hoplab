<!-- level: 1 -->
<!-- labs: 162-aloha -->
# Condividere un canale: ALOHA

Le onde radio si propagano ovunque. Quando più stazioni usano la stessa frequenza, come i dispositivi di una rete Wi-Fi, condividono tutte **un solo canale**: se due trasmettono nello stesso momento, i loro segnali si mescolano e nessuno dei due frame è comprensibile. È una **collisione**, ed entrambi i frame vanno persi.

Immagina una tavolata in cui tutti parlano senza guardare gli altri. Se due persone cominciano insieme, nessuno capisce né l'una né l'altra. Un moderatore potrebbe dare la parola a una persona alla volta, ma spesso una rete non ha un moderatore. Le stazioni devono arrangiarsi da sole, con una regola che ciascuna applica per conto proprio: un protocollo di **accesso al mezzo**.

La regola più antica è la più sbrigativa. In **ALOHA**, una stazione che ha un frame semplicemente lo invia. Se non torna una conferma, suppone una collisione, aspetta un tempo **casuale** e riprova. La casualità è essenziale: se entrambe le stazioni aspettassero lo stesso tempo, colliderebbero di nuovo, all'infinito.

**Slotted ALOHA** aggiunge un solo elemento di disciplina: un orologio comune divide il tempo in intervalli (slot) lunghi quanto un frame, e una stazione può cominciare solo all'inizio di uno slot. Con questa sola modifica il canale trasporta circa il doppio del traffico. Anche così, la maggior parte del tempo del canale va sprecata in silenzio o in collisioni.

<!-- detail: 2 -->

## Come funziona

Prendi frame che durano tutti lo stesso tempo T, il **tempo di frame**. Due numeri descrivono il canale, entrambi misurati in frame per tempo di frame.

- **G**, il carico: quante trasmissioni vengono tentate, frame nuovi più ritentativi.
- **S**, il throughput: quante riescono. S non può mai superare 1, che vorrebbe dire un canale sempre occupato da frame buoni.

**ALOHA puro.** Un frame che comincia all'istante t viene distrutto se qualunque altra stazione comincia fra t − T e t + T: un frame cominciato fino a T prima è ancora in aria, e uno che comincia fino a T dopo ne colpisce la coda. Il **periodo di vulnerabilità** è lungo 2T.

```
        t-T          t          t+T
         |-----------|###########|
          pericolo     il mio frame
         <---- 2T vulnerabile ---->
```

Con stazioni casuali e indipendenti, la probabilità che nessun altro cominci durante 2T è e^(−2G), quindi

```
S = G · e^(−2G)     massimo 0,184 per G = 0,5
```

**Slotted ALOHA.** I frame cominciano solo ai confini degli slot, quindi due frame o si sovrappongono del tutto o per niente. Sono pericolose solo le stazioni che scelgono lo **stesso slot**: il periodo di vulnerabilità è T.

```
S = G · e^(−G)      massimo 0,368 per G = 1
```

Un esempio svolto. Con G = 0,5, ALOHA puro dà S = 0,5 x e^(−1) = 0,18: ogni 100 tempi di frame, 18 portano un frame buono. Slotted ALOHA con lo stesso G dà 0,5 x e^(−0,5) = 0,30.

Entrambe le curve salgono, raggiungono un massimo e poi scendono. Oltre il massimo, più tentativi significano più collisioni, più collisioni significano più ritentativi, e il throughput **diminuisce** mentre il carico cresce. Nel laboratorio 162 dieci stazioni offrono insieme 0,4 nuovi frame per tempo di frame. Slotted ALOHA riesce a trasportarli (S è circa 0,36). ALOHA puro no, perché 0,4 è sopra il suo massimo di 0,18: i ritentativi si accumulano, G cresce, e S si assesta fra 0,13 e 0,15.

Il **backoff** è l'attesa casuale prima di un ritentativo. Più lunga è l'attesa media, minore è il carico che le stazioni mettono sul canale.

<!-- detail: 3 -->

## Per approfondire

**Da dove vengono le formule.** Presuppongono un numero molto grande di stazioni i cui tentativi formano un processo di Poisson: il numero di inizi in un intervallo lungo x tempi di frame è casuale con media G·x, e la probabilità di zero inizi è e^(−G·x). Metti x = 2 oppure x = 1 e ottieni i due risultati. Con sole dieci stazioni e code di lunghezza finita i punti misurati seguono la forma della curva, non la sua ultima cifra.

**Instabilità.** ALOHA con una regola fissa di ritentativo non ha un punto di lavoro stabile ad alto carico: una raffica di collisioni aumenta l'arretrato, che aumenta G, che aumenta le collisioni. I sistemi reali fanno crescere il backoff con il numero di fallimenti (il **backoff esponenziale binario** di Ethernet raddoppia l'intervallo a ogni collisione), così le stazioni rallentano quando il canale è affollato.

**Il costo degli slot** è un orologio comune, che richiede un segnale di sincronizzazione, e l'attesa dello slot successivo anche quando il canale è libero.

**Fare meglio: prima ascoltare.** Le stazioni ALOHA sono sorde. Se una stazione ascolta prima di parlare (**rilevamento della portante**, CSMA) evita di distruggere un frame già in aria. Funziona bene quando il ritardo di propagazione è piccolo rispetto al tempo di frame. L'Ethernet classica ascoltava anche **mentre** trasmetteva e si fermava subito in caso di collisione (CSMA/CD, laboratorio 261). Le radio non riescono a sentire gli altri mentre trasmettono, quindi il Wi-Fi aspetta il silenzio, conta alla rovescia un backoff casuale e si affida ai riscontri (CSMA/CA); il laboratorio 460 mostra che cosa succede quando due stazioni non si sentono a vicenda. Il laboratorio 361 disegna le curve S(G) di tutti questi protocolli.

**L'altra famiglia** evita del tutto le collisioni assegnando dei turni: intervalli di tempo (TDMA), frequenze (FDMA), un token, oppure una stazione base che pianifica tutti, come nelle reti mobili. Sono efficienti a carico elevato ma sprecano capacità quando poche stazioni hanno qualcosa da dire; l'accesso casuale è l'opposto.

**Dove ALOHA sopravvive.** Fu costruito nel 1970 per collegare via radio le isole Hawaii. Slotted ALOHA si usa ancora per il primo messaggio che un telefono cellulare invia a una stazione base, e per leggere le etichette RFID.

Per saperne di più: Kurose e Ross, capitolo 6, paragrafo "Collegamenti broadcast e protocolli di accesso multiplo"; Tanenbaum, capitolo 4, "Protocolli ad accesso multiplo"; Abramson, "The ALOHA System" (1970).

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 162-aloha**: due canali indipendenti con dieci stazioni ciascuno e lo stesso traffico. Il nodo `pure` esegue ALOHA puro, il nodo `slotted` esegue slotted ALOHA. Qui sulla mappa non viaggiano pacchetti: tutto accade nel pannello di ciascun nodo.

- Avvialo con `./netlab run 162-aloha`.
- Seleziona `pure` con `n` e spostati sulla scheda `mac` con i tasti freccia (premi Tab finché il pannello del nodo ha il focus; `z` lo ingrandisce). C'è una riga per stazione: verde è un frame consegnato, rosso una collisione, i puntini gialli una stazione in attesa di riprovare.
- Metti in pausa con spazio e cerca una coppia rossa che si sovrappone solo per un pezzetto: entrambi i frame sono persi per intero.
- Seleziona `slotted` e apri la stessa scheda: tutti i frame partono allineati. Riesci a trovare una sovrapposizione parziale?
- Sotto le righe leggi G e S per entrambi; accanto, `teoria` è l'S che la formula dà per il G misurato. Lascia girare il laboratorio fino a 60 s e confronta: fra 0,13 e 0,15 per pure, circa 0,36 per slotted.

**Cambia e prevedi.** Per ogni esecuzione calcola prima S con le due formule.

- `./netlab run 162-aloha load=0.1`: i due protocolli saranno molto diversi?
- `./netlab run 162-aloha load=0.2`: siamo vicini al limite di ALOHA puro.
- `./netlab run 162-aloha load=1`: quale frazione degli slot porta un frame buono in slotted ALOHA? Che cosa succede alle code delle stazioni?
- `./netlab run 162-aloha stations=3`: con poche stazioni le formule sono ancora una buona guida?
- `./netlab run 162-aloha frame=500ms` rallenta la linea del tempo, così puoi seguire i singoli frame.

## Verifica

- Perché in una collisione vanno persi entrambi i frame, e non solo il secondo?
- Perché l'attesa prima di un ritentativo deve essere casuale?
- Spiega a parole perché il periodo di vulnerabilità di ALOHA puro è di due tempi di frame e quello di slotted ALOHA di uno.
- Calcola S per slotted ALOHA con G = 1 e con G = 3. Che cosa ti dice il confronto?
- Quale informazione permetterebbe a una stazione di evitare la maggior parte delle collisioni, e quale protocollo la usa?
