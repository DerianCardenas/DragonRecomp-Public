#!/usr/bin/env bash
set -euo pipefail

# ============================================================
#   DragonRecomp Studio - Lanzador para Linux
#   Compila (si hace falta) y abre la interfaz grafica.
#   Uso:  ./run-studio.sh  [ruta/opcional/al/rom]
# ============================================================

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

# 1. Si existe dragon-studio en el directorio actual (paquete portable distribuible), ejecutarlo directamente
if [ -x "${REPO_ROOT}/dragon-studio" ]; then
    if [ -d "${REPO_ROOT}/lib" ]; then
        export LD_LIBRARY_PATH="${REPO_ROOT}/lib:${LD_LIBRARY_PATH:-}"
    fi
    exec "${REPO_ROOT}/dragon-studio" "$@"
fi

STUDIO_BIN="${REPO_ROOT}/build/studio/dragon-studio"

if [ ! -x "${STUDIO_BIN}" ]; then
    echo "========================================================"
    echo "  DragonRecomp Studio no esta compilado todavia."
    echo "  Compilando por primera vez (esto puede tardar)..."
    echo "========================================================"

    if ! command -v cmake >/dev/null 2>&1; then
        echo "Error: CMake no esta instalado. Instalalo con tu gestor de paquetes." >&2
        exit 1
    fi

    if [ ! -d "${REPO_ROOT}/build" ]; then
        if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
            CC=clang CXX=clang++ cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" \
                -G Ninja -DCMAKE_BUILD_TYPE=Release
        else
            cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" -DCMAKE_BUILD_TYPE=Release
        fi
    fi

    cmake --build "${REPO_ROOT}/build" --target dragon-studio
fi

exec "${STUDIO_BIN}" "$@"
