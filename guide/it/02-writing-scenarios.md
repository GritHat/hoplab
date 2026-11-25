<!-- level: 0 -->
<!-- labs: 101-ping, 200-static-routing, 102-link-properties -->
# Scrivere i propri laboratori

Un laboratorio è un semplice file di testo con estensione `.nl`. Ogni riga è una direttiva: una parola seguita dai suoi argomenti. Descrivi i nodi, li unisci con dei collegamenti, dai loro degli indirizzi, dici che cosa gira e quando, e hoplab fa il resto. Questo argomento costruisce un laboratorio completo passo per passo; il riferimento completo del linguaggio è in `docs/scenario-format.md`.

Il modo più rapido di cominciare non è un file vuoto. Scegli **Costruisci la tua rete** nel menu: la procedura guidata scrive uno scenario per la topologia che chiedi e lo salva in `~/.local/share/hoplab/scenarios/`. Apri quel file nel tuo editor e modificalo. Oppure copia un laboratorio del catalogo da `scenarios/` e modifica la copia.

## Passo 1: nodi e collegamenti

Crea un file `two-offices.nl`. Un PC in un ufficio raggiunge un server in un altro attraverso due router uniti da una linea lenta.

```
node pc  host
node gw1 router
node gw2 router
node srv host

link pc  gw1 bw=100M delay=1ms
link gw1 gw2 bw=1M delay=20ms queue=20
link gw2 srv bw=100M delay=1ms
```

`node NOME TIPO` crea un nodo; il tipo è `host`, `router` o `switch`. `link A B` unisce due nodi, e le coppie `chiave=valore` descrivono il collegamento: `bw` è la sua velocità in bit al secondo, `delay` il ritardo di propagazione in una direzione, `queue` quanti pacchetti possono attendere di essere trasmessi. Un collegamento senza parametri è da 10 Mbit/s, 10 ms, 50 pacchetti e non perde nulla.

<!-- detail: 2 -->
I nomi cominciano con una lettera e hanno al massimo 12 caratteri. Le interfacce vengono create man mano che si aggiungono i collegamenti, in ordine: il primo collegamento di un host o di un router usa `eth0`, il secondo `eth1`; le porte di uno switch sono `p0`, `p1`... Puoi indicare l'interfaccia esplicitamente con `link gw1:eth1 gw2:eth0`, utile quando gli indirizzi che assegnerai dopo devono corrispondere a un disegno.

