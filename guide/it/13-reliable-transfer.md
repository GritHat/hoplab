<!-- level: 1 -->
<!-- labs: 120-tcp-handshake -->
# Trasferimento affidabile su un canale inaffidabile

La rete sotto di te perde i pacchetti, li danneggia e a volte li consegna due volte. Eppure un file deve arrivare completo, in ordine, senza ripetizioni. Come si costruisce qualcosa di affidabile con parti che non lo sono?

Pensa a quando detti un numero di telefono su una linea disturbata. Dici alcune cifre e aspetti un "ok". Se per un po' non senti nulla, le ripeti. Se l'altra persona sente due volte lo stesso gruppo, deve capire che è una ripetizione e non cifre nuove, quindi numeri i gruppi: "gruppo uno: 347...".

Sono i tre strumenti di ogni protocollo affidabile. Un **riscontro** (acknowledgement, ACK) è l'"ok" rimandato indietro dal receiver. Un **timeout** è la decisione di inviare di nuovo quando nessun ACK arriva in tempo. Un **numero di sequenza** su ogni pacchetto permette al receiver di riconoscere i duplicati e di rimettere le cose in ordine.

Il protocollo più semplice, **stop-and-wait**, invia un pacchetto e aspetta il suo ACK prima di inviare il successivo. È corretto ma lento. I protocolli più veloci tengono in viaggio più pacchetti insieme, dentro una **finestra**, e si distinguono per ciò che fanno quando uno va perso: **Go-Back-N** rimanda tutto dal pacchetto perso in poi, **Selective Repeat** rimanda solo quello perso.

<!-- detail: 2 -->

## Come funziona

**Stop-and-wait.** Il sender trasmette DATA con bit di sequenza 0, avvia un timer e aspetta. Possono andare storte tre cose, e per ciascuna c'è una risposta.

- Il DATA va perso: non arriva nessun ACK, il timer scade, il sender ritrasmette.
- Il DATA arriva danneggiato: il receiver vede un checksum sbagliato e non lo riscontra come buono; il sender finisce per ritrasmettere.
- L'ACK va perso: il sender ritrasmette un pacchetto che il receiver ha già. Il receiver rivede lo stesso bit di sequenza, **non** consegna i dati una seconda volta e ripete l'ACK.

Un bit basta perché l'unico dubbio è "pacchetto nuovo o ripetizione dell'ultimo?". Il prezzo è la velocità: un pacchetto per ogni andata e ritorno. Con 1 s di ritardo per direzione, è un pacchetto ogni 2 s, per quanto veloce sia il collegamento.

**Finestra scorrevole.** Il sender può avere in volo fino a N pacchetti non ancora riscontrati: la finestra. I numeri di sequenza usano k bit e ricominciano da capo, 0 … 2^k − 1. Quando il pacchetto più vecchio viene riscontrato la finestra scorre in avanti e può partire un nuovo pacchetto.

**Go-Back-N.** Il receiver è semplice: accetta solo il prossimo pacchetto in ordine e scarta tutto il resto, rispondendo sempre con il numero dell'ultimo pacchetto ricevuto in ordine. Gli ACK sono **cumulativi**: ACK 5 significa "è arrivato tutto fino a 5". Il sender tiene un solo timer, per il più vecchio pacchetto non riscontrato; quando scade rimanda **tutti** i pacchetti della finestra. Una perdita può costare N ritrasmissioni.

**Selective Repeat.** Anche il receiver ha una finestra e un buffer: conserva i pacchetti che arrivano fuori ordine e riscontra ciascuno **singolarmente**. Il sender ha un timer per pacchetto e rimanda solo quello il cui timer scade. Meno ritrasmissioni, più memoria e più contabilità.

Un esempio con N = 4 e il pacchetto 2 perso fra 0 1 2 3 4 5. Go-Back-N: il receiver scarta 3, 4 e 5, il sender va in timeout e rimanda 2 3 4 5. Selective Repeat: il receiver mette nel buffer 3, 4 e 5, il sender rimanda solo 2, e a quel punto vengono consegnati quattro pacchetti in una volta.

<!-- detail: 3 -->

## Per approfondire

**Quanto può essere grande la finestra?** I numeri di sequenza vengono riutilizzati, quindi un pacchetto in ritardo o ripetuto non deve mai essere scambiato per uno nuovo con lo stesso numero.

Per Go-Back-N il limite è **N ≤ 2^k − 1**. Prendi k = 2 (numeri da 0 a 3) e N = 4. Il sender trasmette 0 1 2 3, arrivano tutti, tutti e quattro gli ACK vanno persi. Il sender va in timeout e rimanda 0. Il receiver ora aspetta il pacchetto successivo, il cui numero è di nuovo 0: accetta un pacchetto vecchio come dati nuovi. Con N = 3 il receiver starebbe aspettando 3 e rifiuterebbe lo 0 ripetuto.

Per Selective Repeat il limite è più severo, **N ≤ 2^(k−1)**, metà dello spazio dei numeri. Prendi k = 2 e N = 3. Il sender trasmette 0 1 2, arrivano tutti, il receiver sposta la sua finestra su 3 0 1, e tutti gli ACK vanno persi. Il sender rimanda il vecchio 0, che cade dentro la nuova finestra del receiver e viene memorizzato come nuovo. Le finestre dei pacchetti vecchi e nuovi non devono mai sovrapporsi, da cui al massimo la metà.

