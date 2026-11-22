#!/usr/bin/env bash
set -euo pipefail

# ============================================================
#   DragonRecomp Studio - Empaquetador Portable para Linux
#   Genera un bundle standalone (.tar.gz y .zip) con el
#   ejecutable dragon-studio, recompiladores, SDL3 y assets.
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

VERSION="${DRAGON_VERSION:-v1.0.0}"
DIST_DIR="${REPO_ROOT}/dist"
PACKAGE_NAME="DragonRecomp-Studio-${VERSION}-linux-x64"
PACKAGE_DIR="${DIST_DIR}/${PACKAGE_NAME}"

echo "========================================================"
echo "  Empaquetando DragonRecomp Studio para Linux x64"
echo "  Version: ${VERSION}"
echo "  Destino: ${PACKAGE_DIR}"
echo "========================================================"

# 1. Asegurar que los binarios esten compilados
BUILD_DIR="${REPO_ROOT}/build"
REQUIRED_TARGETS=(
    "studio/dragon-studio"
    "nes/recompiler/nesrecomp"
    "snes/recompiler/snesrecomp"
    "gb/recompiler/gbrecomp"
    "gba/recompiler/gbarecomp"
    "sms/recompiler/smsrecomp"
    "pce/recompiler/pcerecomp"
    "md/recompiler/mdrecomp"
    "nds/recompiler/ndsrecomp"
    "psp/recompiler/psprecomp"
)

NEED_BUILD=0
for target in "${REQUIRED_TARGETS[@]}"; do
    if [ ! -f "${BUILD_DIR}/${target}" ]; then
        NEED_BUILD=1
        break
    fi
done

if [ "${NEED_BUILD}" -eq 1 ]; then
    echo "Compilando dependencias requeridas..."
    cmake --build "${BUILD_DIR}" --target dragon-studio nesrecomp snesrecomp gbrecomp gbarecomp smsrecomp pcerecomp mdrecomp ndsrecomp psprecomp
fi

# 2. Preparar directorio limpio de empaquetado
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}" "${PACKAGE_DIR}/lib" "${PACKAGE_DIR}/assets"

# 3. Copiar binarios y aplicar strip para optimizar tamano
echo "Copiando binarios principales..."
cp "${BUILD_DIR}/studio/dragon-studio" "${PACKAGE_DIR}/dragon-studio"
cp "${BUILD_DIR}/nes/recompiler/nesrecomp" "${PACKAGE_DIR}/nesrecomp"
cp "${BUILD_DIR}/snes/recompiler/snesrecomp" "${PACKAGE_DIR}/snesrecomp"
cp "${BUILD_DIR}/gb/recompiler/gbrecomp" "${PACKAGE_DIR}/gbrecomp"
cp "${BUILD_DIR}/gba/recompiler/gbarecomp" "${PACKAGE_DIR}/gbarecomp"
cp "${BUILD_DIR}/sms/recompiler/smsrecomp" "${PACKAGE_DIR}/smsrecomp"
cp "${BUILD_DIR}/pce/recompiler/pcerecomp" "${PACKAGE_DIR}/pcerecomp"
cp "${BUILD_DIR}/md/recompiler/mdrecomp" "${PACKAGE_DIR}/mdrecomp"
cp "${BUILD_DIR}/nds/recompiler/ndsrecomp" "${PACKAGE_DIR}/ndsrecomp"
cp "${BUILD_DIR}/psp/recompiler/psprecomp" "${PACKAGE_DIR}/psprecomp"

chmod +x "${PACKAGE_DIR}/dragon-studio" \
         "${PACKAGE_DIR}/nesrecomp" \
         "${PACKAGE_DIR}/snesrecomp" \
         "${PACKAGE_DIR}/gbrecomp" \
         "${PACKAGE_DIR}/gbarecomp" \
         "${PACKAGE_DIR}/smsrecomp" \
         "${PACKAGE_DIR}/pcerecomp" \
         "${PACKAGE_DIR}/mdrecomp" \
         "${PACKAGE_DIR}/ndsrecomp" \
         "${PACKAGE_DIR}/psprecomp"

if command -v strip >/dev/null 2>&1; then
    echo "Reduciendo simbolos debug con strip..."
    strip --strip-unneeded "${PACKAGE_DIR}/dragon-studio" \
                           "${PACKAGE_DIR}/nesrecomp" \
                           "${PACKAGE_DIR}/snesrecomp" \
                           "${PACKAGE_DIR}/gbrecomp" \
                           "${PACKAGE_DIR}/gbarecomp" \
                           "${PACKAGE_DIR}/smsrecomp" \
                           "${PACKAGE_DIR}/pcerecomp" \
                           "${PACKAGE_DIR}/mdrecomp" \
                           "${PACKAGE_DIR}/ndsrecomp" \
                           "${PACKAGE_DIR}/psprecomp" 2>/dev/null || true
fi

# 4. Incluir libSDL3.so.0 portable
echo "Empaquetando runtime SDL3 portable..."
SDL3_CANDIDATES=(
    "/usr/lib/libSDL3.so.0"
    "/usr/lib64/libSDL3.so.0"
    "/usr/local/lib/libSDL3.so.0"
    "/usr/lib/x86_64-linux-gnu/libSDL3.so.0"
    "${BUILD_DIR}/libSDL3.so.0"
)

SDL3_FOUND=""
for p in "${SDL3_CANDIDATES[@]}"; do
    if [ -f "${p}" ]; then
        SDL3_FOUND="${p}"
        break
    fi
