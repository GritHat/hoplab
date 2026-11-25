#!/usr/bin/env bash
#
# demo.sh - Builds and opens sender and receiver side by side (sender on the
#           left, receiver on the right) in a new window of the terminal it
#           is launched from, otherwise of kitty or another terminal of the
#           system, or with no graphical interface in the current terminal.
#
# Usage: ./demo.sh [options] [protocol]        (./demo.sh -h for help)

set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$DIR/$(basename "${BASH_SOURCE[0]}")"

# Messages are in English, or Italian when the language of the environment
# is Italian (same variables as the programs):  t "English" "Italiano"
case "${HOPLAB_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}" in
    it*) LANG_IT=1 ;;
    *)   LANG_IT=0 ;;
esac
t() {
    if [ "$LANG_IT" = 1 ]; then printf '%s' "$2"; else printf '%s' "$1"; fi
}
PRESS_ENTER="$(t 'Press ENTER to close... ' 'Premi INVIO per chiudere... ')"

# ---------------------------------------------------------------------------
# Internal mode: run inside each terminal.
#   demo.sh --run DELAY program arguments...
# Leaves the window open when the program ends, to read the summary.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--run" ]; then
    sleep "$2"
    shift 2
    cd "$DIR" || exit 1
    "$@"
    status=$?
    echo
    read -r -p "$PRESS_ENTER" _
    exit "$status"
fi

# Internal mode: demo.sh --window ... does the same, but runs in a window
# opened on purpose, which stays open at the end of the demo for the summary.
IN_WINDOW=0
if [ "${1:-}" = "--window" ]; then
    IN_WINDOW=1
    shift
    trap 'echo; read -r -p "$PRESS_ENTER" _' EXIT
fi
ORIG_ARGS=("$@")

usage_it() {
    cat <<EOF
Uso: ./demo.sh [opzioni] [protocollo]

Compila e avvia sender e receiver affiancati (sender a sinistra, receiver a
destra).

  protocollo     stop_and_wait (o sw, default), go_back_n (o gbn),
                 selective_repeat (o sr)
  -a HOST        indirizzo del receiver            (default 127.0.0.1)
  -p PORTA       porta UDP                         (default 8142)
  -w N           finestra (GBN e SR)               (default 4)
  -b K           bit dei numeri di sequenza        (default 3: numeri 0…7)
  -s "OPZIONI"   opzioni extra per il sender       es. -s "-l 0.3 -t 2500"
  -r "OPZIONI"   opzioni extra per il receiver     es. -r "-c 0"
  -m MODO        come aprire i terminali:
                   auto      una nuova finestra del terminale da cui lanci
                             la demo, se è tra quelli supportati; altrimenti
                             kitty, altrimenti il primo terminale trovato;
                             senza interfaccia grafica "split" (default)
                   kitty     una finestra kitty massimizzata divisa in due
                   terminal  come auto ma senza kitty: una finestra del
                             terminale corrente o del primo trovato fra
                             gnome-terminal, ptyxis, konsole, xfce4-terminal,
                             xterm, ... divisa in due
                   split     il terminale corrente diviso in due: funziona in
                             qualsiasi terminale, anche nella console di Linux
                             o via ssh; q chiude
                   gnome     due finestre gnome-terminal, ognuna su metà schermo
                   tmux      due pannelli tmux affiancati
  -h             mostra questo aiuto

-w e -b vengono passati a entrambi i programmi quando servono (il receiver
Go-Back-N non ha bisogno della finestra).

Variabili d'ambiente:
  RDT_TERMINAL=NOME   terminale da preferire con "auto" e "terminal"
                      (es. konsole)
  RDT_BASIC=1|0       forza o esclude 16 colori e caratteri della console di
                      Linux (di default solo con TERM=linux)

Esempi:
  ./demo.sh
  ./demo.sh gbn
  ./demo.sh -w 6 -b 4 sr
  ./demo.sh -p 9000 -s "-l 0.3 -c 0.2" -r "-l 0.3" go_back_n
  ./demo.sh -m split sr
  ./demo.sh -m gnome -s "-m 'ciao mondo' -d 600"
EOF
}