**Il timeout** è un compromesso. Troppo corto, e i pacchetti che erano solo lenti vengono inviati due volte; troppo lungo, e ogni perdita blocca il trasferimento. Deve superare il tempo di andata e ritorno, che i protocolli reali misurano di continuo.

**Efficienza.** Stop-and-wait usa una frazione del collegamento pari a circa tempo di trasmissione / (tempo di trasmissione + tempo di andata e ritorno): minuscola sui collegamenti veloci e lunghi. Una finestra abbastanza grande da coprire l'andata e ritorno tiene il collegamento sempre occupato; si chiama pipelining.

**Che cosa aggiungono i sistemi reali.** TCP è un ibrido: ACK cumulativi come Go-Back-N, un buffer in ricezione come Selective Repeat, numeri di sequenza a 32 bit che contano i byte, un timeout adattivo, il fast retransmit dopo tre ACK duplicati, e riscontri selettivi opzionali (SACK). Inoltre cambia continuamente la dimensione della finestra, per proteggere il receiver (controllo di flusso) e la rete (controllo di congestione). I livelli di collegamento come il Wi-Fi usano stop-and-wait su ogni tratta.

Per saperne di più: Kurose e Ross, capitolo 3, paragrafo "Principi del trasferimento dati affidabile" (i protocolli da rdt 1.0 a 3.0, Go-Back-N e Selective Repeat); Tanenbaum, capitolo 3, "Protocolli a finestra scorrevole"; RFC 9293 per TCP.

<!-- detail: 1 -->

## In laboratorio

I laboratori di questo argomento non sono scenari di hoplab ma i tre programmi dimostrativi originali di questo repository: un sender e un receiver che si scambiano **veri pacchetti UDP** sulla tua macchina attraverso un canale che li perde, li corrompe e li ritarda di proposito. Avviali dal menu principale, voce "Demo di trasferimento affidabile", oppure da una shell.

- `./demo.sh` avvia stop-and-wait, `./demo.sh gbn` Go-Back-N, `./demo.sh sr` Selective Repeat. Il sender è a sinistra, il receiver a destra; `q` esce. `./demo.sh -h` elenca tutte le opzioni.
- Ogni lato mostra i pacchetti in viaggio sul canale, il registro degli eventi, le regole del protocollo con quella appena applicata evidenziata, e la striscia dei numeri di sequenza con la finestra e i timer.
- Per default ogni pacchetto viene perso con probabilità 15% e corrotto con probabilità 10%, il ritardo è di 1000 ms per direzione e il timeout di 3000 ms.
- `-w N` imposta la finestra e `-b K` i bit dei numeri di sequenza (default 4 e 3). `-s "..."` passa opzioni aggiuntive al sender e `-r "..."` al receiver: `-l` perdita, `-c` corruzione, `-d` ritardo in ms, `-t` timeout in ms, `-S` un seme per ripetere la stessa esecuzione. Le opzioni `-l` e `-c` del sender agiscono sui DATA, quelle del receiver sugli ACK.

**Che cosa osservare.** Esegui `./demo.sh` e aspetta ciascuno dei tre incidenti elencati sopra: un DATA perso, un pacchetto corrotto, un ACK perso. Per ognuno trova la regola che sender e receiver applicano. Poi esegui `./demo.sh gbn` e `./demo.sh sr` e confronta che cosa viene ritrasmesso dopo una perdita.

**Cambia e prevedi.**

- `./demo.sh -s "-l 0 -c 0" -r "-l 0 -c 0"`: un canale perfetto. Quanti pacchetti al secondo consegna stop-and-wait? Ora le stesse opzioni con `gbn`.
- `./demo.sh -s "-t 1500"`: il timeout è più corto dell'andata e ritorno (2000 ms). Che cosa succederà a ogni pacchetto?
- `./demo.sh gbn -s "-l 0.3 -c 0.2" -r "-l 0.3"` e lo stesso con `sr`: quale dei due invia meno pacchetti in totale?
- `./demo.sh gbn -w 8 -b 3`: la finestra viola N ≤ 2^k − 1 (l'intestazione della finestra ti avverte). Aspetta che vada persa un'intera finestra di ACK.
- `./demo.sh sr -w 3 -b 2 -s "-l 0.4" -r "-l 0.4"`: la finestra viola N ≤ 2^(k−1). Leggi il testo consegnato dal receiver: che cosa non va?

**Il passo successivo.** Il laboratorio 120-tcp-handshake mostra le stesse idee dentro TCP; eseguilo con `./hoplab run 120-tcp-handshake loss=0.1 bytes=50000` e cerca le ritrasmissioni nel registro.

## Verifica

- Perché il receiver invia di nuovo un ACK quando riceve un duplicato, invece di restare in silenzio?
- In stop-and-wait, che cosa va storto se i pacchetti non portano un numero di sequenza e un ACK va perso?
- Con numeri di sequenza a 3 bit, qual è la finestra massima sicura per Go-Back-N? E per Selective Repeat?
- Il pacchetto 2 di 0 … 5 va perso con una finestra di 4. Elenca i pacchetti ritrasmessi da Go-Back-N e da Selective Repeat.
- Che cosa succede se il timeout è più corto del tempo di andata e ritorno? E se è dieci volte più lungo?
