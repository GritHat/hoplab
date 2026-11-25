<!-- level: 2 -->
<!-- labs: 240-dns-hierarchy, 241-dhcp-leases -->
# DNS e DHCP in dettaglio

Due servizi rendono una rete utilizzabile dalle persone, ed entrambi sono basi di dati consultate via UDP.

Il **DNS** associa ai nomi dei dati, anzitutto indirizzi. Nessun server potrebbe contenere tutti i nomi né rispondere per tutti, quindi lo spazio dei nomi è un albero suddiviso in **zone**, ciascuna gestita dal proprio titolare e servita dai propri server **autoritativi**. Una zona affida un sottoalbero a qualcun altro con una **delega**: pubblica quali server ne sono responsabili. La radice sa chi serve `com`; i server di `com` sanno chi serve `example.com`. Per risolvere un nome si parte dalla radice e si seguono le deleghe verso il basso. Gli host non lo fanno da sé: chiedono a un **resolver ricorsivo**, che percorre l'albero per loro e mette in **cache** ogni risposta per il tempo concesso dal titolare del dato, il **TTL**. È la cache a rendere veloce il sistema e a evitare che la radice venga sommersa.

**DHCP** dà a un host la sua configurazione quando entra in una rete: indirizzo, maschera, gateway di default, resolver DNS. L'indirizzo non viene regalato ma concesso in **lease** (affitto) per un tempo limitato. Il client deve rinnovarlo periodicamente; un indirizzo il cui lease scade torna nel pool. I lease permettono a un pool piccolo di servire molti host di passaggio, e quando il pool è vuoto un nuovo host semplicemente non ottiene nulla.

<!-- detail: 2 -->
## Messaggi e record DNS

Domande e risposte hanno un unico formato, normalmente in un datagramma UDP verso la porta 53. Un'intestazione di 12 byte contiene un **identificatore** di 16 bit (copiato nella risposta), i flag e quattro contatori per le sezioni che seguono: question, answer, authority, additional. Flag importanti: QR (risposta), AA (risposta autoritativa), RD (ricorsione richiesta), RA (ricorsione disponibile), TC (troncato), e il codice di risposta a 4 bit (0 = nessun errore, 3 = NXDOMAIN).

Un resource record è (nome, tipo, classe, TTL, dati). Tipi comuni:

- **A** / **AAAA**: indirizzo IPv4 / IPv6 del nome
- **NS**: un name server della zona; **SOA**: parametri della zona
- **CNAME**: il nome è un alias di un altro nome
- **MX**: server di posta con una preferenza; **TXT**: testo libero; **PTR**: risoluzione inversa

## Risoluzione iterativa e ricorsiva

Lo stub resolver di un host invia una sola domanda con RD = 1 al proprio resolver: una query **ricorsiva**, «dammi la risposta finale». Il resolver invia poi query **iterative**: ogni server risponde con ciò che sa, che di solito è un **referral** (rinvio): nessuna risposta, record NS nella sezione authority e gli indirizzi di quei server (**glue**) nella sezione additional.

Laboratorio 240, risoluzione di `www.example.com`:

- resolver → radice: rinvio «`com` è servito da a.gtld» più glue
- resolver → server di com: rinvio «`example.com` è servito da ns.example.com» più glue
- resolver → server autoritativo: risposta `A 10.9.9.9`, TTL 60, AA impostato

Tre query a monte e circa 240 ms. La stessa domanda tre secondi dopo riceve risposta dalla cache in 4 ms con zero query a monte, e il TTL mostrato è 58 s: una cache distribuisce la vita *residua*. Anche i record NS sono finiti in cache, con il loro TTL di 3600 s, quindi una domanda successiva per un altro nome di `example.com` richiede una sola query a monte. Vengono messe in cache anche le risposte negative, per un tempo ricavato dal record SOA della zona (30 s nel laboratorio).

## Lo scambio DHCP

DHCP funziona su UDP, porta 67 per il server e 68 per il client. Il client non ha indirizzo, quindi trasmette da 0.0.0.0 a 255.255.255.255. I messaggi sono frame BOOTP (operazione, identificatore di transazione **xid**, indirizzo del client, **yiaddr** = «il tuo indirizzo», indirizzo hardware del client) seguiti da opzioni; l'opzione 53 è il tipo di messaggio.

