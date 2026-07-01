#!/usr/bin/env sh
set -eu

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"

usage() {
  cat <<EOF
Usage: $0 [--prefix PATH] [--destdir PATH]

Build and install i3-scratchpad-switcher via Makefile targets.

Environment overrides:
  PREFIX   install prefix (default: /usr/local)
  DESTDIR  staging root for packaging (default: empty)
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix)
      shift
      PREFIX="${1:-}"
      ;;
    --destdir)
      shift
      DESTDIR="${1:-}"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf "Unknown argument: %s\n\n" "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if [ -z "$PREFIX" ]; then
  printf "Error: PREFIX cannot be empty.\n" >&2
  exit 1
fi

printf "Building...\n"
make check-deps
make

printf "Installing to %s%s/bin...\n" "$DESTDIR" "$PREFIX"
make install PREFIX="$PREFIX" DESTDIR="$DESTDIR"

printf "Done.\n"
