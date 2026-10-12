# desktop-pet

Chibi maskotka na pulpit (C++17 / Qt6 Widgets + DBus + Network).

## Co robi

* Postać siedzi na dolnej krawędzi ekranu, tuż nad panelem KDE.
  Prawy przycisk -> "Co ma robić?": Siedzi (domyślnie; czasem się rozgląda
  albo macha nogami, czasem jest wesoła lub zamyślona), Leży i śpi, Stoi
  (czasem się zamyśla, wstydzi lub cieszy), Sama decyduje (głównie siedzi
  i leży, rzadko krótki spacer, czasem wesoła chwila), Macha porem, Spaceruje,
  Wesoła (siedzi wesoło, cieszy się, zamyśla), Wstydliwa (zawstydzona i siedzi),
  Tańczy (sekwencja klatek tańca i postój). Wybór zapisuje się
  w `~/.config/desktop-pet/settings.json` (klucz `mode`, pozostałe klucze zostają).
  Tryby są w tabeli `modes()` w `src/pet.cpp`.
* Reakcje: kliknięcie w postać -> losowo zaskoczenie "!" (najczęściej),
  zawstydzenie albo radość; upuszczenie z wysoka -> "plask" i chwila marudzenia
  (w trybie "Macha porem" bez marudzenia); miękkie lądowanie -> krótki "plask"
  albo zawstydzenie; szybkie kręcenie kółkiem (>= 6 ząbków w 1,5 s) ->
  zamyślona, wesoła, radość; zmiana utworu w odtwarzaczu (MPRIS, sprawdzana
  co 3 s, nie częściej niż co 8 s, nie podczas przeciągania i spadania) -> radość.
* Pojedyncze kliknięcie lewym przyciskiem (dymek otwiera się po upływie interwału podwójnego
  kliku systemu, `QApplication::doubleClickInterval()`, domyślnie 400 ms): chmurka nad głową z aktualnym utworem
  i przyciskami ⏮ ⏯ ⏭ oraz suwakiem głośności samej przeglądarki/odtwarzacza
  (strumień w PipeWire, przez `pactl` - reszta dźwięków systemu się nie zmienia).
  Znika sama po kilku sekundach albo po kliknięciu gdziekolwiek poza nią.
  Kolory chmurki pochodzą z palety systemu (jasny/ciemny motyw), przyciski
  i suwak są standardowymi widżetami stylu systemowego (Breeze).
* Podwójne kliknięcie lewym przyciskiem: wyszukiwarka jak KRunner w dymku nad głową.
  Pole tekstowe z fokusem i lista wyników (ikona z motywu, tytuł, podtytuł / kategoria,
  do 8 wierszy widocznych): programy, pliki i dokumenty, ustawienia systemu, kalkulator,
  jednostki, zakładki itd. - dokładnie to, co daje KRunner w Plasmie, bo silnikiem jest
  biblioteka KF6 Runner (`KRunner::RunnerManager`) działająca w procesie programu (te same
  pluginy i ustawienia co w `krunnerrc`; pluginy D-Bus `org.kde.krunner1` też).
  Wyniki pojawiają się po ~120 ms od ostatniego znaku, zapytania są asynchroniczne.
  Strzałki góra/dół (PageUp/PageDown) zmieniają zaznaczenie, Enter uruchamia zaznaczony
  (domyślnie pierwszy), klik w wynik też uruchamia; Esc albo klik poza dymkiem zamyka.
  Okno to `Qt::Tool` + `WindowStaysOnTopHint` (nie `Qt::Popup`, który pod XWayland/KWin nie
  zawsze dostaje klawiaturę), chowa się po utracie aktywności. Jest tylko jeden dymek naraz
  (wyszukiwarka chowa dymek z muzyką i odwrotnie), a postać przy nim nie odchodzi.
  Animacje: przy otwartej wyszukiwarce zamyślona z ręką przy buzi (`ponder`), po
  uruchomieniu wyniku radość (`cheer`, ok. 1,4 s), przy braku wyników zawstydzona (`shy`,
  ok. 1,3 s; dopiero gdy zapytanie się ustabilizuje, ok. 0,35 s bez wyników).
  Kod: `src/search.*` (interfejs `SearchEngine` + `KRunnerEngine`; testy używają atrapy,
  która niczego nie uruchamia), `src/searchbubble.*` (dymek).
