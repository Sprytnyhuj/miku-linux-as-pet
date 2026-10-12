// Stałe, ścieżki i ustawienia programu (odpowiednik początku pet.py).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cmath>
#include <functional>

#include <QJsonValue>
#include <QObject>

namespace cfg {

inline constexpr int TICK_MS = 40;            // 25 klatek na sekundę
inline constexpr double WALK_SPEED = 2.0;     // px na tick
inline constexpr double GRAVITY = 1.2;        // px/tick^2
inline constexpr double MAX_FALL = 30.0;
inline constexpr int DRAG_THRESHOLD = 6;      // px ruchu myszy, po których kliknięcie staje się przeciąganiem

inline constexpr int VOLUME_STEP = 5;             // o ile % zmienia głośność przycisk −/+ albo jeden ząbek kółka
inline constexpr int BUBBLE_TIMEOUT_MS = 7000;    // chmurka znika po tym czasie (chyba że kursor jest nad nią)
inline constexpr int BUBBLE_OVERLAP = 14;         // o ile px ogonek chmurki nachodzi na czubek głowy

// Steam: pierwszy istniejący folder z steamapps/ to instalacja (natywna albo Flatpak).
inline constexpr int STEAM_RESCAN_S = 30;   // co tyle sekund lista gier czytana od nowa (albo po zmianie folderu)
inline constexpr int STEAM_ICON_PX = 24;    // ikonki gier w menu
inline constexpr const char *STEAM_RUN_URL = "steam://rungameid/%1";
inline constexpr const char *STEAM_LIBRARY_URL = "steam://open/games";
inline constexpr int STEAM_FULLY_INSTALLED = 4;   // bit w StateFlags; inne bity = np. trwa aktualizacja

// Python zaokrągla połówki do parzystej (round), std::nearbyint też - zachowanie 1:1.
inline int pyRound(double v) { return static_cast<int>(std::nearbyint(v)); }

// Ścieżki (XDG_CONFIG_HOME ma pierwszeństwo przed ~/.config).
QString configDir();
QString spritesDir();
QString settingsFile();

// Skala sprite'ów z DESKTOP_PET_SCALE (domyślnie 1.0).
double spriteScale();

// settings.json: odczyt całości i zapis; nieznane klucze są zachowywane.
QJsonObject loadSettings();
void saveSettings(const QJsonObject &data);

// Wynik uruchomienia zewnętrznego programu (z limitem czasu - interfejs nie wisi dłużej).
struct ProcResult {
    bool finished = false;   // wystartował i skończył w limicie czasu
    int exitCode = -1;
    QByteArray out;
};
ProcResult runCapture(const QString &program, const QStringList &args, int timeoutMs);

// To samo bez blokowania pętli zdarzeń: wynik (po zakończeniu albo po limicie czasu) trafia do
// `done` w wątku obiektu `ctx`. Gdy ctx zniknie wcześniej, proces jest zabijany, a `done` nie wywołane.
void runAsync(QObject *ctx, const QString &program, const QStringList &args, int timeoutMs,
              std::function<void(const ProcResult &)> done);

// Zapis jednego klucza do settings.json w wątku roboczym (kolejność zapisów zachowana,
// pozostałe klucze zostają). Pętla zdarzeń nie czeka na dysk.
void saveSettingKeyAsync(const QString &key, const QJsonValue &value);
// Czeka na zakończenie zapisów w tle (testy, wyjście z programu).
void flushSettings();

}  // namespace cfg
