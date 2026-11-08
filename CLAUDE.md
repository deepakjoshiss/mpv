# mpv, the owner's fork: Dolby Vision through macOS, and a Jellyvin-style UI

This is mpv for the owner's Mac. The goal is to play nearly every file, including Dolby Vision
profiles 5, 7 and 8, with an OSD that feels like Jellyvin's.

- **Main addition:** `--macos-native-video`. Decoded frames go to macOS's own
  AVSampleBufferDisplayLayer, so WindowServer does the Dolby Vision and HDR processing, as Infuse
  does. libplacebo then draws only the OSD on top.
- **Where the code is:** `video/out/vulkan/context_mac.m` (classify and tag frames),
  `video/out/mac/native_video.swift` (the layers) and `vo_gpu_next.c` (the hooks).
- **Where work happens:** on `dj-dev`, which tracks `fork/dj-dev` (our public fork,
  `deepakjoshiss/mpv`). `master` only mirrors upstream (`origin`, mpv-player/mpv, push disabled).
  This file and `mac-dev/` are ours. Keep commits that touch them separate from code commits, so
  the mpv changes can go upstream on their own one day.
- **The fork is public.** Never put library titles, work names, internal hosts or credentials in
  tracked files or commit messages.

## Always true

1. **A wrong attachment can crash WindowServer and log the owner out.** It has happened once: a
   CFNumber `DolbyCompatibilityID`. That key must be the CFString `"1"`, and only for profile 8.1.
   Before you run a change to frame tagging (`native_tag`) on any display, check the values against
   § Tags below. VinDesk counts too, because it runs in the same WindowServer.
2. **Screenshots come only from the VinDesk display** (`mac-dev/tools/vdshot.sh`). Never capture
   the real screen. It has caught the owner's private windows before.
3. **Never read or print Jellyfin credentials.** Use `mac-dev/tools/with-jellyfin.sh`. Never
   open Jellyvin's `.dev/servers.env` or `.dev/auth.json`. Logs keep the token as `<token>`.
4. **Build only through `mac-dev/`** (`mac-dev/rebuild.sh`). Install nothing globally: no Homebrew
   ffmpeg or libplacebo. See `mac-dev/README.md`.
5. **Commit and push only when asked, each time.** Push only to `fork`. No new branches, worktrees
   or PRs.
6. **Never `git clean -x` or `-X`.** The library snapshot in `mac-dev/` (`lib/`, `include/`, `src/`)
   is gitignored, and it's the only copy.
7. **Use only the company package mirror** (set in the user-level configs). A block is policy:
   `../kodi-webos/CLAUDE.md` § Package downloads.

## Where to look

| If you are… | Read |
|---|---|
| building, running, testing, or running the regression sweep | `mac-dev/README.md` |
| designing or building anything the user sees (OSD, menus, browsing) | Jellyvin's design, § References below |
| changing Dolby Vision handling | § Tags here, then the Kodi Dolby Vision references below |
| wondering why something in the native path is the way it is | `git log` on the files above: each commit message carries its findings |

## Tags (CVPixelBuffer attachments that macOS accepts)

| Kind | Attachments |
|---|---|
| profile 5 | transfer PQ only. No primaries, matrix or CGColorSpace. The RPU goes in `DolbyVisionRPUData` |
| profile 8.1 | BT.2020 / PQ / BT.2020, the CGColorSpace recomputed from those, the RPU, and `DolbyCompatibilityID` = CFString `"1"` |
| profile 8.4 | BT.2020 / HLG / BT.2020 and the RPU |
| profile 7 MEL | converted to 8.1 with libdovi, then tagged as 8.1 |
| profile 7 FEL | composed by libplacebo into a fresh `x420` IOSurface buffer, then tagged as 8.1 (RPU converted the same way), plus chroma location Left |

The following hold for every kind:

- **The RPU format:** `DolbyVisionRPUData` is FFmpeg's `AV_FRAME_DATA_DOVI_RPU_BUFFER` as it is (NAL
  62 without its 2-byte header).
- **Remove VideoToolbox's guesses:** VideoToolbox guesses BT.709 when it lacks colour info
  (`ColorInfoGuessedBy`). Remove that, `GammaLevel`, and any stale CGColorSpace.
- **libdovi 3.4.0 modes:** `2` gives 8.1 and `4` gives 8.4. The header's docs are wrong about this.
  `dovi_parse_unspec62_nalu` wants the `0x7C01` header put back first.
- **What counts as MEL:** an RPU whose `el_type` is "MEL" **and** whose FFmpeg reshaping is a no-op on
  all three curves, with base-layer transfer PQ. Decide on `el_type`, not the mapping: FEL often has
  a no-op mapping too, with the detail in the residual.
- **FEL composition:** the target gets the source's own colour space, so libplacebo skips tone and
  gamut mapping. Checked by composing a MEL title: within one 10-bit code on average. The rare large
  differences come from libplacebo clamping RGB to 0–1 inside its Dolby Vision decode, which its own
  FEL rendering does too.

## References

Read-only for this work. Each has its own rules; don't edit it from here.

**Jellyvin: the base design for anything new we build.**
- **What it is:** the owner's Kodi add-on, a Jellyfin frontend with its own skin, at
  `~/Library/Application Support/Kodi/addons/plugin.video.jellyvin`.
- **How closely to follow it:** new UI should *feel* the same, not match it pixel for pixel (owner).
  - `docs/design.md` § Palette, Type, Radius, Spacing and layout grammar: the tokens.
  - `docs/design.md` § OSD and seek bar, § OSD options: the OSD to follow. It records where Kodi
    forced a divergence. mpv may not have the same limits, but ask before you follow the design over
    Jellyvin.
  - `colors/defaults.xml` for colour values. `fonts/` holds Archivo, under the OFL licence.
- **Background knowledge:**
  - `docs/knowledge/macos-dev.md`: displays, VinDesk/DeskPad, screen capture, ffmpeg on this machine.
  - `docs/knowledge/jellyfin-server.md` and `docs/jellyfin-api.md`: how the server behaves.
  - `docs/playback.md`: how Jellyvin plays and seeks.
- **How it runs its work:** its `CLAUDE.md` and `.claude/skills/` (orient, verify, closeout) are the
  model for working here. Its `.dev/` holds secrets and is never read.

**Kodi: the Dolby Vision and player reference.**
- `~/work/apps/kodi-webos`: the build and TV setup for Kodi on the LG TV. `docs/dolby-vision.md` covers
  Dolby Vision on the TV, and `docs/patches.md` says why each patch exists.
- `~/work/apps/kodi/xbmc`: the Kodi source fork, including webOS Dolby Vision dual-layer work. It has
  its own `AGENTS.md`.
- `~/work/apps/jellyfin/LG-C8-DOLBY-VISION.md`: Dolby Vision container signalling facts, and how
  Jellyfin remuxes it.
