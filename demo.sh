#!/usr/bin/env bash
#
# demo.sh - Compila e apre sender e receiver affiancati (sender a sinistra,
#           receiver a destra) in una nuova finestra del terminale da cui e'
#           lanciato, altrimenti di kitty o di un altro terminale del sistema,
#           o in mancanza di interfaccia grafica nel terminale corrente.
#
# Uso: ./demo.sh [opzioni] [protocollo]        (./demo.sh -h per l'aiuto)

set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$DIR/$(basename "${BASH_SOURCE[0]}")"

# ---------------------------------------------------------------------------
# Modalita' interna: eseguita dentro ciascun terminale.
#   demo.sh --run RITARDO programma argomenti...
# Lascia la finestra aperta a fine programma per leggere il riepilogo.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--run" ]; then
    sleep "$2"
    shift 2
    cd "$DIR" || exit 1
    "$@"
    status=$?
    echo
    read -r -p "Premi INVIO per chiudere... " _
    exit "$status"
fi

# Modalita' interna: demo.sh --window ... fa lo stesso, ma gira in una
# finestra aperta apposta, che resta aperta a fine demo per il riepilogo.
IN_WINDOW=0
if [ "${1:-}" = "--window" ]; then
    IN_WINDOW=1
    shift
    trap 'echo; read -r -p "Premi INVIO per chiudere... " _' EXIT
fi
ORIG_ARGS=("$@")

usage() {
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

HOST=127.0.0.1
PORT=8142
SENDER_OPTS=""
RECEIVER_OPTS=""
MODE=auto
WINDOW=""
BITS=""

# le opzioni possono stare sia prima sia dopo il protocollo
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
            :) echo "L'opzione -$OPTARG richiede un argomento" >&2; usage >&2; exit 1 ;;
            *) echo "Opzione sconosciuta: -$OPTARG" >&2; usage >&2; exit 1 ;;
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
    *) echo "Protocollo sconosciuto: $1 (stop_and_wait, go_back_n, selective_repeat)" >&2; exit 1 ;;
esac
if [ $# -gt 1 ]; then
    echo "Argomento inatteso: $2" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Compilazione
# ---------------------------------------------------------------------------
if [ ! -s "$DIR/${PROTO}_send.c" ] || [ ! -s "$DIR/${PROTO}_recv.c" ]; then
    echo "Protocollo '$PROTO' non disponibile (mancano ${PROTO}_send.c / ${PROTO}_recv.c)" >&2
    exit 1
fi
make -s -C "$DIR" "${PROTO}_send" "${PROTO}_recv" || exit 1

# Le opzioni extra possono contenere testo tra virgolette (es. -m 'ciao mondo')
eval "EXTRA_S=($SENDER_OPTS)" || exit 1
eval "EXTRA_R=($RECEIVER_OPTS)" || exit 1

# Finestra e numeri di sequenza: il sender li usa sempre, il receiver
# Go-Back-N solo i numeri di sequenza (la sua finestra e' 1)
WIN_S=() WIN_R=()
if [ "$PROTO" = stop_and_wait ]; then
    if [ -n "$WINDOW$BITS" ]; then
        echo "Nota: -w e -b non servono a stop-and-wait (un bit, un pacchetto alla volta), ignorati." >&2
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

# Argomenti racchiusi tra apici singoli, per sessioni kitty e comandi tmux
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
# kitty: una finestra divisa in due (layout "horizontal" = affiancate)
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
    # kitty legge la sessione all'avvio: il file si puo' poi eliminare
    (sleep 20; rm -f "$session") >/dev/null 2>&1 &
    echo "Aperta una finestra kitty: SENDER a sinistra, RECEIVER a destra."
}

# ---------------------------------------------------------------------------
# gnome-terminal: due finestre separate, ognuna su metà del monitor principale
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
    # stima delle celle del terminale a partire dalla dimensione del font
    read -r cols rows < <(awk -v W="$W" -v H="$H" -v pt="$pt" -v s="$scale" 'BEGIN {
        px = pt * 96 / 72 * s; cw = px * 0.62; ch = px * 1.3;
        printf "%d %d\n", (W / 2 - 40) / cw, (H - 140) / ch }')
    cols=${RDT_COLS:-$cols}
    rows=${RDT_ROWS:-$rows}
    half=$((W / 2))

    # Su Wayland le finestre non possono scegliere la propria posizione:
    # si usa un'istanza privata di gnome-terminal che gira su XWayland.
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
    echo "Aperte due finestre gnome-terminal (${cols}x${rows}): SENDER a sinistra, RECEIVER a destra."
    if [ "${XDG_SESSION_TYPE:-}" = "wayland" ] && [ ${#app[@]} -eq 0 ]; then
        echo "Su Wayland la posizione non è garantita: affiancale con Super+← e Super+→."
    fi
}

# ---------------------------------------------------------------------------
# tmux: due pannelli affiancati
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
    echo "Sessione tmux '$session' avviata: collegati con  tmux attach -t $session"
}