* Ostrzeżenia o temperaturze GPU (LACT): program czyta temperaturę karty z demona LACT (gniazdo unix
  `/run/lactd.sock`, JSON w wierszach; wyłącznie komendy odczytowe `list_devices` i `device_stats`, nic nie zmienia
  ustawień GPU) co ~3 s, asynchronicznie (QLocalSocket, limit czasu 2 s). Gdy edge albo hotspot (junction)
  przekroczy próg w co najmniej 2 odczytach z rzędu, nad głową pojawia się dymek ("GPU ma 87°C!", "Hotspot 105°C – gorąco!",
  oba naraz w jednym) i postać reaguje (`surprised`, potem `shy` albo `grumpy`, gdy próg przekroczony o >= 10°C). Dymek nie
  bierze fokusu, znika po 10 s albo po kliknięciu, czeka na wyszukiwarkę i dymek z muzyką oraz na koniec przeciągania.
  Bez spamu: kolejne ostrzeżenie dopiero po spadku o >= 5°C poniżej progu i ponownym przekroczeniu albo po 5 min.
  Pozycja "Jaka jest temperatura?" (pod "Co ma robić?") - Miku mówi aktualny odczyt w dymku (ok. 6 s), także przy wyłączonych ostrzeżeniach (wtedy jednorazowy odczyt).
  Menu: "Ostrzeżenia o temperaturze GPU" (ptaszek, domyślnie włączone), nieaktywna pozycja z odczytem
  ("GPU 62°C · hotspot 78°C") albo "LACT niedostępny (powód)" (ponowne próby co 30 s), podmenu "Progi"
  (edge 75/80/85/90, hotspot 90-110). Zapis: `"gpu_alert": {"enabled": true, "edge": 85, "hotspot": 100}` w settings.json.
  Dostęp do gniazda: użytkownik musi być w grupie z `daemon.admin_group`/`admin_user` w `/etc/lact/config.yaml`.
  Ścieżkę gniazda można zmienić zmienną `DESKTOP_PET_LACT_SOCKET` (testy). Kod: `src/gpualert.*`, `AlertBubble` w `src/bubbles.*`.
* Kółko myszy nad postacią: głośniej / ciszej (tylko muzyka).
* Lewy przycisk + ruch myszy: chwyć i przeciągnij. Po puszczeniu postać spada
  z grawitacją na "podłogę" ekranu, nad którym ją upuszczono.
* Prawy przycisk myszy: menu z aktualnym utworem (MPRIS przez QtDBus, np. YouTube
  Music w przeglądarce) i przyciskami: poprzedni / odtwórz-pauza / następny,
  a także "Zakończ".
* Pozycja jest zapamiętywana (`position` w `~/.config/desktop-pet/settings.json`: ekran, x, kierunek)
  i po starcie postać wraca na to miejsce; gdy ekranu nie ma, staje na środku głównego.
  Menu: "Wróć na środek ekranu".
* Gry ze Steama: prawy przycisk -> "Gry" - zainstalowane gry (ostatnio grane
  na górze, z ikonkami z pamięci podręcznej Steama) i "Otwórz bibliotekę Steam".
  Kliknięcie gry otwiera `steam://rungameid/<appid>` (Steam sam ją uruchamia),
  a postać się dziwi. Lista pochodzi z `steamapps/libraryfolders.vdf`
  i `appmanifest_*.acf` (tylko odczyt; Steam natywny albo Flatpak), bez narzędzi
  typu Proton / Steam Linux Runtime.

## Sprite'y (Shimeji)

Program szuka plików `shime*.png` w `~/.config/desktop-pet/sprites/`
(rekurencyjnie, bierze pierwszy folder, w którym jest `shime1.png`).
Mapowanie klatek jest w `src/sprites.cpp`: klatki 1-2-1-3 to chód,
4 to spadanie, 5 to trzymanie za kark, oryginał patrzy w lewo
(dla ruchu w prawo obraz jest odbijany lustrzanie). Skala: `DESKTOP_PET_SCALE`.
Bez sprite'ów rysowana jest prosta zastępcza postać.

## Budowanie i uruchamianie

    ./install.sh [--autostart] [--desktop]

Skrypt buduje program (lokalnie albo w distroboxie `builder`), instaluje go do
`~/.local/share/desktop-pet/desktop-pet` i tworzy launcher `~/.local/bin/desktop-pet`.
Uruchamianie przez XWayland (`QT_QPA_PLATFORM=xcb`), bo na natywnym Waylandzie okno
nie może samo ustawiać swojej pozycji - robi to launcher.

Ręcznie: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
Wymagane: Qt6 (Widgets, DBus, Network, Test), KF6 Runner (`kf6-krunner-devel`; `install.sh` doinstaluje je w kontenerze `builder`, a na hoście potrzebne są tylko biblioteki uruchomieniowe `kf6-krunner` i pluginy Plasmy), CMake, Ninja, `pactl`, `xprop`, `xdg-mime`.

Kod: `src/` (pet, bubbles, search, searchbubble, mpris, volume, steam, vdf, sprites, config),
testy jednostkowe: `tests/tests.cpp`. Tylko jedna instancja naraz (blokada
`$XDG_RUNTIME_DIR/desktop-pet.lock`); SIGINT/SIGTERM kończą program czysto.

Ukryte opcje do zrzutów ekranu (bez okna na pulpicie, z `QT_QPA_PLATFORM=offscreen`):
`--screenshot-bubble plik.png`, `--screenshot-temp plik.png`, `--dark`.
