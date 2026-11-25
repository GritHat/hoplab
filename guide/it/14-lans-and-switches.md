<!-- level: 1 -->
<!-- labs: 100-two-hosts, 170-switch-learning -->
# Reti locali, indirizzi MAC e switch

Una **rete locale** (LAN) è l'insieme dei dispositivi che possono raggiungersi direttamente, senza passare da un router: i computer di un laboratorio, i dispositivi del Wi-Fi di casa. Dentro una LAN i frame vengono consegnati usando gli **indirizzi MAC**, il numero scritto dal costruttore in ogni scheda di rete.

Ma i programmi conoscono indirizzi IP, non indirizzi MAC. Quindi, prima che il primo frame possa partire, chi invia deve fare una domanda. Immagina un'aula in cui conosci il nome di una compagna ma non dove è seduta. Dici ad alta voce: "Chi è Marta?". Tutti sentono; solo Marta risponde: "Sono io, qui." Da quel momento le parli direttamente. Questo è **ARP**: una domanda gridata a tutti (un **broadcast**), una risposta da uno solo, e un appunto conservato per la prossima volta (la **cache ARP**).

La scatola che unisce i cavi di una LAN è uno **switch**. All'inizio non sa nulla, e impara osservando: ogni frame che entra mostra, con il suo indirizzo sorgente, dove si trova quella stazione. Un frame per una stazione che lo switch conosce già esce da una sola porta; un frame per una stazione sconosciuta, o per tutti, esce da tutte le altre porte.

<!-- detail: 2 -->

## Come funziona

Un **indirizzo MAC** è di 48 bit, scritto come sei byte in esadecimale: `02:00:4e:4c:01:00`. L'indirizzo speciale `ff:ff:ff:ff:ff:ff` è l'indirizzo di **broadcast**: ogni stazione della LAN accetta un frame così indirizzato. Un frame Ethernet comincia con il MAC di destinazione, il MAC sorgente e un campo **tipo** di 2 byte che dice che cosa c'è dentro (`0x0800` un pacchetto IP, `0x0806` ARP).

**ARP passo per passo**, come nel laboratorio 100. h1 (`10.0.0.1`) vuole inviare un pacchetto IP a `10.0.0.2`.

- h1 guarda nella sua cache ARP: niente.
- h1 invia in broadcast una **richiesta ARP**: "chi ha 10.0.0.2? ditelo a 10.0.0.1". Il pacchetto aspetta.
- h2 riconosce il proprio indirizzo IP. Per prima cosa annota gli indirizzi di h1 nella propria cache (erano nella richiesta), poi invia una **risposta ARP** direttamente a h1: "10.0.0.2 è a 02:00:4e:4c:02:00".
- h1 memorizza la coppia nella cache e finalmente invia il pacchetto in attesa.

Questo costa un'andata e ritorno in più, una volta sola: nel laboratorio 100 il primo ping impiega 80 ms, il secondo 40 ms.

**Uno switch che impara** tiene una tabella "indirizzo MAC → porta". Per ogni frame fa due cose.

- **Impara**: annota (MAC sorgente, porta da cui il frame è entrato).
- **Inoltra**: cerca il MAC di destinazione. Trovato: invia il frame solo da quella porta. Non trovato, o broadcast: **inonda** (flooding), cioè ne invia una copia da ogni porta tranne quella di arrivo.

Nel laboratorio 170 la richiesta ARP di h1 entra in s1 dalla porta p0. s1 impara "h1 è su p0" e inonda; s2 impara "h1 è su p0" (la sua porta verso s1) e inonda verso h3. La risposta di h3 torna indietro lungo un solo percorso, perché entrambi gli switch ora sanno dov'è h1, e lungo la strada imparano dov'è h3. h2 ha ricevuto il broadcast ma non vede mai la risposta né i ping.

Uno switch non modifica il frame e in questo scambio non ha un indirizzo proprio: gli host non sanno che c'è.

<!-- detail: 3 -->

## Per approfondire

**Perché due indirizzi?** Un indirizzo MAC è piatto: dice chi sei, non dove sei, e funziona solo fin dove arriva un broadcast. Un indirizzo IP ha una struttura (parte di rete e parte di host) e può essere instradato attraverso il mondo. ARP è la colla fra i due, usata solo sull'ultimo salto. Quando la destinazione è su un'altra rete, l'host chiede ad ARP il MAC del suo **router**, non della destinazione (argomento 15).

