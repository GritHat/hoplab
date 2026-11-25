<!-- level: 2 -->
<!-- labs: 250-nat-basics, 251-port-forwarding, 252-load-balancer -->
# NAT e bilanciamento del carico

I router dovrebbero inoltrare i pacchetti senza toccarne gli indirizzi. Due dispositivi molto diffusi violano questa regola di proposito, in direzioni opposte, con lo stesso strumento: una tabella che ricorda come è stato riscritto ogni flusso.

Il **NAT** nasconde molti host dietro un solo indirizzo. Gli indirizzi IPv4 sono scarsi, perciò case e aziende numerano i propri host con intervalli **privati** che chiunque può riusare e che Internet non instrada. Un pacchetto da 192.168.1.10 può raggiungere un server, ma la risposta non sa dove andare. Il router NAT al confine riscrive la *sorgente* di ogni pacchetto in uscita con il proprio indirizzo pubblico e con un numero di porta scelto da lui, e registra la coppia. Quando la risposta arriva su quella porta, riscrive all'indietro la *destinazione*. Poiché le righe nascono solo dai pacchetti in uscita, nessuno dall'esterno può iniziare una conversazione con un host interno, a meno che non sia configurata una regola statica di **port forwarding**.

Un **bilanciatore di carico** (load balancer) nasconde molti server dietro un solo indirizzo. I client parlano con un **IP virtuale**; il bilanciatore sceglie un server reale per ogni nuovo flusso, riscrive la *destinazione* e ricorda la scelta, così che tutto il flusso raggiunga lo stesso server. Inoltre sonda i server con gli **health check** e smette di scegliere quelli che non rispondono.

<!-- detail: 2 -->
## Indirizzi privati e NAPT

L'RFC 1918 riserva 10.0.0.0/8, 172.16.0.0/12 e 192.168.0.0/16. La forma di NAT presente in ogni router domestico è il **NAPT** (network address and port translation, RFC 3022). Per un pacchetto in uscita il router:

- cerca (protocollo, indirizzo interno, porta interna) nella tabella di traduzione e, se non c'è una riga, alloca una porta pubblica libera e ne crea una;
- sostituisce indirizzo e porta sorgente e aggiorna il checksum IP e il checksum TCP/UDP (che copre anche gli indirizzi, attraverso lo pseudo-header);
- fa ripartire il timer di inattività della riga.

Per un pacchetto in arrivo cerca (protocollo, porta pubblica): se la trova riscrive la destinazione e inoltra all'interno; altrimenti il pacchetto viene scartato.

La tabella del laboratorio 250 dopo che h1 e h2 hanno inviato ciascuno un datagramma UDP dalla porta sorgente 49152:

```
proto  interno              pubblico             remoto
icmp   192.168.1.10:2       203.0.113.2:61001    198.51.100.10
udp    192.168.1.10:49152   203.0.113.2:61002    198.51.100.10:7
udp    192.168.1.11:49152   203.0.113.2:61003    198.51.100.10:7
```

I due host hanno scelto la stessa porta; il NAT li distingue assegnando loro porte pubbliche diverse. ICMP echo non ha porte: ne fa le veci il suo campo identifier. Il server vede sempre e solo 203.0.113.2.

Gli *errori* ICMP richiedono più lavoro: un «time exceeded» proveniente da un router esterno cita l'intestazione del pacchetto incriminato, che è quello tradotto. Il NAT deve trovare l'associazione a partire dall'intestazione citata e riscrivere sia la destinazione esterna sia la citazione. È per questo che nel laboratorio traceroute funziona attraverso r1.

## Port forwarding e hairpinning

Una riga statica «porta pubblica 8080 → 192.168.1.10:7» non scade mai e accetta pacchetti da chiunque. Nel laboratorio 251 il client esterno viene respinto prima che la regola esista (il NAT del laboratorio risponde con ICMP port unreachable) e servito dopo. La risposta esce con sorgente 203.0.113.2:8080: il client non viene mai a conoscere l'indirizzo privato. Un host interno che usa l'indirizzo pubblico raggiunge il server solo se il NAT supporta l'**hairpinning**: tradurre un pacchetto che arriva dall'interno e rimandarlo all'interno.

## Che cosa rompe il NAT

