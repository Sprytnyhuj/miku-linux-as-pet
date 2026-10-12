// Lekki dziennik zmian animacji (diagnostyka przerywania animacji). Domyślnie wyłączony:
// włącza go DESKTOP_PET_DEBUG=1 albo plik ~/.config/desktop-pet/debug (sprawdzane raz, przy pierwszym wpisie).
// Zapis do $XDG_STATE_HOME/desktop-pet/anim.log (domyślnie ~/.local/state/...), rotacja ok. 200 KB.
// Bez treści prywatnych: tytuły utworów tylko jako krótki skrót (hash).
#pragma once

#include <QString>

namespace animlog {
bool enabled();
void write(const QString &what);            // jeden wiersz z czasem; nic nie robi, gdy wyłączony
QString shortHash(const QString &text);     // 6 znaków hex; "-" dla pustego tekstu
void resetForTests();                       // ponowne sprawdzenie włączenia (testy)
QString logPath();
constexpr qint64 MAX_BYTES = 200 * 1024;
}  // namespace animlog
