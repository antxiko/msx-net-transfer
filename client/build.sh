#!/bin/bash
#=============================================================================
# build.sh — compila nt.com usando MSXgl (copia en MSXonLIVE)
#
# Como funciona:
#   1. Copia los fuentes (.c, .h, project_config.js) a
#      <MSXgl>/projects/nt/
#   2. Lanza node build.js desde ahi (mismo flujo que el proyecto tetris)
#   3. Devuelve el .COM a este directorio
#
# Salida:
#   ./nt.com  (listo para copiar a un disco/DSK de MSX-DOS 2)
#=============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
MSXGL="${MSXGL:-$HERE/../../MSXonLIVE/MSXgl}"

if [[ ! -d "$MSXGL/engine/script/js" ]]; then
    echo "[ERR] No encuentro MSXgl en: $MSXGL"
    echo "      Pasa la ruta con la variable MSXGL=/ruta/a/MSXgl bash build.sh"
    exit 1
fi

DST="$MSXGL/projects/nt"
mkdir -p "$DST"

echo "[+] Copiando fuentes a $DST"
cp "$HERE/nt.c"        "$DST/"
cp "$HERE/network.h"        "$DST/"
cp "$HERE/msxgl_config.h"   "$DST/"
cp "$HERE/project_config.js" "$DST/"
# Copy local header overrides (shadows MSXgl's engine/src/network/unapi_tcp.h
# with a version that adds __sdcccall(0) to all tcpip_* function declarations)
mkdir -p "$DST/network"
cp "$HERE/network/unapi_tcp.h" "$DST/network/"

# build.sh dentro del proyecto MSXgl
cat > "$DST/build.sh" <<'EOF'
#!/bin/bash
clear
if type -P node; then
    node ../../engine/script/js/build.js "$@"
else
    ../../tools/build/Node/node ../../engine/script/js/build.js "$@"
fi
EOF
chmod +x "$DST/build.sh"

echo "[+] Compilando desde $DST"
cd "$DST"
bash ./build.sh "$@"

# Localizar la salida .com (MSXgl la deja normalmente en out/ o en el dir del proyecto)
COM=""
for cand in "$DST/out/nt.com" "$DST/nt.com" "$DST/out/NT.COM" "$DST/NT.COM"; do
    if [[ -f "$cand" ]]; then COM="$cand"; break; fi
done

if [[ -z "$COM" ]]; then
    echo "[WARN] No encontre nt.com en el directorio de salida estandar."
    echo "       Revisa $DST/out/ manualmente."
    exit 0
fi

cp "$COM" "$HERE/nt.com"
echo "[OK] Compilado: $HERE/nt.com  ($(wc -c < "$HERE/nt.com") bytes)"
