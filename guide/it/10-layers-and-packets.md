<!-- level: 1 -->
<!-- labs: 100-two-hosts, 101-ping -->
# Livelli e pacchetti

Una rete sposta **messaggi** fra programmi che girano su macchine diverse. Nessun software fa da solo tutto il lavoro. Il compito è diviso in **livelli**: ognuno risolve un solo problema e si affida al livello sottostante per il resto.

Pensa a una lettera. Tu scrivi il messaggio. Lo metti in una busta con il nome del destinatario. L'ufficio postale mette la busta in un sacco etichettato con la città di destinazione. Un furgone porta il sacco al centro di smistamento successivo, qualunque cosa contenga. Ogni livello aggiunge la propria etichetta e ignora quelle degli altri.

Le reti fanno esattamente questo, e si chiama **incapsulamento**: i dati di un programma vengono avvolti in un'intestazione di trasporto, poi in una di rete, poi in una di collegamento. Sul cavo viaggia un unico blocco di byte, il **frame**, con le intestazioni una dentro l'altra come buste annidate. Chi riceve le apre nell'ordine opposto.

Ogni livello ha il proprio tipo di **indirizzo**: una porta indica un programma, un indirizzo IP indica una macchina in qualunque parte del mondo, un indirizzo MAC indica una scheda di rete sullo stesso cavo.

<!-- detail: 2 -->

## Come funziona

Internet usa cinque livelli. Dall'alto:

- **Applicazione** (livello 7 nel registro di hoplab, `L7`): il programma e i suoi messaggi, per esempio una richiesta web. Per i livelli inferiori sono semplici byte.
- **Trasporto** (`L4`): consegna i dati al programma giusto su una macchina, usando numeri di **porta** a 16 bit. La sua unità è il **segmento** (TCP) o il **datagramma** (UDP).
- **Rete** (`L3`): porta un **pacchetto** dalla macchina sorgente alla macchina di destinazione attraverso molte reti, usando **indirizzi IP** a 32 bit come `10.0.0.2`.
- **Collegamento** (`L2`): porta un **frame** su un solo cavo o una sola tratta radio, usando **indirizzi MAC** a 48 bit come `02:00:4e:4c:01:00`.
- **Fisico** (`L1`): trasforma i bit del frame in segnali.

I nomi contano: un frame contiene un pacchetto, un pacchetto contiene un segmento, un segmento contiene i dati dell'applicazione.

Un esempio svolto: il `ping` che vedrai nel laboratorio 100. Il programma chiede di inviare 56 byte di dati. ICMP (il protocollo usato da ping) aggiunge un'intestazione di 8 byte. IP aggiunge un'intestazione di 20 byte con indirizzo sorgente `10.0.0.1`, destinazione `10.0.0.2` e pochi altri campi. Ethernet aggiunge 14 byte davanti (MAC di destinazione, MAC sorgente, un campo tipo che dice "segue un pacchetto IP") e un controllo di 4 byte in fondo. Totale sul cavo:

```
 14        20        8       56        4      = 102 byte
[Ethernet][IP      ][ICMP  ][dati    ][contr.]
```

Solo 56 byte su 102 sono dati: il resto è **overhead**, il prezzo delle etichette.

La regola fondamentale è che gli indirizzi dei diversi livelli hanno durate diverse. Quando un pacchetto attraversa un router, gli **indirizzi IP restano gli stessi** dalla prima macchina all'ultima, mentre gli **indirizzi MAC cambiano a ogni salto**, perché ogni frame vive su un solo collegamento. Il router toglie la vecchia intestazione di collegamento, legge l'intestazione IP e costruisce un nuovo frame per il collegamento successivo.

<!-- detail: 3 -->

## Per approfondire

Perché i livelli? Perché ognuno può cambiare senza toccare gli altri. Il Wi-Fi ha sostituito i cavi sotto IP e nessun browser è stato riscritto; HTTP ha avuto nuove versioni e nessun router se n'è accorto. Il costo è una certa inefficienza (intestazioni ripetute, controlli ripetuti) e il fatto che a volte un livello avrebbe bisogno di informazioni che la stratificazione gli nasconde.