**Le tabelle dimenticano.** Le voci della tabella dello switch e della cache ARP scadono (tipicamente dopo 300 s negli switch, meno per ARP), così una stazione che si sposta su un'altra porta viene ritrovata. Una tabella piena o vuota costa solo efficienza: lo switch semplicemente inonda.

**Limiti dello schema.** Il flooding presuppone che fra due stazioni qualsiasi esista esattamente un percorso. Se gli switch sono cablati ad anello, un broadcast circola per sempre e si moltiplica: una **tempesta di broadcast** (laboratorio 270). Il protocollo spanning tree disattiva alcune porte per spezzare gli anelli (laboratorio 271). E poiché ogni broadcast raggiunge ogni stazione, una LAN non può crescere senza limite: le reti grandi la dividono in **VLAN** (laboratorio 203) unite da router.

**Nessuna sicurezza.** ARP crede a qualunque risposta, e uno switch crede a qualunque indirizzo sorgente. Una macchina può rispondere alle richieste ARP al posto di un'altra (ARP spoofing) e riceverne il traffico. Le reti reali aggiungono protezioni negli switch.

**Storia e vocabolario.** Prima degli switch c'erano gli **hub**, che ripetevano ogni bit su ogni porta: tutte le stazioni condividevano un unico canale e i frame potevano collidere (argomento 18). Uno switch dà a ogni stazione un collegamento tutto suo e conserva i frame in code, così le collisioni sono sparite. Il primo byte `02` degli indirizzi di hoplab indica un indirizzo assegnato localmente; le schede reali portano un prefisso del costruttore. IPv6 sostituisce ARP con il Neighbor Discovery, basato su ICMPv6.

Per saperne di più: Kurose e Ross, capitolo 6, paragrafo "Reti locali commutate"; Tanenbaum, capitolo 4, sulla commutazione a livello di collegamento; RFC 826 (ARP); IEEE 802.1D per i bridge.

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 100-two-hosts**: ARP fra due host.

- Avvialo con `./hoplab run 100-two-hosts` e apri la vista Pacchetti (`p`).
- Il primo frame è la richiesta ARP. Premi Invio: controlla il MAC di destinazione `ff:ff:ff:ff:ff:ff` e la domanda al suo interno. Apri la risposta: a chi è indirizzata?
- Seleziona h1 (`n`) e apri la scheda ARP: dopo 40 ms compare una riga.
- A 3 s h2 fa ping verso h1 e non invia alcuna richiesta ARP. Perché? (Imposta il dettaglio 3 e cerca `ARP cache` nel registro.)
- Scrivi `:` e poi `link h1 h2 down`, poi `h1 ping h2`. Che cosa dice il registro?

**Laboratorio 170-switch-learning**: tre host e due switch.

- Avvialo con `./hoplab run 170-switch-learning` e premi `3` per il dettaglio 3, così viene mostrata ogni riga `bridge appreso`.
- Segui sulla mappa la prima richiesta ARP: raggiunge h2 e h3. Poi la risposta: va solo verso h1.
- Seleziona s1 e apri la scheda Tabella MAC. Dopo il primo ping ha due righe; nota su quale porta sta ciascun indirizzo. Dopo 5 s, quando h2 fa ping verso h1, compare una terza riga.
- Seleziona h2 e cerca il contatore `eth.not_for_me` nella scheda Nodo: vale 0 o è assente, perché nessun frame indirizzato a qualcun altro gli viene mai consegnato.

**Cambia e prevedi.**

- Dopo 7 s scrivi `h3 ping h2`. Quali frame verranno inondati e quali seguiranno un solo percorso? Controlla nel registro.
- Esegui `./hoplab run 170-switch-learning delay=20ms`. Un frame da h1 a h3 attraversa tre collegamenti. Che tempo di ping ti aspetti una volta concluso ARP?

## Verifica

- Perché la richiesta ARP viene inviata in broadcast e la risposta ARP no?
- Uno switch è stato appena acceso e riceve un frame da A a B. Che cosa impara, e dove invia il frame?
- Nel laboratorio 170, perché h2 non vede mai i ping fra h1 e h3?
- Nel laboratorio 100 h2 ha risposto alla richiesta ARP di h1. Perché in seguito h2 può fare ping verso h1 senza chiedere?
- Che cosa succede a un frame di broadcast se due switch sono collegati da due cavi?
