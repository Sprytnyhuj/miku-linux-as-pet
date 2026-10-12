#!/usr/bin/env bash
# Instalator desktop-pet (C++/Qt6) - wszystko trafia do katalogu domowego.
#   ./install.sh              budowanie i instalacja / aktualizacja
#   ./install.sh --autostart  dodatkowo uruchamianie przy logowaniu (KDE)
#   ./install.sh --desktop    dodatkowo ikona na pulpicie
# Budowanie: lokalnie (cmake + nagłówki Qt6), a gdy ich brak - w distroboxie "builder".
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"   # -P: ta sama ścieżka co w kontenerze (/home -> /var/home)
BUILD_DIR="$SRC_DIR/build"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/desktop-pet"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/desktop-pet"
BIN_DIR="$HOME/.local/bin"
LAUNCHER="$BIN_DIR/desktop-pet"

AUTOSTART=0
DESKTOP_ICON=0
for arg in "$@"; do
    case "$arg" in
        --autostart) AUTOSTART=1 ;;
        --desktop) DESKTOP_ICON=1 ;;
        -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
        *) echo "Nieznana opcja: $arg" >&2; exit 1 ;;
    esac
done

build_cmd='cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$2" --target desktop-pet'

# Katalog build/ z innym generatorem (albo z innej ścieżki źródeł) wywraca cmake - wtedy czyścimy
# wyłącznie ten katalog (tylko $SRC_DIR/build, tylko gdy to prawdziwy katalog z CMakeCache.txt).
cache="$BUILD_DIR/CMakeCache.txt"
if [ -f "$cache" ] && [ ! -L "$BUILD_DIR" ] && [ "$BUILD_DIR" = "$SRC_DIR/build" ]; then
    cache_gen=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache" | head -n 1 || true)
    cache_home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | head -n 1 || true)
    if [ "$cache_gen" != "Ninja" ] || [ "$cache_home" != "$SRC_DIR" ]; then
        echo "==> Stary katalog build/ (generator: ${cache_gen:-?}) - czyszczę"
        rm -rf -- "$BUILD_DIR"
    fi
fi

echo "==> Budowanie"
if command -v cmake >/dev/null && command -v ninja >/dev/null && pkg-config --exists Qt6Widgets Qt6DBus Qt6Network 2>/dev/null \
        && { [ -d /usr/lib64/cmake/KF6Runner ] || [ -d /usr/lib/cmake/KF6Runner ]; }; then
    bash -c "$build_cmd" _ "$SRC_DIR" "$BUILD_DIR"
elif command -v distrobox >/dev/null; then
    # Wyszukiwarka korzysta z KF6 Runner: nagłówki doinstalowujemy w kontenerze (nie na hoście).
    distrobox enter builder -- bash -c 'rpm -q kf6-krunner-devel >/dev/null 2>&1 || sudo dnf install -y kf6-krunner-devel'
    # Katalog domowy jest współdzielony z kontenerem, więc wynik ląduje w $BUILD_DIR.
    distrobox enter builder -- bash -c "$build_cmd" _ "$SRC_DIR" "$BUILD_DIR"
else
    echo "Brak cmake/ninja/Qt6-devel i distroboxa 'builder' - nie da się zbudować." >&2
    exit 1
fi

echo "==> Foldery"
mkdir -p "$APP_DIR" "$CONF_DIR/sprites" "$BIN_DIR"

echo "==> Kopiowanie programu"
# install zamienia plik atomowo (nowy inode) - działającej kopii nie psuje.
install -m 755 "$BUILD_DIR/desktop-pet" "$APP_DIR/desktop-pet"

cat > "$LAUNCHER" <<EOF2
#!/usr/bin/env bash
# XWayland - na natywnym Waylandzie okno nie może ustawiać własnej pozycji.
export QT_QPA_PLATFORM=xcb
exec "$APP_DIR/desktop-pet" "\$@"
EOF2
chmod 755 "$LAUNCHER"

# Ikona: pierwsza klatka Shimeji, jeśli jest; inaczej ikona z motywu.
ICON=face-smile
SPRITE=$(find "$CONF_DIR/sprites" -name shime1.png 2>/dev/null | sort | head -n 1 || true)
if [ -n "$SPRITE" ]; then
    install -m 644 "$SPRITE" "$APP_DIR/icon.png"
    ICON="$APP_DIR/icon.png"
fi

DESKTOP_ENTRY="[Desktop Entry]
Type=Application
Name=Desktop Pet
Comment=Maskotka na pulpit
Exec=$LAUNCHER
Icon=$ICON
Terminal=false
Categories=Utility;"

mkdir -p "$HOME/.local/share/applications"
printf '%s\n' "$DESKTOP_ENTRY" > "$HOME/.local/share/applications/desktop-pet.desktop"

if [ "$DESKTOP_ICON" = 1 ]; then
    echo "==> Ikona na pulpicie"
    DESKTOP_DIR=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
    mkdir -p "$DESKTOP_DIR"
    printf '%s\n' "$DESKTOP_ENTRY" > "$DESKTOP_DIR/desktop-pet.desktop"
    chmod 755 "$DESKTOP_DIR/desktop-pet.desktop"     # KDE uruchamia tylko "zaufane" (wykonywalne) skróty
fi

if [ "$AUTOSTART" = 1 ]; then
    echo "==> Autostart"
    mkdir -p "$HOME/.config/autostart"
    printf '%s\nX-KDE-autostart-after=panel\n' "$DESKTOP_ENTRY" > "$HOME/.config/autostart/desktop-pet.desktop"
fi

echo
echo "Gotowe. Uruchom: desktop-pet"
echo "Sprite'y Shimeji (shime*.png) wrzuć do: $CONF_DIR/sprites/"