usage() {
    if [ "$LANG_IT" = 1 ]; then
        usage_it
        return
    fi
    cat <<EOF
Usage: ./demo.sh [options] [protocol]

Builds and starts sender and receiver side by side (sender on the left,
receiver on the right).

  protocol       stop_and_wait (or sw, default), go_back_n (or gbn),
                 selective_repeat (or sr)
  -a HOST        address of the receiver           (default 127.0.0.1)
  -p PORT        UDP port                          (default 8142)
  -w N           window (GBN and SR)               (default 4)
  -b K           bits of the sequence numbers      (default 3: numbers 0…7)
  -s "OPTIONS"   extra options for the sender      e.g. -s "-l 0.3 -t 2500"
  -r "OPTIONS"   extra options for the receiver    e.g. -r "-c 0"
  -m MODE        how to open the terminals:
                   auto      a new window of the terminal you launch the
                             demo from, if it is a supported one; otherwise
                             kitty, otherwise the first terminal found;
                             with no graphical interface "split" (default)
                   kitty     a maximized kitty window split in two
                   terminal  like auto but without kitty: a window of the
                             current terminal or of the first found among
                             gnome-terminal, ptyxis, konsole, xfce4-terminal,
                             xterm, ... split in two
                   split     the current terminal split in two: it works in
                             any terminal, also in the Linux console or over
                             ssh; q closes
                   gnome     two gnome-terminal windows, each on half screen
                   tmux      two tmux panes side by side
  -h             show this help

-w and -b are passed to both programs when they need them (the Go-Back-N
receiver does not need the window).

Environment variables:
  RDT_TERMINAL=NAME   terminal to prefer with "auto" and "terminal"
                      (e.g. konsole)
  RDT_BASIC=1|0       forces or excludes 16 colours and Linux console
                      characters (by default only with TERM=linux)
  HOPLAB_LANG=it|en   language of messages (default: from LANG)

Examples:
  ./demo.sh
  ./demo.sh gbn
  ./demo.sh -w 6 -b 4 sr
  ./demo.sh -p 9000 -s "-l 0.3 -c 0.2" -r "-l 0.3" go_back_n
  ./demo.sh -m split sr
  ./demo.sh -m gnome -s "-m 'hello world' -d 600"
EOF
}

HOST=127.0.0.1
PORT=8142
SENDER_OPTS=""
RECEIVER_OPTS=""
MODE=auto
WINDOW=""
BITS=""

# options may come both before and after the protocol
POSITIONAL=()
while [ $# -gt 0 ]; do
    OPTIND=1
    while getopts ":a:p:w:b:s:r:m:h" opt; do
        case "$opt" in
            a) HOST=$OPTARG ;;
            p) PORT=$OPTARG ;;
            w) WINDOW=$OPTARG ;;
            b) BITS=$OPTARG ;;
            s) SENDER_OPTS=$OPTARG ;;
            r) RECEIVER_OPTS=$OPTARG ;;
            m) MODE=$OPTARG ;;
            h) usage; exit 0 ;;
            :) echo "$(t "Option -$OPTARG requires an argument" "L'opzione -$OPTARG richiede un argomento")" >&2; usage >&2; exit 1 ;;
            *) echo "$(t "Unknown option: -$OPTARG" "Opzione sconosciuta: -$OPTARG")" >&2; usage >&2; exit 1 ;;
        esac
    done
    shift $((OPTIND - 1))
    if [ $# -gt 0 ]; then
        POSITIONAL+=("$1")
        shift
    fi
done
set -- "${POSITIONAL[@]}"
case "${1:-sw}" in
    sw|stop_and_wait|stop-and-wait)        PROTO=stop_and_wait ;;
    gbn|go_back_n|go-back-n)               PROTO=go_back_n ;;
    sr|selective_repeat|selective-repeat)  PROTO=selective_repeat ;;
    *) echo "$(t "Unknown protocol" "Protocollo sconosciuto"): $1 (stop_and_wait, go_back_n, selective_repeat)" >&2; exit 1 ;;
