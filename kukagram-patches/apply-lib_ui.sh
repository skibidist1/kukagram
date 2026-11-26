#!/bin/sh
# Applies Kukagram's lib_ui changes on top of the pinned upstream submodule.
set -e
cd "$(dirname "$0")/../Telegram/lib_ui"
if git apply --reverse --check ../../kukagram-patches/lib_ui.patch 2>/dev/null; then
	echo "lib_ui patch is already applied"
else
	git apply ../../kukagram-patches/lib_ui.patch
	echo "lib_ui patch applied"
fi
