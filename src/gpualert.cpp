#include "gpualert.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <utility>

// ---------------------------------------------------------------- ustawienia

GpuAlertConfig GpuAlertConfig::fromSettings(const QJsonObject &settings)
{
    GpuAlertConfig c;
    const QJsonValue v = settings.value(QStringLiteral("gpu_alert"));
    if (!v.isObject())
        return c;
    const QJsonObject o = v.toObject();
    if (o.value(QStringLiteral("enabled")).isBool())
        c.enabled = o.value(QStringLiteral("enabled")).toBool();
    auto temp = [&o](const char *key, int def) {
        const QJsonValue t = o.value(QLatin1String(key));
        if (!t.isDouble() || !std::isfinite(t.toDouble()))
            return def;
        return std::clamp(int(std::lround(t.toDouble())), 30, 150);
    };
    c.edge = temp("edge", DEFAULT_EDGE);
    c.hotspot = temp("hotspot", DEFAULT_HOTSPOT);
    return c;
}

QJsonObject GpuAlertConfig::toJson() const
{
    return QJsonObject{{QStringLiteral("enabled"), enabled},
                       {QStringLiteral("edge"), edge},
                       {QStringLiteral("hotspot"), hotspot}};
}

// ---------------------------------------------------------------- parser

namespace {

// Wspólna część: {"status":"ok","data":...} albo {"status":"error","data":{"description":...}}.
std::optional<QJsonValue> lactData(const QByteArray &line, QString *error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("zła odpowiedź LACT");
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
        if (error)
            *error = QStringLiteral("LACT zwrócił błąd");
        return std::nullopt;
    }
    return o.value(QStringLiteral("data"));
}

std::optional<double> tempOf(const QJsonObject &temps, const char *key)
{
    const QJsonValue cur = temps.value(QLatin1String(key)).toObject().value(QStringLiteral("current"));
    if (cur.isDouble() && std::isfinite(cur.toDouble()))
        return cur.toDouble();
    return std::nullopt;
}

}  // namespace

QString parseLactDeviceId(const QByteArray &line, QString *error)
{
    const auto data = lactData(line, error);
    if (!data)
        return {};
    const QJsonArray arr = data->toArray();
    QString firstAny, firstDedicated;
    for (const QJsonValue &v : arr) {
        const QJsonObject d = v.toObject();
        const QString id = d.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        if (firstAny.isEmpty())
            firstAny = id;
        if (d.value(QStringLiteral("device_type")).toString() != QLatin1String("Dedicated"))
            continue;
        const QString name = d.value(QStringLiteral("name")).toString();
        if (name.contains(QLatin1String("AMD"), Qt::CaseInsensitive)
            || name.contains(QLatin1String("Radeon"), Qt::CaseInsensitive))
            return id;                                   // pierwsza dyskretna AMD
        if (firstDedicated.isEmpty())
            firstDedicated = id;
    }
    if (arr.size() == 1)
        return firstAny;
    const QString pick = !firstDedicated.isEmpty() ? firstDedicated : firstAny;
    if (pick.isEmpty() && error)
        *error = QStringLiteral("brak karty GPU");
    return pick;
}

QString shortGpuName(const QString &name)
{
    QString s = name.trimmed();
    for (const char *prefix : {"AMD ", "Advanced Micro Devices, Inc. ", "Radeon "}) {
        const QString p = QLatin1String(prefix);
        if (s.startsWith(p, Qt::CaseInsensitive) && s.size() > p.size())
            s = s.mid(p.size()).trimmed();
    }
    return s;
}

QString parseLactDeviceName(const QByteArray &line, const QString &id)
{
    const auto data = lactData(line, nullptr);
    if (!data)
        return {};
    for (const QJsonValue &v : data->toArray())
        if (v.toObject().value(QStringLiteral("id")).toString() == id)
            return shortGpuName(v.toObject().value(QStringLiteral("name")).toString());
    return {};
}

std::optional<GpuTemps> parseLactStats(const QByteArray &line, QString *error)
{
    const auto data = lactData(line, error);
    if (!data)
        return std::nullopt;
    const QJsonObject temps = data->toObject().value(QStringLiteral("temps")).toObject();
    GpuTemps t;
    t.edge = tempOf(temps, "edge");
    t.hotspot = tempOf(temps, "junction");
    if (!t.hotspot)
        t.hotspot = tempOf(temps, "hotspot");
    if (!t.edge && !t.hotspot) {
        if (error)
            *error = QStringLiteral("brak odczytu temperatury");
        return std::nullopt;
    }
    const QJsonObject d = data->toObject();
    const QJsonValue rpm = d.value(QStringLiteral("fan")).toObject().value(QStringLiteral("speed_current"));
    if (rpm.isDouble() && rpm.toDouble() > 0)
        t.fanRpm = int(std::lround(rpm.toDouble()));
    const QJsonObject power = d.value(QStringLiteral("power")).toObject();
    for (const char *key : {"current", "average"}) {
        const QJsonValue w = power.value(QLatin1String(key));
        if (w.isDouble() && w.toDouble() >= 0 && std::isfinite(w.toDouble())) {
            t.powerW = w.toDouble();
            break;
        }
    }
    const QJsonValue busy = d.value(QStringLiteral("busy_percent"));
    if (busy.isDouble() && busy.toDouble() >= 0 && busy.toDouble() <= 100)
        t.busyPercent = int(std::lround(busy.toDouble()));
    return t;
}

