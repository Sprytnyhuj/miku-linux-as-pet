// Postać na pulpicie: animacje, grawitacja, przeciąganie, menu, dymek z muzyką.
#pragma once

#include "bubbles.h"
#include "gpualert.h"
#include "mpris.h"
#include "reactions.h"
#include "search.h"
#include "searchbubble.h"
#include "sprites.h"
#include "steam.h"
#include "volume.h"

#include <QPointF>
#include <QElapsedTimer>
#include <QProxyStyle>
#include <QMenu>
#include <QRect>
#include <QScreen>
#include <QWidget>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Tryb do wyboru w menu "Co ma robić?": nazwa i lista (aktywność, waga, min, max ticków).
// 25 ticków = 1 sekunda.
struct ModeActivity {
    const char *name;
    int weight, lo, hi;
};
struct Mode {
    const char *key;
    const char *label;
    std::vector<ModeActivity> activities;
};
const std::vector<Mode> &modes();
const Mode *findMode(const QString &key);
inline constexpr const char *DEFAULT_MODE = "sit";

// Styl systemowy, tylko większe ikonki w podmenu "Gry" (QMenu nie ma setIconSize).
class GamesMenuStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                    const QWidget *widget = nullptr) const override;
};

// Obszar roboczy bez paneli z wyniku `xprop -root _NET_WORKAREA` albo nullopt.
// Sam xprop jest uruchamiany asynchronicznie (Pet::refreshWorkarea), nigdy w pętli zdarzeń.
std::optional<QRect> parseWorkarea(const QString &xpropOutput);

// Pomiar odstępów między tickami step() i czasu trwania samego step() (do testów i debugowania).
struct TickStats {
    qint64 ticks = 0;
    double maxGapMs = 0;       // najdłuższy odstęp między kolejnymi tickami
    double maxStepMs = 0;      // najdłuższe step()
    int gapsOver80Ms = 0;      // odstępy > 2 ticków
};

// Lądowanie: miękkie (vy >= SOFT_LAND_VY, czyli kilkadziesiąt px upadku) daje tylko krótką reakcję.
inline constexpr double SOFT_LAND_VY = 8.0;
inline constexpr int SOFT_LAND_TICKS = 7;   // <= 0,3 s przy TICK_MS = 40

inline constexpr int SEARCH_CHEER_TICKS = 36;   // radość po uruchomieniu wyniku (~1,4 s)
inline constexpr int SEARCH_SHY_TICKS = 32;     // wstyd przy braku wyników (~1,3 s)

// Reakcja krótsza niż to (~0,24 s) wyglądałaby jak jednoklatkowe mignięcie - react() ją wydłuża.
inline constexpr int MIN_REACTION_TICKS = 6;

inline constexpr int SAVE_EVERY_TICKS = 250;   // ~10 s przy TICK_MS = 40

struct PetProbe;   // dostęp testów do stanu

class Pet : public QWidget {
    Q_OBJECT
public:
    Pet();
    ~Pet() override { delete m_bubble; delete m_search; delete m_alert; delete m_info; }
    const TickStats &tickStats() const { return m_tickStats; }
    // Ustawienia aplikacji, bez których zamknięcie ostatniego okna Qt::Tool zamknęłoby program (main i testy).
    static void setupApplication();
    // Oznacza jawne zamknięcie ("Zakończ", SIGINT/SIGTERM); inne wyjście jest logowane jako nieoczekiwane.
    static void markExplicitQuit();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    friend struct ::PetProbe;
    enum class State { Fall, Drag, Floor };

    QRect area(const QPointF &point) const;
    double floorY(const QRect &area) const;
    void applyPos();
    int frameX(const QPixmap &pm) const;
    void pickActivity();
    void startMainActivity(bool newDir = false);
    void setMode(const QString &mode);
    void react(std::vector<std::pair<QString, int>> steps, const char *why = "?");
    void setAnim(const QString &name);
    void step();
    void stepImpl();
    void refreshWorkarea();            // xprop w tle
    QScreen *currentScreen() const;
    void placeCentered(QScreen *screen);
    bool restorePosition();            // z settings.json; false = brak/uszkodzone/ekran nie istnieje
    void savePosition();               // stan floor -> settings.json (w tle)
    void clampToWorkarea();            // x do obszaru roboczego (tylko floor)
    void returnToCenter();
    void buildMenu(QMenu &menu);
    const QPixmap &frame() const;
    bool bubbleOpen() const;
    void openBubble(bool fromClick = false);
    void openSearch();
    void singleClick();                // po upływie interwału podwójnego kliku
    void checkTrack();
    void onTrackPolled(const QString &key, bool playing);
    void placeBubble(Bubble *which = nullptr);   // which: dymek jeszcze niewidoczny, który zaraz się pokaże
    void showMenu(const QPoint &pos);
    void addGamesMenu(QMenu *menu);
    void launchGame(const SteamGame &game);
    // Ostrzeżenia o temperaturze GPU (LACT)
    void setGpuAlertEnabled(bool on);
    void setGpuThresholds(int edge, int hotspot);
    void askCustomThreshold(bool isEdge);
    static constexpr int GPU_EDGE_MIN = 40, GPU_EDGE_MAX = 110, GPU_HOT_MIN = 50, GPU_HOT_MAX = 125;
    void saveGpuConfig();
    void onGpuReading(const GpuTemps &t);
    void tryShowGpuAlert();
    void requeueGpuAlert();            // ostrzeżenie widoczne ustępuje innemu dymkowi - wróci po jego zamknięciu
    void showInfoBubble();             // w następnym obiegu pętli (po zamknięciu menu)
    void hideInfo();
    void placeOne(Bubble *b);
    void addGpuMenu(QMenu *menu);
    void askTemperature();             // "Jaka jest temperatura?" - z pamięci albo jednorazowy odczyt
    void showTemperature(const QString &text, std::optional<TempCard> card = std::nullopt);

