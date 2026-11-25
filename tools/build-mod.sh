#!/usr/bin/env bash
# Build a private netlab binary with the core plus the given module files,
# without the interface (stubbed). For developing a module in isolation:
#
#   tools/build-mod.sh NAME src/a.c [src/b.c ...]   ->  ./netlab-NAME
#
# The binary runs scenarios headless: ./netlab-NAME run FILE --seed 1
set -e
cd "$(dirname "$0")/.."
name=$1; shift
mkdir -p "build/mod-$name"
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -D_DEFAULT_SOURCE -I. -Isrc \
    src/core.c src/link.c src/stack.c src/apps.c src/scenario.c src/main.c tools/ui_stub.c \
    rdt.c channel.c tui.c i18n.c "$@" -lm -o "netlab-$name"
