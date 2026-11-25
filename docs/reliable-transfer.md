# Reliable transfer over UDP: the classic demos

The repository started from these six programs, and they are still here:
implementations in C of the textbook reliable-data-transfer protocols on top of
**real UDP sockets**, with a deliberately unreliable channel and a terminal
interface that shows the packets in flight between sender and receiver.

| Protocol | Sender | Receiver |
|---|---|---|
| Stop-and-wait (alternating bit, rdt 3.0) | `stop_and_wait_send` | `stop_and_wait_recv` |
| Go-Back-N | `go_back_n_send` | `go_back_n_recv` |
| Selective Repeat | `selective_repeat_send` | `selective_repeat_recv` |

They differ from the netlab labs in one way worth knowing: here two real
processes exchange real datagrams through the operating system, in real time;
in netlab everything above the cable is emulated and runs on virtual time.

## Running them

```sh
make                 # builds everything
./demo.sh            # stop-and-wait: sender (left) and receiver (right) side by side
./demo.sh gbn        # Go-Back-N
./demo.sh sr         # Selective Repeat
```

or choose **Reliable transfer demos** in the `./netlab` menu.

`demo.sh` opens sender and receiver in a new maximised window of the terminal
you launch it from, when it is one of the supported ones: kitty,
gnome-terminal, ptyxis, kgx, konsole, xfce4-terminal, mate-terminal, tilix,
terminator, lxterminal, alacritty, foot, wezterm or xterm. Otherwise (for
example from the VS Code terminal) it uses kitty if present, then the first of
the others it finds (or x-terminal-emulator). `-m` picks the mode:

- `kitty`: one maximised kitty window split in two;
- `terminal`: the same without kitty: a window of the current terminal, or of
  the first one found, split in two;
- `split`: the current terminal split in two with no additional program. It is
  what `terminal` uses inside the new window, and what is chosen automatically
  when there is no graphical session (Linux console, ssh); `q` closes;
- `gnome`: two separate gnome-terminal windows, each on half of the primary
  monitor (on Wayland through a gnome-terminal instance on XWayland, because
  Wayland windows cannot choose where they open);
- `tmux`: two tmux panes side by side (chosen automatically inside tmux).

```sh
./demo.sh -p 9000 -s "-l 0.3 -c 0.2" -r "-l 0.3"   # port, sender and receiver options
./demo.sh sr -w 6 -b 4                             # window and sequence-number bits
./demo.sh -m split gbn                             # in the current terminal
RDT_TERMINAL=konsole ./demo.sh                     # prefer konsole to any other
```

In the split modes sender and receiver are side by side when the window is at
least 145 columns wide; when it is narrow but tall (49 rows or more) they are
stacked. The layout follows the window as you resize it.

On the Linux text console (`TERM=linux`, for example Ctrl+Alt+F3) the
interface uses the 16 console colours and only characters present in its
fonts (code page 437). `RDT_BASIC=1` forces this mode in any terminal,
`RDT_BASIC=0` excludes it.

By hand, in two terminals (receiver first):

```sh
./go_back_n_recv [options] [host] [port]
./go_back_n_send [options] [host] [port]
```

They talk on `127.0.0.1:8142` by default — or across two machines if you give
the receiver's address. `-h` describes every option; the main ones:

| Option | Meaning | Default |
|---|---|---|
| `-a HOST`, `-p PORT` | receiver to contact / address to listen on | `127.0.0.1`, `8142` |
| `-l P` | probability of losing a packet sent (DATA or ACK) | 15% |
| `-c P` | probability of flipping a bit of a packet sent | 10% |
| `-d MS` | simulated propagation delay | 1000 ms |
| `-t MS` | retransmission timeout (sender only) | 2.5·delay + 500 |
| `-w N` | window (Go-Back-N, Selective Repeat) | 4 |
| `-b K` | sequence-number bits: numbers 0…2^K−1 (Go-Back-N, Selective Repeat) | 3 |
| `-i MS` | minimum interval between two transmissions (window senders) | delay/4 |
| `-m TEXT`, `-f FILE`, `-s N` | data to send and bytes per packet (sender only) | test sentence, 10 |
| `-S N` | random seed, to repeat the same run | random |

