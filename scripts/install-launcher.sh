#!/usr/bin/env bash
set -euo pipefail

# DragonRecomp Studio - Desktop Launcher Installer
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

EXEC_BIN="${REPO_ROOT}/build/studio/dragon-studio"
if [ ! -f "${EXEC_BIN}" ]; then
    echo "Compilando DragonRecomp Studio primero..."
    cmake --build "${REPO_ROOT}/build" --target dragon-studio
fi

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS_DIR="${DATA_DIR}/applications"
ICON_SCALABLE_DIR="${DATA_DIR}/icons/hicolor/scalable/apps"
ICON_PNG_DIR="${DATA_DIR}/icons/hicolor/256x256/apps"

mkdir -p "${APPS_DIR}" "${ICON_SCALABLE_DIR}" "${ICON_PNG_DIR}"

# Instalar iconos
cp "${REPO_ROOT}/assets/icons/dragon-studio.svg" "${ICON_SCALABLE_DIR}/dragon-studio.svg"
cp "${REPO_ROOT}/assets/icons/dragon-studio.png" "${ICON_PNG_DIR}/dragon-studio.png"

# Crear entrada .desktop con rutas absolutas
DESKTOP_FILE="${APPS_DIR}/dragon-studio.desktop"
cat > "${DESKTOP_FILE}" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=DragonRecomp Studio
GenericName=NES, SNES, GB & GBA Static Recompiler
Comment=Drag & Drop static recompilation of NES, SNES, Game Boy, and GBA ROMs to native Linux executables
Exec=${EXEC_BIN} %f
Icon=dragon-studio
Path=${REPO_ROOT}
Terminal=false
StartupNotify=true
Categories=Game;Emulator;
Keywords=NES;SNES;GameBoy;GBA;Recompiler;Dragon;Emulator;Gaming;
MimeType=application/x-nes-rom;application/x-snes-rom;application/x-gameboy-rom;application/x-gba-rom;
EOF

chmod +x "${DESKTOP_FILE}"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "${DESKTOP_FILE}"
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPS_DIR}" >/dev/null 2>&1 || true
fi

echo "========================================================"
echo " DragonRecomp Studio instalado en el app launcher!"
echo " Entrada: ${DESKTOP_FILE}"
echo " Icono:   ${ICON_SCALABLE_DIR}/dragon-studio.svg"
echo " Ejecutable: ${EXEC_BIN}"
echo "========================================================"
