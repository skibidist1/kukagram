# Kukagram handoff (2026-11-26, Claude → Codex)

Read `README-KUKAGRAM.md` first (working rules). This note is the current
state and the next steps.

## Account search and group menu

- The top of ☰ now has «Группы аккаунтов» and «Поиск аккаунта».
  ⌘⌥K opens the same search. Enter switches to the selected account.
- Local labels are stored in `account-labels.ini` inside the current profile.
  Search also indexes usernames from the existing saved account summaries.
- The group filter stays visible with inactive accounts unloaded. It can
  be cleared from «Все аккаунты» above the filtered list.
- Clang checked each changed translation unit. The Qt dialog checks with
  dummy accounts passed for label persistence, username search, Enter,
  opening a group in the menu, renaming it and deleting it.
- A Release build was already running during the edits. Confirm an
  incremental rebuild against the current tree before packaging.

## Release validation (2026-11-26, Codex)

- The reconstructed source compiled and linked in Release for arm64 at
  `0c40841`. The build log is `~/Developer/kukagram-build/logs/release-codex.zeCQPKGW`.
  The linker warned about dependencies targeting newer macOS versions. The
  final bundle declares macOS 26.6.2 based on its packaged libraries.
- `kukagram-patches/package-app.sh` created the locally signed test ZIP at
  `~/Developer/kukagram-build/packages.noindex/kukagram-release.qJYR1y9N/kukagram-arm64.zip`.
  ZIP integrity and strict deep signature verification passed after extraction.
  The process loaded bundled libraries and no libraries from `/Volumes/KG`.
- On a temporary profile, startup, the QR and phone screens, and the native
  `tdata` folder picker worked. Selecting a synthetic empty folder produced
  the expected “Папка tdata не найдена” message and returned to the QR screen.
  On this Mac, place `TelegramForcePortable` next to the extracted `.app`
  for UI testing as well as passing `-workdir`. This also isolates any launch
  without arguments by the UI automation tool.
- The first UI run reopened the test executable without its profile arguments
  and briefly showed the personal profile. The replacement test run used the
  portable marker with a fresh profile. The installed app's signature and
  executable checksum still match the pre-test values.
- This ZIP uses the public test API configuration. A QR request returned
  `API_ID_PUBLISHED_FLOOD`. Successful authorization, switching accounts,
  the group-filter menu and importing an authorized session remain to be
  tested using separately prepared test accounts and test data.
- The local `Telegram/lib_ui` checkout has the documented patch already
  applied as a local commit. The parent repo still pins the upstream revision.

## Title bar fix (2026-11-26, Claude)

- With Homebrew Qt 6.12 the Release build showed the native macOS title bar
  as a separate strip above Kukagram's own title bar. Qt 6.9+ drops
  `NSWindowStyleMaskFullSizeContentView` unless `Qt::ExpandedClientAreaHint`
  is set. `ui_window_mac.mm` now sets that hint (in `kukagram-patches/lib_ui.patch`).
- Rebuilt and repackaged:
  `~/Developer/kukagram-build/packages.noindex/kukagram-release.FP6DUrBb/`.
  On an isolated portable profile the window shows one merged title bar.

## State

- Source: this repository, branch `kukagram`, pushed to
  `github.com/skibidist1/kukagram` (private). GitHub Actions are disabled on
  purpose; do not re-enable them.
- The source was reconstructed on 2026-11-26 after the original work folder
  was lost (see README). The Release build and packaging results are above.
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

1. Once `~/Developer/kukagram-build/api-private.cmake` is available, reconfigure
   with that file and `TDESKTOP_API_TEST=OFF`, rebuild and run the packager.
2. Test on an isolated profile only (`-workdir` pointing to a temporary
   folder), never on `~/Library/Application Support/kukagram`: start, QR and
   phone login screens, account switching, account groups including the
   "Открыть в меню ☰" group mode, tdata import dialog.
3. Only then replace `/Applications/kukagram.app`, keeping the old bundle as a
   backup, and only with the user's go-ahead.

## Done in this session

- `15be344`: account search in both Kukagram dialogs finds phones typed with
  `+`, spaces or brackets, and shows phones formatted.
- Group mode in the ☰ menu is in the source and compiled in Release.

## Observed in the installed app (not fixed yet)

- Installed build is DEBUG (unoptimized).
- "Next account" is bound to ⌘W, which is macOS "close window"; the shortcut
  dialog does not warn about system conflicts. Synthetic key presses did not
  switch accounts; ask the user whether the shortcuts work for them.
- Cancel/Save buttons are English in Russian dialogs; the menu footer says
  "Telegram Desktop"; the Moon toggle is only reachable by a right click on an
  empty chat; Kukagram menu items sit below the full account list.
