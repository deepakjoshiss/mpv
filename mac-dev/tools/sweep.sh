#!/bin/zsh
# usage: tools/sweep.sh [titles.tsv]     (default out/titles.tsv, made by list_dv.py)
# Regression sweep: plays every title natively in mpv Native.app on the VinDesk display from ~30 min in,
# screenshots it 4 s after playback starts, and writes one row per title to out/sweep/sweep.tsv.
# Run tools/rebuild.sh first; the app, not build/mpv, is what gets tested.
D=${0:A:h:h}; T=$D/tools; O=$D/out/sweep; mkdir -p $O
TITLES=${1:-$D/out/titles.tsv}; MPV="$HOME/Applications/mpv Native.app/Contents/MacOS/mpv"
OUT=$O/sweep.tsv; print -r -- $'title\tjellyfin range\tprofile\tnative path\tvideo after seek\tPPS errors\tbrightness\tstart s' > $OUT
n=0; total=$(wc -l < $TITLES | tr -d ' ')
while IFS=$'\t' read -r name id rng rt; do
  n=$((n+1)); start=$(( rt > 3000 ? 1800 : rt * 4 / 10 )); L=$O/$id.log; P=$O/$id.png; rm -f $L $P
  $T/with-jellyfin.sh python3 $T/play_jf.py $id 70 $L "$MPV" --no-config --vo=gpu-next --gpu-api=vulkan \
    --gpu-context=macvk --hwdec=videotoolbox --macos-native-video "--screen-name=VinDesk Display" \
    --geometry=50%+100+100 --idle=no --keep-open=no --osd-level=0 --mute=yes --start=$start --length=12 \
    >/dev/null 2>&1 &
  # wait up to 60 s for playback to start (macOS has no `timeout`)
  w=0; until [ $w -ge 300 ] || grep -q 'playback restart complete' $L 2>/dev/null || ! kill -0 $! 2>/dev/null; do
    w=$((w+1)); sleep 0.2; done
  mean=""; if grep -q 'playback restart complete' $L 2>/dev/null; then
    sleep 4; $T/vdshot.sh $P >/dev/null; [ -f $P ] && mean=$(python3 $T/winmean.py $P | awk '{print $2}'); fi
  wait
  prof=$(grep -o 'Dolby Vision config record: profile [0-9]*' $L | head -1 | awk '{print $NF}')
  kind=$(grep -o 'Native video: .*' $L | tail -1 | sed 's/Native video: //'); [ -z "$kind" ] && kind="off (libplacebo)"
  vid=$(grep -o 'playback restart complete.*video=[a-z]*' $L | head -1 | sed 's/.*video=//'); [ -z "$vid" ] && vid="no start"
  pps=$(grep -c 'PPS id out of range' $L)
  row="$name"$'\t'"$rng"$'\t'"${prof:-?}"$'\t'"$kind"$'\t'"$vid"$'\t'"$pps"$'\t'"$mean"$'\t'"$start"
  print -r -- "$row" >> $OUT; print -r -- "[$n/$total] $row"
done < $TITLES
