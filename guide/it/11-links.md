<!-- level: 1 -->
<!-- labs: 102-link-properties -->
# Collegamenti: ritardo, banda, perdita e code

Un collegamento è il cavo o la tratta radio che unisce due dispositivi. Sarebbe bello se fosse un tubo perfetto: entrano dei bit, escono subito gli stessi bit. I collegamenti reali fanno invece quattro cose ai tuoi pacchetti, e quasi tutto ciò che noterai mai di una rete ("è lenta", "la chiamata va a scatti") nasce da queste quattro.

Pensa a una strada con un casello. La **distanza** decide quanto dura il viaggio anche senza traffico: è il **ritardo di propagazione**. Il casello lascia passare solo un certo numero di auto al minuto: è la **larghezza di banda**. Quando le auto arrivano più in fretta di quanto il casello le serva, aspettano in fila: è la **coda**. Il piazzale davanti al casello è finito, e quando è pieno le nuove auto vengono respinte: è la **perdita** per trabocco. Ogni tanto un'auto si guasta semplicemente per strada: è la perdita casuale.

Un pacchetto può quindi arrivare tardi per due ragioni diverse, perché viene da lontano o perché è rimasto in fila, e può sparire per due ragioni diverse, il rumore o una coda piena. Imparare a distinguerle è la prima abilità di chi studia le reti.

<!-- detail: 2 -->

## Come funziona

La **larghezza di banda** (o velocità, R) è il numero di bit al secondo che il collegamento può trasportare: 1 Mbit/s è un milione di bit ogni secondo. Un frame di L bit richiede un **tempo di trasmissione** L / R per essere immesso sul collegamento, dal primo bit all'ultimo.

Il **ritardo di propagazione** è il tempo che un bit impiega a percorrere il collegamento: distanza divisa per la velocità del segnale (circa 200 000 km/s nel rame o nella fibra). Non dipende dalla dimensione del frame né dalla banda.

Il frame è arrivato quando arriva il suo ultimo bit:

```
tempo di sola andata = tempo di trasmissione + ritardo di propagazione = L/R + ritardo
```

Un esempio svolto con i numeri del laboratorio 102 (1 Mbit/s, 20 ms). Un frame di ping è di 102 byte = 816 bit, quindi il suo tempo di trasmissione è 816 / 1 000 000 s = 0,816 ms. L'andata dura 20,816 ms; la risposta altrettanto; il ping riporta 41,63 ms. Un datagramma da 1000 byte diventa un frame da 1046 byte = 8368 bit e richiede 8,368 ms: dieci volte di più sullo stesso collegamento.

**Coda.** Un collegamento invia un frame alla volta. I frame che arrivano mentre è occupato aspettano in coda, il primo arrivato è il primo servito. Nel laboratorio 102 una sorgente invia un datagramma da 1000 byte ogni 4 ms (2 Mbit/s), ma al collegamento servono 8,368 ms per ciascuno. La coda cresce di circa un pacchetto ogni 8 ms. Ha posto per 20 pacchetti, quindi dopo circa 0,15 s è piena, e da lì in poi circa un pacchetto su due viene buttato: è lo scarto in coda, o **tail drop**.

**Ritardo di coda.** Un pacchetto che arriva con 20 pacchetti davanti aspetta 20 x 8,368 ms = 167 ms prima che cominci la sua trasmissione. Ecco perché il ping inviato durante il flusso impiega circa 207 ms invece di 41,6.

La **perdita casuale** è un'altra cosa: un frame viene distrutto dal rumore lungo il percorso, con una certa probabilità, per esempio `loss=0.1`, anche su un collegamento scarico.

<!-- detail: 3 -->

## Per approfondire

Su un percorso con più collegamenti i ritardi si sommano, e ogni router deve ricevere un pacchetto intero prima di inoltrarlo (**store and forward**), quindi il tempo di trasmissione si paga di nuovo a ogni salto. La somma da manuale per un salto è elaborazione + coda + trasmissione + propagazione. L'elaborazione qui è trascurabile; l'attesa in coda è l'unico termine che cambia da un pacchetto all'altro, ed è quindi l'origine del **jitter**, la variazione del ritardo che disturba voce e video.

Il **throughput** di un percorso è limitato dal suo collegamento più lento, il **collo di bottiglia**. Banda e ritardo sono indipendenti: un collegamento satellitare può essere veloce (molti Mbit/s) e avere comunque 250 ms di ritardo di propagazione, e nessuna spesa in banda lo accorcerà. Per i messaggi brevi domina il ritardo; per i file grandi domina la banda.

