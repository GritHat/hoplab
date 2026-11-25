title      Loading a web page: HTTP over TCP
title[it]  Caricare una pagina web: HTTP su TCP
level  1
layer  7
topic  http
about  A browser (h1) loads a page from a web server (www) two networks away. The page is an
about  HTML document that embeds a few objects, so the client issues several GET requests.
about  The first load reuses one persistent TCP connection (HTTP/1.1); the second opens a new
about  connection for every object (as HTTP/1.0 did) and pays a handshake each time.
about
about  Watch the request and status lines in the Packets view, and compare the two page load
about  times in the log. Try: raise `delay` and see the gap widen; change the number or size
about  of the objects; ask for a missing page with `h1 http get www/missing` in the console.
about[it]  Un browser (h1) carica una pagina da un server web (www) a due reti di distanza. La
about[it]  pagina è un documento HTML che incorpora alcuni oggetti, quindi il client invia più
about[it]  richieste GET. Il primo caricamento riusa una sola connessione TCP persistente
about[it]  (HTTP/1.1); il secondo apre una nuova connessione per ogni oggetto (come HTTP/1.0) e
about[it]  paga ogni volta un handshake.
about[it]
about[it]  Osserva le righe di richiesta e di stato nella vista Pacchetti e confronta i due
about[it]  tempi di caricamento nel registro. Prova: aumenta `delay` e guarda il divario
about[it]  crescere; cambia numero o dimensione degli oggetti; chiedi una pagina inesistente
about[it]  con `h1 http get www/missing` nella console.
param  delay 30ms One-way delay between the routers
param[it] delay Ritardo fra i router (una direzione)
param  objects 3 Objects embedded in the page
param[it] objects Oggetti incorporati nella pagina
param  objsize 5000 Size of each object (bytes)
param[it] objsize Dimensione di ogni oggetto (byte)

node h1  host   x=10 y=50
node r1  router x=35 y=50
node r2  router x=65 y=50
node www host   x=90 y=50 role=WEB
link h1 r1  delay=2ms bw=100M
link r1 r2  delay=$delay bw=10M
link r2 www delay=2ms bw=100M
auto all

run www http server objects=$objects objsize=$objsize
at 1s run h1 http get www/index.html keepalive=1
at 4s run h1 http get www/index.html keepalive=0

note 1s GET /index.html on a new TCP connection; then one GET per embedded object on the same connection.
note[it] 1s GET /index.html su una nuova connessione TCP; poi un GET per ogni oggetto incorporato sulla stessa connessione.
note 4s Same page, but "Connection: close": every object needs its own handshake. Compare the total time.
note[it] 4s Stessa pagina, ma «Connection: close»: ogni oggetto richiede il suo handshake. Confronta il tempo totale.

duration 8s
expect h1 http.pages == 2
expect h1 http.failed == 0
expect h1 http.errors == 0
expect www http.served >= 2
