#!/bin/bash
#=============================================================================
# build.sh — compila nts.com (Net Transfer server MSX) usando MSXgl
# Estructura calcada de client/build.sh.
#=============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
MSXGL="${MSXGL:-$HERE/../../MSXonLIVE/MSXgl}"

if [[ ! -d "$MSXGL/engine/script/js" ]]; then
    echo "[ERR] No encuentro MSXgl en: $MSXGL"
    echo "      MSXGL=/ruta/a/MSXgl bash build.sh"
    exit 1
fi

DST="$MSXGL/projects/nts"
mkdir -p "$DST"

echo "[+] Copiando fuentes a $DST"
cp "$HERE/nts.c"             "$DST/"
cp "$HERE/network.h"         "$DST/"
cp "$HERE/msxgl_config.h"    "$DST/"
cp "$HERE/project_config.js" "$DST/"
# Header overrides (shadows MSXgl's network/unapi_tcp.h with __sdcccall(0))
mkdir -p "$DST/network"
cp "$HERE/network/unapi_tcp.h" "$DST/network/"

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

COM=""
for cand in "$DST/out/nts.com" "$DST/nts.com" "$DST/out/NTS.COM" "$DST/NTS.COM"; do
    if [[ -f "$cand" ]]; then COM="$cand"; break; fi
done

if [[ -z "$COM" ]]; then
    echo "[WARN] No encontre nts.com en el directorio de salida estandar."
    exit 0
fi

cp "$COM" "$HERE/nts.com"
echo "[OK] Compilado: $HERE/nts.com  ($(wc -c < "$HERE/nts.com") bytes)"
