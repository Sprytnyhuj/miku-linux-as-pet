// Dymki nad głową postaci: wspólny wygląd (kolory z palety systemu) i dymek z muzyką.
#pragma once

#include "gpualert.h"
#include "mpris.h"
#include "volume.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <optional>

// Wspólny wygląd dymków: zaokrąglone tło w kolorach QPalette (Window, ramka Highlight)
// i ogonek do głowy. Kolory pobierane przy każdym malowaniu, więc motyw jasny/ciemny
// działa bez restartu.
class Bubble : public QWidget {
    Q_OBJECT
public:
    static constexpr int TAIL = 12;     // wysokość "ogonka" dymka (dolny margines układu musi go uwzględniać)
    static constexpr double RADIUS = 8; // zaokrąglenie podobne do Breeze

    explicit Bubble(Qt::WindowFlags flags);
    void setTail(std::optional<double> x);   // nullopt = bez ogonka (dymek nie stoi nad głową)

protected:
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *e) override;

private:
    std::optional<double> m_tailX = 0.0;
};

// Dymek: tytuł, wykonawca, przyciski ⏮ ⏯ ⏭ i głośność strumienia odtwarzacza.
class MusicBubble : public Bubble {
    Q_OBJECT
public:
    static constexpr int TEXT_W = 230;

    MusicBubble(Mpris *mpris, TrackWatcher *watcher, PlayerVolume *volume);

    void refresh();                       // pokazuje ostatnie znane dane i zleca odczyt MPRIS i pactl w tle
    // Pokazuje podane dane (refresh() po odpytaniu; zrzuty ekranu bez D-Bus).
    void showPlayer(const std::optional<PlayerInfo> &player, const std::optional<VolumeState> &vol);
    void popup();                         // pokazuje dymek (wcześniej refresh() i ustawić pozycję)
    bool justClosed() const;              // zamknął się przed chwilą (klikiem w postać, który go zamknął)
    void changeVolume(int delta);         // przyciski −/+ i kółko myszy na postaci

protected:
    void hideEvent(QHideEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    void showVolume(const std::optional<VolumeState> &vol);
    void sliderMoved(int percent);
    void press(const QString &method);

    Mpris *m_mpris;
    TrackWatcher *m_watcher;
    PlayerVolume *m_volume;
    std::optional<PlayerInfo> m_player;
    QElapsedTimer m_hiddenAt;
    QLabel *m_title, *m_artist, *m_volLabel;
    QHash<QString, QToolButton *> m_buttons;
    QToolButton *m_volDown, *m_volUp;
    QSlider *m_volSlider;
    QTimer *m_hideTimer, *m_refreshTimer;
};

// Stan odczytu względem progu: norma, blisko (>= próg - 10 °C), powyżej (> próg).
enum class TempState { Ok, Near, Over };
TempState tempState(double value, double limit);
// Kolory stanu dobrane osobno do jasnego i ciemnego tła (po jasności QPalette::Window), z dobrym kontrastem.
QColor tempStateColor(TempState s, const QPalette &pal);
// Zakres miernika: od TEMP_METER_MIN do próg + TEMP_METER_HEADROOM.
constexpr double TEMP_METER_MIN = 30, TEMP_METER_HEADROOM = 15;

// Dane dymka z temperaturą: odczyt (odpowiedź na pytanie), ostrzeżenie (Gorąco!) albo brak LACT.
struct TempCard {
    enum Kind { Reading, Warning, Unavailable } kind = Reading;
    GpuTemps temps;
    int edgeLimit = GpuAlertConfig::DEFAULT_EDGE, hotspotLimit = GpuAlertConfig::DEFAULT_HOTSPOT;
    bool edgeOver = false, hotspotOver = false;   // wyróżnione w ostrzeżeniu
    QString summary;                              // tekst jednowierszowy (m.in. do kolejkowania i testów)
};
// Trzecia, drobna linia ("212 W · wentylator 1450 obr/min · GPU 97%"); pusta, gdy LACT nie podał żadnych danych.
QString tempExtraLine(const GpuTemps &t);

class TempCardView;

// Dymek z temperaturą GPU. Ostrzeżenie automatyczne (popup=false): nie bierze fokusu ani myszy z innych okien
// (Tool + WindowDoesNotAcceptFocus + WA_ShowWithoutActivating), znika po 10 s albo po kliknięciu.
class AlertBubble : public Bubble {
    Q_OBJECT
public:
    static constexpr int TIMEOUT_MS = 10000;
    static constexpr int TEXT_W = 260;   // szerokość tekstu; z marginesami ok. 288 px

    // popup=true: dymek wywołany przez usera (Qt::Popup): klik gdziekolwiek poza nim albo Esc zamyka, kursor nad nim wstrzymuje czas.
    explicit AlertBubble(bool popup = false);
    bool isPopup() const { return m_popup; }
    bool justClosed() const;                  // zamknął się przed chwilą (klik w postać, który go zamknął)
    void setMessage(const QString &text);    // zwykły tekst (bez miernika); ustawia rozmiar (przed ustawieniem pozycji)
    void setCard(const TempCard &card);       // układ z temperaturami, miernikami i dodatkowymi danymi
    QString message() const { return m_message; }
    const std::optional<TempCard> &card() const { return m_card; }
    int meterCount() const;                   // widoczne mierniki (0 przy braku LACT i w zwykłym tekście)
    void setTimeoutMs(int ms) { m_hideTimer->setInterval(ms); }
    int timeoutMs() const { return m_hideTimer->interval(); }

signals:
    void dismissed();                        // klik albo upływ czasu (nie przy ukryciu z zewnątrz)

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    QLabel *m_text;
    TempCardView *m_view;
    QString m_message;
    std::optional<TempCard> m_card;
    QTimer *m_hideTimer;
    QElapsedTimer m_hiddenAt;
    bool m_popup;
};
