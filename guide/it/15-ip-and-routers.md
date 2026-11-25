<!-- level: 1 -->
<!-- labs: 101-ping -->
# Indirizzi IP e router

Una rete locale arriva fin dove arrivano i suoi cavi e i suoi switch. Internet è fatta di milioni di reti così, unite fra loro, e il dispositivo che unisce due reti è un **router**. Il protocollo che permette a un pacchetto di attraversarle tutte è **IP**, l'Internet Protocol.

Ogni interfaccia ha un **indirizzo IP**, e gli indirizzi sono organizzati come i numeri di telefono: la prima parte (il **prefisso**) identifica la rete, come un prefisso telefonico identifica una città, e il resto identifica una macchina al suo interno. È questo che rende possibile una rete mondiale: un router lontano non deve conoscere ogni macchina, ma solo in quale direzione si trova ciascuna rete.

Un host segue una regola semplice. La destinazione è nella mia stessa rete? Allora consegno direttamente. Altrimenti passo il pacchetto al mio **gateway predefinito**, il router della mia rete, e ci pensa lui. Ogni router ripete lo stesso ragionamento con la propria **tabella di instradamento**, un elenco di "per questa rete, vai di là", e fa avanzare il pacchetto di un passo. È l'**inoltro** (forwarding), salto dopo salto, come una lettera che passa da un centro di smistamento al successivo.

Due strumenti di uso quotidiano permettono di vedere tutto questo: **ping** chiede "ci sei, e quanto sei lontano?", **traceroute** chiede "quali router ci sono lungo la strada?".

<!-- detail: 2 -->

## Come funziona

Un indirizzo IPv4 è di 32 bit, scritto come quattro numeri decimali: `10.0.1.10`. Un **prefisso** come `10.0.1.0/24` significa "i primi 24 bit sono la rete": tutti gli indirizzi da `10.0.1.0` a `10.0.1.255`. La stessa informazione si può scrivere come **maschera** `255.255.255.0`.

Nel laboratorio 101, h1 è `10.0.1.10/24` e h2 è `10.0.2.10/24`. I primi 24 bit sono diversi (`10.0.1` contro `10.0.2`), quindi stanno su reti diverse. Il router r1 ha un'interfaccia su ciascuna: `10.0.1.1` e `10.0.2.1`.

**Che cosa fa h1** per inviare a `10.0.2.10`:

- Confronta la destinazione con il proprio prefisso: non è la mia rete.
- Prende la **rotta predefinita** (scritta `0.0.0.0/0`, "tutto il resto"), che punta al gateway `10.0.1.1`.
- Usa ARP per trovare l'indirizzo MAC **del gateway** e gli invia il frame. La destinazione IP al suo interno è sempre `10.0.2.10`.

**Che cosa fa r1**: cerca la destinazione nella sua tabella di instradamento, scegliendo il prefisso corrispondente più specifico; trova che `10.0.2.0/24` è direttamente connessa; decrementa il **TTL**; usa ARP per h2 e invia un nuovo frame.

Il **TTL** (time to live, tempo di vita) è un contatore nell'intestazione IP. Il mittente lo imposta (qui a 64) e ogni router sottrae uno. A zero il pacchetto viene scartato. Questo garantisce che un pacchetto finito in un ciclo non circoli per sempre. Nel laboratorio 101 la risposta di h2 arriva con `ttl=63`: è stato attraversato un router.

**ICMP** è il protocollo che IP usa per i propri messaggi. `ping` invia una **echo request** ICMP e la destinazione risponde con una **echo reply**; il tempo fra le due è il tempo di andata e ritorno. Quando un router scarta un pacchetto rimanda indietro un errore ICMP: **time exceeded** (il TTL è arrivato a zero) o **destination unreachable** (nessuna rotta).

**traceroute** trasforma il TTL in uno strumento. Invia una sonda con TTL 1: il primo router la scarta e rivela il proprio indirizzo nel messaggio "time exceeded". Poi TTL 2, 3, … finché risponde la destinazione stessa.

<!-- detail: 3 -->

## Per approfondire

**Corrispondenza a prefisso più lungo** (longest prefix match). Una tabella di instradamento può contenere più voci che corrispondono a una stessa destinazione, per esempio `10.0.0.0/8`, `10.0.2.0/24` e la predefinita `0.0.0.0/0`. Il router sceglie sempre il prefisso più lungo, il più specifico. Così una sola riga riassume migliaia di reti e le eccezioni restano possibili. Chi riempie le tabelle? A mano nelle reti piccole (laboratorio 200), con i protocolli di instradamento altrove (RIP, OSPF, BGP: laboratori 230, 232, 331).

