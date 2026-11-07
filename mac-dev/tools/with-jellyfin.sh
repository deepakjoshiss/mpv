#!/bin/sh
# usage: tools/with-jellyfin.sh <command> [args...]
# Runs <command> with the Jellyfin server login (JV_OMV_ADDRESS/USER/PASS) from Jellyvin's servers.env in
# its environment. The file is sourced, never read or printed; secrets the tools don't need are dropped.
E="$HOME/Library/Application Support/Kodi/addons/plugin.video.jellyvin/.dev/servers.env"
set -a; . "$E"; set +a
unset LG_SSH_PASS JV_LOCAL_PASS
exec "$@"