    Sprites m_sprites;
    int m_bodyW = 0, m_pad = 0;
    std::optional<QRect> m_drawRect;   // obszar ostatniej maski (klatka przy dole okna)
    Mpris m_mpris;
    PlayerVolume m_volume;               // pactl w tle, wartość z pamięci
    SteamCache m_steam;                  // lista gier i ikonki odświeżane w tle
    TickStats m_tickStats;
    QElapsedTimer m_tickClock;
    bool m_workareaBusy = false;
    double m_savedX = 0;               // ostatnio zapisana pozycja (żeby nie zapisywać bez zmian)
    int m_savedDir = 0;
    int m_saveTicks = 0;
    std::unique_ptr<GamesMenuStyle> m_gamesStyle;   // tworzony przy pierwszym menu

    // state: fall / drag / floor; na podłodze animację wybiera tryb albo reakcja
    State m_state = State::Fall;
    QString m_mode;
    QString m_activity = QStringLiteral("stand");
    int m_activityLeft = 0;
    std::vector<std::pair<QString, int>> m_reactions;   // kolejka (animacja, ticki) - przed trybem
    QString m_anim = QStringLiteral("fall");
    int m_animTick = 0;
    int m_dir = 1;
    double m_vy = 0.0;
    QPointF m_dragOffset;
    std::optional<QPointF> m_pressPos;
    QPointF m_pos;                      // lewy górny róg postaci (okno zaczyna się o m_pad wcześniej)
    MusicBubble *m_bubble;
    SearchBubble *m_search;              // wyszukiwarka (podwójny klik)
    LactClient m_lact;                   // odczyty temperatury GPU z demona LACT (tylko gdy włączone)
    GpuAlertLogic m_gpuLogic;
    GpuAlertConfig m_gpu;
    AlertBubble *m_alert;                // ostrzeżenie automatyczne (nie bierze fokusu ani myszy)
    AlertBubble *m_info;                 // odpowiedź "Jaka jest temperatura?" (Qt::Popup jak dymek z muzyką)
    bool m_infoScheduled = false;        // dymek z menu pokaże się w następnym obiegu pętli
    QString m_gpuPending;                // ostrzeżenie czekające na wolne miejsce nad głową
    bool m_gpuPendingSevere = false;
    QString m_infoPending;               // odpowiedź "Jaka jest temperatura?" czekająca na wolne miejsce
    std::optional<TempCard> m_gpuPendingCard, m_infoCard;   // układ graficzny tych dymków (puste = zwykły tekst)
    bool m_tempWanted = false;           // czekamy na jednorazowy odczyt
    QTimer *m_gpuShowTimer;              // ponawia próbę pokazania czekającego ostrzeżenia
    QTimer *m_clickTimer;                // pojedynczy klik czeka na ewentualny drugi
    QElapsedTimer m_searchClosedByClick;  // klik w postać zamknął wyszukiwarkę (druga połowa pary jej nie otwiera)
    bool m_doubleClick = false;          // trwa podwójny klik: zwolnienie przycisku otwiera wyszukiwarkę
    bool m_clickOpens = true;            // czy oczekujący pojedynczy klik ma otworzyć dymek z muzyką
    std::optional<QRect> m_workarea;
    QTimer *m_workareaTimer, *m_timer, *m_trackTimer;
    WheelSpinDetector m_spin;
    TrackChangeDetector m_track;
    QString m_logTrack, m_logPlayer;      // ostatnio zapisany w dzienniku odczyt MPRIS (tylko diagnostyka)
    TrackWatcher m_watcher;              // asynchroniczne odpytywanie MPRIS
    QElapsedTimer m_clock;               // zegar monotoniczny dla detektorów
};
