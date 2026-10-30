# Trasferimento affidabile su UDP

Implementazioni in C di protocolli di reliable data transfer sopra UDP, con un
canale inaffidabile simulato e un'interfaccia grafica da terminale che mostra i
pacchetti in viaggio tra sender e receiver.

| Protocollo | Sender | Receiver |
|---|---|---|
| Stop-and-wait (bit alternato, rdt 3.0) | `stop_and_wait_send` | `stop_and_wait_recv` |
| Go-Back-N | `go_back_n_send` | `go_back_n_recv` |
| Selective Repeat | `selective_repeat_send` | `selective_repeat_recv` |

## Compilazione e avvio

```sh
make                 # compila tutto
./demo.sh            # stop-and-wait: sender (sinistra) e receiver (destra) affiancati
./demo.sh gbn        # Go-Back-N
./demo.sh sr         # Selective Repeat
```

`demo.sh` apre sender e receiver in una nuova finestra massimizzata del
terminale da cui lo lanci, se è fra quelli supportati: kitty, gnome-terminal,
ptyxis, kgx, konsole, xfce4-terminal, mate-terminal, tilix, terminator,
lxterminal, alacritty, foot, wezterm o xterm. Altrimenti, per esempio dal
terminale di VS Code, usa kitty se c'è, poi il primo di quegli altri che trova
installato (o x-terminal-emulator). `-m` sceglie il modo:

- `kitty`: una finestra kitty massimizzata divisa in due;
- `terminal`: come sopra ma senza kitty: una finestra del terminale corrente o
  del primo trovato, divisa in due come quella di kitty;
- `split`: il terminale corrente diviso in due, senza programmi aggiuntivi. È
  quello che usa `terminal` dentro la nuova finestra, e quello scelto da solo
  quando non c'è un'interfaccia grafica (console di Linux, ssh); `q` chiude;
- `gnome`: due finestre gnome-terminal separate, ognuna su metà del monitor
  principale (su Wayland tramite un'istanza di gnome-terminal su XWayland,
  perché le finestre Wayland non possono scegliere dove aprirsi);
- `tmux`: due pannelli tmux affiancati (scelto da solo se si è già in tmux).

```sh
./demo.sh -p 9000 -s "-l 0.3 -c 0.2" -r "-l 0.3"   # porta e opzioni di sender/receiver
./demo.sh sr -w 6 -b 4                             # finestra e numeri di sequenza
./demo.sh -m split gbn                             # nel terminale corrente
RDT_TERMINAL=konsole ./demo.sh                     # preferisce konsole a ogni altro
```

Nella divisione in due, `split` e `terminal`, sender e receiver sono
affiancati se la finestra ha almeno 145 colonne; se è stretta ma alta, con
almeno 49 righe, stanno uno sopra l'altro. La disposizione si adatta mentre
si ridimensiona la finestra.

Sulla console testuale di Linux (`TERM=linux`, per esempio Ctrl+Alt+F3)
l'interfaccia usa i 16 colori della console e solo caratteri presenti nei suoi
font (code page 437): ╭ diventa ┌, ▶ diventa ►, ✓ diventa √ e così via.
`RDT_BASIC=1` forza questa modalità in qualsiasi terminale, `RDT_BASIC=0` la
esclude.

`-w` e `-b` vengono passati a entrambi i programmi quando servono.

A mano, in due terminali (prima il receiver):

```sh
./go_back_n_recv [opzioni] [host] [porta]
./go_back_n_send [opzioni] [host] [porta]
```

Di default comunicano su `127.0.0.1:8142`. Tutte le opzioni sono descritte da
`-h`; le principali:

| Opzione | Significato | Default |
|---|---|---|
| `-a HOST`, `-p PORTA` | receiver da contattare / indirizzo di ascolto | `127.0.0.1`, `8142` |
| `-l P` | probabilità di perdere un pacchetto inviato (DATA o ACK) | 15% |
| `-c P` | probabilità di invertire un bit di un pacchetto inviato | 10% |
| `-d MS` | ritardo di propagazione simulato | 1000 ms |
| `-t MS` | timeout di ritrasmissione (solo sender) | 2,5·ritardo + 500 |
| `-w N` | finestra (Go-Back-N, Selective Repeat) | 4 |
| `-b K` | bit dei numeri di sequenza: numeri 0…2^K−1 (Go-Back-N, Selective Repeat) | 3 |
| `-i MS` | intervallo minimo fra due invii (sender a finestra) | ritardo/4 |
| `-m TESTO`, `-f FILE`, `-s N` | dati da inviare e byte per pacchetto (solo sender) | frase di prova, 10 |
| `-S N` | seme casuale, per ripetere lo stesso scenario | casuale |

Sender e receiver devono usare gli stessi `-b`, e per Selective Repeat anche lo
stesso `-w` (il receiver Go-Back-N ha sempre finestra 1).

Ogni processo applica perdita e corruzione ai pacchetti che *lui* invia: il
sender ai DATA, il receiver agli ACK. Premere `q` per uscire. Se l'output è
rediretto su file, i programmi stampano solo il registro eventi in testo
semplice.

## Stop-and-wait

