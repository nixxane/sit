# sit — suckless IPTV

A small, fast IPTV player inspired by [IPTVnator](https://github.com/4gray/iptvnator),
built the suckless way: one C binary, configured by editing `config.h`, no
Electron, no Angular, no database server, no telemetry.

> IPTVnator is ~100 MB of Electron + Angular. `sit` should be one binary of
> about 1–2 MB, plus `libmpv`, and start instantly.

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
- Source small enough to read in an afternoon: **target ≤ 5,000 SLOC**.

### Non-goals (things IPTVnator does that we drop)
- Stalker / Ministra portal support (MAC-auth mess; maybe a patch later).
- Multiple video player backends (VideoJS, HTML5, ArtPlayer…) — mpv only.
- Electron PWA / web build / Docker self-hosting.
- Settings GUI. Settings live in `config.h`; runtime state in plain files.
- i18n (16 languages). English strings in `config.h`; translate by patch.
- Built-in remote control server, playlist sharing, cloud sync.
- Theme marketplace. One light + one dark scheme in `config.h`.

---

## 2. Stack

| Concern        | Choice                               | Why |
|----------------|--------------------------------------|-----|
| Language       | C99                                  | suckless canon, tiny binary |
| Window/input   | SDL2 (SDL3 later)                    | portable, OpenGL context, good input |
| Video          | libmpv render API (OpenGL)           | renders video into *our* GL context, so UI overlays video |
| Text           | `stb_truetype.h` (vendored)          | crisp AA text, no FreeType/fontconfig dependency |
| Images (logos) | `stb_image.h` (vendored)             | PNG/JPEG logos |
| HTTP           | libcurl                              | HTTPS, redirects, gzip, everywhere |
| JSON           | `jsmn` (vendored, ~400 lines)        | zero-alloc tokenizer, enough for Xtream |
| XML (XMLTV)    | hand-rolled streaming tag scanner    | XMLTV is trivial; avoid libxml2 |
| gzip           | zlib                                 | `.xml.gz` EPG |
| Rendering      | tiny immediate-mode UI on OpenGL 3.3 (quads + glyph atlas) | full control over looks, ~600 SLOC |

Runtime deps: `libmpv`, `SDL2`, `libcurl`, `zlib`. That's it.

---

## 3. Source layout

```
sit/
├── config.def.h     # copied to config.h on first build — colors, fonts, keys, sources
├── config.mk        # paths, flags (per-OS)
├── Makefile
├── sit.c            # main loop, state machine, event dispatch
├── ui.c / ui.h      # immediate-mode widgets: list, grid, text, rect, image
├── gfx.c / gfx.h    # GL quad batcher, glyph atlas, texture cache
├── player.c / .h    # libmpv wrapper: load, pause, seek, tracks, OSD
├── net.c / net.h    # curl wrapper + on-disk cache + background fetch thread
├── xtream.c / .h    # Xtream Codes API client
├── m3u.c / m3u.h    # M3U parser
├── epg.c / epg.h    # XMLTV streaming parser + now/next lookup
├── store.c / .h     # favorites, history, resume positions (plain TSV files)
├── fuzzy.c / .h     # fzy-style fuzzy matcher
├── util.c / util.h  # arena allocator, strings, die(), etc.
├── vendor/          # stb_truetype.h, stb_image.h, jsmn.h
├── fonts/           # embedded default font (Inter + JetBrains Mono, OFL)
├── sit.1            # man page
└── LICENSE          # MIT/X
```

Memory model: one **arena per playlist load**; free the whole arena on
reload. No per-item `free()` bookkeeping.

---

## 4. Configuration (`config.def.h`)

```c
/* sources: name, type, url/host, user, pass */
static const Source sources[] = {
	{ "Home",   SRC_XTREAM, "http://provider.example:8080", "user", "pass" },
	{ "Public", SRC_M3U,    "https://iptv-org.github.io/iptv/index.m3u", NULL, NULL },
};
static const char *epgurls[] = { NULL }; /* extra XMLTV; Xtream EPG auto-added */

static const char *font      = NULL;    /* NULL = embedded Inter */
static const int   fontsize  = 15;
static const int   rowheight = 36;
static const int   radius    = 6;       /* corner radius for cards/selection */

/* colors (Catppuccin-ish default, dark) */
static const uint32_t col[] = {
	[ColBg]     = 0x1e1e2e, [ColPanel]  = 0x181825, [ColFg]   = 0xcdd6f4,
	[ColDim]    = 0x7f849c, [ColAccent] = 0x89b4fa, [ColSel]  = 0x313244,
	[ColLive]   = 0xf38ba8, [ColGood]   = 0xa6e3a1,
};

static const char *mpvopts[][2] = {
	{ "hwdec", "auto-safe" }, { "cache", "yes" }, { "demuxer-max-bytes", "64MiB" },
};
static const int cachettl = 6 * 3600;   /* seconds before refetching lists */

static const Key keys[] = { /* see §8 */ };
```

Credentials in `config.h` is the suckless way. Also support
`$XDG_CONFIG_HOME/sit/sources` (TSV, one source per line) so people can add
providers without recompiling; that file wins if present.

---

## 5. Data model

```c
typedef enum { KIND_LIVE, KIND_VOD, KIND_SERIES } Kind;

typedef struct {
	char *name, *group, *logo, *url;   /* url NULL for Xtream: built on play */
	char *epgid;                       /* tvg-id / epg_channel_id */
	int   xid;                         /* Xtream stream_id / series_id */
	char *ext;                         /* container_extension for VOD */
	Kind  kind;
	unsigned catchup:1, fav:1;
	int   archdays;                    /* tv_archive_duration */
} Item;

typedef struct { char *name; int id; Item **items; size_t n; } Group;

typedef struct {
	Source *src;
	Group  *groups[3];  size_t ngroups[3];  /* per Kind */
	Arena   arena;
	time_t  fetched;
} Playlist;
```

M3U playlists only ever populate `KIND_LIVE` (or VOD if `group-title`
heuristics say so — keep it simple: all LIVE).

---

## 6. Xtream Codes support (the important part)

Base: `{host}/player_api.php?username={u}&password={p}`

| Action                                  | Use |
|-----------------------------------------|-----|
| *(no action)*                           | auth + `user_info` (exp_date, max_connections, status) + `server_info` (timezone, https port) |
| `get_live_categories`                   | live groups |
| `get_live_streams[&category_id=]`       | channels (stream_id, name, stream_icon, epg_channel_id, tv_archive, tv_archive_duration) |
| `get_vod_categories` / `get_vod_streams`| movies (stream_id, container_extension, rating, cover) |
| `get_vod_info&vod_id=`                  | plot, cast, duration, backdrop — fetched lazily on focus |
| `get_series_categories` / `get_series`  | series list |
| `get_series_info&series_id=`            | seasons → episodes (id, container_extension) — lazily |
| `get_short_epg&stream_id=&limit=4`      | now/next for focused channel (fast path) |
| `get_simple_data_table&stream_id=`      | full day EPG for a channel (catch-up list) |
| `{host}/xmltv.php?username=&password=`  | full XMLTV (optional, background, cached) |

Stream URLs (built at play time, never stored):
- Live: `{host}/live/{u}/{p}/{id}.ts` (fallback `.m3u8` per config)
- VOD: `{host}/movie/{u}/{p}/{id}.{ext}`
- Episode: `{host}/series/{u}/{p}/{episode_id}.{ext}`
- Catch-up: `{host}/timeshift/{u}/{p}/{minutes}/{YYYY-MM-DD:HH-MM}/{id}.ts`

Robustness — real providers are sloppy:
- Numbers arrive as strings or ints (`"stream_id":"123"` vs `123`): accept both.
- `epg_listings[].title`/`description` are **base64**: decode.
- Empty arrays come back as `[]`, `{}`, `null` or `""`: treat all as empty.
- Fetch the three category lists + three stream lists in parallel (curl multi),
  then cache raw JSON to `$XDG_CACHE_HOME/sit/<hash>/<action>.json`.
  Startup reads cache instantly, refreshes in background if older than `cachettl`.
- Show account status (expiry date, active/max connections) in the status bar.
- Never log or print credentials; mask password in any URL shown in the UI.

---

## 7. UI design ("suckless but not ugly")

Principles: one font family, one accent color, generous spacing, no borders
where whitespace works, no gradients, rounded selection pill, subtle 120 ms
eased transitions for selection and panel slide. Everything drawn by our ~600
SLOC GL batcher, so it looks identical on every OS.

### Browse layout
```
┌──────────────────────────────────────────────────────────────────────────┐
│  sit   Live  Movies  Series  ★ Favorites  ⟲ Recent        ⌕ search…     │  top bar
├───────────────┬───────────────────────────────┬──────────────────────────┤
│ All           │ [logo] BBC One HD        ● LIVE│                          │
│ UK            │ [logo] BBC Two HD             │   ▶ live preview (mpv)   │
│▌News          │▌[logo] Sky News     ★         │                          │
│ Sports        │ [logo] CNN International      │──────────────────────────│
│ Kids          │ [logo] Al Jazeera English     │ Now  14:00–15:00         │
│ Movies        │ …                             │ Afternoon Live           │
│               │                               │ ███████░░░░ 62%          │
│               │                               │ Next 15:00  The Briefing │
├───────────────┴───────────────────────────────┴──────────────────────────┤
│ Home · Xtream · exp 2027-03-01 · 1/2 conn       2,431 channels   ? help │  status
└──────────────────────────────────────────────────────────────────────────┘
```
- Three panes: **groups → items → details/preview**. Panes collapse below
  ~900 px width (groups become a popup).
- Movies/Series use a **poster grid** in the middle pane (covers lazy-loaded,
  texture cache with LRU eviction, placeholder = initials on tinted card).
- Series: grid → season tabs → episode list with watched/resume markers.
- **Fullscreen player**: video fills window; overlay (fades after 2 s idle):
  title, now/next, progress bar for VOD, track picker (audio/subs) via mpv.
- Channel zapping while fullscreen: `Up/Down` or number entry, mini OSD.
- Search (`/`): floating fuzzy palette over everything, results grouped by
  kind, matched characters in accent color.
- EPG grid view (`g`): time across, channels down — **milestone 4**, optional.

### Look
- Default font: Inter (UI) embedded via `xxd -i` at build time; ~300 KB.
- HiDPI: scale from SDL drawable size / window size ratio.
- Logos: rounded 4 px, fixed 28×28 box, letterboxed on dark tile.
- Dark default, light scheme via `-l` flag or `config.h`.

---

## 8. Keys (defaults, all in `config.h`)

| Key            | Action |
|----------------|--------|
| `h j k l` / arrows | navigate panes / items |
| `Enter`        | play / open |
| `Esc` / `Backspace` | back |
| `Tab`          | cycle Live / Movies / Series / Favorites / Recent |
| `/`            | fuzzy search |
| `f`            | toggle favorite |
| `Space`        | pause |
| `←/→`, `Shift` | seek ±10 s / ±60 s (VOD, catch-up) |
| `[` `]`        | previous / next channel |
| `0–9`          | jump to channel number |
| `a` / `s`      | cycle audio / subtitle track |
| `F` / `F11`    | fullscreen |
| `e`            | EPG for channel (catch-up if available) |
| `r`            | refresh source |
| `1–9` + `Alt`  | switch source |
| `q`            | quit (saves resume positions) |

Mouse works too (click, scroll, double-click to fullscreen) — not ugly
means not hostile to normal people.

---

## 9. State on disk

Plain text, greppable, hand-editable:
```
$XDG_DATA_HOME/sit/favorites   # TSV: source \t kind \t id-or-url \t name
$XDG_DATA_HOME/sit/history     # TSV: unix-time \t source \t kind \t id \t name
$XDG_DATA_HOME/sit/resume      # TSV: source \t kind \t id \t seconds \t duration
$XDG_CACHE_HOME/sit/...        # raw JSON / M3U / XMLTV / logos, deletable anytime
```
Windows: `%APPDATA%\sit\` and `%LOCALAPPDATA%\sit\cache\`.

---

## 10. Threading

- **Main thread**: SDL events, UI, GL, mpv render.
- **One net thread**: job queue → curl multi. Results posted back with
  `SDL_PushEvent`. UI never blocks on network.
- EPG parse runs on the net thread after download; swaps in atomically.
- mpv has its own threads; we only use `mpv_render_context` + wakeup callback.

---

## 11. Milestones

1. **Skeleton** (~1,200 SLOC) — SDL window, GL batcher, glyph atlas, list
   widget, libmpv render in-window, play a hardcoded URL. Keys from `config.h`.
2. **Xtream live** — auth, categories, streams, play live, status bar
   account info, disk cache, background refresh. *This is the MVP.*
3. **Xtream VOD + Series** — poster grid, lazy `get_vod_info` /
   `get_series_info`, resume positions, episode list.
4. **EPG** — `get_short_epg` now/next, XMLTV import, catch-up playback,
   optional EPG grid.
5. **M3U** — parser (`tvg-id`, `tvg-logo`, `group-title`, `tvg-chno`,
   `catchup`), local file + URL.
6. **Polish** — fuzzy search palette, favorites/recent, transitions,
   light theme, HiDPI, mouse, man page.
7. **Ports** — Windows (MSYS2 static-ish build, ship `libmpv-2.dll`),
   macOS, *BSD. Release tarball + Windows zip.

Each milestone ends with: builds warning-free with
`-std=c99 -pedantic -Wall -Wextra`, runs under ASan for a session, SLOC
counted (`wc -l *.c *.h` minus vendor).

---

## 12. Build

```make
# config.mk (Linux)
PKGS    = sdl2 mpv libcurl zlib
CFLAGS  = -std=c99 -pedantic -Wall -Wextra -O2 `pkg-config --cflags $(PKGS)`
LDFLAGS = `pkg-config --libs $(PKGS)` -lGL -lpthread -lm
```
```
make && sudo make install
```
Windows (MSYS2 UCRT64):
```
pacman -S mingw-w64-ucrt-x86_64-{gcc,SDL2,mpv,curl,zlib,pkgconf} make
make OS=win
```

---

## 13. Testing

- `test/` with fixture files: real-world-ugly Xtream JSON (string ids,
  `{}` for empty, base64 EPG), malformed M3U, gzipped XMLTV.
- `make test` builds tiny parser test binaries; no framework.
- A fake Xtream server script (`test/fakextream.py`, ~80 lines) serving the
  fixtures + a looping test `.ts` for end-to-end checks.

---

## 14. Open questions

- SDL2 now or straight to SDL3? (SDL3 is stable; leaning SDL3 if MSYS2 ships it.)
- Embed preview video in the details pane, or only play fullscreen?
  Preview is nicer but doubles provider connections if `max_connections=1`
  → make it a `config.h` toggle, default **off** for 1-connection accounts.
- Ship Stalker portal as an out-of-tree patch, suckless-style?