Sender and receiver must use the same `-b`, and for Selective Repeat the same
`-w` (the Go-Back-N receiver always has a window of 1). Each process applies
loss and corruption to the packets *it* sends: the sender to DATA, the
receiver to ACKs. Press `q` to quit. When the output is redirected to a file
the programs print only the event log as plain text.

The interface language follows `NETLAB_LANG` / `LANG` (English and Italian).

## Stop-and-wait

Every packet carries one sequence bit (0/1) and the ACK carries the same bit.

- The sender transmits DATA *b*, starts the timer and waits for ACK *b*. An ACK
  with the other bit refers to the previous transfer, so DATA *b* must be sent
  again; the same happens if the ACK is corrupted or the timer expires.
- A receiver that gets an intact, expected DATA *b* delivers it and answers
  ACK *b*. A duplicate (the other bit) is not delivered again but its ACK is
  repeated; for a corrupted packet it repeats the ACK of the last correct bit.
- A FIN, numbered and acknowledged like data, closes the transfer.

With a timeout shorter than the round-trip time (2·delay) the sender
retransmits before the ACK can arrive, so from then on every packet is sent at
least twice (the header warns about it).

## Go-Back-N

The sender keeps up to N packets in flight, the window [base, base+N−1], with
sequence numbers modulo 2^K. A single timer watches packet `base`.

- **Sender.** Sends packet `nextseqnum` when `nextseqnum < base+N`, starting
  the timer if the window was empty. ACK *n* is cumulative: it confirms every
  packet up to *n*, so `base = n+1` and the timer restarts, or stops if nothing
  is left in flight. When the timer expires **all** packets from `base` to
  `nextseqnum−1` are sent again. Corrupted or duplicate ACKs are ignored.
- **Receiver.** Has no buffer: it accepts only `expectedseqnum`, delivers it
  and answers with that number. Anything else, out of order or corrupted, is
  discarded and the ACK of the last in-order packet is repeated.

N ≤ 2^K − 1 is required: with N = 2^K, if all the ACKs of a window are lost,
the ACK of the packet before `base` cannot be told from that of the last
packet in flight (`./demo.sh gbn -w 8 -b 3` shows it).

## Selective Repeat

As in Go-Back-N the sender window holds N packets, but each has its own timer
and the receiver acknowledges packets one by one.

- **Sender.** ACK *n* inside the window marks *n* as acknowledged and stops its
  timer; if *n* is `send_base` the window slides to the first packet not yet
  acknowledged. When the timer of *n* expires **only** *n* is sent again.
- **Receiver.** Has a window of N positions from `rcv_base`. A packet in the
  window is acknowledged and buffered; if it is `rcv_base` it is delivered
  together with all the following ones already buffered. A packet in
  [`rcv_base`−N, `rcv_base`−1] was already delivered: its ACK is repeated,
  because evidently the sender did not get it. Everything else, and corrupted
  packets, are ignored without an ACK.

N ≤ 2^(K−1) is required, otherwise the receiver's two windows overlap and an
old retransmitted packet can be delivered as new: with
`./demo.sh sr -w 3 -b 2 -s "-l 0.4" -r "-l 0.4"` the received text ends up
with repeated pieces. In both protocols the header flags windows that are too
large.

## What the interface shows

Besides the channel and the event log, each window shows:

- the rules of the protocol, with the one just applied highlighted;
- the sequence-number strip: one cell per packet with its number (modulo 2^K),
  the pointers (`base`/`send_base`, `nextseqnum`, `expectedseqnum`,
  `rcv_base`), the window and, under the cells, the timers.

The sender transmits at most one packet every `-i` milliseconds (the simulated
transmission time), so the packets of a window travel one behind the other
instead of overlapping. With N·interval ≥ RTT the channel is never empty.

## Files

- `rdt.h/.c`: packet format (12-byte header and Internet checksum),
  command-line options, data to send, sockets, time and random numbers.
- `channel.h/.c`: simulated unreliable channel (loss, corruption, delay) and
  packets in flight.
- `tui.h/.c`: terminal graphics (panels, event log, channel animation, rules,
  window strip), drawing in half of the screen (`RDT_PANE`) and the Linux
  console mode. netlab's interface is built on the same library.
- `i18n.h/.c`: message catalogs (shared with netlab).
- `stop_and_wait_*.c`, `go_back_n_*.c`, `selective_repeat_*.c`: senders and
  receivers of the three protocols.
- `demo.sh`: builds and opens sender and receiver side by side.
