# Kukagram handoff (2026-11-26, Codex → Claude)

Read `README-KUKAGRAM.md` first (working rules). This note is the current
state and the next steps.

## Hotkeys: user's choice wins (2026-11-26, Claude)

- The user binds ⌘W (next account) and ⌘E (previous) on purpose and
  relies on them. Codex's reserved-shortcut block from `fd8d9f6` silently
  replaced ⌘W with the default, so the user lost their switching keys.
  The block was removed at the user's request: any combination can be
  bound again, including macOS ones. Do not re-add such a restriction.
- Rebuilt, packaged (`kukagram-release.pb3XGUYh`) and reinstalled into
  `/Applications`; the saved `keyboard-shortcuts.ini` was never modified.

## Private API build (2026-11-26, Claude)

- The user filled `~/Developer/kukagram-build/api-private.cmake` (outside Git,
  mode 600). Its format was checked without printing the values. `out-release`
  was reconfigured with `-C` that file and `TDESKTOP_API_TEST=OFF`.
- The build finished without errors and was rebuilt incrementally at `fd8d9f6`.
  `ninja -n` then reported nothing left to build. Packaged and signed:
  `~/Developer/kukagram-build/packages.noindex/kukagram-release.6ScR73h2/`.
  `codesign --verify --deep --strict` passed.
- On a fresh portable profile the window shows one merged title bar. With
  `-debug`, `help.getConfig` returned a config (`this_dc: 2`) and no RPC
  error, so the server accepts the private API keys. Debug logs contain
  the api_id, so the test profile was deleted afterwards.
- With the user's go-ahead this build replaced `/Applications/kukagram.app`
  at 22:27. Backups made first (verified identical, 35998 files):
  `~/Developer/kukagram-backups/before-release-20261126-2227/` holds the old
  DEBUG app and a copy of `~/Library/Application Support/kukagram`. To roll
  back: quit Kukagram, restore both from there. The new app started and kept
  running; the user checks their accounts themselves.
- Codex's `b175df3` (search, labels, username peek) and `fd8d9f6` (reserved
  macOS shortcuts) were reviewed and no problems were found. The username
  peek matches the write order in `serialize_peer.cpp`.

## Resume here: audit paused for handoff

The user asked to check Kukagram for bugs, then explicitly paused the work
to continue with Claude. The audit is incomplete. No installed app was
replaced, no personal profile was launched and no real account/session data
was opened in this audit session.

- The audit started at `b175df3`. The working tree was clean except for the
  documented local `Telegram/lib_ui` patch; its reverse-apply check passed.
  `git pull --ff-only` found no incoming changes.
- One confirmed bug was fixed in `core/application.cpp`: account shortcuts
  could intercept macOS Close/Quit/Hide/Minimize and full screen. The recorder
  now rejects these combinations without replacing the previous binding.
  Loading an old conflicting binding restores that action's default.
  Physical key checks also cover the Russian keyboard layout.
- A standalone Qt check compiled the actual shortcut implementation from
  `application.cpp` and passed reserved-key rejection, Russian physical keys,
  default bindings, old-setting fallback, rejected-save preservation,
  recording again after rejection and normal key matching. Before the fix,
  the same implementation accepted Command-W as an account shortcut.
  Rerun: `python3 ../kukagram-build/checks.noindex/shortcuts/run.py`.
  This uses temporary settings only. The helper is outside Git and requires
  the toolchain mounted at `/Volumes/KG`.
- Clang `-fsyntax-only` passed for the full changed `application.cpp` using
  its real Release compilation flags. `git diff --check` passed. This is
  not a completed full build or a live login test.
- At the handoff check, the pre-existing Release builder was still running:
  ninja PID `68275`, progress `1135/1204`, log
  `~/Developer/kukagram-build/logs/release-api.log`, with no compiler errors
  found in that log. Wait for that process before building again. Since
  sources changed during its run, perform an incremental build against HEAD
  afterwards; do not package stale outputs.
- Never print full compiler process arguments, the compilation database or
  private CMake values: they contain API credentials. Use process names,
  PIDs and elapsed time for monitoring.

Continue by finishing the Release build, checking any failures, rebuilding
against the final HEAD, then packaging/signing and testing on a synthetic
portable profile as described below. Do not claim all bugs have been ruled
out. Test successful authorization/switching only with separately prepared
test accounts; do not use the user's real accounts or `tdata`.

The latest design discussion suggested a visible active-account label above
the message field, favorites, switching between the last two accounts,
local notes and a clearer background-activity screen. These are proposals;
the user has not selected one for implementation.


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
- "Next account" was bound to ⌘W in the installed version. The current source
  rejects this conflict, but the fix has not been installed or tested with
  real account switching.
- Cancel/Save buttons are English in Russian dialogs; the menu footer says
  "Telegram Desktop"; the Moon toggle is only reachable by a right click on an
  empty chat; Kukagram menu items sit below the full account list.