- **Le connessioni in ingresso**: server e applicazioni peer-to-peer dietro un NAT sono irraggiungibili senza configurazione o espedienti.
- **I protocolli che trasportano indirizzi nel carico utile** (FTP, SIP): l'indirizzo privato dentro i dati è inutile all'esterno, a meno che il NAT capisca e riscriva il protocollo applicativo.
- **Le connessioni inattive**: la riga scade e la connessione muore in silenzio; le applicazioni inviano keepalive.
- **La stratificazione stessa**: un dispositivo di livello rete ora dipende dalle porte del trasporto, quindi i nuovi protocolli di trasporto non lo attraversano, e l'autenticazione dell'intestazione di IPsec fallisce.

## Bilanciamento a livello 4

Il bilanciatore mantiene un pool di server, una **tabella dei flussi** (indirizzo e porta del client → server) e, per ogni server, uno stato di salute. Per il primo pacchetto di un flusso seleziona un server; con il **round robin** prende semplicemente il successivo della lista. Riscrive la destinazione dall'IP virtuale al server; al ritorno riscrive la sorgente con l'IP virtuale. I pacchetti successivi seguono la tabella dei flussi: senza di essa una connessione TCP verrebbe mandata a un server che non ne ha mai visto l'handshake.

Nel laboratorio 252 gli health check sono ICMP echo ogni secondo; un server viene tolto dopo 2 mancate risposte consecutive (`fall`). s2 cade a 2,0 s: i controlli a 2,0 e 3,0 s falliscono, e a 4,0 s esce dal pool e i suoi 10 flussi vengono riassegnati. In quei due secondi le richieste dei flussi legati a s2 vanno perse: circa un terzo di 20 richieste al secondo, 13 nel laboratorio.

<!-- detail: 3 -->
## In profondità: NAT

**Comportamenti** (RFC 4787 per UDP, RFC 5382 per TCP, RFC 5508 per ICMP). *Mapping*: un NAT endpoint-independent usa la stessa porta pubblica per tutto ciò che un socket interno invia; uno simmetrico alloca una nuova porta per ogni destinazione. *Filtering*: chi può inviare a una porta associata: chiunque (full cone), solo gli indirizzi già contattati, solo le coppie indirizzo e porta già contattate (port-restricted, il default di netlab), o solo la destinazione dell'associazione. Queste proprietà decidono se due host dietro NAT possono collegarsi direttamente con l'**hole punching**: entrambi apprendono i propri endpoint pubblici da un server (STUN, RFC 8489), si inviano pacchetti nello stesso momento, e ciascun NAT scambia il pacchetto dell'altro per una risposta. Quando non funziona serve un relay (TURN, RFC 8656); ICE (RFC 8445) prova tutte le opzioni.

**Limiti.** Un indirizzo pubblico offre circa 64000 porte per protocollo, un tetto ai flussi contemporanei. Il carrier-grade NAT condivide un indirizzo fra molti clienti (intervallo condiviso 100.64.0.0/10, RFC 6598), aggiunge una seconda traduzione e rende difficile attribuire il traffico a un abbonato. Timeout tipici: da 30 s a qualche minuto per UDP, ore per le connessioni TCP stabilite.

**Non è un firewall.** Il NAT blocca il traffico non sollecitato in ingresso come effetto collaterale, ma non offre alcuna politica. Gli host possono chiedere al NAT un'associazione con UPnP o PCP (RFC 6887). La critica architetturale è nell'RFC 2993; la terminologia nell'RFC 2663. IPv6 elimina il bisogno del NAT, non quello del filtraggio.

## In profondità: bilanciamento

**Il percorso di ritorno.** Riscrivendo solo la destinazione, i server devono rimandare le risposte attraverso il bilanciatore (nel laboratorio 252 è il loro router). Alternative: il *full NAT* riscrive anche la sorgente, e i server perdono l'indirizzo del client; il *direct server return* consegna a server che possiedono l'IP virtuale su un'interfaccia di loopback e rispondono direttamente al client, così il bilanciatore gestisce solo i piccoli pacchetti in ingresso.

**Scegliere un server.** Il round robin ignora il carico; least connections si adatta ai flussi lunghi; l'hash del flusso non richiede tabella, e quindi più bilanciatori dietro ECMP possono scegliere allo stesso modo, ma il semplice hash modulo N riassegna quasi tutti i flussi quando il pool cambia. Il consistent hashing (Karger e altri, 1997) e il Maglev hashing (Eisenbud e altri, NSDI 2016) ne spostano solo circa 1/N. «The power of two choices» (Mitzenmacher) spiega perché scegliere il migliore fra due server presi a caso è quasi altrettanto buono che conoscere tutti i carichi.

