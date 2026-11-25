<!-- level: 2 -->
<!-- labs: 103-ipv6-first, 204-ipv6-slaac, 205-ipv6-pmtu, 206-dual-stack -->
# IPv6: indirizzi, Neighbor Discovery e autoconfigurazione

IPv4 ha indirizzi a 32 bit: circa quattro miliardi, meno dei dispositivi che ne vorrebbero uno. **IPv6** è la stessa idea di livello di rete — pacchetti con un indirizzo sorgente e uno di destinazione, inoltrati salto dopo salto con il longest prefix match — con **indirizzi a 128 bit** e un'intestazione ridisegnata con trent'anni di esperienza. Non sostituisce IPv4 in un giorno preciso: i due convivono sugli stessi cavi (**dual stack**), distinti dal campo tipo del frame Ethernet.

Tre cose appaiono diverse appena si guarda un laboratorio. Un'interfaccia ha **più indirizzi**, e uno di essi, l'indirizzo **link-local** che inizia con `fe80::`, esiste prima che qualcuno configuri qualcosa. **Non ci sono ARP né broadcast**: trovare l'indirizzo MAC di un vicino, trovare i router, verificare che un indirizzo sia libero sono compiti di un solo protocollo, il **Neighbor Discovery**, con messaggi ICMPv6 inviati a gruppi multicast. E un host può **configurarsi da solo** a partire da ciò che il router annuncia, senza un server DHCP.

Un'altra regola sorprende chi conosce IPv4: **i router non frammentano mai** un pacchetto IPv6. Se non entra nel collegamento successivo lo scartano e avvisano la sorgente, che deve inviare pacchetti più piccoli.

<!-- detail: 2 -->
## Scrivere gli indirizzi

Un indirizzo è fatto di 128 bit scritti come otto gruppi di quattro cifre esadecimali: `2001:0db8:0000:0000:0000:0000:0000:0010`. Gli zeri iniziali di un gruppo si omettono, e **una sola** sequenza di gruppi tutti a zero si sostituisce con `::`, ottenendo `2001:db8::10`. Un prefisso si scrive come in IPv4: `2001:db8:1::/64`. Quasi ogni sottorete è una **/64**: 64 bit di prefisso di rete, 64 bit di **identificatore di interfaccia**.

I tipi di indirizzo che si incontrano nei laboratori:

- **link-local** `fe80::/10`: valido solo su un collegamento, mai inoltrato da un router; ogni interfaccia ne ha uno
- **unicast globale** `2000::/3`: gli indirizzi instradabili; i laboratori usano il prefisso riservato alla documentazione `2001:db8::/32`
- **multicast** `ff00::/8`: `ff02::1` sono tutti i nodi del collegamento, `ff02::2` tutti i router, `ff02::1:ffXX:XXXX` il gruppo *solicited-node* degli indirizzi che terminano con XX:XXXX
- **non specificato** `::`, la sorgente di un nodo che non ha ancora un indirizzo

netlab costruisce l'identificatore di interfaccia dall'indirizzo MAC (EUI-64 modificato): il MAC `02:00:4e:4c:01:00` dà `fe80::4eff:fe4c:100` — si inserisce `ff:fe` al centro e si inverte un bit del primo byte.

## L'intestazione

L'intestazione IPv6 è sempre di **40 byte** e ha otto campi: versione, traffic class, flow label, lunghezza del payload, **next header**, **hop limit**, sorgente, destinazione. Rispetto a IPv4:

- nessun checksum dell'intestazione: collegamenti e trasporti hanno già il proprio, e i router non devono più ricalcolarne uno a ogni salto
- nessun campo di frammentazione e nessuna opzione: sono passati alle **intestazioni di estensione**, concatenate dal campo next header (un pacchetto IPv6 che porta un frammento di un datagramma UDP ha next header 44, Fragment, il cui next header è 17, UDP)
- il TTL si chiama hop limit, che è ciò che è sempre stato

Il prezzo dell'eliminazione del checksum dell'intestazione è che il **checksum UDP diventa obbligatorio**, e che ogni checksum di trasporto copre una pseudo-intestazione con i due indirizzi a 128 bit.

## Neighbor Discovery

Cinque messaggi ICMPv6 (tipi 133–137; netlab realizza i primi quattro) svolgono il lavoro di ARP, della scoperta dei router e altro ancora. Tutti sono inviati con hop limit 255 e scartati se arrivano con un valore inferiore, quindi non possono provenire da un altro collegamento.

