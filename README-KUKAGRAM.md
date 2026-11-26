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
Kukagram's only submodule change (emoji source cache in `Telegram/lib_ui`) is
kept in `kukagram-patches/lib_ui.patch` and applied by the script above.

## How this source was recovered

The original working folder was lost on 2026-11-24. This tree was rebuilt on
2026-11-26 from the Codex session logs (recorded edits and file listings), the
source checkpoint archives and resources extracted from the installed app.
Files were checked against the logged listings wherever those exist.

Known gaps:

- The tree has not been compiled yet: the build toolchain was lost as well.
- `ReadTdataAccounts` in `storage/storage_domain.cpp` contains about twenty
  hand-written lines (passcode wrap key derivation and account info decrypt)
  where the logged source was truncated.

## Not in this repository

`api-private.cmake` (Telegram API id and hash), `tdata`, account sessions and
the user's profile folder are private and must never be committed.

## Before replacing the installed app

Build arm64 cleanly, run `git diff --check`, verify the signed bundle with
`codesign --verify --deep --strict`, start it on an isolated test profile and
test QR login, phone login, account switching, groups and tdata import. Keep
the current `/Applications/kukagram.app` until the new bundle passes.
