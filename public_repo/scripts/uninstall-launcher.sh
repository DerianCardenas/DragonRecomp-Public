#!/usr/bin/env bash
set -euo pipefail

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS_DIR="${DATA_DIR}/applications"
ICON_SCALABLE_DIR="${DATA_DIR}/icons/hicolor/scalable/apps"
ICON_PNG_DIR="${DATA_DIR}/icons/hicolor/256x256/apps"

rm -f "${APPS_DIR}/dragon-studio.desktop"
rm -f "${ICON_SCALABLE_DIR}/dragon-studio.svg"
rm -f "${ICON_PNG_DIR}/dragon-studio.png"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPS_DIR}" >/dev/null 2>&1 || true
fi

echo "DragonRecomp Studio desinstalado del app launcher."
