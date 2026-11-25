<!-- level: 2 -->
<!-- labs: 203-vlans, 270-broadcast-storm, 271-spanning-tree -->
# VLAN e spanning tree

Una rete Ethernet commutata è un unico **dominio di broadcast**: un frame inviato all'indirizzo di broadcast, o a un indirizzo che nessuno switch ha ancora appreso, raggiunge tutte le porte. Al crescere della rete due cose vanno storte.

Primo: tutti stanno nella stessa rete. Reparti che dovrebbero essere separati vedono i broadcast gli uni degli altri, e un host che si comporta male disturba tutti. Le **VLAN** dividono un unico insieme fisico di switch in più LAN logiche. Ogni porta appartiene a una VLAN; un frame viene consegnato solo alle porte della stessa VLAN; un collegamento fra switch (un **trunk**) trasporta più VLAN aggiungendo a ogni frame una piccola etichetta (tag). Per passare da una VLAN all'altra serve un router, esattamente come fra due LAN separate.

Secondo: una rete senza collegamenti ridondanti si interrompe al primo cavo guasto, ma una rete *con* collegamenti ridondanti contiene cicli, e i frame Ethernet non hanno TTL. Un solo broadcast in un ciclo viene copiato all'infinito: una **tempesta di broadcast** (broadcast storm). Lo **spanning tree protocol** (STP) permette agli switch di accordarsi su un sottoinsieme dei collegamenti privo di cicli, di bloccare gli altri e di sbloccarli quando l'albero si spezza.

<!-- detail: 2 -->
## Il tag 802.1Q

Una porta di accesso appartiene a una sola VLAN e scambia normali frame senza tag con un host che delle VLAN non sa nulla. Su un trunk lo switch inserisce un tag di 4 byte fra l'indirizzo MAC sorgente e l'EtherType:

- **TPID** (16 bit) = 0x8100: dice «segue un tag».
- **PCP** (3 bit): classe di priorità; **DEI** (1 bit): scartabile.
- **VID** (12 bit): l'identificatore della VLAN, da 1 a 4094.

Il frame massimo passa da 1518 a 1522 byte. Lo switch che riceve legge il VID, toglie il tag e inoltra solo verso le porte di quella VLAN. La tabella degli indirizzi MAC è tenuta per VLAN: una riga è (VLAN, MAC) → porta.

Nel laboratorio 203 gli host a1 e a2 sono nella VLAN 10 e b1, b2 nella VLAN 20, su due switch uniti da un trunk. Tutti e quattro hanno indirizzi in 10.0.0.0/24. a1 esegue un ping verso a2 attraverso il trunk; ma quando a1 prova con b2 la sua richiesta ARP viene inondata solo nella VLAN 10, b2 non la vede mai e ARP fallisce. Stessa sottorete IP, stessi cavi, eppure nessuna connettività a livello 2.

## Perché i cicli sono fatali

Nel laboratorio 270 tre switch formano un triangolo senza STP. Una sola richiesta ARP di h1 viene inondata da s1 verso s2 e s3; ciascuno la inonda verso l'altro; entrambe le copie tornano a s1, che le inonda di nuovo. Due copie circolano per sempre, in senso orario e antiorario. A ogni giro ogni host riceve una copia, e ogni switch vede l'indirizzo di h1 arrivare da porte diverse, per cui la sua tabella continua a cambiare. In nove secondi ogni switch inonda circa 1200 frame, tutti nati da uno solo.

## L'algoritmo dello spanning tree

Ogni bridge ha un identificatore: una **priorità** di 16 bit (32768 per default) seguita dal suo indirizzo MAC. I bridge si scambiano **BPDU** di configurazione, inviate all'indirizzo multicast 01:80:C2:00:00:00, che contengono: identificatore della radice, costo del cammino verso la radice, identificatore del bridge mittente, identificatore della porta mittente e i timer (message age, max age, hello time, forward delay). Una BPDU è *migliore* di un'altra se ha un identificatore di radice più basso; a parità, un costo più basso; poi un identificatore del mittente più basso; poi un identificatore di porta più basso. Seguono tre scelte:

