# sit — suckless IPTV

A small, fast IPTV player inspired by [IPTVnator](https://github.com/4gray/iptvnator),
built the suckless way: one C binary, configured by editing `config.h`, no
Electron, no Angular, no database server, no telemetry.

This file is the plan **and** the record of where the work stands. Keep §0
current: when you finish or change something, update it in the same commit.
Working rules for agents are in `AGENTS.md`.

---

## 0. Status (last updated 2026-10-27)

| # | Milestone | State |
|---|-----------|-------|
| 1 | Skeleton: SDL/GL window, text, lists, libmpv in-window | **done** |
| 2 | Xtream live: login, categories, channels, account in status bar, disk cache | **done**, confirmed on a real provider |
| 3 | Xtream movies + series: poster grid, details, episodes, resume | **done**, confirmed on a real provider |
| – | Pulled forward from 6: search, favorites, recent, Arabic/Hebrew | **done** |
| – | Memory: slim catalogs, capped poster cache, smaller mpv buffers | **done** |
| 4 | EPG: now/next, guide page, catch-up | **done** (Xtream only; XMLTV moved to 5) |
| 5 | M3U playlists + XMLTV guide | M3U **done** (`m3u.c`; parser checked with a headless harness, not yet seen in the UI); XMLTV not started |
| 6 | Polish: bundled font, light theme, man page, CJK, number entry | not started |
| 7 | Ports and releases | not started; Linux build **never tried** |

Everything above was tested against `test/fakextream.py`; milestones 2 and 3
were also confirmed by the owner on a real provider. **Not yet confirmed on a
real provider:** the EPG/catch-up of milestone 4 (catch-up time format and URL
style vary between panels), search/favorites/recent, the Arabic rendering,
and the memory numbers.

### Open items, roughly in order
1. Owner to try EPG and catch-up on their provider; fix the timeshift time
   format or switch to `timeshift.php` if replays start at the wrong time.
2. Linux build (WSL is available on the dev machine) before the repo goes public.
3. Milestone 5, rest: try an `m3u` source in the UI (sources line
   `name<TAB>m3u<TAB>url-or-file`; add a `/m3u` route to the fake server for
   a test), then XMLTV (streaming parser, `.xml.gz` via zlib) matched by
   `tvg-id` into the `Epg` structures. M3U ids are list positions, so
   favorites can drift if the provider reorders the list; `catchup*`
   attributes are ignored.
4. Milestone 6 items; CJK/Thai still render as `?`.
5. Code size is ~5,850 non-blank lines without vendor and config (target was 5,000). `sit.c` is
   ~2,450 of them; splitting pages out of it is fair game if it stays readable.

---

## 1. Goals / non-goals

### Goals
- **Xtream Codes API: first-class** (Live, VOD, Series, EPG, catch-up).
- M3U / M3U8 playlists (URL or local file) with `#EXTINF` attributes.
- XMLTV EPG (URL or file, `.xml` and `.xml.gz`).
- Embedded playback via **libmpv** (it handles every codec, HLS, TS, hwdec).
- Keyboard-first UI that is **actually nice to look at**: proper fonts,
  anti-aliasing, a coherent palette, smooth scrolling, channel logos.
- Favorites, recently watched, resume position for VOD/series.
- Fuzzy search across everything.
- Runs on Linux, *BSD, Windows (MSYS2/MinGW), macOS.
- Source small enough to read in an afternoon.

### Non-goals
- Stalker / Ministra portals (maybe an out-of-tree patch later).
- Multiple player backends — mpv only.
- Web build, Docker, settings GUI (settings live in `config.h`), i18n,
  remote control, cloud sync, theme marketplace.

---

## 2. Stack (as built)

| Concern | Choice | Notes |
|---------|--------|-------|
| Language | C99, `-pedantic -Wall -Wextra`, warning-free | |
| Window/input | SDL2 | |
| Video | libmpv render API into an offscreen FBO | `BLOCK_FOR_TARGET_TIME=0`, or the UI runs at the video's frame rate |
| Text | `vendor/stb_truetype.h` | system fonts + per-glyph fallback fonts (`config.h`) |
| Images | `vendor/stb_image.h` | decoded and shrunk on the net thread |
| HTTP | libcurl multi on one worker thread | |
| JSON | own parser, `json.c` | **not jsmn**: Xtream needs unescaping and numbers-as-strings anyway |
| RTL text | own Arabic shaping + reduced bidi in `gfx.c` | presentation forms, lam-alef, bracket pairing |
| Rendering | one GL 3.3 quad batcher with SDF rounded rects | |

Runtime deps: SDL2, libmpv, libcurl. zlib arrives with XMLTV (milestone 5).

---

## 3. Source layout (as built)

```
config.def.h   defaults; make copies it to config.h (ignored by git)
config.mk      paths, flags, OS=linux|win|mac
sit.c          everything the user sees: pages, drawing, keys, main loop
gfx.c/.h       GL batcher, glyph atlas, fallback fonts, Arabic shaping, bidi, wrap
ui.c/.h        animated list and grid widgets
player.c/.h    libmpv wrapper; masks credentials in error text
net.c/.h       worker thread, curl multi, disk cache (fresh/stale), JSON parse off-thread
json.c/.h      JSON parser into an arena
xtream.c/.h    Xtream API: account, catalogs, details, EPG, stream/catch-up urls
epg.c/.h       per-channel guide cache, now/next, guide page catalog
img.c/.h       poster/logo textures, LRU capped at 64 MB
store.c/.h     resume positions, favorites, recent (TSV files)
playlist.h     Source, Item, Catalog, Playlist
util.c/.h      arena, files, dirs, utf-8, base64, urlenc, die()
vendor/        stb_truetype.h, stb_image.h (public domain)
test/fakextream.py   fake Xtream server for end-to-end tests
```

Memory model: a **Catalog per list** (live, movies, series, episodes,
favorites, recent, guide, search results) owns an arena; rebuilt whole,
never edited. Catalogs copy only the fields they show; the provider's JSON
is freed after the build (episodes keep theirs: one series, small).

---

## 4. Behaviour worth knowing

- **Pages:** Live, Movies, Series, Favorites, Recent are tabs; the episode
  page (Enter on a series) and the guide page (`e` on a channel) sit on top
  of a tab. Each page remembers its selection. Selection survives list
  rebuilds by group name + item id.
- **Sources:** `%APPDATA%\sit\sources` / `~/.config/sit/sources` (TSV:
  name, `xtream`, url, user, pass), also read as `sources.txt` (Notepad).
  Else `sources[]` in `config.h`. URLs on the command line form a source.
- **Cache:** lists 6 h (`cachettl`), account 5 min, details 3 days, short EPG
  10 min, full guide 30 min, images 30 days. A stale copy is shown at once
  while the fresh one loads. The cache dir name hashes host+user+password.
- **Details:** fetched when the selection rests 0.25 s; now/next for the
  focused channel after 0.3 s. Lists show a channel's programme only once
  its guide is in memory — no request per row.
- **Catch-up:** `{host}/timeshift/{u}/{p}/{minutes}/{YYYY-MM-DD:HH-MM}/{id}.ts`,
  the start taken verbatim from the guide's `start` (provider's own time).