# ---------------------------------------------------------------------------
# split: il terminale corrente diviso in due, senza tmux. Ognuno dei due
# programmi disegna nella sua meta' dello schermo (RDT_PANE=left|right:
# affiancati, oppure uno sopra l'altro se la finestra e' stretta e alta) e
# scrive il riepilogo finale in un file (RDT_PANE_LOG); questo script tiene
# la tastiera e con q (o Ctrl+C) li chiude entrambi.
# ---------------------------------------------------------------------------
run_split() {
    local rep_s rep_r saved key sp rp quit=0
    if [ ! -t 0 ] || [ ! -t 1 ]; then
        echo "La modalità split va avviata da un terminale" >&2
        return 1
    fi
    rep_s=$(mktemp "${TMPDIR:-/tmp}/rdt-sender.XXXXXX") || return 1
    rep_r=$(mktemp "${TMPDIR:-/tmp}/rdt-receiver.XXXXXX") || return 1
    saved=$(stty -g)
    trap 'quit=1' TERM HUP
    stty -echo -icanon -isig min 0 time 0
    # schermo alternativo, cursore nascosto, niente a-capo automatico, titolo
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
        # uno dei due e' terminato da solo (errore o segnale): si chiude tutto
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
# terminal: una nuova finestra massimizzata del terminale del sistema, che
# esegue questo script in modalita' split
# ---------------------------------------------------------------------------
TERMINALS="gnome-terminal ptyxis kgx konsole xfce4-terminal mate-terminal tilix
           terminator lxterminal alacritty foot wezterm xterm x-terminal-emulator"

# TERM_CMD = comando che apre il terminale $1 eseguendo il resto degli argomenti
term_cmd() {
    local t=$1 title="RDT $PROTO"
    shift
    case "$t" in
        gnome-terminal) TERM_CMD=(gnome-terminal --maximize --title="$title" -- "$@") ;;
        ptyxis)         TERM_CMD=(ptyxis --new-window --maximize -- "$@") ;;
        kgx)            TERM_CMD=(kgx -- "$@") ;;
        # Konsole non sa aprirsi massimizzato: si chiede una finestra piu' grande
        # dello schermo, che il window manager riduce allo schermo. Konsole
        # ricorda la dimensione delle finestre alla chiusura: con uno stato a
        # parte quella della demo non cambia le altre finestre Konsole.
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
        # xterm di serie ha il fondo bianco: l'interfaccia e' pensata per un fondo scuro
        xterm)          TERM_CMD=(xterm -maximized -T "$title" -fa Monospace -fs 11
                                  -bg black -fg '#d0d0d0' -e "$@") ;;
        *)              TERM_CMD=("$t" -T "$title" -e "$@") ;;   # x-terminal-emulator e simili
    esac
}

run_terminal() {
    local t=$1 pid
    term_cmd "$t" "$SELF" --window "${ORIG_ARGS[@]}" -m split
    if [ "$t" = gnome-terminal ]; then
        "${TERM_CMD[@]}" >/dev/null 2>&1 || return 1        # ritorna subito, con l'esito
    else
        # se il terminale non parte (opzione sconosciuta, niente display...)
        # di solito esce entro poco: in quel caso si prova il successivo
        setsid "${TERM_CMD[@]}" >/dev/null 2>&1 </dev/null &
        pid=$!
        sleep 0.5
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" || return 1
        fi
    fi
    echo "Aperta una finestra $t divisa in due: SENDER a sinistra, RECEIVER a destra"
    echo "(sopra e sotto se la finestra è stretta e alta)."
    echo "Se è troppo piccola ingrandiscila: l'interfaccia si adatta. q per uscire."
}

# Terminale grafico da cui e' stato lanciato lo script: il padre del leader
# della sessione, cioe' della shell aperta dal terminale. Niente se non e' uno
# di quelli supportati (VS Code, ssh, console, file manager, ...).
calling_terminal() {
    local stat sid ppid
    stat=$(< "/proc/$$/stat") || return 1
    read -r _ _ _ sid _ <<< "${stat##*\) }"    # stato ppid pgrp sessione
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

# Apre la demo in una nuova finestra del primo terminale installato fra quelli
# indicati: kitty con le sue finestre, gli altri divisi in due (split)
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
        # kitty ha la sua modalita': qui solo se chiesto con RDT_TERMINAL
        [ "$CALLER" = kitty ] && CALLER=""
        run_window ${RDT_TERMINAL:-} $CALLER $TERMINALS ||
            { echo "Nessun terminale grafico trovato: prova -m split" >&2; exit 1; } ;;
    split)    run_split ;;
    gnome)    run_gnome ;;
    tmux)     run_tmux ;;
    auto)
        # il terminale scelto con RDT_TERMINAL, poi quello da cui si e' partiti,
        # poi kitty e gli altri nell'ordine dell'elenco
        if [ -n "${TMUX:-}" ]; then
            run_tmux
        elif run_window ${RDT_TERMINAL:-} $CALLER kitty $TERMINALS; then
            :
        elif [ -t 0 ] && [ -t 1 ]; then
            run_split
        else
            echo "Nessun terminale disponibile. Avvia a mano in due terminali:" >&2
            echo "  cd $DIR && ./${PROTO}_recv -a $HOST -p $PORT" >&2
            echo "  cd $DIR && ./${PROTO}_send -a $HOST -p $PORT" >&2
            exit 1
        fi
        ;;
    *) echo "Modo sconosciuto: $MODE (auto, kitty, terminal, split, gnome, tmux)" >&2; exit 1 ;;
esac