**Gli health check** sono un rilevatore di guasti con il solito compromesso: un intervallo breve e un `fall` piccolo reagiscono in fretta ma espellono server sani per una sola sonda persa; i sistemi reali richiedono anche più successi (`rise`) prima della riammissione, e svuotano le connessioni prima delle rimozioni pianificate. Un ICMP echo dimostra che la macchina è accesa, non che il servizio funziona: in produzione i controlli aprono una connessione TCP o richiedono un URL.

**Livello 7.** Un proxy che termina TCP e TLS può instradare per URL o cookie, a un costo maggiore per connessione.

**netlab semplifica:** i laboratori usano flussi UDP di richiesta/risposta, e un flusso termina dopo `life` richieste anziché con un FIN.

<!-- detail: 1 -->
## In laboratorio

**Laboratorio 250: `./netlab run 250-nat-basics`**

- Prima dei 4 s non c'è NAT: il server risponde, e isp registra `nessuna rotta verso 192.168.1.10`. Nota chi riceve l'errore ICMP.
- A 5 s apri la vista Pacchetti (`p`) e confronta la stessa echo request sul collegamento sw-r1 e su r1-isp: indirizzo sorgente, identifier, checksum. Seleziona r1 e apri la sua scheda `nat`.
- A 9 s leggi le due associazioni UDP; a 12 s apri l'ICMP time exceeded di isp prima e dopo r1.
- Prevedi, poi prova: `./netlab run 250-nat-basics preserve=1`. Quali porte pubbliche ottengono h1 e h2 a 9 s?
- Prevedi, poi prova: `./netlab run 250-nat-basics timeout=500ms`. Quante associazioni crea il ping di tre pacchetti a 5 s, e perché? Poi `type=symmetric` e confronta i numeri di porta.

**Laboratorio 251: `./netlab run 251-port-forwarding`**

- A 1 s r1 registra `nessuna associazione, non c'è nessuno all'interno a cui consegnarlo`; a 4 s viene aggiunta la regola; a 5 s il datagramma raggiunge web. Nella vista Pacchetti confronta la sua destinazione prima e dopo r1, e la sorgente della risposta prima e dopo r1.
- A 7 s h1 prova l'indirizzo pubblico dall'interno e fallisce.
- Prevedi, poi prova: `./netlab run 251-port-forwarding hairpin=1`. Apri il pacchetto che web riceve a 7 s: qual è il suo indirizzo sorgente, e perché il NAT deve riscriverlo?

**Laboratorio 252: `./netlab run 252-load-balancer`**

- Seleziona lb e apri la sua scheda `lb`: pool, tabella dei flussi, contatori. Nel registro `nuovo flusso ... → s2 (rr)` ruota fra s2, s3, s1. Seleziona c1 per il conteggio di chi ha risposto.
- A 2 s s2 cade; a 4 s `health check fallito, escluso dal pool` e `10/30 flussi hanno perso il loro server`; a 9 s s2 ritorna.
- Prevedi, poi prova: `./netlab run 252-load-balancer check=250ms`. Quando viene tolto s2 e quante richieste vanno perse? Poi `fall=1` con l'intervallo di default.
- Prevedi, poi prova: `./netlab run 252-load-balancer algo=leastconn`. Dopo i 9 s, com'è la quota di nuovi flussi di s2 rispetto al round robin, e perché?

## Verifica

- Due host interni aprono connessioni TCP dalla stessa porta sorgente verso lo stesso server web. Scrivi la tabella di traduzione e le intestazioni di un pacchetto per ciascun verso, da entrambi i lati del NAT.
- Perché un NAT deve ricalcolare il checksum TCP anche se non tocca il carico utile TCP?
- Spiega perché un host dietro un NAPT non può ospitare un server pubblico senza configurazione aggiuntiva, e che cosa cambia una regola di port forwarding.
- Un bilanciatore di livello 4 usa il round robin senza tabella dei flussi. Che cosa va storto con TCP?
- Gli health check avvengono ogni 2 s con `fall` = 3 e un server riceve 300 richieste al secondo. Stima le richieste perse quando cade. Qual è il rischio di impostare `fall` = 1?
