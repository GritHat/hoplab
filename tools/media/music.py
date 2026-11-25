#!/usr/bin/env python3
""""Round Trip" - the netlab theme, synthesised from scratch (numpy only).

    music.py out.wav [seconds]

A small electronic piece in A minor at 96 BPM written for the demo video:
it opens with lone "pings" answered by their echo on the other channel (a
request and its reply), a pad and a bass join, then an arpeggio that climbs
like a congestion window and falls back, drums, a half-time bridge, and a
last chorus that thins out to the two pings it started with. Every sound is
computed here: plucked and pad voices by additive synthesis, a sine bass, a
swept-sine kick, noise hats, a stereo feedback delay.
"""
import sys
import wave

import numpy as np

SR = 44100
BPM = 96.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT

NOTE = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}


def hz(name):
    """'A3' -> 220.0"""
    return 440.0 * 2 ** ((NOTE[name[:-1]] + 12 * (int(name[-1]) - 4) - 9) / 12.0)


def env(n, attack, decay_tau):
    t = np.arange(n) / SR
    return np.minimum(t / max(attack, 1e-4), 1.0) * np.exp(-t / decay_tau)


def pluck(f, dur, bright=1.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for k in range(1, 9):                       # each harmonic fades faster than the one below
        out += np.sin(2 * np.pi * f * k * t) / k ** (2.2 - bright) * np.exp(-t * (3.0 + 2.2 * k))
    return out * np.minimum(t / 0.004, 1.0)


def pad(freqs, dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for f in freqs:
        for det in (-0.004, 0.0, 0.004):        # three slightly detuned voices
            out += np.sin(2 * np.pi * f * (1 + det) * t) + 0.3 * np.sin(2 * np.pi * 2 * f * (1 + det) * t)
    shape = np.minimum(t / 0.6, 1.0) * np.minimum((dur - t) / 0.6, 1.0)
    return out * np.clip(shape, 0, 1) / (3 * len(freqs))


def bass(f, dur):
    n = int(dur * SR)
    t = np.arange(n) / SR
    return (np.sin(2 * np.pi * f * t) + 0.35 * np.sin(4 * np.pi * f * t)) * env(n, 0.006, dur * 0.7)


def kick():
    n = int(0.28 * SR)
    t = np.arange(n) / SR
    phase = 2 * np.pi * (45 * t + (120 - 45) * 0.045 * (1 - np.exp(-t / 0.045)))
    return np.sin(phase) * np.exp(-t / 0.11)


def hat(rng, open_=False):
    n = int((0.16 if open_ else 0.05) * SR)
    noise = np.diff(rng.standard_normal(n + 1))  # crude high-pass
    return noise * np.exp(-np.arange(n) / SR / (0.05 if open_ else 0.012)) * 0.5


def ping(f):
    n = int(0.5 * SR)
    t = np.arange(n) / SR
    return (np.sin(2 * np.pi * f * t) + 0.25 * np.sin(2 * np.pi * 2.01 * f * t)) * env(n, 0.002, 0.11)


class Mix:
    def __init__(self, seconds):
        self.buf = np.zeros((int(seconds * SR) + SR * 3, 2))

    def add(self, sound, at, gain=1.0, pan=0.0):
        i = int(at * SR)
        if i >= len(self.buf):
            return
        sound = sound[: len(self.buf) - i]
        self.buf[i:i + len(sound), 0] += sound * gain * np.sqrt((1 - pan) / 2)
        self.buf[i:i + len(sound), 1] += sound * gain * np.sqrt((1 + pan) / 2)


def delay(buf, seconds, feedback, mix):
    """Stereo ping-pong delay"""
    d = int(seconds * SR)
    out = buf.copy()
    tap = buf.copy()
    for _ in range(5):
        tap = np.roll(tap[:, ::-1], d, axis=0) * feedback
        tap[:d] = 0
        out += tap * mix
    return out


CHORDS = {
    "Am": ["A2", "A3", "C4", "E4"], "F": ["F2", "A3", "C4", "F4"], "C": ["C3", "G3", "C4", "E4"],
    "G": ["G2", "G3", "B3", "D4"], "Dm": ["D3", "A3", "D4", "F4"], "E": ["E2", "G#3", "B3", "E4"],
}
VERSE = ["Am", "F", "C", "G"]
BRIDGE = ["Dm", "Am", "E", "E"]


def compose(seconds):
    rng = np.random.default_rng(42)
    bars = max(16, int(np.ceil(seconds / BAR)))
    lead, backing, drums = Mix(bars * BAR), Mix(bars * BAR), Mix(bars * BAR)
    bridge_at = bars // 2                       # four bars of bridge in the middle
    for b in range(bars):
        t0 = b * BAR
        in_bridge = bridge_at <= b < bridge_at + 4
        chord = (BRIDGE if in_bridge else VERSE)[b % 4]
        root, *tones = CHORDS[chord]
        intro = b < 4
        outro = b >= bars - 3
        full = not intro and not outro and not in_bridge

        backing.add(pad([hz(n) for n in tones], BAR + 0.5), t0, 0.5 if not intro else 0.3)
        # request ... reply: the motif of the piece
        if intro or outro or b % 4 == 0:
            lead.add(ping(hz("E6")), t0, 0.5, -0.7)
            lead.add(ping(hz("A5")), t0 + 1.5 * BEAT, 0.4, 0.7)
        if b >= 2 and not outro:
            pattern = [0, 0.75, 1.5, 2.5, 3.0, 3.5] if full else [0, 1.5, 2.5]
            for beat in pattern:
                backing.add(bass(hz(root), BEAT * 0.7), t0 + beat * BEAT, 0.9)
        if b >= 4 and not outro:
            # the arpeggio climbs through the chord, a little further every
            # bar of the phrase, then starts again: a sawtooth
            steps = 8 if in_bridge else 16
            climb = (b % 4) + 1
            for s in range(steps):
                idx = (s * climb) % (len(tones) * 2)
                f = hz(tones[idx % len(tones)]) * (2 if idx >= len(tones) else 1) * 2
                lead.add(pluck(f, 0.5, 0.9 if full else 0.5), t0 + s * BAR / steps,
                         0.34 * (0.7 + 0.3 * ((s % 4) == 0)), 0.5 * np.sin(s * 0.9))
        if b >= 6 and not outro:
            for beat in ([0, 2] if in_bridge else [0, 1, 2, 3]):
                drums.add(kick(), t0 + beat * BEAT, 0.9)
            for s in range(8):
                if in_bridge and s % 2:
                    continue
                drums.add(hat(rng, open_=(s == 7 and b % 4 == 3)), t0 + s * BEAT / 2,
                          0.22 if s % 2 else 0.12, 0.3 if s % 2 else -0.3)
    out = delay(lead.buf, 0.75 * BEAT, 0.45, 0.5) + backing.buf * 0.9 + drums.buf
    out = out[: int(seconds * SR)]
    fade = int(3.0 * SR)
    out[-fade:] *= np.linspace(1, 0, fade)[:, None] ** 2
    out[: int(0.05 * SR)] *= np.linspace(0, 1, int(0.05 * SR))[:, None]
    out = np.tanh(out * 1.1)                    # soft limiter
    return out / np.max(np.abs(out)) * 0.89


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "round-trip.wav"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 75.0
    pcm = (compose(seconds) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"{path}: {seconds:.1f} s")


if __name__ == "__main__":
    main()
