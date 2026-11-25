#!/usr/bin/env python3
"""Record the hoplab demo: screenshots, preview GIF, video with music.

    tools/media/record.py [--out media] [--only ping,tcp] [--keep]

It drives the real program inside a private tmux server (its own socket: your
tmux sessions are not touched), captures the screen as text with colour codes
several times a second, renders every capture to an image (ansi2png.py), and
assembles the frames with ffmpeg. The soundtrack is synthesised by music.py.

Needs: tmux, ffmpeg, rsvg-convert, python3 with Pillow and numpy, and the
DejaVu fonts. Run it from the repository root after `make`.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from multiprocessing import Pool

from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ansi2png  # noqa: E402

COLS, ROWS, SIZE = 160, 44, 18
W, H = 1920, 1080
FPS = 12
SOCK = "hoplab-media"
SANS = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
SANS_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
MONO_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
BG = (16, 17, 22)
BLUE, GREEN, YELLOW, PINK = (95, 175, 255), (135, 215, 135), (255, 215, 95), (255, 135, 215)

# name, command, seconds, [(time, tmux keys...)], [(time, caption)]
SEGMENTS = [
    ("ping", "./hoplab run 101-ping --seed 1", 15.0,
     [(8.6, "s"), (12.0, "p"), (12.4, "Up", "Up", "Up", "Up", "Enter"), (13.2, "]", "]", "]", "]"),
      (14.0, "]", "]", "]")],
     [(0, "Real frames on an emulated network: ARP, then the ping crosses the router"),
      (8.6, "The same exchange as a sequence diagram"),
      (12.0, "Every packet, dissected field by field down to the bytes")]),
    ("tcp", "./hoplab run 320-tcp-congestion --seed 1 --speed 2.5", 10.0,
     [(0.4, "Tab", "Left"), (6.5, "g")],
     [(0, "TCP congestion control on a bottleneck: the sawtooth, live"),
      (6.5, "Any quantity a protocol records can be plotted")]),
    ("ospf", "./hoplab run 232-ospf-costs --seed 1", 8.0,
     [(0.4, "Tab", "Left")],
     [(0, "Routing protocols at work: OSPF floods link state and runs Dijkstra")]),
    ("raft", "./hoplab run 380-raft-election --seed 1 --speed 0.6", 10.0,
     [(0.4, "Tab", "Left"), (5.0, "l")],
     [(0, "Distributed algorithms too: Raft elects a leader, loses it, elects another")]),
    ("menu", "./hoplab", 14.0,
     [(1.2, "1"), (2.0, "Down"), (2.5, "Down"), (3.0, "Down"), (3.5, "Down"), (4.0, "PageDown"),
      (4.6, "PageDown"), (5.2, "PageDown"), (5.8, "PageDown"), (6.6, "Escape"), (7.2, "2"),
      (8.2, "Down"), (8.6, "Down"), (9.0, "Down"), (9.4, "Down"), (9.8, "Down"), (10.6, "Right"),
      (11.4, "3"), (12.2, "PageDown"), (13.0, "PageDown")],
     [(0, "One menu: catalog, study guide, topology wizard"),
      (1.2, "77 ready-made labs in four levels, from the first ping to consensus"),
      (7.2, "A study guide by level and topic, readable at three depths, in English and Italian")]),
    ("windows", "WINDOWS", 11.0, [],
     [(0, "The same lab as separate processes joined by UDP tunnels — on one screen, or on several machines")]),
]


def tmux(*args, env=None, capture=False):
    cmd = ["tmux", "-L", SOCK] + list(args)
    if capture:
        return subprocess.run(cmd, env=env, capture_output=True, text=True).stdout
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return ""


def caption_at(captions, t):
    cur = ""
    for when, text in captions:
        if t >= when:
            cur = text
    return cur


def record_segment(seg, tmp):
    name, cmd, secs, keys, captions = seg
    inner_dir = os.path.join(tmp, "inner")
    env = dict(os.environ, HOPLAB_LANG="en", LANG="C.UTF-8")
    env.pop("TMUX", None)
    if cmd == "WINDOWS":
        # the tiled mode uses tmux itself: give it a private server too and
        # look at it through the recording pane
        os.makedirs(inner_dir, exist_ok=True)
        cmd = f"env -u TMUX TMUX_TMPDIR={inner_dir} ./hoplab windows 101-ping --mode tmux --seed 1"
    tmux("kill-session", "-t", "rec")
    tmux("new-session", "-d", "-s", "rec", "-x", str(COLS), "-y", str(ROWS), cmd, env=env)
    frames, pending, t0 = [], list(keys), time.time()
    n = int(secs * FPS)
    for i in range(n):
        target = t0 + i / FPS
        while time.time() < target:
            time.sleep(0.004)
        t = time.time() - t0
        while pending and pending[0][0] <= t:
            tmux("send-keys", "-t", "rec", *pending.pop(0)[1:])
        frames.append((tmux("capture-pane", "-t", "rec", "-e", "-p", capture=True), caption_at(captions, t)))
    tmux("send-keys", "-t", "rec", "q")
    time.sleep(0.2)
    tmux("kill-session", "-t", "rec")
    if os.path.isdir(inner_dir):
        ienv = dict(env, TMUX_TMPDIR=inner_dir)
        for s in subprocess.run(["tmux", "list-sessions", "-F", "#S"], env=ienv, capture_output=True,
                                text=True).stdout.split():
            subprocess.run(["tmux", "kill-session", "-t", s], env=ienv, stderr=subprocess.DEVNULL)
    print(f"  {name}: {len(frames)} frames")
    return frames


def grid_box():
    """Where the terminal grid sits inside a rendered frame (with a small margin)"""
    font = ImageFont.truetype(ansi2png.FONT, SIZE)
    asc, desc = font.getmetrics()
    gw, gh = int(font.getlength("M") * COLS + 0.5), (asc + desc) * ROWS
    cap = int(SIZE * 3.2)
    ox, oy = (W - gw) // 2, (H - cap - gh) // 2
    return (ox - 12, oy - 8, ox + gw + 12, oy + gh + 8)


def render_one(job):
    text, caption, path = job
    ansi2png.render(text, COLS, ROWS, SIZE, W, H, caption).save(path)
    return path


def logo(size):
    out = tempfile.NamedTemporaryFile(suffix=".png", delete=False).name
    subprocess.run(["rsvg-convert", "-w", str(size), "media/logo.svg", "-o", out], check=True)
    img = Image.open(out).convert("RGBA")
    os.unlink(out)
    return img


def card(lines, sub, small, w=W, h=H, shot=None):
    """Title / end card: logo on the left, text on the right."""
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    k = w / 1920.0
    if shot is not None:                        # a dimmed screenshot behind
        back = shot.resize((w, h)).filter(ImageFilter.GaussianBlur(3 * k))
        img = Image.blend(img, back, 0.22)
        d = ImageDraw.Draw(img)
    lg = logo(int(520 * k))
    img.paste(lg, (int(150 * k), (h - lg.height) // 2), lg)
    x = int(760 * k)
    big = ImageFont.truetype(MONO_BOLD, int(168 * k))
    mid = ImageFont.truetype(SANS_BOLD, int(52 * k))
    sm = ImageFont.truetype(SANS, int(36 * k))
    y = int(h / 2 - 250 * k)
    d.text((x, y), lines, font=big, fill=(240, 242, 248))
    y += int(215 * k)
    d.rectangle([x, y, x + int(140 * k), y + int(8 * k)], fill=BLUE)
    d.rectangle([x + int(150 * k), y, x + int(290 * k), y + int(8 * k)], fill=GREEN)
    d.rectangle([x + int(300 * k), y, x + int(440 * k), y + int(8 * k)], fill=YELLOW)
    y += int(40 * k)
    d.text((x, y), sub, font=mid, fill=(240, 242, 248))
    y += int(90 * k)
    for line in small:
        d.text((x, y), line, font=sm, fill=(170, 176, 190))
        y += int(54 * k)
    return img


def thumbnail(shot, out):
    """1280x720: logo, name, promise, and a real screenshot leaning in."""
    w, h = 1280, 720
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    for i in range(h):                          # a faint vertical gradient
        d.line([(0, i), (w, i)], fill=(16 + i // 60, 17 + i // 50, 22 + i // 24))
    s = shot.crop((0, 0, shot.width, shot.height - 60)).resize((900, 478))
    mask = Image.new("L", s.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s.width - 1, s.height - 1], 22, fill=255)
    frame = Image.new("RGBA", (s.width + 12, s.height + 12), (0, 0, 0, 0))
    ImageDraw.Draw(frame).rounded_rectangle([0, 0, frame.width - 1, frame.height - 1], 26, fill=BLUE + (255,))
    frame.paste(s, (6, 6), mask)
    frame = frame.rotate(-5, expand=True, resample=Image.BICUBIC)
    shadow = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 170), (0, 0, frame.width, frame.height), frame.split()[3])
    img.paste(shadow.filter(ImageFilter.GaussianBlur(18)), (560, 250), shadow.filter(ImageFilter.GaussianBlur(18)))
    img.paste(frame, (540, 225), frame)
    d = ImageDraw.Draw(img)
    lg = logo(250)
    img.paste(lg, (50, 40), lg)
    d.text((320, 62), "hoplab", font=ImageFont.truetype(MONO_BOLD, 150), fill=(245, 246, 250),
           stroke_width=6, stroke_fill=BG)
    d.text((60, 330), "Watch the network\nthink.", font=ImageFont.truetype(SANS_BOLD, 62), fill=YELLOW,
           stroke_width=8, stroke_fill=BG, spacing=10)
    f = ImageFont.truetype(SANS_BOLD, 34)
    y = 520
    for text, col in (("every packet, every layer", BLUE), ("30+ protocols · 77 labs", GREEN),
                      ("in your terminal", PINK)):
        d.text((60, y), text, font=f, fill=col, stroke_width=6, stroke_fill=BG)
        y += 52
    img.save(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="media")
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", action="store_true", help="keep the frames directory")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    # HOPLAB_MEDIA_TMP: a SHORT directory (tmux socket paths are limited to about 100 bytes)
    tmp = tempfile.mkdtemp(prefix="nlm-", dir=os.environ.get("HOPLAB_MEDIA_TMP"))
    fdir = os.path.join(tmp, "frames")
    os.makedirs(fdir)
    only = [s for s in a.only.split(",") if s]

    print("recording")
    recorded = {}
    for seg in SEGMENTS:
        if only and seg[0] not in only:
            continue
        recorded[seg[0]] = record_segment(seg, tmp)

    print("rendering")
    jobs, index, first = [], 0, {}
    title_frames, end_frames = int(3.5 * FPS), int(5 * FPS)
    index += title_frames                       # the title card goes first
    for name, frames in recorded.items():
        first[name] = index
        for text, caption in frames:
            jobs.append((text, caption, os.path.join(fdir, f"{index:05d}.png")))
            index += 1
    with Pool() as pool:
        pool.map(render_one, jobs, chunksize=8)

    def frame(name, t):
        return Image.open(os.path.join(fdir, f"{first[name] + int(t * FPS):05d}.png"))

    hero = frame("tcp", 9.0) if "tcp" in first else Image.open(jobs[len(jobs) // 2][2])
    title = card("hoplab", "A network laboratory in your terminal",
                 ["Every packet, every layer, every protocol —", "from the first ping to consensus algorithms."],
                 shot=hero)
    for i in range(title_frames):
        title.save(os.path.join(fdir, f"{i:05d}.png"))
    end = card("hoplab", "make && ./hoplab",
               ["30+ protocols and tools · 77 labs · a study guide in four levels",
                "One process, tiled windows, or several machines",
                "C11, no dependencies · English and Italian"], shot=hero)
    for i in range(end_frames):
        end.save(os.path.join(fdir, f"{index + i:05d}.png"))
    total = index + end_frames
    seconds = total / FPS

    # stills for the README
    stills = {"ping": ("ping", 5.0), "sequence": ("ping", 11.0), "dissection": ("ping", 14.6),
              "tcp": ("tcp", 6.0), "graphs": ("tcp", 9.5), "ospf": ("ospf", 6.0), "raft": ("raft", 4.5),
              "catalog": ("menu", 6.0), "guide": ("menu", 13.5), "windows": ("windows", 9.5)}
    for out, (name, t) in stills.items():
        if name in first:
            box = grid_box()
            shot = frame(name, t).crop(box)
            shot.resize((1440, 1440 * shot.height // shot.width)).save(
                os.path.join(a.out, f"screenshot-{out}.png"), optimize=True)
    thumbnail(hero, os.path.join(a.out, "thumbnail.png"))
    title.resize((1280, 720)).save(os.path.join(a.out, "social-card.png"), optimize=True)

    if not only:
        print(f"music and video ({seconds:.1f} s)")
        wav = os.path.join(tmp, "round-trip.wav")
        subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "music.py"), wav, f"{seconds:.2f}"],
                       check=True)
        ff = ["ffmpeg", "-v", "error", "-y"]
        subprocess.run(ff + ["-framerate", str(FPS), "-i", os.path.join(fdir, "%05d.png"), "-i", wav,
                             "-c:v", "libx264", "-preset", "slow", "-crf", "23", "-pix_fmt", "yuv420p", "-r", "24",
                             "-c:a", "aac", "-b:a", "160k", "-shortest", "-movflags", "+faststart",
                             os.path.join(a.out, "hoplab-demo.mp4")], check=True)
        subprocess.run(ff + ["-i", wav, "-c:a", "libvorbis", "-q:a", "5", os.path.join(a.out, "round-trip.ogg")],
                       check=True)
        # silent preview for the README: the ping lab, small
        s0, n = first["ping"], int(15.0 * FPS)
        subprocess.run(ff + ["-framerate", str(FPS), "-start_number", str(s0), "-i",
                             os.path.join(fdir, "%05d.png"), "-frames:v", str(n), "-vf",
                             "crop=%d:%d:%d:%d,scale=960:-1:flags=lanczos,split[a][b];" % (
                                 grid_box()[2] - grid_box()[0], grid_box()[3] - grid_box()[1], grid_box()[0],
                                 grid_box()[1]) +
                             "[a]palettegen=max_colors=64[p];[b][p]paletteuse=dither=none",
                             os.path.join(a.out, "preview.gif")], check=True)
    if a.keep:
        print("frames kept in", fdir)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    tmux("kill-session", "-t", "rec")
    print("done:", ", ".join(sorted(os.listdir(a.out))))


if __name__ == "__main__":
    main()