Quanto deve essere lunga una coda? Una coda corta scarta pacchetti durante brevi raffiche che avrebbe potuto assorbire. Una coda lunga non scarta mai, ma sotto sovraccarico prolungato aggiunge soltanto ritardo: il laboratorio 102 con `queue=100` perde la stessa frazione di pacchetti e fa aspettare quasi un secondo quelli che passano. I buffer sovradimensionati dei router domestici hanno causato proprio questo problema, noto come **bufferbloat**. Nota anche che nessuna coda può rimediare a una sorgente che continua a inviare più in fretta del collegamento: la frazione persa tende a 1 − R/velocità offerta qualunque sia il buffer. Il vero rimedio è che i mittenti rallentino, ed è ciò che fa il controllo di congestione di TCP (laboratorio 320), aiutato da code più intelligenti che scartano o marcano i pacchetti in anticipo (laboratori 371 e 372) o servono i flussi in modo equo (laboratorio 373).

Semplificazioni di netlab: la perdita è indipendente da un frame all'altro, mentre il rumore reale arriva spesso a raffiche; e la coda si misura in pacchetti, mentre i dispositivi reali contano spesso i byte.

Per saperne di più: Kurose e Ross, capitolo 1, paragrafo "Ritardi, perdite e throughput nelle reti a commutazione di pacchetto"; Peterson e Davie, "Computer Networks: A Systems Approach", paragrafo sulle prestazioni (banda e latenza); RFC 7567 sulla gestione delle code.

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 102-link-properties**: due host e un collegamento da 1 Mbit/s con 20 ms di ritardo e una coda di 20 pacchetti.

- Avvialo con `./netlab run 102-link-properties`.
- Da 0 a 1,5 s il collegamento è scarico e h1 fa tre ping verso h2. Leggi i tempi nel Registro (`l`). Il primo è di circa 82 ms perché attende ARP; gli altri sono 41,63 ms. Verifica con 2 x (20 + 0,816).
- A 3 s h1 avvia un flusso da 2 Mbit/s. Osserva il numero della coda accanto a h1 sulla mappa salire fino a 20, poi le righe `coda piena` comparire nel registro.
- A 4 s h1 fa di nuovo ping, otto volte. Circa metà delle richieste trova la coda piena e viene scartata come i pacchetti del flusso; quelle che entrano tornano dopo circa 207 ms. Nel cavo non è cambiato nulla: il tempo in più è tutta attesa (20 pacchetti da circa 8,4 ms ciascuno).
- Seleziona h1 con `n` e apri la scheda Collegamenti; seleziona h2 e premi `g` per la vista Grafici, con il ritardo misurato dal sink.

**Cambia e prevedi.** Scrivi la tua previsione prima di ogni esecuzione.

- `./netlab run 102-link-properties delay=100ms`: quale sarà il tempo di ping a collegamento scarico?
- `./netlab run 102-link-properties bw=500k`: il tempo di ping a collegamento scarico cambia pochissimo, perché? Quale frazione del flusso andrà persa? (I ping a 4 s ora non trovano quasi mai posto: la coda è piena quasi sempre.)
- `./netlab run 102-link-properties bw=4M`: ci sarà una coda? Si perderà qualcosa? (La verifica `stream` del laboratorio calcola la perdita attesa da `bw` e `queue`: confrontala con il tuo numero.)
- `./netlab run 102-link-properties queue=100`: il sink perde meno pacchetti? Che cosa succede al ritardo del ping a 4 s?
- `./netlab run 102-link-properties loss=0.2`: perché un ping riesca devono sopravvivere sia la richiesta sia la risposta. Quale frazione di ping ti aspetti che riesca?

Puoi anche modificare un collegamento mentre il laboratorio gira: `:` e poi `link h1 h2 loss=0.3`, oppure `link h1 h2 down`.

## Verifica

- Un frame da 1500 byte viene inviato su un collegamento da 10 Mbit/s con 5 ms di ritardo di propagazione. Quando arriva il suo ultimo bit?
- Raddoppi la banda di un collegamento. Quale dei due ritardi cambia, e di quanto?
- Sullo stesso percorso un ping impiega 40 ms di notte e 300 ms la sera. Quale dei quattro effetti è la causa più probabile?
- Perché una coda più lunga non riduce la perdita di una sorgente che invia al doppio della velocità del collegamento?
- Indica una situazione in cui il ritardo di propagazione conta più della banda, e una in cui vale il contrario.