- **Neighbor Solicitation / Advertisement.** Per risolvere `2001:db8:1::1` un nodo invia una NS al gruppo solicited-node `ff02::1:ff00:1`, trasportata in un frame per il MAC multicast `33:33:ff:00:00:01`. Sono interessati solo i nodi i cui indirizzi terminano con quei 24 bit, invece di tutti come con un broadcast ARP. Il proprietario risponde con una NA unicast che porta il suo MAC. Le voci della **cache dei vicini** hanno uno stato: *incompleto* mentre si chiede, *raggiungibile* per circa 30 secondi dopo una conferma, poi *da verificare* (stale); una voce stale viene ancora usata, ma fa partire una verifica unicast.
- **Duplicate address detection (DAD).** Prima di usare un indirizzo, un nodo invia una NS *per il proprio indirizzo* dalla sorgente `::`. Se qualcuno risponde, l'indirizzo è occupato. Solo dopo un secondo di silenzio l'indirizzo smette di essere *provvisorio*.
- **Router Solicitation / Advertisement.** Un host chiede a `ff02::2` se c'è un router; i router rispondono, e ripetono periodicamente, con un RA che porta i prefissi del collegamento e per quanto tempo il router può essere usato come gateway predefinito.

## Autoconfigurazione (SLAAC)

A un host bastano quattro passi e nessun server. Forma il proprio indirizzo link-local e lo verifica con la DAD. Invia una Router Solicitation. Da ogni prefisso del Router Advertisement marcato *autonomo* forma prefisso + identificatore di interfaccia, e verifica anche quello. Installa una rotta predefinita attraverso l'indirizzo link-local del router. In netlab un router annuncia con `run r1 ra`; la rotta predefinita compare nella scheda Rotte con sorgente `ra`.

## Frammentazione e MTU del percorso

Ogni collegamento IPv6 deve trasportare pacchetti di **1280 byte**. Un router che non può inoltrare un pacchetto perché è più grande del collegamento successivo invia un ICMPv6 **Packet Too Big** con la MTU di quel collegamento e lo scarta. La sorgente registra il valore per quella destinazione (**scoperta della MTU del percorso**) e, per un datagramma che continua a non entrare, lo frammenta da sé con l'intestazione Fragment: offset in unità di 8 byte, un flag M per «seguono altri frammenti», un'identificazione a 32 bit. Il riassemblaggio avviene solo a destinazione, come in IPv4.

<!-- detail: 3 -->
## Approfondimento

**Perché il ridisegno conta per i router.** Un'intestazione fissa di 40 byte senza checksum e senza frammentazione significa che il percorso di inoltro è: decrementare l'hop limit, cercare la destinazione, inviare. Tutto ciò che è facoltativo sta in intestazioni di estensione che i router dovrebbero ignorare (solo Hop-by-Hop è per loro). In pratica, i pacchetti con intestazioni di estensione vengono spesso scartati in Internet (lo ha misurato la RFC 7872), ed è uno dei motivi per cui i trasporti evitano del tutto la frammentazione IPv6 e tengono i propri segmenti sotto la MTU del percorso.

**La scoperta della MTU del percorso è fragile.** Dipende dall'arrivo del messaggio Packet Too Big. I firewall che scartano tutto ICMPv6 creano buchi neri: i pacchetti grandi spariscono senza errore. La RFC 4890 elenca i messaggi ICMPv6 che non vanno mai filtrati; la RFC 8899 descrive una scoperta che non si affida a ICMP, sondando con pacchetti di dimensione crescente. Nota inoltre che la MTU del percorso si impara **per ciascun verso**: nel laboratorio la richiesta del secondo ping arriva, ed è la risposta a morire.

**Privacy e identificatore di interfaccia.** Un identificatore ricavato dall'indirizzo MAC è lo stesso su ogni rete che il dispositivo visita, quindi lo rende tracciabile. Gli host reali usano oggi identificatori casuali temporanei (RFC 8981) o stabili ma opachi (RFC 7217). netlab mantiene la forma EUI-64 perché permette di riconoscere il nodo in un indirizzo.

**Sicurezza del Neighbor Discovery.** Come ARP, ND si fida del collegamento: chiunque può rispondere a una solicitation, o annunciarsi come router. La regola dell'hop limit 255 prova soltanto che il messaggio viene dallo stesso collegamento. Gli switch si difendono con RA Guard (RFC 6105); la risposta crittografica, SEND (RFC 3971), è quasi assente nella pratica.

