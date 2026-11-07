# usage: tools/with-jellyfin.sh python3 tools/play_jf.py <item_id> <seconds> <log_file> <mpv binary> [mpv args...]
# Streams a Jellyfin item with mpv for <seconds> (or until mpv exits), then redacts the token from <log_file>.
# The auth header reaches mpv through a private (0600) temporary config file, deleted afterwards.
import os, subprocess, sys, tempfile
import jf

item, seconds, logf, mpv, extra = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5:]
token, _, auth = jf.login()
header = f"Authorization: {auth}"
fd, conf = tempfile.mkstemp(suffix=".conf")
os.write(fd, f"http-header-fields-append=%{len(header)}%{header}\n".encode()); os.close(fd)
p = subprocess.Popen([mpv, f"--include={conf}", f"--log-file={logf}", *extra, jf.stream_url(item)],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    p.wait(timeout=seconds)
except subprocess.TimeoutExpired:
    p.terminate(); p.wait(timeout=10)
finally:
    os.unlink(conf)
    if os.path.exists(logf):
        t = open(logf, encoding="utf-8", errors="replace").read()
        open(logf, "w").write(t.replace(token, "<token>"))
print("mpv exit", p.returncode)