- **Bridge radice** (root bridge): il bridge con l'identificatore più basso. Tutti partono dichiarandosi radice e rinunciano quando sentono una BPDU migliore.
- **Porta radice** (root port): su ogni altro bridge, la porta che offre il cammino meno costoso verso la radice (costo ricevuto più costo della porta).
- **Porta designata** (designated port): su ogni collegamento, la porta del bridge che offre il cammino migliore verso la radice. Tutte le porte della radice sono designate.

Ogni altra porta è **bloccata**: ascolta le BPDU ma non inoltra dati e non apprende indirizzi.

Esempio svolto (laboratorio 271, tutti i collegamenti costano 100): s1 ha il MAC più basso ed è la radice. s2 e s3 la raggiungono direttamente: porte radice con costo 100. Sul collegamento s2-s3 entrambi annunciano costo 100; la parità è risolta dall'identificatore del mittente, s2 vince e tiene una porta designata, s3 blocca la sua. Il triangolo è diventato un albero.

## Stati e timer

Una porta che deve inoltrare attraversa **blocking → listening → learning → forwarding**, restando un *forward delay* in listening (niente dati, niente apprendimento) e uno in learning (apprende, niente dati). La radice invia una BPDU ogni *hello time*; l'informazione memorizzata scade dopo *max age*. I valori standard sono 2 s, 20 s e 15 s; hoplab usa 1 s, 6 s e 3 s.

Quando a 18 s il collegamento s1-s2 si guasta, s2 perde la sua porta radice. s3 trasforma la sua porta bloccata in designata, che arriva a forwarding dopo 2 × 3 s: l'interruzione dura due forward delay (30 s con i timer standard). Un guasto non visibile direttamente, come la caduta della radice, viene rilevato solo allo scadere di max age: fino a 20 + 30 = 50 s.

<!-- detail: 3 -->
## In profondità

**Cambi di topologia.** Dopo un cambiamento dell'albero le stazioni possono essere raggiungibili da porte diverse, e le righe obsolete delle tabelle farebbero sparire il traffico per i 300 s del normale invecchiamento. Un bridge che rileva un cambiamento invia una notifica di cambio di topologia verso la radice; la radice imposta un flag nelle sue BPDU e tutti i bridge accorciano il tempo di invecchiamento al forward delay. hoplab svuota semplicemente le tabelle, che è ciò che fa RSTP.

**Rapid spanning tree** (802.1w, confluito in 802.1D-2004) sostituisce i timer con un handshake proposta/accordo sui collegamenti punto-punto, definisce i ruoli di porta alternate e backup in modo che una porta radice di riserva subentri all'istante, e tratta le porte verso gli host come porte edge. La convergenza richiede ben meno di un secondo. `rapid=1` di hoplab lo realizza: BPDU RST con i flag proposal e agreement, il sync del bridge che accetta, porte alternate che diventano subito porte radice, porte edge automatiche (nessun BPDU per tre hello), e informazioni scartate dopo tre hello mancanti invece di max age. Non sono modellate le porte backup né le regole proprie di 802.1w per i cambi di topologia. `portfast=1` rende edge fin dall'inizio le porte verso gli host.

**Scegliere la radice.** Con le priorità di default la radice è il bridge con il MAC più basso, di solito il più vecchio. Gli operatori impostano esplicitamente la priorità della radice voluta (e di una di riserva) e proteggono il progetto con root guard e BPDU guard sulle porte di accesso. Il costo delle porte orienta la scelta dei collegamenti: i valori classici sono 100 per 10 Mbit/s, 19 per 100 Mbit/s e 4 per 1 Gbit/s; hoplab calcola 10⁹ / banda, che dà 100 per i suoi collegamenti di default a 10 Mbit/s.

**Modi di guasto.** STP fallisce «in apertura»: una porta che smette di *ricevere* BPDU (una fibra unidirezionale, una CPU sovraccarica) comincia a inoltrare e crea proprio il ciclo che doveva impedire. Loop guard e UDLD esistono per questo.

**VLAN e STP.** Un solo albero per tutte le VLAN spreca i collegamenti bloccati; alberi per VLAN (PVST+) o il Multiple Spanning Tree (802.1s) permettono a VLAN diverse di bloccare collegamenti diversi. La **VLAN nativa** del trunk viaggia senza tag, il che rende possibile l'attacco di VLAN hopping con doppio tag; la buona pratica è etichettare tutto. Il VID a 12 bit è troppo piccolo per i data center del cloud: 802.1ad impila due tag, e VXLAN (RFC 7348) trasporta su UDP un identificatore a 24 bit.