esac
if [ $# -gt 1 ]; then
    echo "$(t "Unexpected argument" "Argomento inatteso"): $2" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [ ! -s "$DIR/${PROTO}_send.c" ] || [ ! -s "$DIR/${PROTO}_recv.c" ]; then
    echo "$(t "Protocol '$PROTO' not available (missing ${PROTO}_send.c / ${PROTO}_recv.c)" "Protocollo '$PROTO' non disponibile (mancano ${PROTO}_send.c / ${PROTO}_recv.c)")" >&2
    exit 1
fi
make -s -C "$DIR" "${PROTO}_send" "${PROTO}_recv" || exit 1

# Extra options may contain quoted text (e.g. -m 'hello world')
eval "EXTRA_S=($SENDER_OPTS)" || exit 1
eval "EXTRA_R=($RECEIVER_OPTS)" || exit 1

# Window and sequence numbers: the sender always uses them, the Go-Back-N
# receiver only the sequence numbers (its window is 1)
WIN_S=() WIN_R=()
if [ "$PROTO" = stop_and_wait ]; then
    if [ -n "$WINDOW$BITS" ]; then
        echo "$(t "Note: -w and -b are of no use to stop-and-wait (one bit, one packet at a time), ignored." "Nota: -w e -b non servono a stop-and-wait (un bit, un pacchetto alla volta), ignorati.")" >&2
    fi
else
    [ -n "$WINDOW" ] && WIN_S+=(-w "$WINDOW")
    [ -n "$BITS" ] && WIN_S+=(-b "$BITS") && WIN_R+=(-b "$BITS")
    [ -n "$WINDOW" ] && [ "$PROTO" = selective_repeat ] && WIN_R+=(-w "$WINDOW")
fi

SEND_PROG=("./${PROTO}_send" -a "$HOST" -p "$PORT" "${WIN_S[@]}" "${EXTRA_S[@]}")
RECV_PROG=("./${PROTO}_recv" -a "$HOST" -p "$PORT" "${WIN_R[@]}" "${EXTRA_R[@]}")
SEND_CMD=("$SELF" --run 0.6 "${SEND_PROG[@]}")
RECV_CMD=("$SELF" --run 0   "${RECV_PROG[@]}")

# Arguments wrapped in single quotes, for kitty sessions and tmux commands
quote() {
    local a out=""
    for a in "$@"; do
        out+="'${a//\'/\'\\\'\'}' "
    done
    printf '%s' "$out"
}

has() { command -v "$1" >/dev/null 2>&1; }
gui() { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }

# ---------------------------------------------------------------------------
# kitty: one window split in two ("horizontal" layout = side by side)
# ---------------------------------------------------------------------------
run_kitty() {
    local session
    session=$(mktemp "${TMPDIR:-/tmp}/rdt-demo.XXXXXX") || return 1
    cat > "$session" <<EOF
new_tab RDT $PROTO
enabled_layouts horizontal
layout horizontal
launch --title SENDER $(quote "${SEND_CMD[@]}")
launch --title RECEIVER $(quote "${RECV_CMD[@]}")
EOF
    kitty --detach --start-as=maximized --title "RDT $PROTO" --session "$session" || return 1
    # kitty reads the session at start-up: the file can then be removed
    (sleep 20; rm -f "$session") >/dev/null 2>&1 &
    echo "$(t "Opened a kitty window: SENDER on the left, RECEIVER on the right." "Aperta una finestra kitty: SENDER a sinistra, RECEIVER a destra.")"
}

# ---------------------------------------------------------------------------
# gnome-terminal: two separate windows, each on half of the primary monitor
# ---------------------------------------------------------------------------
monitor_geometry() {
    local g=""
    if has xrandr; then
        g=$(xrandr --query 2>/dev/null | awk '/ connected primary/ { for (i = 1; i <= NF; i++)
                if ($i ~ /^[0-9]+x[0-9]+\+[0-9]+\+[0-9]+$/) { print $i; exit } }')
        [ -z "$g" ] && g=$(xrandr --query 2>/dev/null | awk '/ connected/ { for (i = 1; i <= NF; i++)
                if ($i ~ /^[0-9]+x[0-9]+\+[0-9]+\+[0-9]+$/) { print $i; exit } }')
    fi
    echo "${g:-1920x1080+0+0}"
}

terminal_font_size() {
    local prof key font
    prof=$(gsettings get org.gnome.Terminal.ProfilesList default 2>/dev/null | tr -d "'")
    key="org.gnome.Terminal.Legacy.Profile:/org/gnome/terminal/legacy/profiles:/:$prof/"
    if [ -n "$prof" ] && [ "$(gsettings get "$key" use-system-font 2>/dev/null)" = "false" ]; then
        font=$(gsettings get "$key" font 2>/dev/null)
    else
        font=$(gsettings get org.gnome.desktop.interface monospace-font-name 2>/dev/null)
    fi
    font=${font//\'/}
    font=${font##* }
    [[ "$font" =~ ^[0-9]+([.][0-9]+)?$ ]] && echo "$font" || echo 11
}

dbus_has_name() {
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner "$1" 2>/dev/null | grep -q true
}

run_gnome() {
    local W H X Y pt scale cols rows half app=() id=org.reti.RdtDemo i
    IFS='x+' read -r W H X Y <<< "$(monitor_geometry)"
    pt=$(terminal_font_size)
    scale=$(gsettings get org.gnome.desktop.interface text-scaling-factor 2>/dev/null || echo 1)
    # estimate of the terminal cells from the font size
    read -r cols rows < <(awk -v W="$W" -v H="$H" -v pt="$pt" -v s="$scale" 'BEGIN {
        px = pt * 96 / 72 * s; cw = px * 0.62; ch = px * 1.3;
        printf "%d %d\n", (W / 2 - 40) / cw, (H - 140) / ch }')
    cols=${RDT_COLS:-$cols}
    rows=${RDT_ROWS:-$rows}
    half=$((W / 2))

    # On Wayland windows cannot choose their own position: a private
    # instance of gnome-terminal running on XWayland is used.
    if [ "${XDG_SESSION_TYPE:-}" = "wayland" ] && [ -x /usr/libexec/gnome-terminal-server ] && has gdbus; then
        if ! dbus_has_name "$id"; then
            GDK_BACKEND=x11 setsid /usr/libexec/gnome-terminal-server --app-id "$id" >/dev/null 2>&1 &
            for i in $(seq 1 40); do
                dbus_has_name "$id" && break
                sleep 0.1
            done
        fi
        dbus_has_name "$id" && app=(--app-id "$id")
    fi

    gnome-terminal "${app[@]}" --geometry="${cols}x${rows}+${X}+${Y}" --title=SENDER \
        -- "${SEND_CMD[@]}" || return 1
    gnome-terminal "${app[@]}" --geometry="${cols}x${rows}+$((X + half))+${Y}" --title=RECEIVER \
        -- "${RECV_CMD[@]}" || return 1
    echo "$(t "Opened two gnome-terminal windows (${cols}x${rows}): SENDER on the left, RECEIVER on the right." "Aperte due finestre gnome-terminal (${cols}x${rows}): SENDER a sinistra, RECEIVER a destra.")"
    if [ "${XDG_SESSION_TYPE:-}" = "wayland" ] && [ ${#app[@]} -eq 0 ]; then
        echo "$(t "On Wayland the position is not guaranteed: tile them with Super+← and Super+→." "Su Wayland la posizione non è garantita: affiancale con Super+← e Super+→.")"
    fi
}

# ---------------------------------------------------------------------------
# tmux: two panes side by side
# ---------------------------------------------------------------------------
run_tmux() {
    local session="rdt-$PROTO-$$"
    if [ -n "${TMUX:-}" ]; then
        tmux split-window -h -d "$(quote "${RECV_CMD[@]}")" &&
            exec "${SEND_CMD[@]}"
    fi
    tmux new-session -d -s "$session" -x "$(tput cols 2>/dev/null || echo 200)" \
        -y "$(tput lines 2>/dev/null || echo 50)" "$(quote "${SEND_CMD[@]}")" || return 1
    tmux split-window -h -t "$session" "$(quote "${RECV_CMD[@]}")"
    tmux select-pane -t "$session".0
    if [ -t 0 ] && [ -t 1 ]; then
        exec tmux attach -t "$session"
    fi
    echo "$(t "tmux session '$session' started: attach with" "Sessione tmux '$session' avviata: collegati con")  tmux attach -t $session"
}

# ---------------------------------------------------------------------------
# split: the current terminal split in two, without tmux. Each of the two
# programs draws in its half of the screen (RDT_PANE=left|right: side by
# side, or one above the other if the window is narrow and tall) and writes
# the final summary to a file (RDT_PANE_LOG); this script holds the keyboard
# and with q (or Ctrl+C) closes both.
# ---------------------------------------------------------------------------
run_split() {
    local rep_s rep_r saved key sp rp quit=0
    if [ ! -t 0 ] || [ ! -t 1 ]; then
        echo "$(t "Split mode must be started from a terminal" "La modalità split va avviata da un terminale")" >&2
        return 1
    fi
    rep_s=$(mktemp "${TMPDIR:-/tmp}/rdt-sender.XXXXXX") || return 1
    rep_r=$(mktemp "${TMPDIR:-/tmp}/rdt-receiver.XXXXXX") || return 1
    saved=$(stty -g)
    trap 'quit=1' TERM HUP
    stty -echo -icanon -isig min 0 time 0
    # alternate screen, hidden cursor, no automatic line wrap, title
    printf '\033[?1049h\033[?25l\033[?7l\033[2J'
    [ "${TERM:-}" = linux ] || printf '\033]0;RDT %s\007' "$PROTO"

    cd "$DIR" || return 1
    RDT_PANE=right RDT_PANE_LOG="$rep_r" "${RECV_PROG[@]}" </dev/null 2>>"$rep_r" &
    rp=$!
    sleep 0.6
    RDT_PANE=left RDT_PANE_LOG="$rep_s" "${SEND_PROG[@]}" </dev/null 2>>"$rep_s" &
    sp=$!

    while [ "$quit" -eq 0 ]; do
        if read -rsn1 -t 0.2 key; then
            case "$key" in q|Q|$'\003') break ;; esac
        fi
        # one of the two ended by itself (error or signal): close everything
        kill -0 "$sp" 2>/dev/null && kill -0 "$rp" 2>/dev/null || break
    done
    kill -TERM "$sp" "$rp" 2>/dev/null
    wait "$sp" "$rp" 2>/dev/null

    printf '\033[0m\033[?7h\033[?25h\033[?1049l'
    [ "${TERM:-}" = linux ] && printf '\033[2J\033[H'
    stty "$saved" 2>/dev/null
    trap - TERM HUP
    printf '\033[1;34m── SENDER ──\033[0m\n'
    cat "$rep_s"
    printf '\n\033[1;32m── RECEIVER ──\033[0m\n'
    cat "$rep_r"
    rm -f "$rep_s" "$rep_r"
}

# ---------------------------------------------------------------------------
# terminal: a new maximized window of the system terminal, which runs this
# script in split mode
# ---------------------------------------------------------------------------
TERMINALS="gnome-terminal ptyxis kgx konsole xfce4-terminal mate-terminal tilix
           terminator lxterminal alacritty foot wezterm xterm x-terminal-emulator"

# TERM_CMD = command that opens terminal $1 running the rest of the arguments
term_cmd() {
    local t=$1 title="RDT $PROTO"
    shift
    case "$t" in
        gnome-terminal) TERM_CMD=(gnome-terminal --maximize --title="$title" -- "$@") ;;
        ptyxis)         TERM_CMD=(ptyxis --new-window --maximize -- "$@") ;;
        kgx)            TERM_CMD=(kgx -- "$@") ;;
        # Konsole cannot open maximized: a window larger than the screen is
        # requested, which the window manager shrinks to the screen. Konsole
        # remembers the window size on close: with a separate state the
        # demo's size does not change the other Konsole windows.
        konsole)        local state=${XDG_CACHE_HOME:-$HOME/.cache}/rdt-demo/konsole
                        mkdir -p "$state"
                        TERM_CMD=(env XDG_STATE_HOME="$state" konsole -p "tabtitle=$title"
                                  -p TerminalColumns=400 -p TerminalRows=150 -e "$@") ;;
        xfce4-terminal) TERM_CMD=(xfce4-terminal --maximize --title="$title" -x "$@") ;;
        mate-terminal)  TERM_CMD=(mate-terminal --maximize --title="$title" -x "$@") ;;
        tilix)          TERM_CMD=(tilix --maximize -t "$title" -e "$(quote "$@")") ;;
        terminator)     TERM_CMD=(terminator --maximise -T "$title" -x "$@") ;;
        lxterminal)     TERM_CMD=(lxterminal -t "$title" -e "$(quote "$@")") ;;
        alacritty)      TERM_CMD=(alacritty --title "$title" -e "$@") ;;
        foot)           TERM_CMD=(foot --maximized --title="$title" "$@") ;;
        wezterm)        TERM_CMD=(wezterm start -- "$@") ;;
        # stock xterm has a white background: the interface is designed for a dark one
        xterm)          TERM_CMD=(xterm -maximized -T "$title" -fa Monospace -fs 11
                                  -bg black -fg '#d0d0d0' -e "$@") ;;
        *)              TERM_CMD=("$t" -T "$title" -e "$@") ;;   # x-terminal-emulator and the like
    esac
}

