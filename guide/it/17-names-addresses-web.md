<!-- level: 1 -->
<!-- labs: 140-dns-first, 141-dhcp-join, 121-http-page -->
# Nomi, indirizzi e il web

Apri il portatile in biblioteca, scrivi il nome di un sito, e compare una pagina. In quel secondo o due, tre servizi hanno lavorato per te, ciascuno costruito sui livelli sottostanti.

Per prima cosa al portatile serviva un indirizzo. È arrivato senza sapere nulla di questa rete, quindi ha chiesto ad alta voce: "C'è qualcuno che può darmi un indirizzo?". Un server ha risposto con tutto il necessario: un indirizzo IP, il prefisso, il gateway predefinito, l'indirizzo di un name server. Questo è **DHCP**. È come arrivare in albergo: la reception ti dà il numero di una stanza e ti dice dove sono le cose, e la stanza è tua solo per la durata del soggiorno.

Poi il nome doveva diventare un numero, perché i pacchetti portano indirizzi, non nomi. Il portatile ha chiesto a un server **DNS**: "qual è l'indirizzo di www.example.com?". Il DNS è l'elenco telefonico di Internet.

Infine il browser ha aperto una connessione TCP verso quell'indirizzo e ha chiesto la pagina con **HTTP**: "GET questa pagina", e il server ha risposto "200 OK, eccola". La pagina cita immagini e altri oggetti, quindi il browser chiede anche ciascuno di quelli.

Tre domande, tre risposte: chi sono qui, dov'è il server, dammi la pagina.

<!-- detail: 2 -->

## Come funziona

**DHCP: quattro messaggi**, facili da ricordare come D-O-R-A.

- **DISCOVER**: il client, che non ha un indirizzo, invia un broadcast: "c'è un server DHCP?".
- **OFFER**: un server propone un indirizzo, nel laboratorio 141 `10.0.1.100`.
- **REQUEST**: il client chiede esattamente quell'indirizzo (ancora in broadcast, così gli altri server sanno quale offerta è stata scelta).
- **ACK**: il server conferma, con la maschera, il gateway predefinito e la durata del **lease** (60 s nel laboratorio), trascorsa la quale l'indirizzo va rinnovato.

Solo ora l'host può configurare la propria interfaccia e la propria rotta predefinita. DHCP viaggia su UDP, porte 67 (server) e 68 (client).

**DNS: una domanda, una risposta**, normalmente su UDP porta 53. La domanda contiene un nome e un **tipo di record**; la risposta contiene dei record, ciascuno con un **TTL** che dice per quanti secondi può essere ricordato.

- Un record **A** associa un nome a un indirizzo IPv4: `www.example.com → 10.0.2.11` nel laboratorio 140.
- Un record **MX** indica il server di posta di un dominio.
- Se il nome non esiste la risposta è l'errore **NXDOMAIN**.

**HTTP: testo su TCP.** Il client invia una richiesta, il server una risposta:

```
GET /index.html HTTP/1.1          HTTP/1.1 200 OK
Host: www                         Content-Length: 138
                                  (riga vuota, poi i 138 byte)
```

La prima riga della risposta porta un **codice di stato**: 200 OK, 404 Not Found. La pagina del laboratorio 121 incorpora tre oggetti, quindi il browser fa in tutto quattro richieste.

**Una connessione o molte?** Con una **connessione persistente** (HTTP/1.1) tutte e quattro le richieste usano la stessa connessione TCP: un solo handshake. Aprire una nuova connessione per ogni oggetto (come faceva HTTP/1.0) costa un handshake in più, cioè un'andata e ritorno in più, per oggetto. Nel laboratorio 121 l'andata e ritorno è di circa 68 ms, e il secondo modo di caricare impiega circa 3 x 68 = 204 ms in più.

<!-- detail: 3 -->

## Per approfondire

**Il DNS non è un elenco solo ma una gerarchia.** Nessun server conosce tutti i nomi. Lo spazio dei nomi è un albero: la radice, i domini di primo livello (`com`, `it`), poi i domini sottostanti, ciascuno gestito dal suo proprietario su server **autoritativi**. Il tuo portatile parla con un **resolver ricorsivo** (di solito gestito dal tuo provider), che scende l'albero al posto tuo e conserva in **cache** ogni risposta per il suo TTL. La cache è ciò che rende veloce il DNS, e il TTL è il compromesso: TTL lunghi significano meno domande ma modifiche lente. I laboratori 240 e 340 mostrano la gerarchia, la cache e il TTL al lavoro. Il laboratorio 140 ha un solo server autoritativo, il caso più semplice. Il DNS in chiaro non ha autenticazione: una risposta falsa può avvelenare una cache (laboratorio 440), ed è per questo che esistono DNSSEC e il DNS cifrato.

**Dettagli di DHCP.** Perché un lease e non un'assegnazione permanente? Perché i dispositivi se ne vanno senza salutare, e gli indirizzi devono tornare nel pool (il laboratorio 241 mostra rinnovo, scadenza e un pool esaurito). Il broadcast non attraversa i router, quindi le reti grandi usano dei relay agent per raggiungere un server centrale. E DHCP si fida di chi risponde per primo: un server abusivo può distribuire un gateway falso.

