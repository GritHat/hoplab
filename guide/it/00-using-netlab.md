<!-- level: 0 -->
<!-- labs: 100-two-hosts, 101-ping, 102-link-properties, 120-tcp-handshake -->
# Usare netlab

netlab è un laboratorio di reti che vive nel terminale. Emula host, router e switch che si scambiano frame veri, byte per byte (Ethernet, ARP, IPv4, ICMP, UDP, TCP e i protocolli che ci stanno sopra), su collegamenti che hanno una banda, un ritardo, una coda e, quando lo vuoi, perdite ed errori di bit. Niente tocca la tua rete reale: tutto gira su un orologio virtuale che puoi rallentare, fermare e far avanzare un passo alla volta.

Un **laboratorio** (o scenario) è un piccolo file di testo che descrive una rete e ciò che vi accade. netlab ha un catalogo di laboratori ordinati dal primo ping agli algoritmi di consenso, e questa guida ti dice che cosa osservare in ciascuno.

## Per cominciare

Avvia il programma senza argomenti per avere il menu:

```
./netlab
```

Muoviti con le frecce e premi Invio, oppure premi il numero di una voce. **Catalogo degli scenari** elenca i laboratori pronti: scegline uno, premi Invio, poi di nuovo Invio su **Avvia**. Da questa guida puoi anche avviare i laboratori elencati in fondo a un argomento premendo la loro lettera (`a`, `b`, `c`, ...).

Se conosci già il nome di un laboratorio puoi saltare il menu:

```
./netlab run 101-ping
./netlab run ping
```

<!-- detail: 2 -->
Il menu ha sette voci: **Catalogo degli scenari**, **Guida allo studio** (quella che stai leggendo), **Costruisci la tua rete** (un modulo che scrive uno scenario per te: topologia, parametri dei collegamenti, instradamento, traffico), **Apri un file di scenario**, **Moduli** (il manuale di ogni protocollo e strumento), **Demo di trasferimento affidabile** (i programmi originali stop-and-wait, Go-Back-N e Selective Repeat su UDP reale) ed **Esci**.

Nel catalogo, `/` cerca nei titoli e negli argomenti, `Tab` filtra per livello dello stack, e la parte destra mostra la descrizione del laboratorio selezionato, la sua difficoltà, il numero di nodi e di collegamenti e i suoi parametri. Le reti costruite con la procedura guidata sono salvate in `~/.local/share/netlab/scenarios/` e compaiono nel catalogo sotto **I miei scenari**.

In questa guida: `↑` `↓` scelgono un argomento, `→` o Invio passano al testo, `←` torna all'elenco, `PagSu` `PagGiù` scorrono, `1` `2` `3` scelgono la profondità di lettura, `/` cerca, `Esc` esce.

