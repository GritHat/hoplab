<!-- level: 1 -->
<!-- labs: 120-tcp-handshake, 101-ping -->
# Il livello di trasporto: porte, UDP e TCP

IP porta un pacchetto alla macchina giusta. Ma una macchina esegue molti programmi insieme: un browser, una chat, un gioco. A quale di loro va consegnato il pacchetto? E se il pacchetto si perde per strada? Rispondere a queste due domande è il compito del **livello di trasporto**.

Un indirizzo IP è come l'indirizzo di un palazzo; una **porta** è il numero di un appartamento al suo interno. Ogni pacchetto porta una porta sorgente e una porta di destinazione, così il sistema operativo può consegnare i dati al programma giusto e la risposta può ritrovare la strada.

I protocolli di trasporto sono due, con caratteri opposti.

**UDP** è una cartolina. Scrivi, imbuchi, e basta: nessuna garanzia che arrivi, nessun avviso se non arriva. È semplice e immediato.

**TCP** è una telefonata. Prima le due parti stabiliscono una **connessione** ("pronto?" "pronto, ti sento" "bene"). Poi tutto ciò che viene detto arriva, in ordine, senza buchi né ripetizioni, perché TCP numera i byte, aspetta i riscontri e invia di nuovo ciò che è andato perso. Alla fine entrambe le parti si salutano.

Le pagine web, la posta e i trasferimenti di file usano TCP. Le risoluzioni dei nomi, la voce in tempo reale e molti giochi usano UDP.

<!-- detail: 2 -->

## Come funziona

Una **porta** è un numero a 16 bit, da 0 a 65535. I server aspettano su porte note (80 per il web, 53 per il DNS; in hoplab il servizio echo usa la 7). Un client lascia scegliere al sistema una **porta effimera** temporanea: nei laboratori vedrai 49152. Una conversazione è identificata da quattro valori: indirizzo sorgente, porta sorgente, indirizzo di destinazione, porta di destinazione.

**UDP** aggiunge un'intestazione di soli 8 byte: porta sorgente, porta di destinazione, lunghezza, checksum. Un invio del programma diventa un **datagramma**. Se nessuno ascolta sulla porta di destinazione, l'host risponde con un ICMP "port unreachable".

**TCP** offre un **flusso di byte**: il programma scrive byte a un capo ed essi escono all'altro nello stesso ordine. TCP taglia il flusso in **segmenti**, ciascuno con un'intestazione di 20 byte che contiene le porte, un **numero di sequenza**, un **numero di riscontro** (acknowledgement), dei flag (SYN, ACK, FIN, RST) e una finestra.

**Apertura: l'handshake a tre vie.**

- Il client invia **SYN** con il proprio numero di sequenza iniziale x.
- Il server risponde **SYN+ACK**: il suo numero iniziale y, e ack = x + 1.
- Il client risponde **ACK** con ack = y + 1. Ora i dati possono scorrere.

**Dati.** Il numero di sequenza di un segmento è il numero del suo primo byte. Chi riceve risponde con un **ACK cumulativo**: il numero del prossimo byte che si aspetta. Nel laboratorio 120, 4000 byte viaggiano in tre segmenti, perché un segmento porta al massimo MSS = 1460 byte: 1460 + 1460 + 1080. Contando il primo byte di dati come numero 1, gli ACK dicono 1461, 2921, 4001 (i numeri veri partono da un grande valore casuale, come vedrai nella vista Pacchetti). Un segmento che non viene riscontrato in tempo viene inviato di nuovo.

**Chiusura.** Ogni direzione si chiude separatamente: una parte invia **FIN**, l'altra lo riscontra, poi invia il proprio FIN, che viene riscontrato a sua volta. SYN e FIN contano ciascuno come un byte nella numerazione.

**Rifiuto.** Un SYN inviato a una porta su cui nessun programma è in ascolto riceve un **RST** (reset): "connessione rifiutata".

<!-- detail: 3 -->

## Per approfondire

**Perché tre messaggi per aprire?** Ogni parte deve conoscere il numero di sequenza iniziale dell'altra e confermarlo. Con due messaggi il server non saprebbe se il suo numero è stato ricevuto; e un vecchio SYN arrivato in ritardo potrebbe aprire una connessione che nessuno vuole. I numeri iniziali sono scelti in modo imprevedibile, così che i segmenti di una connessione precedente, o falsificati da un attaccante, non vengano accettati come validi.

**Anche chiudere è delicato.** La parte che chiude per prima resta nello stato `TIME_WAIT` prima di dimenticare la connessione, nel caso il suo ultimo ACK sia andato perso e l'altro FIN venga ripetuto. Puoi leggere i nomi degli stati nel registro di hoplab.