run_terminal() {
    local t=$1 pid
    term_cmd "$t" "$SELF" --window "${ORIG_ARGS[@]}" -m split
    if [ "$t" = gnome-terminal ]; then
        "${TERM_CMD[@]}" >/dev/null 2>&1 || return 1        # returns at once, with the outcome
    else
        # if the terminal does not start (unknown option, no display...) it
        # usually exits shortly: in that case the next one is tried
        setsid "${TERM_CMD[@]}" >/dev/null 2>&1 </dev/null &
        pid=$!
        sleep 0.5
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" || return 1
        fi
    fi
    echo "$(t "Opened a $t window split in two: SENDER on the left, RECEIVER on the right" "Aperta una finestra $t divisa in due: SENDER a sinistra, RECEIVER a destra")"
    echo "$(t "(top and bottom if the window is narrow and tall)." "(sopra e sotto se la finestra è stretta e alta).")"
    echo "$(t "If it is too small enlarge it: the interface adapts. q to quit." "Se è troppo piccola ingrandiscila: l'interfaccia si adatta. q per uscire.")"
}

# Graphical terminal the script was launched from: the parent of the session
# leader, that is of the shell opened by the terminal. Nothing if it is not
# one of the supported ones (VS Code, ssh, console, file manager, ...).
calling_terminal() {
    local stat sid ppid
    stat=$(< "/proc/$$/stat") || return 1
    read -r _ _ _ sid _ <<< "${stat##*\) }"    # state ppid pgrp session
    stat=$(< "/proc/$sid/stat") || return 1
    read -r _ ppid _ <<< "${stat##*\) }"
    case "$(< "/proc/$ppid/comm")" in
        kitty)           echo kitty ;;
        gnome-terminal*) echo gnome-terminal ;;   # gnome-terminal-server
        ptyxis*)         echo ptyxis ;;           # ptyxis-agent
        kgx)             echo kgx ;;
        konsole)         echo konsole ;;
        xfce4-terminal)  echo xfce4-terminal ;;
        mate-terminal*)  echo mate-terminal ;;
        tilix)           echo tilix ;;
        terminator)      echo terminator ;;
        lxterminal)      echo lxterminal ;;
        alacritty)       echo alacritty ;;
        foot*)           echo foot ;;
        wezterm*)        echo wezterm ;;          # wezterm-gui
        xterm)           echo xterm ;;
        *)               return 1 ;;
    esac
} 2>/dev/null

