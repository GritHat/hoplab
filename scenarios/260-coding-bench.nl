title      The coding bench: codes at work, bit by bit
title[it]  Il banco dei codici: i codici al lavoro, bit per bit
level  2
layer  2
topic  coding
about  No network here: one node runs the coding bench, which encodes a short text, flips chosen bits, decodes and explains what happened. Open the node's `coding` panel: data bits are blue, redundancy bits yellow, flipped bits red.
about  The script walks through the classic cases: one flip against a parity bit (caught), two flips (missed: the wrong text is delivered), the same two flips against a CRC (caught), one flip in a Hamming(7,4) block (the syndrome names the bit, which is flipped back), two flips in the same block (the syndrome points at an innocent bit: miscorrection), the same against Hamming(8,4) whose extra parity bit recognises the double error, three copies with a majority vote, and a burst of 80 bits repaired by Reed-Solomon. It ends with a sweep of the chosen code over a range of bit error rates.
about  Try your own: `lab coding show text=NETLAB code=hamming74 flips=3`, `... burst=1`, `... pos=0,1,2`; `lab coding sweep code=rs size=500 burst=40`; `lab coding list`.
about[it]  Qui non c'è rete: un nodo esegue il banco dei codici, che codifica un breve testo, inverte i bit scelti, decodifica e spiega cos'è successo. Apri il pannello `coding` del nodo: i bit di dati sono blu, quelli di ridondanza gialli, quelli invertiti rossi.
about[it]  Lo script percorre i casi classici: un'inversione contro un bit di parità (scoperta), due inversioni (sfuggite: viene consegnato il testo sbagliato), le stesse due contro un CRC (scoperte), un'inversione in un blocco Hamming(7,4) (la sindrome indica il bit, che viene reinvertito), due nello stesso blocco (la sindrome indica un bit innocente: correzione sbagliata), le stesse contro Hamming(8,4), il cui bit di parità in più riconosce l'errore doppio, tre copie con voto a maggioranza, e un burst di 80 bit riparato da Reed-Solomon. Si chiude con una scansione del codice scelto su vari tassi di errore.
about[it]  Prova: `lab coding show text=NETLAB code=hamming74 flips=3`, `... burst=1`, `... pos=0,1,2`; `lab coding sweep code=rs size=500 burst=40`; `lab coding list`.
param      text HELLO Text to protect
param[it]  text Testo da proteggere
param      code hamming84 Code measured by the final sweep
param[it]  code Codice misurato dalla scansione finale
param+     frames 1500 Frames per point of the sweep
param+[it] frames Frame per punto della scansione

node lab host x=50 y=50

run lab coding show text=$text code=parity pos=3
at 3s  run lab coding show text=$text code=parity pos=3,4
at 6s  run lab coding show text=$text code=crc32 pos=3,4
at 9s  run lab coding show text=$text code=hamming74 pos=9
at 12s run lab coding show text=$text code=hamming74 pos=7,8
at 15s run lab coding show text=$text code=hamming84 pos=8,9
at 18s run lab coding show text=$text code=rep3 flips=3
at 21s run lab coding show text=$text code=rs flips=80 burst=1
at 24s run lab coding sweep code=$code frames=$frames

note 0s      One parity bit, one flipped bit: the count of ones is odd, the receiver knows the frame is bad.
note[it] 0s  Un bit di parità, un bit invertito: il numero di uni è dispari, il receiver sa che il frame è rovinato.
note 3s      Two flips: the count is even again. The parity check passes and a wrong text is delivered.
note[it] 3s  Due inversioni: il numero di uni torna pari. Il controllo di parità passa e viene consegnato un testo sbagliato.
note 6s      The same two flips against CRC-32: caught.
note[it] 6s  Le stesse due inversioni contro CRC-32: scoperte.
note 9s      Hamming(7,4): the three parity checks that fail spell, in binary, the position of the wrong bit.
note[it] 9s  Hamming(7,4): i tre controlli di parità che falliscono scrivono, in binario, la posizione del bit sbagliato.
note 12s     Two flips in one block: the syndrome points at a third, innocent bit. The code "corrects" and delivers garbage.
note[it] 12s Due inversioni in un blocco: la sindrome indica un terzo bit, innocente. Il codice "corregge" e consegna dati sbagliati.
note 15s     Hamming(8,4) adds an overall parity bit: syndrome non-zero but parity right means two errors. Give up honestly.
note[it] 15s Hamming(8,4) aggiunge un bit di parità globale: sindrome non nulla ma parità giusta significa due errori. Meglio rinunciare.
note 18s     Three copies: wherever they disagree, the majority wins. Simple, and three times the bits.
note[it] 18s Tre copie: dove non concordano vince la maggioranza. Semplice, e il triplo dei bit.
note 21s     Reed-Solomon corrects bytes, not bits: a burst of 80 bits is only about ten bad bytes out of the sixteen it can repair.
note[it] 21s Reed-Solomon corregge byte, non bit: un burst di 80 bit sono solo una decina di byte sbagliati sui sedici che sa riparare.
note 24s     The sweep: frames delivered, discarded and silently wrong as the bit error rate grows.
note[it] 24s La scansione: frame consegnati, scartati e sbagliati in silenzio al crescere del tasso di errore.

duration 26s
expect lab coding.show_ok == 1
expect lab coding.show_fixed >= 8
expect lab coding.sweeps == 1
