# usage: tools/with-jellyfin.sh python3 tools/play_jf.py <item_id> <seconds> <log_file> <mpv binary> [mpv args...]
# Streams a Jellyfin item with mpv for <seconds> (or until mpv exits), then redacts the token from <log_file>.
# The auth header reaches mpv through a private (0600) temporary config file, deleted afterwards.
# Stopping this script (SIGTERM, Ctrl-C) stops mpv and still cleans up; an mpv that ignores SIGTERM is killed.
import os, re, signal, subprocess, sys, tempfile
import jf

item, seconds, logf, mpv, extra = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5:]
token, _, auth = jf.login()
header = f"Authorization: {auth}"
fd, conf = tempfile.mkstemp(suffix=".conf")
os.write(fd, f"http-header-fields-append=%{len(header)}%{header}\n".encode()); os.close(fd)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))   # so `finally` runs
p = None
try:
    p = subprocess.Popen([mpv, f"--include={conf}", f"--log-file={logf}", *extra, jf.stream_url(item)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p.wait(timeout=seconds)
except subprocess.TimeoutExpired:
    pass
finally:
    if p and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:   # a hung mpv must not skip the cleanup below
            p.kill()
            p.wait()
    os.unlink(conf)
    if os.path.exists(logf):
        t = open(logf, encoding="utf-8", errors="replace").read()
        t = re.sub(r'Token="[^"<]*"', 'Token="<token>"', t.replace(token, "<token>"))
        open(logf, "w").write(t)
print("mpv exit", p.returncode if p else None)
