"""Jellyfin login shared by the tools. Needs JV_OMV_* in the environment: run under with-jellyfin.sh.

The token is never printed. Jellyfin wants it in `Authorization: MediaBrowser ..., Token="..."`;
X-Emby-Authorization answers 400 and X-Emby-Token 401 on this server.
"""
import json, os, urllib.request

BASE = os.environ["JV_OMV_ADDRESS"].rstrip("/")
CLIENT = 'MediaBrowser Client="mpv-test", Device="mac", DeviceId="mpv-test-mac", Version="0.1"'
FFMPEG_DIR = "/Users/deepak.joshi/work/apps/jellyfin/jellyfin-ffmpeg/"  # ffmpeg/ffprobe, not on PATH


def login():
    """Returns (token, user id, value for the Authorization header)."""
    body = json.dumps({"Username": os.environ["JV_OMV_USER"], "Pw": os.environ["JV_OMV_PASS"]}).encode()
    r = urllib.request.Request(BASE + "/Users/AuthenticateByName", data=body,
                               headers={"Content-Type": "application/json", "Authorization": CLIENT})
    d = json.load(urllib.request.urlopen(r, timeout=30))
    return d["AccessToken"], d["User"]["Id"], f'{CLIENT}, Token="{d["AccessToken"]}"'


def get(path, auth):
    return json.load(urllib.request.urlopen(
        urllib.request.Request(BASE + path, headers={"Authorization": auth}), timeout=60))


def stream_url(item):
    return f"{BASE}/Videos/{item}/stream?static=true&mediaSourceId={item}"