- **DISCOVER** (client, broadcast): c'è un server?
- **OFFER** (server): propone yiaddr con maschera (opzione 1), router (3), DNS (6), durata del lease (51), identificatore del server (54)
- **REQUEST** (client, broadcast): «accetto l'offerta del server S»; in broadcast perché gli altri server ritirino la propria
- **ACK** (server): il lease comincia. NAK se l'indirizzo non è più disponibile.

## I timer del lease

Con un lease di L secondi il client imposta **T1 = L/2** e **T2 = 7L/8**. A T1 invia una REQUEST in unicast al proprio server (*renewing*); un ACK fa ripartire tutti i timer. Se entro T2 non arriva nulla, invia la REQUEST in broadcast a qualsiasi server (*rebinding*). A L deve smettere di usare l'indirizzo e ricominciare da DISCOVER. RELEASE restituisce l'indirizzo in anticipo.

Nel laboratorio 241 L = 10 s: h1 ottiene l'indirizzo a 1,01 s e rinnova a 6,01 s, 11,02 s e così via, ogni 5 s. Il pool ha due indirizzi: li prendono h1 e h2, e alla DISCOVER di h3 non risponde nessuno (`pool esaurito, nessuna offerta`); h3 riprova ogni 2 s. Se il server muore dopo un rinnovo all'istante t, i client provano a t + 5 s (T1), vanno in broadcast a t + 8 s (T2, netlab arrotonda per difetto ai secondi interi) e perdono l'indirizzo a t + 10 s.

<!-- detail: 3 -->
## In profondità: DNS

**Perché UDP, e che cosa comporta.** Un datagramma per verso rende le query economiche, ma un resolver accetta qualsiasi risposta che corrisponda per indirizzo sorgente, porta, identificatore e domanda. Un attaccante fuori dal percorso che li indovina può iniettare un record falso nella cache (Kaminsky, 2008); per questo i resolver rendono casuali sia l'identificatore sia la porta sorgente (RFC 5452). DNSSEC (RFC 4033-4035) firma i record in modo che le risposte siano verificabili. Le risposte oltre i 512 byte richiedono EDNS (RFC 6891) o un nuovo tentativo su TCP (RFC 7766).

**Comportamento della cache.** Il TTL è un compromesso scelto dal titolare della zona: valori lunghi risparmiano query e sopravvivono ai guasti dei server, valori brevi fanno entrare in vigore le modifiche in fretta. Una modifica non è mai istantanea: i vecchi dati vivono nelle cache finché non scadono. La cache negativa è definita nell'RFC 2308. I resolver danno fiducia ai dati in base alla loro provenienza (RFC 2181): il glue basta solo per proseguire il percorso. Una delega che punta a un server che non serve la zona è una *lame delegation*, causa frequente di risoluzioni lente.

**Il sistema reale.** Esistono 13 nomi di root server, ciascuno un gruppo anycast di centinaia di macchine. I resolver misurano i server e preferiscono i più rapidi, e la query minimisation (RFC 9156) rivela a ogni server solo le etichette che gli servono. DNS su TLS (RFC 7858) e su HTTPS (RFC 8484) nascondono le query fra stub e resolver. Riferimento: RFC 1034 e RFC 1035 (Mockapetris, 1987).

**netlab semplifica:** solo la classe IN; niente TCP, EDNS o DNSSEC; le deleghe devono avere il glue.

## In profondità: DHCP

**Relay.** I broadcast non attraversano i router. Un relay agent in ogni sottorete inoltra le DISCOVER in unicast a un server centrale e scrive il proprio indirizzo nel campo *giaddr*, che dice al server quale pool usare. Il server di netlab serve solo la propria sottorete.

**Stato.** Un server reale salva i lease su disco, così un riavvio non distribuisce indirizzi ancora in uso, e molti sondano un indirizzo (ICMP echo) prima di offrirlo; i client verificano con ARP (RFC 5227) e rispondono DECLINE in caso di conflitto. Un client che si riavvia chiede direttamente il suo indirizzo precedente (INIT-REBOOT). Il server di netlab tiene i lease solo in memoria.