Gli altri parametri di un collegamento: `loss` (probabilità che un frame vada perso, `0.05` oppure `5%`), `burst` (le perdite arrivano a raffiche di questa lunghezza media), `jitter` (ritardo aggiuntivo casuale), `ber` (tasso di errore sui bit), `corrupt` (probabilità di un bit invertito per frame), `dup`, `reorder`, `mtu`, `qdisc` (la disciplina di coda: `red`, `codel`, `fq`...) e `codec` (il codice a rilevazione o correzione d'errore del collegamento). I tempi si scrivono `500us`, `20ms`, `1.5s`; le velocità `56k`, `10M`, `1G`.

<!-- detail: 3 -->
La mappa viene disposta automaticamente. Per collocare tu i nodi aggiungi `x=` e `y=` (entrambi, da 0 a 100, l'origine è l'angolo in alto a sinistra) alle righe `node`; `role=TESTO` aggiunge una piccola etichetta accanto al nodo sulla mappa, come `role=DNS`. Un file può avere al massimo 64 nodi, 160 collegamenti e 16 interfacce per nodo.

<!-- detail: 1 -->
## Passo 2: gli indirizzi

La via facile è una riga sola:

```
auto all
```

`auto all` dà a ogni rete della topologia la propria sottorete (`10.0.1.0/24`, `10.0.2.0/24`, ... nell'ordine in cui sono scritti i collegamenti; i router ricevono `.1`, `.2`, gli host `.10`, `.11`), dà a ogni host un gateway predefinito, e installa su ogni nodo le rotte più brevi verso tutte le reti. Usala ogni volta che indirizzamento e instradamento non sono l'argomento del laboratorio.

<!-- detail: 2 -->
Quando l'argomento è proprio l'indirizzamento o l'instradamento, fallo a mano:

```
ip pc 192.168.1.10/24
ip gw1:eth0 192.168.1.1/24
ip gw1:eth1 10.9.9.1/30
ip gw2:eth0 10.9.9.2/30
ip gw2:eth1 192.168.2.1/24
ip srv 192.168.2.10/24
gw pc gw1
gw srv 192.168.2.1
route gw1 192.168.2.0/24 via gw2
route gw2 192.168.1.0/24 via 10.9.9.1
```

`ip NODO A.B.C.D/LUN` imposta l'indirizzo della prima interfaccia che non ne ha; `ip NODO:IF ...` indica l'interfaccia. `gw NODO GATEWAY` imposta la rotta predefinita e `route NODO PREFISSO via GATEWAY` una rotta statica; il gateway può essere un indirizzo o il nome di un vicino che condivide una sottorete con il nodo.

Le due metà di `auto` si possono usare separatamente. `auto addr` assegna gli indirizzi e i gateway predefiniti degli host, ma lascia i router senza rotte verso le reti lontane: è ciò che serve quando a scoprirle deve essere un protocollo di instradamento come RIP o OSPF. `auto routes` calcola le rotte statiche a partire da indirizzi che hai scritto tu.

<!-- detail: 3 -->
`auto` viene applicato una volta sola, dopo che l'intero file è stato letto, e non sovrascrive mai ciò che hai impostato a mano: una rete in cui qualche interfaccia ha già un indirizzo viene lasciata stare, e un host che ha già una rotta predefinita la conserva. Ovunque si trovi la riga `auto` nel file, essa vede tutte le righe `ip`. Gli switch non hanno indirizzi; tutti i nodi attaccati allo stesso switch (o alla stessa VLAN, impostata con `vlan SWITCH:PORTA N`) condividono una sottorete.

<!-- detail: 1 -->
## Passo 3: che cosa succede

Ora fai accadere qualcosa. `run NODO MODULO argomenti` avvia un protocollo o uno strumento su un nodo al tempo zero; `at TEMPO comando` esegue un comando in un momento successivo.

```
run pc ping srv count=3
at 5s link gw1 gw2 down
at 6s run pc ping srv count=2
at 9s link gw1 gw2 up
at 10s run pc ping srv count=2
duration 14s
```

`duration` dice quanto dura il laboratorio quando gira senza interfaccia. Nell'interfaccia continua a girare finché non esci.

Dopo `at` puoi scrivere tutto ciò che potresti digitare sulla riga di comando di un laboratorio in esecuzione: `run ...`, `link A B down`, `link A B loss=0.2`, `node N down`, `route ...`, `stop NODO MODULO`.

<!-- detail: 2 -->
I moduli e i loro argomenti sono elencati nella voce **Moduli** del menu e da `./hoplab modules` e `./hoplab help MODULO`. Una destinazione può essere il nome di un nodo (`srv`), un'interfaccia (`gw1:eth1`) o un indirizzo.

Gli eventi che dipendono dal caso (perdite, timer casuali) dipendono da un seme. Un'esecuzione avviata con `--seed N` va sempre allo stesso modo; senza, ogni esecuzione è diversa. Scrivere `seed 7` nel file fissa il seme per chiunque non passi `--seed`.

<!-- detail: 1 -->
## Passo 4: racconta la storia

Metti i metadati in cima al file, e spiega ciò che accade con delle note:

```
title  Two offices and a slow line
level  0
layer  3
about  A PC reaches a server across a slow line between two routers. At 5 s the line fails; at 9 s it is repaired.

note 0   The first ping waits for ARP on every hop, the next ones do not.
note 5s  The line is down: gw1 has no usable route and answers with an ICMP error.
note 9s  The line is back, and so are the replies.
```

`title` è il nome mostrato nel catalogo e `about` la descrizione (più righe `about` vengono unite in un solo paragrafo). `note TEMPO testo` è il commento: quando il laboratorio raggiunge quel tempo il testo compare in fondo alla mappa, e la vista Guida elenca tutte le note come una cronologia.

<!-- detail: 2 -->
`level` è il percorso (1 Rudimenti, 2 Fondamenti, 3 Avanzato, 4 Frontiera) e `layer` il livello dello stack (1 fisico, 2 collegamento, 3 rete, 4 trasporto, 7 applicazione, 8 distribuito): il catalogo li usa per raggruppare e filtrare. `topic PAROLA` aggiunge una parola chiave per la ricerca nel catalogo. Una riga `about` senza testo comincia un nuovo paragrafo.

Le righe che cominciano con `#` sono commenti.

<!-- detail: 1 -->
## Passo 5: i parametri

Un parametro trasforma un laboratorio in una famiglia di esperimenti. Dichiaralo con un nome, un valore predefinito e un'etichetta, poi usalo come `$nome` in qualunque punto del file:

```
param  loss 0 Loss on the slow line (0..1)
param+ delay 20ms One-way delay of the slow line

link gw1 gw2 bw=1M delay=$delay loss=$loss queue=20
```

Il modulo di avvio del catalogo mostra un campo per ogni parametro. `param+` dichiara un parametro **avanzato**, mostrato solo quando il modulo è impostato sulle impostazioni avanzate. Dalla riga di comando: `./hoplab run two-offices.nl loss=0.1`.

<!-- detail: 2 -->
Una riga `param` deve venire prima delle righe che usano il parametro. Funziona anche `${nome}`, per quando segue direttamente una lettera. Un valore dato sulla riga di comando prevale su quello predefinito.

<!-- detail: 1 -->
## Passo 6: le verifiche

Una verifica dice che cosa deve essere vero alla fine dell'esecuzione. Trasforma il laboratorio in un test di sé stesso:

```
expect pc ping.reply == 5
expect pc ping.error >= 1
```

`expect NODO CONTATORE OP VALORE` confronta un contatore di un nodo con un numero; gli operatori sono `==`, `!=`, `>=`, `<=`, `>` e `<`. I contatori sono tenuti dai moduli (`ping.reply`, `tcp.sink_bytes`, `ip.forwarded`...): per vedere quali esistono, esegui il laboratorio, seleziona il nodo e leggi la sezione **Contatori** della sua scheda Nodo al dettaglio 2.

<!-- detail: 2 -->
Le verifiche vengono controllate quando un'esecuzione senza interfaccia raggiunge `duration`, e stampate quando esci dall'interfaccia. Un contatore mai toccato vale 0. Scrivi le verifiche per i valori predefiniti dei parametri: con `loss=0.5` il laboratorio qui sopra giustamente fallisce.

<!-- detail: 1 -->
## Passo 7: esegui e controlla

```
./hoplab run two-offices.nl
./hoplab run two-offices.nl --headless --detail 1
./hoplab check two-offices.nl
```

Il primo comando apre l'interfaccia. Il secondo stampa il registro, poi le verifiche con i loro valori e `PASS` o `FAIL`. Il terzo fa lo stesso in silenzio con un seme fisso, per un file o per molti. Un errore nel file viene segnalato con il numero di riga:

```
two-offices.nl:7: unknown node or interface in 'link gw1 gw3'
```

Per vedere il tuo laboratorio nel catalogo, metti il file in `~/.local/share/hoplab/scenarios/`: compare sotto **I miei scenari**.

<!-- detail: 2 -->
## Le traduzioni

Un laboratorio può portare con sé le proprie traduzioni. Aggiungi il codice della lingua fra parentesi quadre a `title`, `about`, `note` e `param`:

```
title      Two offices and a slow line
title[it]  Due uffici e una linea lenta
about[it]  Un PC raggiunge un server attraverso una linea lenta tra due router.
note 5s    The line is down.
note[it] 5s La linea è guasta.
param      loss 0 Loss on the slow line (0..1)
param[it]  loss Perdita sulla linea lenta (0..1)
```

Quando hoplab gira in italiano le righe `[it]` sostituiscono quelle semplici; in qualunque altra lingua vengono ignorate. Una nota tradotta sostituisce la nota con lo stesso tempo, e `param[it]` dà soltanto il nome e l'etichetta tradotta: il valore predefinito resta nella riga `param` semplice, che deve venire prima.

<!-- detail: 3 -->
## Il file completo

```
title      Two offices and a slow line
title[it]  Due uffici e una linea lenta
level 0
layer 3
about      A PC reaches a server across a slow line between two routers. At 5 s the line fails; at 9 s it is repaired.
about[it]  Un PC raggiunge un server attraverso una linea lenta tra due router. A 5 s la linea si guasta; a 9 s viene riparata.
param      loss 0 Loss on the slow line (0..1)
param[it]  loss Perdita sulla linea lenta (0..1)
param+     delay 20ms One-way delay of the slow line
param[it]  delay Ritardo della linea lenta (una direzione)

node pc  host
node gw1 router
node gw2 router
node srv host role=server
link pc  gw1 bw=100M delay=1ms
link gw1 gw2 bw=1M delay=$delay loss=$loss queue=20
link gw2 srv bw=100M delay=1ms
auto all

run pc ping srv count=3
note 0     The first ping waits for ARP on every hop, the next ones do not.
note[it] 0 Il primo ping attende ARP a ogni salto, i successivi no.
at 5s link gw1 gw2 down
at 6s run pc ping srv count=2
note 5s    The line is down: gw1 has no usable route and answers with an ICMP error.
note[it] 5s La linea è guasta: gw1 non ha più una rotta utilizzabile e risponde con un errore ICMP.
at 9s link gw1 gw2 up
at 10s run pc ping srv count=2
note 9s    The line is back, and so are the replies.
note[it] 9s La linea è tornata, e con lei le risposte.
duration 14s
expect pc ping.reply == 5
expect pc ping.error >= 1
```

Cose utili da sapere quando un laboratorio cresce. Una riga `run` viene eseguita al tempo zero, dopo l'indirizzamento, nell'ordine del file; `at 0 ...` è la stessa cosa. In un file il tempo di `at` è assoluto, dall'inizio del laboratorio. `node N down` e `node N up` sono comandi per un laboratorio in esecuzione, perciò in un file richiedono `at`: `at 3s node gw2 down`. Un laboratorio può essere eseguito su più macchine aggiungendo righe `place` e `monitor`: vedi `docs/distributed.md`. E quando il protocollo che ti serve non esiste ancora, `docs/writing-modules.md` spiega come aggiungerne uno.
