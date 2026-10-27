# Working on sit

Read `plan.md` first: §0 says what is done, what is open and what has not
been tested. When you finish something, update §0 in the same commit. Do
not restart finished milestones or redo decisions recorded there without
asking the owner.

## Style

suckless C, like dwm: C99, tabs, return type on its own line, declarations
at the top of blocks, short comments only where the why is not obvious.
Builds must stay free of warnings with `-std=c99 -pedantic -Wall -Wextra`.
No new dependencies without asking; vendored code goes in `vendor/`.
Defaults go in `config.def.h`; `config.h` is the user's copy and is not
committed.

## Build

Windows, MSYS2 UCRT64 (installed at `C:\msys64`; there is no compiler on
the normal PATH):

    MSYSTEM=UCRT64 C:\msys64\usr\bin\bash.exe -lc "cd <repo> && make OS=win"

From PowerShell set `$env:MSYSTEM='UCRT64'; $env:CHERE_INVOKING='1'` first.
`sit.exe` needs `C:\msys64\ucrt64\bin` on PATH to start. If the link fails
with "cannot open output file sit.exe", the owner has sit open: build test
binaries elsewhere and ask them to close it before the final link.

Linux: `make` (never tried yet — see plan §0).

## Testing

There are no unit tests. Features are checked end to end:

1. Run the fake server: `python test/fakextream.py 8099 video.mp4`
   (user `demo`, password `s3cr/t!`; `FAKE_SCALE=40` for big lists; make a
   video with the ffmpeg line in its docstring). Extend it for new features.
2. Point sit at it with a sources file and throwaway dirs:
   `XDG_CACHE_HOME=... XDG_DATA_HOME=... sit -s <file>`, so the owner's
   real cache, favorites and resume files are never touched.
3. To see the result, compile a copy with a temporary `#ifdef SHOT` block
   at the end of `draw()` that drives the UI on a timer and dumps frames
   with `glReadPixels` to PPM (convert with ffmpeg). Never commit the hook.
   The test window takes keyboard focus, so stray keystrokes from the owner
   can change what it shows; check the fake server's log when in doubt.

## Safety

- Never put credentials in the repo, logs, screenshots or messages. The
  owner's provider login is in `%APPDATA%\sit\sources`; do not read it.
  To debug that file, print its structure (field counts), not its contents.
- The cache keeps the provider's account answer, which includes the
  password; do not share cache files either.
- The repo is private. Ask before making it public, pushing, or creating
  releases.

## Gotchas

- `core.autocrlf` is on; `.gitattributes` keeps everything LF.
- Shell heredocs with `\n` or `\x` escapes get mangled through PowerShell;
  write patch scripts to files instead.
- Measure memory from inside the process (`K32GetProcessMemoryInfo`);
  outside samples start before the first frame and mislead.