// ---------------------------------------------------------------- logika progów

void GpuAlertLogic::setThresholds(int edge, int hotspot)
{
    if (m_edge.threshold != edge) {
        m_edge = Sensor{double(edge)};
    }
    if (m_hot.threshold != hotspot) {
        m_hot = Sensor{double(hotspot)};
    }
}

void GpuAlertLogic::reset()
{
    m_edge.over = 0;
    m_hot.over = 0;
}

bool GpuAlertLogic::Sensor::step(const std::optional<double> &v, qint64 now)
{
    if (!v || *v <= threshold) {
        over = 0;
        if (v && *v <= threshold - HYSTERESIS)
            armed = true;
        return false;
    }
    ++over;
    if (over < CONSECUTIVE)
        return false;
    return armed || (lastAlert && now - *lastAlert >= REPEAT_MS);
}

std::optional<GpuAlertLogic::Alert> GpuAlertLogic::update(const GpuTemps &t, qint64 nowMs)
{
    const bool edgeWasArmed = m_edge.armed, hotWasArmed = m_hot.armed;
    const bool e = m_edge.step(t.edge, nowMs);
    const bool h = m_hot.step(t.hotspot, nowMs);
    if (!e && !h)
        return std::nullopt;
    // Drugi czujnik, który też już przekracza próg (>= CONSECUTIVE odczytów), dołącza do tego samego dymka.
    const bool withEdge = e || m_edge.over >= CONSECUTIVE;
    const bool withHot = h || m_hot.over >= CONSECUTIVE;
    Alert a;
    auto deg = [](double v) { return QString::number(std::lround(v)) + QStringLiteral("°C"); };
    if (withEdge && withHot) {
        a.text = QStringLiteral("GPU ma %1, hotspot %2 – gorąco!").arg(deg(*t.edge), deg(*t.hotspot));
    } else if (withEdge) {
        a.text = QStringLiteral("GPU ma %1!").arg(deg(*t.edge));
    } else {
        a.text = QStringLiteral("Hotspot %1 – gorąco!").arg(deg(*t.hotspot));
    }
    // Czujnik, który sam ostrzega, zaczyna odliczanie od nowa. Czujnik tylko dołączony do tekstu (nowe
    // przekroczenie drugiego) zachowuje swój zegar - świeże przekroczenie hotspotu nie przesuwa przypomnienia
    // o edge. Przy przypomnieniu (czujnik stale wysoki) dołączony też je dostaje, więc nie ostrzega minutę później.
    const bool repeat = (e && !edgeWasArmed) || (h && !hotWasArmed);
    for (Sensor *s : {&m_edge, &m_hot}) {
        const bool trigger = (s == &m_edge) ? e : h;
        const bool in = (s == &m_edge) ? withEdge : withHot;
        if (!in || (!trigger && !repeat))
            continue;
        s->armed = false;
        s->lastAlert = nowMs;
    }
    a.edgeOver = withEdge;
    a.hotOver = withHot;
    a.severe = (withEdge && *t.edge >= m_edge.threshold + 10) || (withHot && *t.hotspot >= m_hot.threshold + 10);
    return a;
}

// ---------------------------------------------------------------- klient LACT

LactClient::LactClient(QObject *parent) : QObject(parent), m_path(defaultSocketPath())
{
    m_timer.setSingleShot(true);
    m_timeout.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &LactClient::pollNow);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_busy)
            fail(QStringLiteral("brak odpowiedzi"));
    });
}

QString LactClient::defaultSocketPath()
{
    const QByteArray env = qgetenv("DESKTOP_PET_LACT_SOCKET");
    return env.isEmpty() ? QStringLiteral("/run/lactd.sock") : QString::fromLocal8Bit(env);
}

void LactClient::setIntervals(int pollMs, int retryMs, int timeoutMs)
{
    m_pollMs = pollMs;
    m_retryMs = retryMs;
    m_timeoutMs = timeoutMs;
}

void LactClient::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    if (on) {
        pollNow();
    } else {
        m_timer.stop();
        if (m_once)
            return;                       // jednorazowy odczyt w toku - dokończy się sam, bez ponowień
        teardown();
        m_busy = false;
        m_deviceId.clear();
        m_latest.reset();
        m_reason.clear();
    }
}

