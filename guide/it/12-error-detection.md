<!-- level: 1 -->
<!-- labs: 160-error-detection, 161-error-correction -->
# Rilevare e correggere gli errori sui bit

I bit viaggiano come segnali elettrici, ottici o radio, e i segnali vengono disturbati: un motore si avvia lì vicino, un'altra radio trasmette, il segnale è semplicemente debole. Ogni tanto chi riceve legge un 1 dove era stato inviato uno 0. Un solo bit invertito può trasformare un importo di 100 euro in 228, quindi chi riceve deve almeno **sapere** quando un frame è danneggiato.

Il trucco è sempre lo stesso: chi invia aggiunge alcuni bit in più calcolati a partire dai dati, la **ridondanza**. Chi riceve ripete il calcolo e confronta. Se i risultati differiscono, qualcosa è cambiato lungo la strada.

Un esempio di tutti i giorni è l'ultimo carattere del codice fiscale o l'ultima cifra del numero di una carta di pagamento: è calcolato dagli altri, così un errore di battitura viene notato subito.

Con poca ridondanza chi riceve può solo dire "questo è sbagliato" e buttare il frame: è la **rilevazione d'errore**. Con più ridondanza può capire **quale** bit è sbagliato e reinvertirlo: è la **correzione d'errore**. Rilevare costa poco, ma poi qualcuno deve inviare di nuovo i dati; correggere non richiede un secondo tentativo, ma costa bit in più su ogni frame, danneggiato o no.

<!-- detail: 2 -->

## Come funziona

Il **tasso di errore sul bit** (BER) è la probabilità che un bit venga invertito. Con BER 1e-4, in media è sbagliato un bit ogni 10 000. Un datagramma da 500 byte diventa un frame da 546 byte = 4368 bit, quindi circa il 35% dei frame contiene almeno un errore: un BER piccolo danneggia una grossa parte dei frame lunghi.

**Bit di parità.** Si aggiunge un bit in modo che il numero totale di 1 sia pari. I dati `1011001` hanno quattro 1, quindi il bit di parità è 0. Se un bit si inverte, il conteggio diventa dispari e chi riceve se ne accorge. Se se ne invertono due, il conteggio torna pari e l'errore passa. La parità rileva ogni numero dispari di inversioni e nessun numero pari.

**Checksum.** Si trattano i dati come un elenco di numeri a 16 bit, si sommano e si invia la somma (complementata). È il checksum di Internet usato da IP, UDP e TCP. Si calcola facilmente via software e scopre la maggior parte degli errori, ma due modifiche che si compensano nella somma passano inosservate.

**CRC** (controllo a ridondanza ciclica). Si tratta l'intero frame come un unico enorme numero binario, lo si divide per un numero fisso concordato in anticipo e si invia il resto. Ethernet e Wi-Fi aggiungono un CRC a 32 bit. Rileva ogni raffica di errori lunga fino a 32 bit, e qualunque altro danno gli sfugge con una probabilità di circa una su quattro miliardi.

**Correzione.** Il codice più semplice invia ogni bit tre volte e decide a maggioranza: `1` diventa `111`, e un `101` ricevuto viene letto come `1`. Corregge un errore per terna e triplica il frame. **Hamming(7,4)** è più furbo: ogni 4 bit di dati aggiunge 3 bit di controllo, ciascuno dei quali copre un diverso sottoinsieme delle 7 posizioni. La combinazione dei controlli che falliscono, la **sindrome**, letta come numero binario, è la posizione del bit sbagliato. Corregge un errore in ogni blocco di 7 al costo di 1,75 volte la dimensione.

<!-- detail: 3 -->

## Per approfondire

L'idea che unifica tutti questi codici è la **distanza**. La distanza di Hamming fra due parole di codice valide è il numero di posizioni in cui differiscono. Se la distanza minima di un codice è d, il codice può rilevare fino a d − 1 errori, oppure correggerne fino a (d − 1) / 2, arrotondato per difetto. La parità ha d = 2 (ne rileva 1, ne corregge 0); la ripetizione tripla e Hamming(7,4) hanno d = 3 (ne correggono 1). E qui sta l'insidia: un codice usato per correggere non sa distinguere un errore da due. Con due inversioni nello stesso blocco Hamming(7,4) "ripara" con sicurezza un terzo bit e consegna dati sbagliati. La variante (8,4) aggiunge un bit di parità complessivo e arriva a d = 4: ne corregge uno, ne rileva due (SECDED, lo schema delle memorie dei server).