<!-- detail: 3 -->
Il nome di uno scenario sulla riga di comando può essere il percorso di un file, un nome del catalogo con o senza `.nl`, o soltanto la parte dopo il numero: `ping` trova `101-ping.nl`. `./netlab list` stampa il catalogo, `./netlab guide` questa guida, `./netlab guide ARGOMENTO` un solo argomento (come testo semplice quando l'uscita è rediretta).

netlab cerca `scenarios/`, `guide/` e `locale/` accanto al proprio eseguibile, oppure in `../share/netlab` rispetto a esso; imposta la variabile d'ambiente `NETLAB_HOME` per indicare un altro posto.

<!-- detail: 1 -->
## Lo schermo

Un laboratorio in esecuzione ha tre aree. In alto a sinistra c'è la **mappa della rete**: gli host sono disegnati come `●`, i router come `◆`, gli switch come `▣`, e un nodo che si è guastato come `✗`. I frame viaggiano lungo i collegamenti come piccole etichette colorate con il nome del loro protocollo (ARP, ICMP, TCP, DNS, ...); un frame che va perso diventa una `✗` rossa a metà strada. La riga di testo in fondo alla mappa è il commento del laboratorio: cambia man mano che la storia procede.

In alto a destra c'è l'**ispettore del nodo** selezionato: le sue interfacce e i suoi indirizzi, la tabella di instradamento, la cache ARP, i collegamenti, e una scheda per ogni protocollo che vi gira.

Il **pannello inferiore** mostra una fra cinque viste: il registro degli eventi, l'elenco dei pacchetti catturati, un diagramma di sequenza, i grafici e la guida del laboratorio. La riga in cima mostra il tempo virtuale in millisecondi, se il tempo scorre o è in pausa, la velocità e il livello di dettaglio.

<!-- detail: 2 -->
### L'ispettore del nodo

Le schede sono **Nodo** (interfacce, indirizzi e il nodo all'altro capo di ciascuna; dal dettaglio 2 anche indirizzi MAC, conteggi dei frame e contatori del nodo), **Rotte** (destinazione, next hop, interfaccia, metrica e origine della rotta; le rotte appena apprese sono evidenziate) oppure **Tabella MAC** su uno switch, **ARP** (non sugli switch), **Collegamenti** (velocità, ritardo, perdita, MTU, quanto è piena la coda di uscita e quale disciplina la gestisce) e poi una scheda per ogni modulo che ha qualcosa da mostrare: `ping`, `tcp`, `rip`, `dns`, `raft`... Cambia scheda con `←` `→` quando l'ispettore ha il fuoco.

### Il pannello inferiore

- **Registro** (`l`): una riga per evento con il tempo, il nodo, il livello (`L2`, `L3`, `L4`, `L7`...) e che cosa è successo. `f` mostra solo il nodo selezionato. Con il fuoco sul pannello, `↑` `↓` `PagSu` `PagGiù` scorrono all'indietro e `Fine` torna alla riga più recente.
- **Pacchetti** (`p`): ogni frame messo su un collegamento, dal più recente, con la sua sorte (perso, danneggiato, scartato da una coda, corretto). `↑` `↓` selezionano un frame; Invio ne apre la dissezione, livello per livello e campo per campo. `[` e `]` passano da un campo all'altro, e in una finestra larga un dump esadecimale evidenzia i byte del campo selezionato.
- **Sequenza** (`s`): gli stessi frame come frecce fra colonne di nodi, il classico diagramma temporale. Invio apre il frame selezionato nella vista Pacchetti.
- **Grafici** (`g`): le serie temporali registrate dal nodo selezionato (una finestra di congestione, un tempo di andata e ritorno, la lunghezza di una coda). Se il nodo selezionato non ne registra, selezionane un altro con `n`.
- **Guida** (`u`): la descrizione del laboratorio, la sua cronologia con un segno di spunta su ciò che è già avvenuto, la teoria dei moduli attivi sul nodo selezionato e alcuni comandi da provare.

<!-- detail: 3 -->
Sulla mappa, dal dettaglio 2 ogni nodo mostra il suo primo indirizzo IP e un collegamento la cui coda non è vuota mostra un piccolo numero: i pacchetti in attesa. Al dettaglio 3 (e con al massimo 12 collegamenti) ogni collegamento mostra anche velocità e ritardo. Un frame danneggiato ma consegnato lo stesso è segnato con `↯`, uno riparato da un codice a correzione d'errore con `✓`. Un nodo con un ruolo (`DNS`, `NAT`, `LB`...) porta una piccola etichetta.

In una finestra più stretta di 100 colonne o più bassa di 28 righe la disposizione diventa a colonna singola: la mappa o l'ispettore (quello che ha avuto il fuoco per ultimo) sopra il pannello inferiore. Premi `z` per dare tutta la finestra all'area che ha il fuoco. Il minimo è 44 per 12.

Quando esci da un laboratorio che dichiara delle verifiche, netlab le stampa con il loro esito: un modo rapido per vedere se l'esecuzione è andata come l'autore intendeva.

<!-- detail: 1 -->
## I tasti

- `spazio` mette in pausa e riprende; `.` in pausa avanza al prossimo evento.
- `+` e `-` fanno scorrere il tempo più in fretta o più piano; `a` attiva e disattiva il salto automatico dei tempi morti.
- `1` `2` `3` scelgono il livello di dettaglio: panoramica, messaggi dei protocolli, ogni frame.
- `Tab` sposta il fuoco fra rete, nodo e pannello inferiore; le frecce agiscono sull'area che ha il fuoco: selezionano un nodo sulla mappa, cambiano scheda nell'ispettore, scorrono nel pannello inferiore.
- `n` e `N` selezionano il nodo successivo e il precedente.
- `l` `p` `s` `g` `u` scelgono la vista inferiore: registro, pacchetti, sequenza, grafici, guida.
- `Invio` nell'elenco dei pacchetti apre o chiude la dissezione del frame selezionato; `[` e `]` scelgono il campo precedente o successivo.
- `f` limita il registro al nodo selezionato; `z` ingrandisce l'area che ha il fuoco a tutta la finestra.
- `k` fa cadere il nodo selezionato, o lo ripristina se è caduto.
- `:` apre la riga di comando; `?` mostra i tasti; `q` esce.

<!-- detail: 2 -->
Alcuni tasti hanno sinonimi: anche `h` apre l'aiuto, anche `/` apre la riga di comando, `=` e `_` funzionano come `+` e `-`, e `Maiusc+Tab` sposta il fuoco all'indietro. Con il fuoco sulla mappa, Invio lo passa all'ispettore.

<!-- detail: 1 -->
## Tempo e dettaglio

Il tempo in un laboratorio è virtuale. Un frame che impiega 10 ms ad attraversare un collegamento sarebbe invisibile a velocità reale, perciò netlab sceglie una velocità al rallentatore alla quale un frame impiega circa un secondo ad attraversare un collegamento tipico, e la mostra nella riga in cima (per esempio `1/50×`). Premi `+` o `-` per raddoppiarla o dimezzarla. Quando nulla si muove, netlab salta direttamente al prossimo evento invece di farti aspettare: è il `+skip` accanto alla velocità, e `a` lo disattiva quando vuoi sentire le pause vere, come un timeout di ritrasmissione.

Metti in pausa con `spazio` e poi premi `.` più volte per avanzare di un evento alla volta: è il modo migliore per leggere un handshake o un aggiornamento di instradamento.

I tre **livelli di dettaglio** decidono quanto vedi. Il livello 1 è la panoramica: solo l'esito di ogni azione, ciò che un principiante dovrebbe notare. Il livello 2 aggiunge i messaggi dei protocolli: richieste ARP, segmenti TCP, aggiornamenti di instradamento, scarti dalle code. Il livello 3 mostra ogni frame e ogni passo interno. Il livello filtra il registro, aggiunge informazioni alla mappa e all'ispettore, e rivela una parte maggiore della guida.

<!-- detail: 3 -->
La velocità può andare da 1/100000 del tempo reale a 1000 volte più veloce. Se passi `--speed X` sulla riga di comando, o lo scenario ne imposta una, la scelta automatica viene saltata. Poiché il tempo virtuale avanza solo elaborando eventi, un'esecuzione con lo stesso seme casuale (`--seed N`) si ripete identica, qualunque sia la velocità.

<!-- detail: 1 -->
## La riga di comando

Premi `:` e scrivi un comando, poi Invio. Il più comune avvia un protocollo o uno strumento su un nodo: il nodo, il modulo, poi i suoi argomenti.

```
h1 ping h2
h1 ping 10.0.2.10 count=10 interval=200ms
h1 traceroute h2
link r1 r2 down
link r1 r2 up
link h1 r1 loss=0.3 delay=50ms
node r2 down
node r2 up
help
help ping
```

`link A B down` taglia un collegamento e `up` lo ripristina; `link A B chiave=valore` ne cambia i parametri mentre il laboratorio gira. `node N down` fa cadere un nodo: i suoi timer si fermano e i moduli che vi girano perdono lo stato volatile. `help` elenca i moduli e `help MODULO` stampa nel registro la sintassi di uno di essi.

<!-- detail: 2 -->
Sulla riga di comando, `↑` e `↓` richiamano i comandi precedenti, `Tab` completa il nome di un nodo, `Ctrl+U` svuota la riga ed `Esc` annulla. L'esito compare per qualche secondo sull'ultima riga: `✓` e il comando, oppure `✗` e il motivo.

Altri comandi:

- `stop NODO MODULO` ferma un modulo su un nodo.
- `at TEMPO comando` esegue un comando più tardi; sulla riga di comando il tempo si conta da adesso (`at 5s link r1 r2 down`).
- `route NODO add PREFISSO via GATEWAY`, `route NODO del PREFISSO`, `gw NODO GATEWAY` e `ip NODO:IF A.B.C.D/LEN` modificano rotte e indirizzi.
- `set speed X` e `set detail N` fanno ciò che fanno i tasti; così anche `pause`, `resume` e `quit`.
- `log un testo qualsiasi` scrive una tua riga nel registro.

Tutto ciò che un modulo accetta è descritto nella voce **Moduli** del menu e da `./netlab help MODULO`.

<!-- detail: 1 -->
## I parametri

Molti laboratori hanno dei parametri: una probabilità di perdita, una banda, l'algoritmo di controllo di congestione. Quando avvii un laboratorio dal catalogo un modulo li mostra; cambia un valore e premi Invio su **Avvia**. La riga **Impostazioni** passa da **semplici** (i parametri che contano di più) ad **avanzate** (tutti, più il livello di dettaglio, la scala dei tempi e il seme casuale).

Sulla riga di comando, i parametri sono coppie `nome=valore` dopo lo scenario:

```
./netlab run 101-ping loss=0.2
./netlab run 320-tcp-congestion cc=cubic queue=100
```

Il catalogo mostra i parametri del laboratorio selezionato con i loro valori predefiniti.

<!-- detail: 2 -->
## Senza interfaccia

Con `--headless`, o ogni volta che l'uscita non è un terminale, netlab esegue il laboratorio più in fretta che può e stampa il registro degli eventi, una riga per evento. Si ferma alla `duration` dello scenario, o al tempo indicato con `--until`.

```
./netlab run 101-ping --headless --detail 2
./netlab run 101-ping --headless --until 20s --seed 7 | less
```

Un laboratorio può terminare con delle **verifiche**, affermazioni come "h1 ha ricevuto 3 risposte al ping". Un'esecuzione senza interfaccia le verifica e stampa `PASS` o `FAIL`; `--quiet` stampa soltanto quello. `./netlab check` esegue così ogni laboratorio del catalogo (o quelli che indichi) e riferisce quanti sono falliti: usalo dopo aver modificato uno scenario o un protocollo.

```
./netlab check
./netlab check 101-ping my-lab.nl
```

<!-- detail: 3 -->
`netlab check` usa sempre il seme 1, perciò i suoi verdetti non cambiano da un'esecuzione all'altra. Il codice di uscita di un'esecuzione senza interfaccia è 0 quando ogni verifica è soddisfatta, 1 quando una fallisce, 2 quando lo scenario non può essere caricato.

<!-- detail: 2 -->
## Finestre affiancate

`./netlab windows NOME` (oppure **Esegui in: finestre affiancate** nel modulo di avvio) esegue lo stesso laboratorio come più processi fianco a fianco: un riquadro mostra l'intera rete, e fino a cinque altri riquadri eseguono ciascuno un solo nodo con la propria interfaccia, proprio come se ognuno fosse un computer separato. I collegamenti fra i riquadri sono veri datagrammi UDP sulla tua macchina.

```
./netlab windows 101-ping
./netlab windows 230-rip-convergence --nodes r1,r3
```

netlab usa kitty o tmux, quello che trova; `--mode print` stampa soltanto i comandi, da eseguire a mano nei tuoi terminali. Per impostazione predefinita ogni host e ogni router ha un riquadro (i primi cinque); `--nodes` li sceglie.

## Più macchine

Un laboratorio può anche essere distribuito su computer veri. Ciascuno esegue alcuni dei nodi con `netlab node`, e uno di essi può mostrare la vista globale con `netlab monitor`:

```
./netlab plan my-lab.nl
./netlab node my-lab.nl h1
./netlab node my-lab.nl r1,h2
./netlab monitor my-lab.nl
```

Tre regole lo fanno funzionare. Ogni macchina deve usare **lo stesso file di scenario**, che dice dove gira ogni nodo con righe `place NODO HOST` e dove si trova il monitor con una riga `monitor HOST`. Le macchine devono raggiungersi via **UDP**: per impostazione predefinita il nodo numero i del file (contando da 0) ascolta sulla porta 47000 + i e il monitor sulla 46999; `--baseport N` le sposta tutte. E tutti i processi devono usare la stessa `--speed`.

La via facile è il menu: scegli un laboratorio, imposta **Esegui in** su **più macchine**, scrivi l'indirizzo della macchina di ogni nodo, e netlab scrive la copia dello scenario con le righe `place` e stampa il comando per ogni macchina.

<!-- detail: 3 -->
Un processo che riferisce a un monitor non avvia subito il proprio orologio: mostra "in attesa che il monitor avvii il laboratorio" finché il monitor non risponde, così che i primi frame non vengano inviati a processi che non esistono ancora. Il monitor fa partire tutti insieme quando ha sentito ogni nodo, quindi l'ordine in cui avvii i processi non conta; premere Invio in un processo in attesa lo avvia comunque. In un laboratorio distribuito non esiste una pausa globale: ogni processo ha il proprio orologio, e metterne in pausa uno ferma soltanto quello. Vedi `docs/distributed.md` per i dettagli.

<!-- detail: 1 -->
## La lingua

netlab parla la lingua del tuo sistema quando ne ha una traduzione (al momento inglese e italiano). Per scegliere esplicitamente:

```
./netlab --lang it
NETLAB_LANG=it ./netlab
```

L'interfaccia, le descrizioni dei laboratori e questa guida seguono la lingua; il gergo dei protocolli nel registro (`ARP who has ...`, `[SYN, ACK]`) resta com'è sul cavo.

## Come proseguire

Leggi **Il percorso di studio** per vedere come sono organizzati i laboratori e in che ordine affrontarli. Poi apri il primo, **Due host e un cavo**: premi `a` qui sotto. Quando vorrai costruire una rete tua, la voce di menu **Costruisci la tua rete** ti fa partire in un minuto, e **Scrivere i propri laboratori** spiega il linguaggio degli scenari.
