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

## The published video

The demo is on YouTube: <https://www.youtube.com/watch?v=bQqE4w2FR7c>. The same file is kept here as
`media/hoplab-demo.mp4`. The title, description, chapters and tags used for
the upload, in English and Italian, are in [`youtube.txt`](youtube.txt); if
the video is recorded again (new labs, new numbers), update that file and the
description on YouTube with it.
