# The demo video, the preview and the artwork

Everything in `media/` is generated from the program itself and can be rebuilt:

```sh
make
python3 tools/media/record.py          # needs tmux, ffmpeg, rsvg-convert, Pillow, numpy, DejaVu fonts
```

| File | What it is |
|---|---|
| `media/logo.svg`, `logo.png` | the logo (hand-written SVG) |
| `media/thumbnail.png` | 1280×720 thumbnail for the video |
| `media/social-card.png` | 1280×720 card for link previews (repository "social preview") |
| `media/preview.gif` | silent 15-second preview used at the top of the README |
| `media/hoplab-demo.mp4` | the one-minute demo, 1920×1080, with the soundtrack |
| `media/round-trip.ogg` | the soundtrack alone |
| `media/screenshot-*.png` | stills used in the README |

`record.py` drives the real program inside a private tmux server (its own
socket; your sessions are untouched), captures the screen as text with colour
codes twelve times a second, renders every capture with `ansi2png.py`, and
assembles the frames with ffmpeg. Nothing is mocked: what you see is what the
labs do with seed 1.

## The logo

Three nodes inside a terminal window: a host (circle), a router (diamond) and
a switch (square), in the colours the interface uses for them. The direct link
between host and router is dashed and carries a red cross: a packet was lost
there. The arc above it reaches the router all the same, carrying a packet:
the retransmission that gets through. It is the idea the repository started
from — reliable transfer over an unreliable channel — and what every protocol
in the lab does in its own way.

## The soundtrack: "Round Trip"

Written for this video and synthesised from scratch by
[`tools/media/music.py`](../tools/media/music.py) (numpy only, no samples):
A minor, 96 BPM. It opens with a lone high "ping" on the left answered by a
lower one on the right — a request and its reply — which stays as the motif.
A pad and a bass join, then a plucked arpeggio that climbs a little further
every bar of each phrase and falls back, like a congestion window; drums, a
half-time bridge on Dm–Am–E, a last chorus, and an ending that thins out to
the two pings. `python3 tools/media/music.py out.wav 75` renders any length.

## Publishing the video

The video is in the repository but is not on YouTube: uploading needs your
account. After uploading `media/hoplab-demo.mp4` with `media/thumbnail.png`,
replace the link marked `VIDEO` near the top of `README.md` (and the same link
in `README.it.md`) with the YouTube URL.

Suggested metadata:

**Title**

> hoplab — a network laboratory in your terminal (every packet, every layer, 71 labs)

**Description**

> hoplab emulates a whole network inside one terminal program: hosts, routers
> and switches exchange real, byte-exact frames over links with delay, loss and
> queues. Watch packets cross the map, open any of them down to the bytes, read
> every node's tables, plot what the protocols measure, break things and see
> what happens.
>
> 30+ protocols and tools — ARP, switching, VLANs, spanning tree, IPv4, ICMP,
> IPv6, RIP, OSPF, BGP, NAT, queue management (RED, CoDel, fair queueing), UDP, TCP
> congestion control (Reno, CUBIC, BBR), load balancing, DNS, DHCP, HTTP, error
> correcting codes, medium access, Raft, gossip/SWIM, vector clocks, two-phase
> commit, Chord, CRDTs — 71 ready-made labs in four levels, and a study guide
> from the first ping to consensus algorithms. One window, tiled windows as
> separate processes, or several machines. C11, no dependencies, English and
> Italian.
>
> 0:00 hoplab
> 0:03 ARP and ping across a router, as a map, a sequence diagram, and byte by byte
> 0:18 TCP congestion control: the sawtooth
> 0:28 OSPF
> 0:36 Raft leader election
> 0:46 Catalog and study guide
> 1:00 One process per node, joined by UDP tunnels
>
> Source: <repository URL>
> Music: "Round Trip", synthesised by a script in the repository.

**Tags**: computer networks, networking course, TCP, congestion control, OSPF,
BGP, Raft, distributed systems, network simulator, terminal, TUI, C, education

(Adjust the chapter times to the final cut: `record.py` prints the length, and
the segment durations are at the top of the script.)
</content>