**Riferimenti.** Radia Perlman, «An Algorithm for Distributed Computation of a Spanning Tree in an Extended LAN» (1985); IEEE 802.1D e 802.1Q. I grandi data center oggi preferiscono fabric instradate con ECMP, oppure TRILL / Shortest Path Bridging (802.1aq), che usano tutti i collegamenti.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 203: `./hoplab run 203-vlans`**

- Nella vista Pacchetti (`p`) seleziona un frame sul collegamento s1-s2 e premi Invio: trova il tag 802.1Q e il suo VID. Apri lo stesso frame sul collegamento a1-s1: nessun tag.
- Seleziona s1 (`n`) e leggi la scheda Tabella MAC: le righe hanno una colonna VLAN.
- A 6 s a1 cerca b2: tre richieste ARP, poi `ARP nessuna risposta`. Segui sulla mappa dove va il broadcast.
- Prevedi, poi prova: `:b1 ping a2 count=1`, e `:link s1 s2 down` seguito da `:a1 ping a2 count=1`. Quali falliscono, e a quale livello?

**Laboratorio 270: `./hoplab run 270-broadcast-storm`**

- Dopo 1 s la mappa non si calma più. Seleziona uno switch e guarda crescere `bridge.flood`; a dettaglio 3 la porta di h1 nella tabella continua a cambiare.
- Prevedi, poi prova: `:link s2 s3 down`. In quanto tempo si ferma la tempesta? Poi `:link s2 s3 up`: riparte da sola?
- Prevedi, poi prova: `./hoplab run 270-broadcast-storm delay=1ms`. Di che fattore cresce il contatore delle inondazioni?

**Laboratorio 271: `./hoplab run 271-spanning-tree`**

- Seleziona ogni switch e apri la sua scheda `stp`: ruolo e stato di ogni porta. Segui blocking, listening (0 s), learning (3 s), forwarding (6 s). s3 blocca la porta p0.
- A 14 s h2 fa ping verso h1, poi fra i due partono due flussi costanti (50 datagrammi al secondo per verso). A 18 s il collegamento s1-s2 si guasta. La p0 di s3 diventa designata; la nuova porta radice di s2 ha costo 200. L'inoltro riprende a 24 s: due forward delay. Il laboratorio conta i datagrammi di h2 mai arrivati a h1 (le ultime righe del registro, `check outage`): circa 300, cioè 6 s.
- Prevedi, poi prova: `./hoplab run 271-spanning-tree rapid=1`. A dettaglio 2 segui lo scambio nel registro: s3 propone su p0, s2 risponde con un agreement, s3 inoltra. Quanti datagrammi si perdono ora? (Uno o nessuno: l'interruzione è il tempo che due BPDU impiegano ad attraversare un collegamento.) Apri un BPDU nella vista Pacchetti e leggi i flag: ruolo, proposal, agreement. Perché all'inizio le porte verso gli host hanno aspettato 3 s, e che cosa cambia con `portfast=1`?
- Prevedi, poi prova: riavvia, e intorno ai 22 s digita `:node s1 down`. Chi diventa radice, e quanto dura l'interruzione rispetto al guasto del collegamento? Mettila in relazione con `maxage` e `fwd`.

## Verifica

- Disegna un frame Ethernet con tag 802.1Q e indica dimensione e significato di ogni campo del tag. Dove viene aggiunto il tag e dove viene tolto?
- Due host sono nella stessa sottorete IP ma in VLAN diverse. Descrivi esattamente che cosa succede quando uno esegue un ping verso l'altro.
- Quattro bridge con identificatori B1 < B2 < B3 < B4 sono collegati ad anello B1-B2-B3-B4-B1 con costi uguali. Qual è la radice, quali sono le porte radice e quale porta viene bloccata?
- Perché una porta attende in listening e learning prima di inoltrare? Che cosa potrebbe succedere se inoltrasse subito?
- Con i timer standard, per quanto tempo si può perdere la connettività dopo un guasto che i bridge non vedono direttamente, e perché più a lungo che dopo il guasto di un collegamento?
