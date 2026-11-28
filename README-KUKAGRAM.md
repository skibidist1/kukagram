# Kukagram

Kukagram is a personal macOS (Apple Silicon) fork of Telegram Desktop 7.4.1.
Bundle id `io.kukagram.desktop`, up to 1000 local accounts.

## Getting the sources

```sh
git clone <this repository> kukagram
cd kukagram
git submodule update --init --recursive
sh kukagram-patches/apply-lib_ui.sh
```

The submodules stay on the upstream revisions pinned by Telegram Desktop 7.4.1.
Kukagram's submodule changes (emoji source cache and macOS title bar in
`Telegram/lib_ui`) are kept in `kukagram-patches/lib_ui.patch` and applied
by the script above.

## How this source was recovered

The original working folder was lost on 2026-11-24. This tree was rebuilt on
2026-11-26 from the Codex session logs (recorded edits and file listings), the
source checkpoint archives and resources extracted from the installed app.
Files were checked against the logged listings wherever those exist.

Known gaps:

- `ReadTdataAccounts` in `storage/storage_domain.cpp` contains about twenty
  hand-written lines (passcode wrap key derivation and account info decrypt)
  where the logged source was truncated.

## Building and packaging on this Mac

With the arm64 tools mounted at `/Volumes/KG`, run these commands from the
repository root:

```sh
bash ../kukagram-build/build-release.sh
bash kukagram-patches/package-app.sh
```

The packager prints the paths to the ad-hoc signed app and ZIP under
`../kukagram-build/packages.noindex`. It checks the bundle dependencies,
the signature and ZIP integrity. The build uses the Telegram test API
credentials unless configured with the local private API file. When switching
an existing build to private credentials, reconfigure with
`-C ../kukagram-build/api-private.cmake -DTDESKTOP_API_TEST=OFF` before rebuilding.

## History rewrite (2026-11-27)

The Kukagram commits on `kukagram` were rewritten to remove personal data
from old notes and author names, and force-pushed. The code is identical.
`~/Developer/kukagram` is already up to date. Any other clone must be
re-cloned, not pulled. Commits in this repository use the local identity
`kukagram <kukagram@localhost>` (set in `.git/config`); keep it that way.
The repository is public: never commit account names, phone numbers,
sessions or API credentials.

## Working rules (Claude and Codex)

This project is worked on by both Claude and Codex. Every agent follows these
rules.

1. Work only in `~/Developer/kukagram`, the clone of this repository. Never
   copy the project into `~/Documents`, `~/Desktop` or `~/Documents/Codex`:
   those folders are synced with iCloud, which is how the original source was
   lost.
2. One agent at a time. Before starting, run `git status` and `git pull`; if
   the tree has uncommitted changes you did not make, stop and ask the user.
3. Finish every session with a commit and `git push` to `origin`, so the next
   agent starts from the current state and every change can be reverted.
4. Building and replacing `/Applications/kukagram.app` follows the checklist
   below and is done by one agent per change, never in parallel.
5. Do not take another agent's report on trust: check claims against the code
   and, where possible, by building and running.

Rules for the user, in Russian:

- Открывай Codex и Claude только в папке `~/Developer/kukagram`.
- Не запускай их одновременно на этом проекте: сначала один закончил, потом
  другой начал.
- В конце каждой сессии проси закоммитить и отправить изменения на GitHub.

## Account search and groups

- Press ⌘⌥K or choose «Поиск аккаунта» in ☰. Search by name, local
  label, username or phone. Select a result and press Enter to switch.
- Select «Изменить подпись» to save a local label for an account.
- Account shortcuts can use system combinations such as ⌘W. While Kukagram
  handles an assigned combination, it takes priority over the window action.
- In «Группы аккаунтов», choose a group and click «Открыть в меню ☰».
  The menu shows the group name and its members. «Все аккаунты» restores
  the full list.

## Not in this repository

`api-private.cmake` (Telegram API id and hash), `tdata`, account sessions and
the user's profile folder are private and must never be committed.

## Before replacing the installed app

Build arm64 cleanly, run `git diff --check`, verify the signed bundle with
`codesign --verify --deep --strict`, start it on an isolated test profile and
test QR login, phone login, account switching, groups and tdata import. Keep
the current `/Applications/kukagram.app` until the new bundle passes.