**Sicurezza.** DHCP non ha autenticazione. Un server abusivo può rispondere più in fretta di quello vero e farsi gateway e DNS delle vittime; un client che falsifica molti indirizzi hardware può esaurire il pool (starvation). Gli switch si difendono con il DHCP snooping: i messaggi da server sono accettati solo dalle porte fidate.

**La durata del lease** è l'analogo del TTL del DNS: lease brevi riciclano in fretta gli indirizzi in un bar, al prezzo di più traffico e di dipendenza dal server; lease lunghi si adattano alle reti stabili. Riferimenti: RFC 2131, RFC 2132 (opzioni), RFC 951 (BOOTP), RFC 8415 (DHCPv6); gli host IPv6 possono anche configurarsi senza server (SLAAC, RFC 4862).

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 240: `./netlab run 240-dns-hierarchy`**

- A 1 s segui il registro a dettaglio 2: il resolver r interroga a turno root, com, auth. Sulla mappa le query lasciano r su tre collegamenti diversi.
- Nella vista Pacchetti (`p`) premi Invio sulla risposta della radice: contatore delle risposte a 0, NS nella sezione authority, glue nella sezione additional. Confronta con la risposta finale di auth (flag AA, una risposta).
- A 4 s la stessa domanda: `0 query a monte, dalla cache`, TTL 58 s. Seleziona r e apri la sua scheda `dns` per vedere la cache.
- A 6 s lo script esegue `dig ... trace=1` da h1: questa volta è h1 a compiere il percorso da solo, senza il resolver. Il registro lo mostra interrogare la radice, essere rimandato ai server di com, poi al server di example.com, e ricevere la risposta autoritativa: gli stessi tre passi compiuti da r a 1 s.
- Prevedi, poi prova: `:h1 dig example.com server=r`. Quante query a monte, e verso chi? Poi `:h1 dig mail.example.com server=r` due volte: qual è la risposta, e che cosa cambia la seconda volta?
- Prevedi, poi prova: `:r dns flush` e poi `:h1 dig www.example.com server=r`. Quante query a monte adesso? Poi `:h1 dig example.com type=NS server=r` e leggi il TTL.

**Laboratorio 241: `./netlab run 241-dhcp-leases`**

- Segui DISCOVER, OFFER, REQUEST, ACK per h1 a 1 s; apri ciascuno nella vista Pacchetti e trova xid, yiaddr e le opzioni 53, 51 e 54. Nota gli indirizzi sorgente e destinazione della DISCOVER.
- A 3 s h3 trova il pool esaurito. A 6 s h1 raggiunge T1 e rinnova con una REQUEST in unicast. Apri la scheda `dhcp` di s per vedere la tabella dei lease.
- Prevedi, poi prova: `:h1 dhcp release`. Che cosa succede a h3, ed entro quanti secondi?
- Prevedi, poi prova: riavvia, e intorno agli 8 s digita `:node s down`. Scrivi quando h1 registrerà T1, T2 e `lease scaduto`, poi controlla. Riaccendere il nodo con `:node s up` non riavvia il servizio: esegui `:s dhcp server pool=10.0.5.100-10.0.5.101 lease=10s router=10.0.5.1` e guarda chi ottiene i due indirizzi questa volta.
- Prova anche `./netlab run 241-dhcp-leases lease=4`: a quali istanti rinnova h1?

## Verifica

- Un resolver con la cache vuota risolve `mail.cs.example.org`. Elenca le query che invia, il tipo di risposta a ciascuna e il contenuto della sua cache alla fine.
- Distingui le query ricorsive da quelle iterative. Chi invia le une e le altre, e perché i root server rifiutano la ricorsione?
- Un sito cambia l'indirizzo del proprio server web. Il record A aveva un TTL di 86400. Che cosa osservano gli utenti, per quanto tempo, e come avrebbe dovuto preparare la modifica l'amministratore?
- Un client ottiene un lease di 8 ore alle 09:00 e il suo server si spegne alle 12:00. A che ora il client invia messaggi, di che tipo e a quale indirizzo, e quando perde l'indirizzo?
- Perché la REQUEST DHCP è in broadcast anche se il client conosce già l'indirizzo del server?