# Opens the demo in a new window of the first installed terminal among the
# given ones: kitty with its own windows, the others split in two (split)
run_window() {
    local t tried=" "
    gui || return 1
    for t in "$@"; do
        case "$tried" in *" $t "*) continue ;; esac
        tried+="$t "
        has "$t" || continue
        if [ "$t" = kitty ]; then
            run_kitty && return 0
        else
            run_terminal "$t" && return 0
        fi
    done
    return 1
}

CALLER=$(calling_terminal)

case "$MODE" in
    kitty)    run_kitty ;;
    terminal)
        # kitty has its own mode: here only if asked for with RDT_TERMINAL
        [ "$CALLER" = kitty ] && CALLER=""
        run_window ${RDT_TERMINAL:-} $CALLER $TERMINALS ||
            { echo "$(t "No graphical terminal found: try -m split" "Nessun terminale grafico trovato: prova -m split")" >&2; exit 1; } ;;
    split)    run_split ;;
    gnome)    run_gnome ;;
    tmux)     run_tmux ;;
    auto)
        # the terminal chosen with RDT_TERMINAL, then the one we started from,
        # then kitty and the others in the order of the list
        if [ -n "${TMUX:-}" ]; then
            run_tmux
        elif run_window ${RDT_TERMINAL:-} $CALLER kitty $TERMINALS; then
            :
        elif [ -t 0 ] && [ -t 1 ]; then
            run_split
        else
            echo "$(t "No terminal available. Start by hand in two terminals:" "Nessun terminale disponibile. Avvia a mano in due terminali:")" >&2
            echo "  cd $DIR && ./${PROTO}_recv -a $HOST -p $PORT" >&2
            echo "  cd $DIR && ./${PROTO}_send -a $HOST -p $PORT" >&2
            exit 1
        fi
        ;;
    *) echo "$(t "Unknown mode" "Modo sconosciuto"): $MODE (auto, kitty, terminal, split, gnome, tmux)" >&2; exit 1 ;;
esac
