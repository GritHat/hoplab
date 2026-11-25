<!-- level: 2 -->
<!-- labs: 220-tcp-loss, 221-tcp-flow-control, 120-tcp-handshake -->
# TCP: affidabilità e controllo di flusso

IP consegna i pacchetti senza garanzie: possono andare persi, arrivare duplicati o fuori ordine. TCP costruisce sopra IP un **flusso di byte affidabile e ordinato** fra due applicazioni. Tre idee fanno la maggior parte del lavoro.

**Numerazione.** Ogni byte del flusso ha un numero di sequenza. Ogni segmento dice quali byte trasporta, e il ricevente risponde con un **riscontro cumulativo** (ACK): il numero del prossimo byte che si aspetta, cioè «ho tutto ciò che viene prima».

**Ritrasmissione.** Il mittente conserva una copia di ciò che ha inviato finché non viene riscontrato. Si accorge di una perdita in due modi. Se nessun riscontro arriva entro il **timeout di ritrasmissione** (RTO), un valore stimato continuamente dai tempi di andata e ritorno misurati, rinvia il più vecchio segmento non riscontrato. Se invece i segmenti successivi arrivano, il ricevente ripete lo stesso riscontro; tre di questi **ACK duplicati** fanno scattare una **ritrasmissione rapida** (fast retransmit) senza attendere il timer.

**Controllo di flusso.** Il ricevente ha un buffer finito, svuotato al ritmo della sua applicazione. In ogni segmento annuncia una **finestra di ricezione**: quanti altri byte può accettare. Il mittente non tiene mai in sospeso più dati non riscontrati di così. Un lettore lento rallenta dunque il mittente al proprio passo, senza perdere un byte.

<!-- detail: 2 -->
## I campi dell'intestazione che contano qui

L'intestazione TCP di 20 byte contiene porta sorgente e destinazione, **numero di sequenza** (32 bit: il numero del primo byte di dati del segmento), **numero di riscontro** (32 bit: il prossimo byte atteso, valido quando il flag ACK è impostato), lunghezza dell'intestazione, flag (SYN, ACK, FIN, RST, PSH, URG), **finestra** (16 bit), checksum e urgent pointer. SYN e FIN consumano ciascuno un numero di sequenza, ed è per questo che possono essere riscontrati come i dati.

## Apertura e chiusura

L'handshake a tre vie sincronizza i numeri di sequenza iniziali: SYN (seq = x), SYN+ACK (seq = y, ack = x + 1), ACK (ack = y + 1). L'opzione MSS annuncia il segmento più grande che ciascun lato accetta (1460 su Ethernet). Ogni direzione viene chiusa separatamente: FIN, ACK, e più tardi FIN, ACK dall'altro lato. Il lato che chiude per primo attende in TIME_WAIT per il doppio della vita massima di un segmento, così che un ultimo ACK perso possa essere ripetuto e i vecchi segmenti si estinguano. A un SYN verso una porta senza nessuno in ascolto si risponde con RST.

Nel laboratorio 120, 4000 byte viaggiano in tre segmenti (1460 + 1460 + 1080); i numeri di ACK, relativi a quello iniziale, sono 1461, 2921 e 4001, e il FIN viene riscontrato con 4002.

## Il timer di ritrasmissione (RFC 6298)

Per ogni nuovo campione R del tempo di andata e ritorno:

```
primo campione:  SRTT = R              RTTVAR = R / 2
in seguito:      RTTVAR = 3/4 RTTVAR + 1/4 |SRTT - R|
                 SRTT   = 7/8 SRTT   + 1/8 R
RTO = SRTT + 4 * RTTVAR       (inizialmente 1 s; minimo 1 s nell'RFC)
```

Esempio con il primo campione del laboratorio 220, R = 88 ms: SRTT = 88, RTTVAR = 44, RTO = 88 + 176 = 264 ms. Un secondo campione di 50 ms dà RTTVAR = 33 + 9,5 = 42,5, SRTT = 77 + 6,25 = 83,25, RTO = 253 ms. Il termine di varianza è ciò che tiene l'RTO a distanza di sicurezza da un RTT irregolare.

Altre due regole. **Algoritmo di Karn**: non prendere mai un campione da un segmento ritrasmesso, perché l'ACK è ambiguo. **Backoff esponenziale**: a ogni scadenza l'RTO raddoppia (1, 2, 4 s, ... fino a 60 s) e torna al valore calcolato quando vengono riscontrati nuovi dati.