Per parlare di livelli si usano due modelli. Il **modello OSI** ne ha sette; Internet in pratica ha i cinque elencati sopra, e i numeri OSI sono sopravvissuti come nomi: si dice "switch di livello 2" e "bilanciatore di livello 7", e hoplab etichetta così le righe del registro. Non aspettarti che ogni protocollo si incastri bene: ARP sta fra i livelli 2 e 3, ICMP viaggia dentro IP ma fa parte del livello di rete, e i tunnel mettono un intero pacchetto dentro un altro pacchetto.

Il livello IP è volutamente minimo: promette solo il **best effort**, cioè i pacchetti possono andare persi, essere duplicati, ritardati o riordinati. Tutto il resto (affidabilità, ordine, sicurezza) si costruisce ai bordi, negli host. È il **principio end-to-end**, ed è la ragione per cui il nucleo della rete è potuto restare semplice e crescere tanto. Si parla anche di "clessidra": molte applicazioni sopra, molte tecnologie di collegamento sotto, un solo protocollo stretto, IP, nel mezzo.

Le dimensioni hanno un limite: un frame Ethernet porta al massimo 1500 byte di pacchetto, la **MTU**. I pacchetti più grandi vanno divisi (il laboratorio 101 mostra un datagramma da 3000 byte tagliato in tre frammenti).

Per saperne di più: Kurose e Ross, "Reti di calcolatori e Internet. Un approccio top-down", capitolo 1 (il paragrafo sui livelli di protocollo); Tanenbaum, "Reti di calcolatori", capitolo 1 sui modelli di riferimento; RFC 1122 ("Requirements for Internet Hosts") per l'elenco ufficiale dei livelli; Saltzer, Reed e Clark, "End-to-End Arguments in System Design".

<!-- detail: 1 -->

## In laboratorio

**Laboratorio 100-two-hosts**: la rete più piccola, due host e un cavo.

- Avvialo con `./hoplab run 100-two-hosts`. Premi spazio per mettere in pausa quando vuoi leggere.
- Premi `p` per la vista Pacchetti. Ogni riga è un frame. Spostati sul primo frame ICMP e premi Invio: la dissezione mostra le intestazioni una dentro l'altra, Ethernet, poi IP, poi ICMP, con ogni campo e i byte che lo trasportano. Usa `[` e `]` per scorrere i campi.
- Trova i tre tipi di indirizzo: gli indirizzi MAC nell'intestazione Ethernet, gli indirizzi IP nell'intestazione IP. Il ping non ha porte: quelle compaiono nel laboratorio 101, nel datagramma UDP a 8 s.
- Somma le dimensioni delle intestazioni e confronta con i 102 byte calcolati sopra.

**Laboratorio 101-ping**: aggiunge un router fra due reti.

- Avvialo con `./hoplab run 101-ping` e apri la vista Pacchetti.
- Confronta la stessa echo request sui due collegamenti, `h1 → r1` e `r1 → h2`. Prima prevedi: quali indirizzi sono uguali, quali diversi? Poi controlla nella dissezione. Guarda anche il campo TTL dell'intestazione IP: 64 sul primo collegamento, 63 sul secondo.
- A 8 s parte un datagramma UDP: aprilo e trova la quarta intestazione, UDP, con le sue due porte.

**Cambia e prevedi.** Nel laboratorio 101 scrivi `:` e poi `h1 ping h2 size=1000`. Quanti byte avrà ogni frame sul cavo? (Scrivi la somma prima di aprire il frame.) Poi prova `h1 ping h2 size=2000`: questo pacchetto non entra in 1500 byte. Che cosa ti aspetti di vedere nella vista Pacchetti?

## Verifica

- Metti in ordine dal più esterno al più interno, in un frame che porta una richiesta web: segmento TCP, messaggio HTTP, pacchetto IP, frame Ethernet.
- Un pacchetto va da h1 a h2 attraverso un router. Quali indirizzi cambia il router, e quali lascia intatti?
- Perché un ping da 56 byte occupa 102 byte sul cavo?
- Una porta, un indirizzo IP e un indirizzo MAC rispondono ciascuno a un diverso "chi?". Quale?
- Che cosa andrebbe riscritto in un browser se il tuo computer passasse dal cavo al Wi-Fi? Perché?
