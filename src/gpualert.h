// Ostrzeżenia o temperaturze GPU: klient demona LACT (gniazdo unix, JSON w wierszach),
// parser odpowiedzi i logika progów (bez Qt Widgets, żeby dało się ją testować).
// Wysyłamy WYŁĄCZNIE komendy odczytowe (list_devices, device_stats).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <optional>

struct GpuTemps {
    std::optional<double> edge;      // temps.edge.current
    std::optional<double> hotspot;   // temps.junction.current (LACT nazywa hotspot "junction")
    // Dodatkowe dane z tej samej odpowiedzi device_stats (tylko do wyświetlenia w dymku; mogą być puste).
    std::optional<double> powerW = std::nullopt;    // power.current albo power.average
    std::optional<int> fanRpm = std::nullopt;     // fan.speed_current
    std::optional<int> busyPercent = std::nullopt;  // busy_percent
    QString card = QString();                  // skrócona nazwa karty z list_devices (np. "RX 9070 XT")
};

Q_DECLARE_METATYPE(GpuTemps)

// Ustawienia z settings.json: "gpu_alert": {"enabled": true, "edge": 85, "hotspot": 100}.
struct GpuAlertConfig {
    static constexpr int DEFAULT_EDGE = 85, DEFAULT_HOTSPOT = 100;
    bool enabled = true;
    int edge = DEFAULT_EDGE;
    int hotspot = DEFAULT_HOTSPOT;

    static GpuAlertConfig fromSettings(const QJsonObject &settings);   // braki i złe typy -> domyślne
    QJsonObject toJson() const;
};

// Odpowiedź list_devices: id karty do odpytywania (pierwsza dyskretna AMD, przy jednej - ta).
// Pusty wynik = brak karty; error opisuje błąd odpowiedzi.
QString parseLactDeviceId(const QByteArray &line, QString *error = nullptr);
// Nazwa karty z list_devices dla podanego id, skrócona do modelu ("AMD Radeon RX 9070 XT" -> "RX 9070 XT").
QString parseLactDeviceName(const QByteArray &line, const QString &id);
QString shortGpuName(const QString &name);
// Odpowiedź device_stats -> temperatury; nullopt przy błędzie (error) albo braku temperatur.
std::optional<GpuTemps> parseLactStats(const QByteArray &line, QString *error = nullptr);

// Decyzja o ostrzeżeniu. Każdy czujnik (edge, hotspot) osobno:
//  * "przekroczenie" = odczyt > próg; ostrzeżenie dopiero przy >= 2 odczytach z rzędu;
//  * po ostrzeżeniu kolejne dopiero po spadku do <= próg - 5 i ponownym przekroczeniu
//    albo (temperatura stale wysoka) po co najmniej 5 minutach;
//  * czujniki, które ostrzegają w tym samym odczycie (albo już drugi odczyt z rzędu przekracza próg), idą w jednym tekście;
//  * zegar 5 min zeruje tylko czujnik, który sam ostrzega (świeże przekroczenie drugiego czujnika go nie przesuwa),
//    chyba że to przypomnienie - wtedy dołączony czujnik też dostaje nowy zegar.
class GpuAlertLogic {
public:
    static constexpr int CONSECUTIVE = 2;
    static constexpr int HYSTERESIS = 5;               // °C
    static constexpr qint64 REPEAT_MS = 5 * 60 * 1000;

    struct Alert {
        QString text;
        bool severe = false;       // >= 10 °C ponad progiem -> mina "grumpy" zamiast "shy"
        bool edgeOver = false, hotOver = false;   // które czujniki są w tekście ostrzeżenia
    };

    void setThresholds(int edge, int hotspot);
    void reset();                  // brak odczytu (LACT niedostępny, wyłączenie): licznik "z rzędu" od zera
    std::optional<Alert> update(const GpuTemps &t, qint64 nowMs);

private:
    struct Sensor {
        explicit Sensor(double t = 0) : threshold(t) {}
        double threshold = 0;
        int over = 0;              // kolejne odczyty ponad progiem
        bool armed = true;         // spadek o >= HYSTERESIS od ostatniego ostrzeżenia (albo jeszcze nie było)
        std::optional<qint64> lastAlert;
        bool step(const std::optional<double> &v, qint64 now);   // true = ostrzeż
    };
    Sensor m_edge{double(GpuAlertConfig::DEFAULT_EDGE)}, m_hot{double(GpuAlertConfig::DEFAULT_HOTSPOT)};
};

// Klient LACT: co POLL_MS odczyt, a po błędzie co RETRY_MS. Asynchronicznie (QLocalSocket),
// z limitem czasu; nic nie blokuje pętli zdarzeń. Działa tylko po setEnabled(true).
class LactClient : public QObject {
    Q_OBJECT
public:
    static constexpr int POLL_MS = 3000;
    static constexpr int RETRY_MS = 30000;
    static constexpr int TIMEOUT_MS = 2000;

    explicit LactClient(QObject *parent = nullptr);
    // DESKTOP_PET_LACT_SOCKET (testy) albo /run/lactd.sock.
    static QString defaultSocketPath();
    void setSocketPath(const QString &path) { m_path = path; }
    void setIntervals(int pollMs, int retryMs, int timeoutMs);
    void setEnabled(bool on);
    bool enabled() const { return m_enabled; }
    bool available() const { return m_latest.has_value(); }
    const std::optional<GpuTemps> &latest() const { return m_latest; }
    const QString &reason() const { return m_reason; }    // niepusty, gdy ostatni odczyt się nie udał
    bool busy() const { return m_busy; }
    // Wiek ostatniego odczytu z pamięci (ms); -1 gdy brak.
    qint64 latestAgeMs() const { return m_latest ? m_latestAt.elapsed() : -1; }
    // Jednorazowy odczyt (device_stats) na życzenie - działa też przy wyłączonych ostrzeżeniach i wtedy
    // NIE włącza cyklicznego odpytywania ani nie zapisuje wyniku. Wynik: onceFinished.
    void readOnce();

signals:
    void reading(const GpuTemps &temps);
    void unavailable(const QString &reason);
    void onceFinished(bool ok, const GpuTemps &temps, const QString &reason);

private:
    enum class Cmd { ListDevices, DeviceStats };
    static QByteArray commandLine(Cmd cmd, const QString &id);   // jedyne miejsce budujące komendy
    void pollNow();
    void send(Cmd cmd);
    void finishRequest(const QByteArray &line);
    void fail(const QString &reason);
    void teardown();

    QString m_path, m_deviceId, m_deviceName, m_reason;
    QByteArray m_in;
    bool m_enabled = false, m_busy = false, m_once = false;
    int m_pollMs = POLL_MS, m_retryMs = RETRY_MS, m_timeoutMs = TIMEOUT_MS;
    Cmd m_cmd = Cmd::ListDevices;
    QLocalSocket *m_sock = nullptr;
    QTimer m_timer, m_timeout;
    std::optional<GpuTemps> m_latest;
    QElapsedTimer m_latestAt;
};