done

if [ -n "${SDL3_FOUND}" ]; then
    cp -L "${SDL3_FOUND}" "${PACKAGE_DIR}/lib/libSDL3.so.0"
    echo "  -> libSDL3.so.0 incluida desde ${SDL3_FOUND}"
else
    echo "  Aviso: no se encontro libSDL3.so.0 local; el paquete dependera de SDL3 del sistema."
fi

# 5. Copiar assets y bases de conocimiento
echo "Copiando assets y configuraciones knowledge..."
cp -r "${REPO_ROOT}/assets"/* "${PACKAGE_DIR}/assets/"
cp -r "${REPO_ROOT}/knowledge" "${PACKAGE_DIR}/knowledge"

# 6. Crear lanzador portable run-studio.sh
cat > "${PACKAGE_DIR}/run-studio.sh" << 'EOF'
#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [ -d "${SCRIPT_DIR}/lib" ]; then
    export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${LD_LIBRARY_PATH:-}"
fi

exec "${SCRIPT_DIR}/dragon-studio" "$@"
EOF
chmod +x "${PACKAGE_DIR}/run-studio.sh"

# 7. Crear entrada .desktop portable
cat > "${PACKAGE_DIR}/dragon-studio.desktop" << EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=DragonRecomp Studio
GenericName=NES, SNES, GB, GBA & SMS/GG Static Recompiler
Comment=Drag & Drop static recompilation of NES, SNES, Game Boy, GBA, Master System, and Game Gear ROMs to native Linux executables
Exec=./run-studio.sh %f
Icon=./assets/icons/dragon-studio.png
Terminal=false
StartupNotify=true
Categories=Game;Emulator;
Keywords=NES;SNES;GameBoy;GBA;MasterSystem;GameGear;SMS;Recompiler;Dragon;Emulator;Gaming;
MimeType=application/x-nes-rom;application/x-snes-rom;application/x-gameboy-rom;application/x-gba-rom;application/x-sms-rom;application/x-gamegear-rom;
EOF

# 8. Crear README.txt
cat > "${PACKAGE_DIR}/README.txt" << EOF
======================================================================
  DragonRecomp Studio ${VERSION} — Paquete Portable para Linux (x86_64)
======================================================================

Convierte tus juegos clasicos de NES, SNES, Game Boy / Game Boy Color,
Game Boy Advance y Sega Master System / Game Gear en programas nativos
de alto rendimiento para PC.

Consolas soportadas:
  - Nintendo Entertainment System (NES / 6502)
  - Super Nintendo (SNES / 65816)
  - Game Boy & Game Boy Color (Sharp SM83)
  - Game Boy Advance (ARM7TDMI ARMv4T + Thumb)
  - Sega Master System & Game Gear (Zilog Z80 / VDP Mode 4 / SN76489 PSG)
  - PC Engine & TurboGrafx-16 (HuC6280 / HuC6270 VDC / HuC6260 VCE / PSG)

COMO EJECUTAR EN LINUX:
  1. Haz doble clic en 'run-studio.sh' (o ejecuta './run-studio.sh' en la terminal).
  2. Arrastra y suelta tu ROM (.nes, .sfc, .smc, .gb, .gbc, .gba, .sms, .gg, .pce) en la ventana.
  3. Haz clic en "Recompilar y Ejecutar" para jugar a 60 FPS fluidos con
     sincronizacion de audio, guardados SRAM (.sav) y save states (F5 / F7).

REQUISITOS DEL SISTEMA:
  - Linux x86_64 (glibc >= 2.31)
  - Compilador Clang o GCC instalado (para compilar los modulos C99 del juego)
  - Soporte de graficos OpenGL 3.3+ o Vulkan
  - Libreria SDL3 portable ya incluida en 'lib/libSDL3.so.0'

======================================================================
EOF

# 9. Generar archivos comprimidos (.tar.gz y .zip)
echo "Generando archivos comprimidos..."
cd "${DIST_DIR}"

tar -czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
echo "  -> Creado: ${DIST_DIR}/${PACKAGE_NAME}.tar.gz"

if command -v zip >/dev/null 2>&1; then
    zip -rq "${PACKAGE_NAME}.zip" "${PACKAGE_NAME}"
    echo "  -> Creado: ${DIST_DIR}/${PACKAGE_NAME}.zip"
elif command -v 7z >/dev/null 2>&1; then
    7z a -tzip -bso0 "${PACKAGE_NAME}.zip" "${PACKAGE_NAME}" >/dev/null
    echo "  -> Creado: ${DIST_DIR}/${PACKAGE_NAME}.zip"
elif command -v bsdtar >/dev/null 2>&1; then
    bsdtar -acf "${PACKAGE_NAME}.zip" "${PACKAGE_NAME}"
    echo "  -> Creado: ${DIST_DIR}/${PACKAGE_NAME}.zip"
fi

# 10. Resumen y checksums
echo "========================================================"
echo "  Paquetes generados exitosamente en ${DIST_DIR}:"
ls -lh "${DIST_DIR}/${PACKAGE_NAME}.tar.gz" "${DIST_DIR}/${PACKAGE_NAME}.zip" 2>/dev/null || true
echo "--------------------------------------------------------"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${DIST_DIR}/${PACKAGE_NAME}.tar.gz"
    if [ -f "${DIST_DIR}/${PACKAGE_NAME}.zip" ]; then
        sha256sum "${DIST_DIR}/${PACKAGE_NAME}.zip"
    fi
fi
echo "========================================================"