- **Resume:** saved every 5 s while playing, flushed every 30 s and on
  stop/quit; >95 % counts as watched; resumes from 30 s on.
- **Credentials:** never drawn; masked (raw and url-encoded) in mpv errors.

---

## 5. Keys (defaults, `config.def.h`)

| Key | Action |
|-----|--------|
| arrows / `hjkl` | move; left/right switch panes (seek in full view) |
| `Enter` | play / open; again on what plays: full view |
| `Esc`, `Backspace` | back |
| `Tab`, `Shift+Tab` | Live, Movies, Series, Favorites, Recent |
| `/`, `Ctrl+F` | search |
| `f` | favorite |
| `e` | guide of a channel (also from the player) |
| `Space` | pause |
| `[` `]` | previous / next channel |
| `a` `s` | audio / subtitle track |
| `-` `=` `m` | volume, mute |
| `r` | refresh |
| `Alt+1..9` | switch source |
| `F11`, `Shift+F` | fullscreen |
| `x` | stop |
| `q` | quit |

Not done from the original plan: `0–9` channel number entry, EPG grid view.

---

## 6. State on disk

```
config:  ~/.config/sit/sources            (Windows %APPDATA%\sit)
data:    ~/.local/share/sit/resume         source kind id seconds duration
         ~/.local/share/sit/favorites      source kind id series time name logo ext seriesname
         ~/.local/share/sit/recent         same, newest first, 100 kept
cache:   ~/.cache/sit/xt-<hash>/...        JSON per action; img/<hash> posters
                                           (Windows %LOCALAPPDATA%\sit)
```

---

## 7. Measured (fake server, Windows 11)

- UI frame rate with video playing: 60 → 177 fps after `BLOCK_FOR_TARGET_TIME=0`.
- Fixed memory ~230 MB (GL driver, mpv, SDL); lists ~0.3 KB per item
  (48,000 channels = 13 MB). Owner's real use: ~1 GB before the memory work,
  ~613 MB after (not a controlled comparison).

---

## 8. Remaining milestones

5. **M3U + XMLTV** — `#EXTINF` (`tvg-id`, `tvg-name`, `tvg-logo`,
   `group-title`, `tvg-chno`, `catchup*`), URL or file, as a `m3u` source
   type in the sources file. XMLTV streaming parser (no libxml2), `.xml.gz`
   via zlib, matched by `tvg-id`; feeds the same `Epg` structures.
6. **Polish** — bundled default font, light theme, man page `sit.1`,
   CJK/Thai glyphs, channel number entry, maybe an EPG grid.
7. **Ports** — Linux and *BSD builds verified, macOS, Windows zip with DLLs.

Each milestone ends with: warning-free build, tested against the fake
server (extend it for new features), §0 updated.