**HTTP oggi.** HTTP/1.1 invia le richieste una dopo l'altra su una connessione, quindi i browser aprono più connessioni in parallelo. HTTP/2 fa viaggiare molte richieste insieme su una sola connessione; HTTP/3 gira su QUIC, sopra UDP, per evitare che un pacchetto perso blocchi tutti i flussi. Quasi tutto il traffico è **HTTPS**: un handshake TLS dopo quello TCP cifra la conversazione e dimostra l'identità del server, al costo di ulteriori andate e ritorni. Le cache e le reti di distribuzione dei contenuti (CDN) avvicinano agli utenti copie degli oggetti. Poiché ogni passo costa andate e ritorni, per il tempo di caricamento di una pagina il ritardo del percorso conta spesso più della sua banda.

**Tutta la storia in ordine**: DHCP (una volta, entrando in rete), ARP per il gateway, domanda DNS, handshake TCP, (handshake TLS,) richiesta e risposta HTTP, altre richieste per gli oggetti incorporati.

Per saperne di più: Kurose e Ross, capitolo 2 ("Livello di applicazione": Web e HTTP, DNS) e il paragrafo "Un giorno nella vita di una richiesta di una pagina web" alla fine del capitolo 6; RFC 1034 e RFC 1035 (DNS), RFC 2131 (DHCP), RFC 9110 e RFC 9112 (HTTP).

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 141-dhcp-join**: h1 arriva senza indirizzo; gw è router e server DHCP.

- Avvialo con `./hoplab run 141-dhcp-join`. Seleziona h1 (`n`): la sua interfaccia non ha indirizzo.
- A 1 s leggi i quattro messaggi nel Registro (`l`) e poi la riga `DHCP assegnato`. Apri la vista Pacchetti (`p`) e controlla il MAC di destinazione del DISCOVER.
- Apri la scheda Rotte di h1: è comparsa una rotta predefinita marcata `dhcp`. Lo stesso ACK portava anche l'indirizzo di un name server (opzione 6: la riga dice `dns 10.0.1.1`).
- A 4 s h1 fa ping verso h2, che sta su un'altra rete: funziona solo grazie al gateway imparato da DHCP.
- A 7 s h1 esegue `dig h2.lab.example` senza indicare un server: la domanda va al name server imparato da DHCP. Un solo scambio ha dato all'host tutto ciò che gli serve per usare la rete.

**Laboratorio 140-dns-first**: h1 fa tre domande al name server ns.

- Avvialo con `./hoplab run 140-dns-first`. A 1 s, 3 s e 5 s leggi domanda e risposta nel registro: un record A, un record MX, NXDOMAIN.
- Apri la domanda DNS nella vista Pacchetti e trova: UDP, porta 53, il nome, il tipo. Nella risposta trova il TTL.
- Il `ping` di hoplab accetta indirizzi e nomi di nodo, non nomi DNS: per usare la risposta scrivi `:` e `h1 ping 10.0.2.11`.

**Laboratorio 121-http-page**: h1 carica una pagina da www, due volte.

- Avvialo con `./hoplab run 121-http-page` e premi `2`. A 1 s la pagina viene caricata su una sola connessione persistente, a 4 s con una connessione per oggetto.
- Nel registro conta le righe `SYN` di ciascun caricamento e leggi le due righe `completata`: circa 422 ms e 559 ms. Il primo caricamento comprende anche circa 68 ms di ARP che il secondo non paga: la vera differenza sono tre handshake.
- Nella vista Pacchetti trova un frame con `GET` e uno con `200 OK` e aprili: la richiesta è testo leggibile.

**Cambia e prevedi.**

- Laboratorio 141: scrivi `h1 dhcp release`, poi `h1 ping h2`. Che cosa dice il registro? Poi `h1 dhcp client`.
- Laboratorio 140: `h1 dig example.com type=TXT` e `h1 dig mail.example.com`. Quale dei due fallisce, e con quale risposta?
- Laboratorio 121: `./hoplab run 121-http-page delay=100ms`. L'andata e ritorno diventa di circa 208 ms: quanto durerà in più il secondo caricamento rispetto al primo? Poi prova `objects=8`.
- Laboratorio 121: scrivi `h1 http get www/missing`. Quale codice di stato torna indietro?

## Verifica

- Perché il DHCP DISCOVER deve essere un broadcast?
- Elenca le quattro cose che un host impara tipicamente da DHCP.
- Che cosa dice il TTL di un record DNS a chi lo riceve?
- Una pagina incorpora 10 oggetti e l'andata e ritorno è di 100 ms. Quanto tempo fanno risparmiare, più o meno, le connessioni persistenti rispetto a una connessione per oggetto?
- Metti in ordine: richiesta HTTP, domanda DNS, handshake TCP, scambio DHCP, ARP per il gateway.