**IP promette poco.** È **best effort** e **senza connessione**: ogni pacchetto è trattato da solo, può andare perso, e due pacchetti possono seguire percorsi diversi. Un router non conserva memoria delle conversazioni. L'affidabilità la aggiungono gli host (argomento 16).

**L'intestazione IP** è di 20 byte. Oltre a indirizzi e TTL porta la lunghezza totale, un numero di protocollo che dice che cosa c'è dentro (1 ICMP, 6 TCP, 17 UDP), un checksum dell'intestazione che ogni router deve ricalcolare perché il TTL cambia, e i campi per la **frammentazione**. Un pacchetto più grande della MTU del collegamento successivo viene tagliato in frammenti che sono riassemblati solo a destinazione (la fine del laboratorio 101, e il laboratorio 201).

**Limiti degli strumenti.** Ping misura l'andata e ritorno, non ciascuna direzione. Traceroute mostra solo i router del percorso di andata, ciascuno identificato dall'interfaccia da cui è entrata la sonda; i percorsi possono cambiare fra una sonda e l'altra; molti router limitano o rifiutano le risposte ICMP, quindi una risposta mancante non dimostra un guasto. Il traceroute reale su Unix invia sonde UDP verso porte improbabili, mentre qui e su Windows usa echo ICMP.

**Indirizzi insufficienti.** 32 bit danno circa 4 miliardi di indirizzi, meno dei dispositivi in uso. Convivono due rimedi: gli indirizzi privati riutilizzati dietro un **NAT** (laboratorio 250), e **IPv6** con indirizzi a 128 bit. I prefissi di lunghezza qualsiasi (CIDR) hanno sostituito nel 1993 le vecchie classi fisse A, B e C.

Per saperne di più: Kurose e Ross, capitolo 4, paragrafo "Il protocollo Internet (IP): IPv4, indirizzamento, IPv6 e altro"; Tanenbaum, capitolo 5, "Il livello di rete in Internet"; RFC 791 (IP), RFC 792 (ICMP), RFC 4632 (CIDR).

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 101-ping**: h1, il router r1 e h2, su due reti.

- Avvialo con `./hoplab run 101-ping`.
- Seleziona h1 con `n` e apri la scheda Rotte: una riga per la sua rete, una rotta predefinita via `10.0.1.1`. Seleziona r1: una riga per ciascuna delle due reti a cui è connesso.
- 0 s, ping. Nel Registro (`l`) il primo scambio mostra due domande ARP: h1 chiede del gateway, r1 chiede di h2. Nessuno chiede a h1 il MAC di h2: perché? Le risposte mostrano `ttl=63`.
- Apri la vista Sequenza (`s`) per vedere ogni pacchetto fermarsi su r1 e ripartire.
- 5 s, traceroute. Osserva `IP TTL scaduto` su r1, il messaggio `time exceeded` e le due righe del risultato: il salto 1 è `10.0.1.1`, il salto 2 è h2.
- 8 s, un datagramma da 3000 byte viene frammentato in tre pacchetti (vedi "Per approfondire").

**Cambia e prevedi.** Scrivi `:` per aprire la riga di comando.

- `h1 ping h2 ttl=1`: chi risponderà, e con quale messaggio?
- `h1 ping 10.0.9.9`: nessuna rete ha questo indirizzo. Quale nodo se ne accorgerà, e che cosa dirà a h1?
- `link r1 h2 down` e poi `h1 ping h2`: h1 ha ancora la sua rotta predefinita. Che cosa risponde ora r1? Ripristina il collegamento con `link r1 h2 up`.
- Esegui `./hoplab run 101-ping loss=0.3`: la perdita agisce sul collegamento h1–r1 in entrambe le direzioni. Quale frazione dei ping dovrebbe riuscire?

## Verifica

- `10.0.1.200` e `10.0.2.3` sono nella stessa rete /24? E nella stessa /16?
- h1 invia un pacchetto a h2 attraverso r1. Quale indirizzo MAC di destinazione e quale indirizzo IP di destinazione porta il frame sul primo collegamento?
- Perché un pacchetto arriva con un TTL più basso di quello con cui è partito, e a che cosa serve il TTL?
- Come fa traceroute a scoprire un router che non è la destinazione di nessuno dei suoi pacchetti?
- Un host ha l'indirizzo giusto ma nessun gateway predefinito. Che cosa può raggiungere, e che cosa no?