Ogni pacchetto porta un bit di sequenza (0/1) e l'ACK riporta lo stesso bit.

- Il sender invia DATA *b*, avvia il timer e aspetta ACK *b*. Se arriva un ACK
  con il bit diverso, quell'ACK si riferisce al trasferimento precedente, quindi
  DATA *b* va ritrasmesso; lo stesso se l'ACK è corrotto o se scade il timer.
- Il receiver che riceve DATA *b* integro e atteso lo consegna e risponde ACK *b*.
  Se riceve un duplicato (l'altro bit) non lo riconsegna ma ritrasmette l'ACK; se
  riceve un pacchetto corrotto rinvia l'ACK dell'ultimo bit corretto.
- Un FIN numerato e riscontrato come i dati chiude il trasferimento.

Con un timeout più corto dell'RTT (2·ritardo) il sender ritrasmette prima
dell'arrivo dell'ACK, quindi da quel punto ogni pacchetto parte almeno due
volte (l'intestazione lo segnala).

## Go-Back-N

Il sender tiene in volo fino a N pacchetti, la finestra [base, base+N−1], con
numeri di sequenza modulo 2^K. Un solo timer sorveglia il pacchetto `base`.

- **Sender.** Invia il pacchetto `nextseqnum` se `nextseqnum < base+N`, e avvia
  il timer se la finestra era vuota. Un ACK *n* è cumulativo: conferma tutti i
  pacchetti fino a *n*, quindi `base = n+1` e il timer riparte, o si ferma se non
  resta nulla in volo. Allo scadere del timer ritrasmette **tutti** i pacchetti
  da `base` a `nextseqnum−1`. ACK corrotti o duplicati sono ignorati.
- **Receiver.** Non ha buffer: accetta solo `expectedseqnum`, lo consegna e
  risponde ACK con quel numero. Ogni altro pacchetto, fuori ordine o corrotto,
  viene scartato e il receiver rinvia l'ACK dell'ultimo ricevuto in ordine.

Serve N ≤ 2^K − 1: con N = 2^K, se si perdono tutti gli ACK di una finestra,
l'ACK del pacchetto prima di `base` è indistinguibile da quello dell'ultimo
pacchetto in volo (`./demo.sh gbn -w 8 -b 3` per vederlo).

## Selective Repeat

Come Go-Back-N la finestra del sender è di N pacchetti, ma ognuno ha il suo
timer e il receiver riscontra i pacchetti uno per uno.

- **Sender.** Un ACK *n* nella finestra segna *n* come riscontrato e ne ferma
  il timer; se *n* è `send_base` la finestra scorre fino al primo pacchetto non
  ancora riscontrato. Allo scadere del timer di *n* si ritrasmette **solo** *n*.
- **Receiver.** Ha una finestra di N posti a partire da `rcv_base`. Un pacchetto
  nella finestra viene riscontrato e messo nel buffer; se è `rcv_base` viene
  consegnato insieme a tutti i successivi già nel buffer. Un pacchetto in
  [`rcv_base`−N, `rcv_base`−1] è già stato consegnato: il receiver rinvia l'ACK,
  perché evidentemente il sender non l'ha ricevuto. Tutto il resto, e i
  pacchetti corrotti, vengono ignorati senza ACK.

Serve N ≤ 2^(K−1), altrimenti le due finestre del receiver si sovrappongono e
un pacchetto vecchio ritrasmesso può essere consegnato come nuovo: con
`./demo.sh sr -w 3 -b 2 -s "-l 0.4" -r "-l 0.4"` il testo ricevuto finisce
per contenere pezzi ripetuti. In entrambi i protocolli l'intestazione segnala
le finestre troppo grandi.

### Cosa mostra l'interfaccia

Oltre al canale e al registro eventi, ogni finestra mostra:

- le regole del protocollo, con quella appena applicata evidenziata;
- la striscia dei numeri di sequenza: una cella per pacchetto con il suo
  numero (modulo 2^K), i puntatori (`base`/`send_base`, `nextseqnum`,
  `expectedseqnum`, `rcv_base`), la finestra e, sotto le celle, i timer.

Il sender invia al massimo un pacchetto ogni `-i` millisecondi (il tempo di
trasmissione simulato), così i pacchetti di una finestra viaggiano uno dietro
l'altro invece che sovrapposti. Con N·intervallo ≥ RTT il canale non resta mai
vuoto.

## File

- `rdt.h/.c`: formato del pacchetto (intestazione di 12 byte e checksum di
  Internet), opzioni da riga di comando, dati da inviare, socket, tempo e
  numeri casuali.
- `channel.h/.c`: canale inaffidabile simulato (perdita, corruzione, ritardo) e
  pacchetti in volo.
- `tui.h/.c`: grafica da terminale (pannelli, registro eventi, animazione del
  canale, regole, striscia della finestra), disegno in metà schermo
  (`RDT_PANE`) e modalità per la console di Linux.
- `stop_and_wait_*.c`, `go_back_n_*.c`, `selective_repeat_*.c`: sender e
  receiver dei tre protocolli.
- `demo.sh`: compila e apre sender e receiver affiancati.