**L'affidabilità di TCP è il meccanismo dell'argomento 13**, diventato adulto: i numeri di sequenza contano byte invece di pacchetti; il timeout si adatta al tempo di andata e ritorno misurato; tre ACK duplicati fanno scattare un fast retransmit senza aspettare il timer (laboratorio 220). Sopra tutto questo TCP regola quanto invia: il **controllo di flusso** rispetta la finestra annunciata da chi riceve (laboratorio 221), il **controllo di congestione** stima quanto la rete può trasportare e rallenta in caso di perdita (laboratorio 320). Il parametro `iw` del laboratorio 120 è il valore iniziale di quella stima.

**Che cosa TCP non dà.** Nessun confine fra i messaggi: due scritture possono arrivare come una sola lettura, quindi le applicazioni devono segnare dove finiscono i messaggi. Nessuna garanzia sui tempi: un segmento perso trattiene tutto ciò che lo segue (**head-of-line blocking**), il che è un male per l'audio in tempo reale. Nessuna sicurezza: la aggiunge TLS, sopra.

**Perché allora UDP?** Perché a volte la cura è peggiore del male. Una domanda e una risposta che stanno ciascuna in un pacchetto (DNS) non meritano un handshake. La voce in tempo reale preferisce un buco a una ripetizione in ritardo. E UDP è la base su cui si costruiscono nuovi protocolli di trasporto dentro le applicazioni: **QUIC**, usato da HTTP/3, realizza connessioni, affidabilità e cifratura sopra UDP.

Il costo di una connessione è reale: un handshake è un'andata e ritorno prima del primo byte utile, e l'argomento 17 lo misura su una pagina web.

Per saperne di più: Kurose e Ross, capitolo 3 ("Livello di trasporto": multiplexing, UDP, gestione della connessione TCP); Tanenbaum, capitolo 6; RFC 768 (UDP), RFC 9293 (TCP), RFC 9000 (QUIC).

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 120-tcp-handshake**: tutta la vita di una connessione TCP fra h1 e h2, con 50 ms di ritardo per direzione.

- Avvialo con `./hoplab run 120-tcp-handshake` e premi `2` per il dettaglio 2.
- Apri la vista Sequenza (`s`): le frecce disegnano l'handshake a 1 s, i segmenti di dati, gli ACK che tornano e i due FIN.
- Apri la vista Pacchetti (`p`) e premi Invio sul SYN, sul SYN+ACK e sull'ACK. Leggi le porte (49152 e 5001), i flag, e verifica la regola ack = numero di sequenza dell'altra parte + 1.
- Apri i tre segmenti di dati e i loro ACK: di quanto cresce il numero di sequenza dall'uno all'altro? Che cosa dice ogni ACK?
- Il registro riporta `connesso (200.2 ms)`. A un handshake serve un'andata e ritorno, 100 ms: da dove viene il resto? (Guarda che cosa precede il SYN+ACK.)
- A 6 s h1 prova la porta 81: trova il RST e la riga `connessione rifiutata`.

**Laboratorio 101-ping**, la parte UDP.

- Avvia `./hoplab run 101-ping` e vai a 8 s: `udp → 10.0.2.10:7`. Nessun handshake: il datagramma parte subito e l'eco torna dopo una sola andata e ritorno.
- Questo datagramma è così grande che viene tagliato in tre frammenti, e solo il primo porta l'intestazione UDP. Per vedere meglio scrivi `:` e `h1 udp h2 msg=ciao`, poi apri quel frame nella vista Pacchetti e trova l'intestazione UDP con i suoi quattro campi. Confronta i suoi 8 byte con l'intestazione TCP del laboratorio 120.

**Cambia e prevedi.**

- `./hoplab run 120-tcp-handshake delay=100ms`: dopo quanto tempo sarà stabilita la connessione?
- `./hoplab run 120-tcp-handshake bytes=20000`: quanti segmenti? Osserva nella vista Sequenza quanti viaggiano insieme all'inizio e più avanti.
- `./hoplab run 120-tcp-handshake loss=0.1 bytes=50000`: cerca `fast retransmit` e `timeout di ritrasmissione` nel registro e leggi la riga finale di riepilogo. Ogni esecuzione è diversa. Un ACK perso spesso non causa alcuna ritrasmissione: perché?
- Nel laboratorio 101 scrivi `:` e `h1 udp h2 port=9 msg=hi`: sulla porta 9 nessuno ascolta. Che cosa tornerà indietro? Confronta con ciò che ha fatto TCP sulla porta 81.

## Verifica

- Due schede del browser sullo stesso computer caricano pagine dallo stesso server. Come fa la risposta di ciascuna a raggiungere la scheda giusta?
- Scrivi i tre segmenti dell'handshake, con i flag e la relazione fra i loro numeri.
- Un host ha ricevuto i byte da 1 a 2920 e poi riceve il segmento che comincia a 4381. Quale numero di ACK invia?
- Indica un'applicazione per la quale UDP è la scelta migliore, e spiega perché.
- Che differenza c'è fra un SYN che non riceve risposta e uno che riceve un RST?
