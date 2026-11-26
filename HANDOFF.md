# Kukagram handoff (2026-11-26, Claude → Codex)

Read `README-KUKAGRAM.md` first (working rules). This note is the current
state and the next steps.

## State

- Source: this repository, branch `kukagram`, pushed to
  `github.com/skibidist1/kukagram` (private). GitHub Actions are disabled on
  purpose; do not re-enable them.
- The source was reconstructed on 2026-11-26 after the original work folder
  was lost (see README). It now configures and compiles: a Release build
  reached 1647 of 2437 steps with no errors when this note was written.
- `/Applications/kukagram.app` is the old DEBUG build installed by Codex on
  2026-11-22. It is untouched. Do not replace it without the checklist in the
  README.

## Build toolchain (outside the repository)

Everything lives in `~/Developer/kukagram-build`:

- `setup.sh` builds the toolchain in stages, each marked by `.done-<stage>`
  (brew, patches, rnnoise, tg_owt, tde2e, rust, tdesktop_rust are done).
  Logs are in `logs/`.
- arm64 Homebrew is on the disk image `kukagram-tools.sparseimage`, mounted
  at `/Volumes/KG`. Homebrew bottles only relocate to a prefix no longer than
  `/opt/homebrew`, which is why the short mount point is required. After a
  reboot `setup.sh` remounts it; manually:
  `hdiutil attach -nobrowse -mountpoint /Volumes/KG ~/Developer/kukagram-build/kukagram-tools.sparseimage`.
  The user's own Homebrew in `/usr/local` is x86_64 and must not be used.
- Built libraries: `Libraries/local/{tg_owt,tde2e,rnnoise,tdesktop_rust}`.
  Packages: qtbase 6.12, ffmpeg@6, openssl@3, cmake, ninja (see
  `BUILD_ENVIRONMENT.txt`).
- `build-release.sh` configures `out-release` (Release, packaged mode,
  `-DTDESKTOP_API_TEST=ON` unless `~/Developer/kukagram-build/api-private.cmake`
  exists) and builds the `Telegram` target with 3 jobs. Log:
  `logs/release.log`. It was started in a Claude terminal tab and keeps
  running; check `pgrep -f build-release.sh` and the log.

Problems already solved, keep these settings:

- Command Line Tools only (no Xcode): libc++ headers are passed with
  `-isystem $SDK/usr/include/c++/v1`.
- Deployment target is macOS 13.0: with 15.0 the SDK makes
  `CGDisplayCreateImage` / `CGDisplayStreamCreate` unavailable and tg_owt and
  lib_webrtc fail.
- Eight submodules had only a `.git` file; they were restored with
  `git reset --hard HEAD` inside each. If a submodule looks empty, do the same.
- `Telegram/lib_ui` stays pinned to upstream; Kukagram's lib_ui change is in
  `kukagram-patches/lib_ui.patch` (local branch `kukagram` in the submodule
  already has it applied).

## Next steps

1. Wait for `build-release.sh` to finish. It runs ninja with `-k 0`, so one
   pass reports every failing file (`grep '^FAILED' logs/release.log`). Fix
   the code, commit, rerun the script (ninja resumes). Missing pieces are
   usually edits Codex made with Python scripts: search the old Codex session
   logs in `~/.codex/sessions` for the missing identifier, as was done for
   `introTdataImportLeft` in `intro.style`.
2. Package and sign the bundle. The old `package-app.sh` was lost; write a
   new one: copy the built `.app` to a temporary folder, ad-hoc sign nested
   frameworks and the app (`codesign --force --deep --sign -`), then
   `codesign --verify --deep --strict`.
3. Real Telegram API credentials: the build uses public test credentials.
   Ask the user to put `api-private.cmake` with `TDESKTOP_API_ID` /
   `TDESKTOP_API_HASH` into `~/Developer/kukagram-build/` (never into git),
   then reconfigure.
4. Test on an isolated profile only (`-workdir` pointing to a temporary
   folder), never on `~/Library/Application Support/kukagram`: start, QR and
   phone login screens, account switching, account groups including the
   "Открыть в меню ☰" group mode, tdata import dialog.
5. Only then replace `/Applications/kukagram.app`, keeping the old bundle as a
   backup, and only with the user's go-ahead.

## Done in this session

- `15be344`: account search in both Kukagram dialogs finds phones typed with
  `+`, spaces or brackets, and shows phones formatted.
- Group mode in the ☰ menu (written by Codex on 2026-11-23, never built
  before) is in the source and compiles as part of this build.

## Observed in the installed app (not fixed yet)

- Installed build is DEBUG (unoptimized).
- "Next account" is bound to ⌘W, which is macOS "close window"; the shortcut
  dialog does not warn about system conflicts. Synthetic key presses did not
  switch accounts; ask the user whether the shortcuts work for them.
- Cancel/Save buttons are English in Russian dialogs; the menu footer says
  "Telegram Desktop"; the Moon toggle is only reachable by a right click on an
  empty chat; Kukagram menu items sit below the full account list.
