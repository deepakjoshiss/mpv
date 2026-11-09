# mac-dev: building and testing our mpv

The build folder of this repo, our mpv fork, whose main addition is
`--macos-native-video`. With that option on, macOS's own video layer shows the picture and does
the Dolby Vision and HDR processing, as QuickTime and Infuse do. mpv still draws the OSD and
subtitles on top.

Nothing in this folder is installed globally. There is no Homebrew ffmpeg or libplacebo: mpv
builds against the library snapshot kept here.

## Build and run

| To | Run |
|---|---|
| Build after a code change | `./rebuild.sh`: compiles mpv, then rebuilds `/Applications/mpv Native.app` |
| Set up from scratch, or after a `meson.build`/`meson.options` change | `./setup-mpv.sh`, then `./rebuild.sh` |
| Run the fresh build without the app | `./mpv-dev <file>` (same as the app, from `build/mpv/`) |
| Everyday use | open files with **mpv Native.app** |

- **mpv Native.app vs mpv.app.** mpv Native.app lives beside the stock `/Applications/mpv.app`.
  It has its own bundle id (`io.mpv.native`) and its own log at `~/Library/Logs/mpv-native.log`.
- **Shared config.** Both apps read `~/.config/mpv/mpv.conf`, which sets `macos-native-video=yes`.
  The stock app doesn't know that option, prints one "option not found" line at start-up, and plays
  normally.
- **mpv Native's own settings** go in `~/.config/mpv/mpv-native.conf`, which only this app reads
  (`make-app.sh` puts an `include` of it in the app's own `mpv.conf`). It's read before the shared
  `mpv.conf`, so an option set in both takes the shared value. The owner's copy turns on
  `--macos-osd` and loads every shared script except uosc and thumbfast.
- **Signing.** Ad hoc only, with no Apple or work identity.

## Checking a change

Work up this ladder and stop at the first step that fails:

1. **Headless: the log.** Play the file with `-v` and read what mpv chose. The line
   `Native video: on, Dolby Vision profile …` names the path. The frame attachments are listed under
   `Native video frame attachments:`. Also check the error counts, such as `PPS id out of range` and
   `Invalid NAL unit size`.
2. **The VinDesk display.** VinDesk is a virtual display. Play there with
   `--screen-name="VinDesk Display"` and capture it with `tools/vdshot.sh shot.png`. Never capture the
   real screen. `tools/winmean.py shot.png` gives the mean brightness inside the window area: black
   is about 0, and a normal picture is 20–60.
3. **The owner's eyes.** Colour and brightness are judged against Infuse on the real screen. Only the
   owner does this step.

### Jellyfin files

The NAS library is reached through Jellyfin. The login comes from Jellyvin's `servers.env`, which
`tools/with-jellyfin.sh` sources and never prints. Every log is written with the token replaced by
`<token>`.

| Tool | What it does |
|---|---|
| `tools/with-jellyfin.sh python3 tools/list_dv.py > out/titles.tsv` | lists every movie Jellyfin labels Dolby Vision |
| `tools/with-jellyfin.sh python3 tools/play_jf.py <id> <secs> <log> <mpv> [args]` | streams one item with mpv |
| `tools/with-jellyfin.sh python3 tools/probe_nals.py <id> <seek>…` | shows the first NAL types after each seek (are VPS/SPS/PPS there?) |
| `tools/sweep.sh [titles.tsv]` | the regression sweep, below |

### The regression sweep

`tools/sweep.sh` plays every title in mpv Native.app on the VinDesk display, from 30 minutes in. For
each title it takes a screenshot and writes one row to `out/sweep/sweep.tsv`. Columns: the profile,
the native path taken, whether video plays after the seek, PPS errors, and brightness. Each title takes up to
about half a minute. Run `./rebuild.sh` first, because the sweep tests the app.

Baseline, 2026-11-07, 31 titles:

| Count | Result |
|---|---|
| 17 | native, profile 8.1 |
| 7 | native, profile 7 MEL shown as 8.1 |
| 6 | native, profile 5 |
| 1 | native, profile 7 FEL composed by libplacebo, shown as 8.1 (since 2026-11-08; compose takes a median of 10 ms per 4K frame) |

- **Every title** has 0 PPS errors.
- **One profile 5 file stops after the seek** (`eof`). It has broken NAL sizes, and stock mpv.app
  fails on it the same way.
- **One file says profile 5 in its container, but its RPU says 7 MEL.** It plays on the MEL path,
  and it's counted under MEL above.
- **Coverage gap.** Jellyfin doesn't label every Dolby Vision file. At least one known P7 FEL file
  isn't in the list, so the sweep doesn't cover it.

`out/` is not tracked. It holds title lists, logs and screenshots of the library. Library titles
stay there and never go in tracked files: the fork is public.

## What's in here

| Path | What | Tracked |
|---|---|---|
| `lib/` | the dylibs of `/Applications/mpv.app/Contents/MacOS/lib` (FFmpeg, libplacebo, …) plus `libMoltenVK.dylib`, with unversioned symlinks, plus `libdovi.a` | no |
| `include/` | headers matching those dylibs: FFmpeg at `baa68e68b3` (all 7 library versions match), libplacebo at `0e435227` with a generated `config.h` (Vulkan, OpenGL, shaderc, lcms, dovi on), vulkan-headers v1.4.368, LuaJIT 2.1, uchardet, libdovi | no |
| `src/` | the sources the headers and `libdovi.a` came from (`dovi_tool` at tag `libdovi-3.4.0`) | no |
| `pkgconfig/` | hand-written `.pc` files pointing at `lib/` and `include/` relative to themselves (`${pcfiledir}`); `libplacebo.pc` carries the `pl_has_*` variables mpv's meson checks | yes |
| `native.ini` | meson native file: pkg-config from `venv/`, search path `pkgconfig/` only, both relative (`@DIRNAME@`) | yes |
| `venv/` | meson 1.12.1 and pkgconf 3.0.7.post0 from PyPI (Homebrew's pkgconf is broken here). A venv has its own location baked in: after moving the repo, delete it and recreate it with `python3 -m venv venv && venv/bin/pip install meson==1.12.1 pkgconf==3.0.7.post0` | no |
| `vulkan/icd.d/` | MoltenVK ICD for `mpv-dev`, its library path relative to the ICD file | yes |
| `build/mpv/` | meson build directory. It records absolute paths, so after moving the repo run `./setup-mpv.sh` again | no |

**The untracked folders hold the only copy of the snapshot.** They survive checkout, rebase and
`git clean -fd`. Never run `git clean` with `-x` (or `-X`) in this repo: it deletes ignored files.

**The snapshot is frozen.** Updating `/Applications/mpv.app` doesn't change `lib/`. To move to newer
libraries:
1. Copy the dylibs again.
2. Check the headers in `include/` still match: same `LIB*_VERSION_MAJOR` as the dylibs' install
   names.
3. Run `./setup-mpv.sh`.

## Network on this machine

- **TLS and package downloads.** This machine's network intercepts TLS and blocks public package
  registries. Packages come from a mirror set in the user-level configs: `~/.cargo/config.toml` for
  cargo (libdovi) and pip's config for PyPI. The rules live in local notes outside the repo:
  `~/.claude/CLAUDE.md` and `~/work/apps/kodi-webos/CLAUDE.md` § Package downloads. Never turn TLS
  checks off, and never point a tool back at a public registry.
- **Jellyfin auth.** Jellyfin wants the token in `Authorization: MediaBrowser …, Token="…"`.
  `X-Emby-Authorization` gets 400, and `X-Emby-Token` gets 401.