hoplab usa un RTO minimo di 200 ms (parametro `rto_min`), come Linux, invece di 1 s come nell'RFC.

## Ritrasmissione rapida

Un ricevente che riceve un segmento oltre un buco lo mette nel buffer e rinvia subito l'ACK relativo al buco. Il mittente conta: il terzo duplicato (quattro ACK identici in tutto) viene preso come prova della perdita, e il segmento mancante viene ritrasmesso immediatamente. La soglia di tre tollera un lieve riordino. La ritrasmissione rapida ha bisogno che arrivino segmenti successivi: una perdita alla fine di un trasferimento, o con una finestra minuscola, può essere riparata solo dal timer. Entrambi i tipi di perdita riducono anche la finestra di congestione, argomento di un altro capitolo.

## Controllo di flusso

Il mittente rispetta: LastByteSent − LastByteAcked ≤ min(cwnd, rwnd). Il ricevente annuncia rwnd = dimensione del buffer − (byte ricevuti − byte letti dall'applicazione).

Quando rwnd arriva a 0 il mittente si ferma. L'aggiornamento di finestra che la riapre è un semplice ACK, inviato una volta e mai ritrasmesso; se andasse perso entrambi i lati aspetterebbero per sempre. Perciò il mittente avvia un **persist timer** e invia periodicamente una **sonda a finestra zero** (zero-window probe) di un byte, che costringe a rispondere con un ACK contenente la finestra corrente.

Nel laboratorio 221 il buffer è di 8000 byte e l'applicazione legge 16 kbit/s = 2000 byte/s. I primi segmenti riempiono il buffer in pochi millisecondi; da lì in poi 40000 byte richiedono 20 s: il goodput riportato è 15,95 kbit/s su un collegamento da 10 Mbit/s.

<!-- detail: 3 -->
## In profondità

**I riscontri cumulativi sono robusti ma poveri di informazione.** Un ACK perso è innocuo, perché il successivo lo copre. Ma un mittente che ha perso più segmenti di una finestra viene a sapere di un solo buco per ogni giro. I **riscontri selettivi** (SACK, RFC 2018) elencano i blocchi ricevuti oltre il buco; il mittente ne tiene un tabellone e ripara tutti i buchi di una finestra in circa un giro (RFC 6675). NewReno (RFC 6582) è il meglio che si possa fare senza. hoplab negozia SACK in modo predefinito; `sack=0` su un `tcp send` ripiega su NewReno, e il laboratorio `321-tcp-sack` confronta i due.

**ACK ritardati.** I riceventi reali riscontrano un segmento pieno ogni due, o dopo al massimo 500 ms (RFC 1122; tipicamente fra 40 e 200 ms), per risparmiare pacchetti. Questo interagisce male con l'algoritmo di Nagle (RFC 896), che trattiene i segmenti piccoli finché ci sono dati non riscontrati: sono i classici stalli di 40 ms dei protocolli richiesta/risposta, e la ragione dell'opzione TCP_NODELAY. hoplab fa entrambe le cose: gli ACK aspettano un secondo segmento o 40 ms (`delack=`, 0 per riscontrare tutto subito) e Nagle è attivo (`nagle=0` per disattivarlo).

**I timer negli stack reali.** Il minimo di 1 s dell'RFC fu scelto contro i timeout spuri; Linux usa 200 ms, e nei data center si scende ancora. L'opzione timestamp (RFC 7323) fornisce un campione per ogni ACK, ritrasmissioni comprese, rendendo superflua la restrizione di Karn, e protegge dal riavvolgimento dei numeri di sequenza (PAWS) sui collegamenti veloci. Lo stesso RFC definisce il window scaling: il campo finestra a 16 bit limita la finestra a 64 kB, troppo poco per un percorso lungo e veloce, perciò i due lati concordano nel SYN un fattore di scala. Un timeout spurio può essere riconosciuto e annullato (F-RTO, RFC 5682; Eifel). Il Linux moderno usa un rilevamento delle perdite basato sul tempo (RACK-TLP, RFC 8985), che invia anche una sonda per trasformare le perdite in coda in recuperi rapidi.

**Sindrome della finestra sciocca.** Un ricevente che annuncia ogni minimo spazio liberato, o un mittente che lo sfrutta, riempie la rete di segmenti minuscoli. La regola di Clark: non annunciare un aumento finché non vale almeno un MSS o metà del buffer. Negli stack reali il persist timer cresce esponenzialmente (da circa 5 s fino a 60 s); hoplab sonda a intervallo fisso di 500 ms.

**Casi limite della chiusura.** I due lati possono chiudere contemporaneamente; un lato può chiudere la propria direzione di invio e continuare a ricevere (half-close); un'applicazione che chiude con dati non letti provoca un RST. TIME_WAIT tiene occupata una coppia di porte per 2 MSL, un fastidio per i server molto carichi. I numeri di sequenza iniziali devono essere imprevedibili (RFC 6528), altrimenti un attaccante fuori dal percorso potrebbe iniettare dati.

**Riferimenti.** RFC 9293 (TCP, 2022, sostituisce l'RFC 793), RFC 6298 (RTO), RFC 5681 (fast retransmit e fast recovery), RFC 1122; Jacobson, «Congestion Avoidance and Control» (1988), da cui proviene lo stimatore della varianza dell'RTT; Karn e Partridge, «Improving Round-Trip Time Estimates in Reliable Transport Protocols» (1987).

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 120: `./hoplab run 120-tcp-handshake`**

- Apri la vista Sequenza (`s`) a dettaglio 2: SYN, SYN+ACK, ACK, tre segmenti di dati, i loro ACK, FIN nelle due direzioni. Nella vista Pacchetti (`p`) premi Invio su ciascuno e leggi Seq, Ack, flag e Win.
- A 6 s un SYN verso la porta 81 riceve RST: `connessione rifiutata`.
- Prevedi, poi prova: `./hoplab run 120-tcp-handshake bytes=20000 iw=2`. Quanti segmenti sono in volo nel primo giro, e nel secondo? Poi `loss=0.1`: trova una ritrasmissione e stabilisci se è stata rapida o per timeout.

**Laboratorio 220: `./hoplab run 220-tcp-loss`**

- Seleziona h1 e apri la sua scheda `tcp`: srtt, rto, cwnd, ssthresh e i contatori di fast retransmit e timeout.
- Nel registro a dettaglio 2 cerca `3 ACK duplicati: fast retransmit` e `timeout di ritrasmissione (RTO ...)`. Apri i Grafici (`g`) per l'RTT e la finestra.
- Nella vista Pacchetti trova un gruppo di ACK con lo stesso numero di Ack seguito dal segmento ritrasmesso.
- Prevedi, poi prova: `./hoplab run 220-tcp-loss loss=0.2`. Quale tipo di riparazione domina ora, e perché? Cerca valori di RTO di 1 s, 2 s, 4 s: da dove vengono?
- Prevedi, poi prova: `./hoplab run 220-tcp-loss delay=100ms`. Stima l'RTT e l'RTO dopo il primo campione prima di guardare.

**Laboratorio 221: `./hoplab run 221-tcp-flow-control`**

- Nella vista Pacchetti osserva il campo Win degli ACK di h2 scendere a 0 in circa 50 ms. Nel registro: `il ricevitore annuncia finestra 0`, poi ogni 500 ms `sonda a finestra zero`.
- Alla fine confronta il goodput con `readrate`, e nota le 0 ritrasmissioni: non si è perso nulla.
- Prevedi, poi prova: `./hoplab run 221-tcp-flow-control rwnd=32000`. Il trasferimento finisce prima? Come cambia il numero di sonde?
- Prevedi, poi prova: `./hoplab run 221-tcp-flow-control readrate=64k`. Calcola prima la durata.

## Verifica

- L'host A invia tre segmenti di 1000 byte con numeri di sequenza 5000, 6000 e 7000; il secondo va perso. Quali numeri di ACK invia B, e che cosa fa A, supponendo che A continui poi a inviare nuovi segmenti?
- Calcola SRTT, RTTVAR e RTO dopo i campioni 100 ms, 120 ms e 80 ms, partendo senza alcuna stima.
- Perché a TCP serve l'algoritmo di Karn, e perché l'RTO raddoppia dopo un timeout?
- Un ricevente ha un buffer di 4000 byte e la sua applicazione non legge nulla. Descrivi i segmenti scambiati dal primo byte di dati fino a quando l'applicazione legge finalmente 2000 byte. Che cosa succederebbe senza il persist timer?
- Perché SYN e FIN consumano un numero di sequenza, e perché il lato che chiude per primo attende in TIME_WAIT?