**Che cosa netlab tralascia.** Non c'è MLD: i nodi non annunciano i gruppi multicast che ascoltano, e gli switch si limitano a inondare il multicast, quindi il risparmio dei gruppi solicited-node rispetto al broadcast si vede in chi *elabora* una solicitation, non in chi la riceve. Non ci sono DHCPv6, il messaggio redirect, gli indirizzi di privacy; le intestazioni di estensione diverse da Fragment vengono saltate senza essere interpretate; gli indirizzi appresi con SLAAC non scadono; la cache dei vicini non ha lo stato DELAY (una voce stale passa direttamente a una verifica dopo cinque secondi). I protocolli di instradamento del laboratorio (RIP, OSPF, BGP) e TCP funzionano solo su IPv4: le rotte IPv6 sono statiche o automatiche.

**Transizione.** Il dual stack è il caso semplice: entrambi i protocolli ovunque. Dove non è possibile, i tunnel trasportano un protocollo dentro l'altro, e i traduttori (NAT64 con DNS64, RFC 6146/6147; 464XLAT, RFC 6877) permettono a reti solo IPv6 di raggiungere l'Internet IPv4. Nulla di tutto ciò è nel laboratorio.

Fonti primarie: RFC 8200 (IPv6), RFC 4291 (architettura degli indirizzi), RFC 5952 (rappresentazione testuale), RFC 4443 (ICMPv6), RFC 4861 (Neighbor Discovery), RFC 4862 (SLAAC), RFC 8201 (scoperta della MTU del percorso). Manuali: Kurose e Ross, capitolo 4, la sezione su IPv6; Peterson e Davie, sezione 4.1.

<!-- detail: 1 -->
## In laboratorio

Inizia con `103-ipv6-first`, a dettaglio 2.

- Nel primo secondo non succede nulla di «utile»: ogni interfaccia invia due Neighbor Solicitation da `::`, una per l'indirizzo link-local e una per quello globale. È la DAD. Seleziona h1 (`n`), apri la scheda Nodo e osserva gli indirizzi perdere l'etichetta «provvisorio».
- A 2 s parte il ping. Nella vista Sequenza (`s`) trova la solicitation «Who has 2001:db8:1::1?» e il suo advertisement: lo stesso ruolo che ARP aveva nel laboratorio 101. Apri la solicitation nella vista Pacchetti (`p`, Invio): destinazione `ff02::1:ff00:1`, destinazione Ethernet `33:33:ff:00:00:01`, hop limit 255.
- Apri la scheda ARP di h1: la tabella IPv4 è vuota, la cache dei vicini IPv6 contiene il router, *raggiungibile*; aspetta e diventa *da verificare*.
- Prevedi, poi prova: `:h1 ping fe80::4eff:fe4c:200` (l'indirizzo link-local di r1) funziona; l'indirizzo link-local di h2 no. Perché?

Poi `204-ipv6-slaac`: nessuno ha dato un indirizzo agli host.

- Osserva le schede Nodo e Rotte di h1 fra 1 s e 2 s: router solicitation, advertisement, un nuovo indirizzo marcato SLAAC, una rotta predefinita con sorgente `ra`.
- Apri il Router Advertisement nella vista Pacchetti e leggi l'opzione del prefisso e la durata del router.
- Scrivi `:stop r1 ra`, poi guasta e riavvia h1 premendo `k` due volte. Prevedi che cosa ha h1 dopo due secondi, e se può fare ping a h2.

`205-ipv6-pmtu`: eseguilo una volta, poi esegui il laboratorio 201 e confronta chi taglia il pacchetto. Porta il parametro `size` a 1200: quali messaggi spariscono? In `206-dual-stack` confronta le due echo request campo per campo, poi spiega perché togliere una rotta IPv4 lascia intatto IPv6.

## Verifica

- Scrivi `2001:0db8:0000:0000:0000:ff00:0042:8329` nella forma più breve. Perché `::` può comparire una sola volta?
- Un host con MAC `02:00:4e:4c:03:00` riceve il prefisso `2001:db8:b::/64` in un Router Advertisement. Quale indirizzo forma, e che cosa deve accadere prima che lo usi?
- Le Neighbor Solicitation sono inviate a un gruppo multicast anziché in broadcast. Su un collegamento con mille host, quali nodi elaborano una solicitation per un indirizzo, e come fa la scheda di rete del destinatario a saperlo?
- Il collegamento centrale di un percorso ha MTU 1280 e un host invia un pacchetto da 1500 byte. Elenca i pacchetti che viaggiano finché un datagramma UDP da 3000 byte viene consegnato, e di' chi lo frammenta.
- Perché il checksum UDP è facoltativo su IPv4 e obbligatorio su IPv6?
</content>
