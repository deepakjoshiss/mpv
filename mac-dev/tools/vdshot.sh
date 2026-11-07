#!/bin/sh
# usage: tools/vdshot.sh <out.png> [max_width]
# Screenshot of the VinDesk display through VinDesk's API (VinDesk must be running). Never capture the
# real screen instead. Sent with nc on purpose: VinDesk names the caller after the first non-Apple process
# up the chain, so nc from this shell counts as Claude Code / the terminal (already allowed), while a new
# caller such as Homebrew python3 gets a permission prompt and the request times out.
case "$1" in /*) P="$1" ;; *) P="$PWD/$1" ;; esac
printf '{"op":"screenshot","path":"%s","max_width":%s}\n' "$P" "${2:-960}" |
  nc -U "$HOME/Library/Application Support/VinDesk/api.sock"
