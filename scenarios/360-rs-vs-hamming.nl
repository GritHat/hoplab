title      Reed-Solomon against Hamming: random errors, bursts and interleaving
title[it]  Reed-Solomon contro Hamming: errori casuali, burst e interleaving
level  3
layer  2
topic  coding
about  Two very noisy links (one bit in a thousand is wrong) carry the same traffic. h1–h2 uses Hamming(7,4): rate 4/7, one correctable bit per 7-bit block. r1–r2 uses Reed-Solomon (255,223): rate 0.87, sixteen correctable bytes per 255-byte block. Reed-Solomon spends a quarter of the redundancy and still delivers more: long blocks average the noise out, which is the lesson of Shannon's theorem.
about  Then the bench on node lab measures what the links cannot show: errors that arrive in bursts (as on radio channels and scratched disks). Plain Hamming collapses, because a burst puts several errors in one block. The same code behind an interleaver, which spreads each burst over many blocks, recovers. Reed-Solomon barely notices: a burst of 12 bits is two bad bytes.
about  Read the three tables in the log (or the lab's coding panel) row by row. Try: `lab coding sweep code=rs size=500 burst=200` to find where Reed-Solomon breaks; `code=rep3`; a longer `burst` than the interleaver depth.
about[it]  Due collegamenti molto rumorosi (un bit su mille è sbagliato) portano lo stesso traffico. h1–h2 usa Hamming(7,4): rate 4/7, un bit correggibile per blocco di 7. r1–r2 usa Reed-Solomon (255,223): rate 0,87, sedici byte correggibili per blocco di 255. Reed-Solomon spende un quarto della ridondanza e consegna comunque di più: i blocchi lunghi mediano il rumore, ed è la lezione del teorema di Shannon.
about[it]  Poi il banco sul nodo lab misura ciò che i collegamenti non possono mostrare: errori che arrivano a burst (come sui canali radio e sui dischi graffiati). Hamming da solo crolla, perché un burst mette più errori nello stesso blocco. Lo stesso codice dietro un interleaver, che distribuisce ogni burst su molti blocchi, si riprende. Reed-Solomon quasi non se ne accorge: un burst di 12 bit sono due byte sbagliati.
about[it]  Leggi le tre tabelle nel registro (o nel pannello coding di lab) riga per riga. Prova: `lab coding sweep code=rs size=500 burst=200` per trovare dove Reed-Solomon cede; `code=rep3`; un `burst` più lungo della profondità dell'interleaver.
param      ber 1e-3 Bit error rate of the two links
param[it]  ber Tasso di errore sul bit dei due collegamenti
param      burst 12 Burst length (bits) in the bench
param[it]  burst Lunghezza dei burst (bit) nel banco
param+     frames 1000 Frames per point of each sweep
param+[it] frames Frame per punto di ogni scansione

node h1 host x=15 y=20
node h2 host x=85 y=20
node r1 host x=15 y=50
node r2 host x=85 y=50
node lab host x=50 y=85
link h1 h2 bw=2M delay=5ms ber=$ber codec=hamming74
link r1 r2 bw=2M delay=5ms ber=$ber codec=rs
auto all

run h2 sink
run r2 sink
run h1 cbr h2 rate=80k size=500 duration=10s
run r1 cbr r2 rate=80k size=500 duration=10s
at 11s run lab coding sweep code=hamming74 burst=$burst frames=$frames size=200
at 13s run lab coding sweep code=hamming74i burst=$burst frames=$frames size=200
at 15s run lab coding sweep code=rs burst=$burst frames=$frames size=200

note 1s      Same noise on both links. Hamming almost doubles every frame; Reed-Solomon adds 13%.
note[it] 1s  Stesso rumore sui due collegamenti. Hamming quasi raddoppia ogni frame; Reed-Solomon aggiunge il 13%.
note 10s     Compare sink.received on h2 and r2: the cheaper code delivered more.
note[it] 10s Confronta sink.received su h2 e r2: il codice meno costoso ha consegnato di più.
note 11s     Bench, errors in bursts. First plain Hamming(7,4): a burst means several errors in one block.
note[it] 11s Banco, errori a burst. Prima Hamming(7,4) da solo: un burst significa più errori nello stesso blocco.
note 13s     Same code, interleaved: each burst now touches every block at most once.
note[it] 13s Stesso codice con interleaving: ogni burst ora tocca ciascun blocco al più una volta.
note 15s     Reed-Solomon: a burst is just a couple of bad bytes.
note[it] 15s Reed-Solomon: un burst è solo un paio di byte sbagliati.

duration 17s
expect r2 sink.received >= 195
expect h2 sink.received >= 150
expect lab coding.sweeps == 3
expect lab coding.hamming74.good <= 75
expect lab coding.hamming74i.good >= 80
expect lab coding.rs.good >= 85