// Jedyne miejsce, które buduje wysyłane komendy: tylko odczyt.
QByteArray LactClient::commandLine(Cmd cmd, const QString &id)
{
    QJsonObject o;
    if (cmd == Cmd::ListDevices) {
        o.insert(QStringLiteral("command"), QStringLiteral("list_devices"));
    } else {
        o.insert(QStringLiteral("command"), QStringLiteral("device_stats"));
        o.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("id"), id}});
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

void LactClient::readOnce()
{
    m_once = true;
    if (m_busy)
        return;                           // wynik trwającego odczytu zaraz dojdzie
    m_busy = true;
    send(m_deviceId.isEmpty() ? Cmd::ListDevices : Cmd::DeviceStats);
}

void LactClient::pollNow()
{
    if (!m_enabled || m_busy)
        return;
    m_busy = true;
    send(m_deviceId.isEmpty() ? Cmd::ListDevices : Cmd::DeviceStats);
}

void LactClient::teardown()
{
    m_timeout.stop();
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
}

void LactClient::send(Cmd cmd)
{
    teardown();
    m_cmd = cmd;
    m_in.clear();
    m_sock = new QLocalSocket(this);
    const QByteArray line = commandLine(cmd, m_deviceId);
    connect(m_sock, &QLocalSocket::connected, this, [this, line] { m_sock->write(line); });
    connect(m_sock, &QLocalSocket::readyRead, this, [this] {
        m_in += m_sock->readAll();
        // Puste wiersze pomijamy; odpowiedź to pierwszy niepusty wiersz (reszta bloku jest ignorowana).
        for (;;) {
            const qsizetype nl = m_in.indexOf('\n');
            if (nl < 0)
                break;
            const QByteArray line = m_in.left(nl);
            if (line.trimmed().isEmpty()) {
                m_in.remove(0, nl + 1);
                continue;
            }
            finishRequest(line);
            return;
        }
        if (m_in.size() > (1 << 20))
            fail(QStringLiteral("zła odpowiedź LACT"));
    });
    connect(m_sock, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError err) {
        switch (err) {
        case QLocalSocket::ServerNotFoundError:
            fail(QStringLiteral("brak gniazda LACT"));
            break;
        case QLocalSocket::ConnectionRefusedError:
            fail(QStringLiteral("demon LACT nie odpowiada"));
            break;
        case QLocalSocket::SocketAccessError:
            fail(QStringLiteral("brak uprawnień do gniazda LACT"));
            break;
        case QLocalSocket::PeerClosedError:
            if (m_busy && m_in.isEmpty())
                fail(QStringLiteral("LACT zamknął połączenie"));
            break;
        default:
            fail(QStringLiteral("błąd połączenia z LACT"));
        }
    });
    connect(m_sock, &QLocalSocket::disconnected, this, [this] {
        if (!m_busy)
            return;
        if (!m_in.trimmed().isEmpty())
            finishRequest(m_in);
        else
            fail(QStringLiteral("LACT zamknął połączenie"));
    });
    m_timeout.start(m_timeoutMs);
    m_sock->connectToServer(m_path);
}

void LactClient::finishRequest(const QByteArray &line)
{
    QString err;
    if (m_cmd == Cmd::ListDevices) {
        const QString id = parseLactDeviceId(line, &err);
        if (id.isEmpty())
            return fail(err.isEmpty() ? QStringLiteral("brak karty GPU") : err);
        m_deviceId = id;
        m_deviceName = parseLactDeviceName(line, id);
        send(Cmd::DeviceStats);               // od razu temperatury wybranej karty
        return;
    }
    auto temps = parseLactStats(line, &err);
    teardown();
    if (temps)
        temps->card = m_deviceName;
    if (!temps) {
        m_deviceId.clear();                   // karta mogła zniknąć - przy następnej próbie lista od nowa
        return fail(err);
    }
    m_busy = false;
    const bool once = std::exchange(m_once, false);
    if (m_enabled) {
        m_reason.clear();
        m_latest = temps;
        m_latestAt.restart();
        m_timer.start(m_pollMs);
        emit reading(*temps);
    } else {
        m_deviceId.clear();               // wyłączone: nic nie zostaje w pamięci
    }
    if (once)
        emit onceFinished(true, *temps, QString());
}

void LactClient::fail(const QString &reason)
{
    teardown();
    m_busy = false;
    m_deviceId.clear();
    m_latest.reset();
    m_reason = reason;
    if (m_enabled)
        m_timer.start(m_retryMs);
    const bool once = std::exchange(m_once, false);
    emit unavailable(reason);
    if (once)
        emit onceFinished(false, GpuTemps{}, reason);
}
