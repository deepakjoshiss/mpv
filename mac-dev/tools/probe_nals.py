# usage: tools/with-jellyfin.sh python3 tools/probe_nals.py <item_id> <seek>...
# Prints the container's HEVC extradata size, then the NAL types of the first video packets after each
# seek (packets copied raw, length-prefixed). Shows whether parameter sets exist after a seek.
import subprocess, sys
import jf

item, seeks = sys.argv[1], sys.argv[2:]
_, _, auth = jf.login()
hdr, url = f"Authorization: {auth}\r\n", jf.stream_url(item)
p = subprocess.run([jf.FFMPEG_DIR + "ffprobe", "-v", "error", "-headers", hdr, "-select_streams", "v:0",
                    "-show_entries", "stream=extradata_size,codec_tag_string:format=format_name", "-of", "compact", url],
                   capture_output=True, text=True)
print(p.stdout.strip().replace("\n", " | "))
for s in seeks:
    raw = subprocess.run([jf.FFMPEG_DIR + "ffmpeg", "-hide_banner", "-loglevel", "error", "-headers", hdr, "-ss", s,
                          "-i", url, "-map", "0:v:0", "-frames:v", "4", "-c", "copy", "-f", "data", "-"],
                         capture_output=True).stdout
    i, types = 0, []
    while i + 4 <= len(raw):
        n = int.from_bytes(raw[i:i + 4], "big")
        if n <= 0 or i + 4 + n > len(raw):
            break
        types.append((raw[i + 4] >> 1) & 0x3f); i += 4 + n
    print(f"seek {s}: first NAL types {types[:24]}  (32=VPS 33=SPS 34=PPS 19/20=IDR 21=CRA 62=RPU 63=EL)")