Rilevare e ritrasmettere, o correggere? Dipende dal canale. Su cavo o fibra gli errori sono rari, quindi un CRC più la ritrasmissione (argomento 13) non spreca quasi nulla. Su un collegamento radio rumoroso, o quando l'andata e ritorno è lunga (una sonda spaziale) o non esiste un canale di ritorno (la TV in diretta), vince la **correzione d'errore in avanti** (FEC). La maggior parte dei sistemi reali usa entrambe: Wi-Fi e reti mobili correggono dentro il livello radio, poi controllano con un CRC, poi ritrasmettono ciò che è ancora sbagliato.

Gli errori reali arrivano spesso a **raffiche**, che mettono in crisi i codici pensati per inversioni isolate. I codici di Reed-Solomon lavorano su byte interi, e l'interleaving distribuisce una raffica su molte parole di codice (laboratori 260 e 360).

I livelli ripetono il controllo di proposito. Il CRC di collegamento protegge una sola tratta; i checksum di IP e di UDP/TCP proteggono dai danni dentro i router e dai collegamenti privi di controllo. Nel laboratorio 160 vedi il checksum UDP fare da ultima difesa.

Per saperne di più: Kurose e Ross, capitolo 6, paragrafo "Tecniche di rilevazione e correzione degli errori"; Tanenbaum, capitolo 3, "Rilevazione e correzione degli errori"; RFC 1071 ("Computing the Internet Checksum"); l'articolo di Hamming del 1950 "Error Detecting and Error Correcting Codes".

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 160-error-detection**: tre collegamenti rumorosi identici, uno sopra l'altro. In alto (a1–a2): nessun controllo sul collegamento. Al centro (b1–b2): un bit di parità. In basso (c1–c2): CRC-32.

- Avvialo con `./hoplab run 160-error-detection` e leggi il Registro (`l`) a dettaglio 2.
- Cerca le righe con `errori sui bit`. Finiscono con `rilevati, frame scartato` oppure con `NON rilevati dal collegamento: frame consegnato danneggiato`. Su quale collegamento compare ciascun tipo? Sul collegamento con la parità guarda il numero di bit invertiti in ogni riga: che cosa noti?
- Quando un frame danneggiato passa, pochi millisecondi dopo segue una riga `UDP checksum errato: datagramma scartato`: l'ha fermato il livello di trasporto.
- Seleziona a2, poi b2, poi c2 con `n` e confronta il contatore `udp.bad_checksum` (a dettaglio 2 la scheda Nodo elenca i contatori del nodo selezionato). Aspettati circa 70, circa 15 ed esattamente 0 su 200 datagrammi.
- Scrivi `:` e `a1 coding show code=parity pos=3,4`: due inversioni ingannano il bit di parità e il testo HELLO viene consegnato alterato. Poi `a1 coding show code=parity pos=3`.

**Laboratorio 161-error-correction**: ancora tre collegamenti, con più rumore. In alto: solo CRC. Al centro: Hamming(7,4). In basso: tre copie.

- Avvialo con `./hoplab run 161-error-correction`. Ora il registro mostra anche `corretti dal codice di linea`.
- Alla fine confronta `sink.received` su c2, h2 e r2: circa 60 su 200 sul collegamento con il CRC, 200 su 200 sugli altri, senza alcuna ritrasmissione.
- Confronta `sink.delay_ms`: circa 9,4 ms con il CRC, 12,6 ms con Hamming, 18 ms con tre copie. La correzione si paga in tempo sul filo.

**Cambia e prevedi.**

- `./hoplab run 160-error-detection ber=1e-3`: quanti datagrammi riceverà c2? Un rilevatore perfetto aiuta?
- `./hoplab run 161-error-correction ber=3e-3`: che cosa comincerà a succedere sul collegamento Hamming? Osserva `udp.bad_checksum` su h2.
- `./hoplab run 161-error-correction ber=1e-5`: quale collegamento è ora il più conveniente?
- Nel laboratorio 161 scrivi `c1 coding show code=hamming74 pos=3` e poi `pos=3,4`.

## Verifica

- I dati `1100101` vengono inviati con un bit di parità pari. Quanto vale il bit? Quali sequenze ricevute verrebbero accettate pur essendo sbagliate?
- Perché, a parità di BER, i frame lunghi hanno più probabilità di essere danneggiati di quelli corti?
- Un collegamento con CRC consegna solo i frame che ritiene corretti. Perché c2 ha comunque ricevuto meno datagrammi di quelli inviati?
- Che cosa fa Hamming(7,4) quando si invertono due bit dello stesso blocco?
- Indica una situazione in cui ritrasmettere è impossibile o troppo lento, e la correzione è l'unica scelta.
