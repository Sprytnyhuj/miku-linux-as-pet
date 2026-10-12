#include "mpris.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QTimer>
#include <QDBusVariant>
#include <QVariantMap>
#include <tuple>

namespace {

constexpr int kTimeoutMs = 1500;   // wywołania D-Bus nie blokują interfejsu dłużej
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayerIface = QStringLiteral("org.mpris.MediaPlayer2.Player");

// Properties.Get -> wartość (rozpakowany variant) albo nieprawidłowy QVariant.
QVariant getProp(const QString &service, const QString &iface, const QString &prop)
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, kPath,
                                                    QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("Get"));
    m << iface << prop;
    m.setAutoStartService(false);
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, kTimeoutMs);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    QVariant v = r.arguments().first();
    if (v.canConvert<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    return v;
}

// Wartość z mapy Metadata jako napis; lista napisów -> pierwszy element.
QString metaString(const QVariantMap &meta, const QString &key, bool isList)
{
    QVariant v = meta.value(key);
    if (v.canConvert<QDBusVariant>())
        v = v.value<QDBusVariant>().variant();
    if (v.canConvert<QDBusArgument>())
        v = QVariant::fromValue(qdbus_cast<QStringList>(v.value<QDBusArgument>()));
    if (isList || v.metaType().id() == QMetaType::QStringList) {
        const QStringList l = v.toStringList();
        return l.isEmpty() ? QString() : l.first();
    }
    return v.toString();
}

// Metadata (QDBusArgument albo QVariantMap) -> tytuł, wykonawca, adres.
void fillFromMetadata(PlayerInfo *i, const QVariant &meta)
{
    QVariantMap m;
    if (meta.canConvert<QDBusArgument>())
        m = qdbus_cast<QVariantMap>(meta.value<QDBusArgument>());
    else if (meta.metaType().id() == QMetaType::QVariantMap)
        m = meta.toMap();
    else
        return;
    i->title = metaString(m, QStringLiteral("xesam:title"), false);
    i->artist = metaString(m, QStringLiteral("xesam:artist"), true);
    i->url = metaString(m, QStringLiteral("xesam:url"), false);
}

}  // namespace

QStringList Mpris::players() const
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ListNames"));
    const QDBusMessage r = bus.call(m, QDBus::Block, kTimeoutMs);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    QStringList out;
    for (const QString &n : r.arguments().first().toStringList()) {
        if (n.size() > int(qstrlen(MPRIS_PREFIX)) && n.startsWith(QLatin1String(MPRIS_PREFIX)))
            out << n;
    }
    return out;
}

PlayerInfo Mpris::info(const QString &name) const
{
    PlayerInfo i;
    i.name = name;
    i.identity = getProp(name, QStringLiteral("org.mpris.MediaPlayer2"), QStringLiteral("Identity")).toString();
    const QString status = getProp(name, kPlayerIface, QStringLiteral("PlaybackStatus")).toString();
    if (!status.isEmpty())
        i.status = status;
    fillFromMetadata(&i, getProp(name, kPlayerIface, QStringLiteral("Metadata")));
    return i;
}

int Mpris::bestIndex(const QList<PlayerInfo> &infos)
{
    int best = -1;
    std::tuple<bool, bool, bool> bestRank;
    for (int k = 0; k < infos.size(); ++k) {
        const PlayerInfo &i = infos[k];
        const bool ytm = i.url.contains(QLatin1String("music.youtube.com"))
                         || i.title.contains(QLatin1String("YouTube Music"));
        const std::tuple<bool, bool, bool> rank{i.status == QLatin1String("Playing"), ytm,
                                                i.status == QLatin1String("Paused")};
        if (best < 0 || rank > bestRank) {        // przy remisie zostaje pierwszy
            best = k;
            bestRank = rank;
        }
    }
    return best;
}

std::optional<PlayerInfo> Mpris::current() const
{
    QList<PlayerInfo> infos;
    for (const QString &n : players())
        infos << info(n);
    const int k = bestIndex(infos);
    if (k < 0)
        return std::nullopt;
    return infos[k];
}

void Mpris::call(const QString &name, const QString &method) const
{
    QDBusMessage m = QDBusMessage::createMethodCall(name, kPath, kPlayerIface, method);
    m.setAutoStartService(false);
    QDBusConnection::sessionBus().asyncCall(m, kTimeoutMs);
}

PlayerInfo playerInfoFromProps(const QString &name, const QVariantMap &props)
{
    PlayerInfo i;
    i.name = name;
    QVariant status = props.value(QStringLiteral("PlaybackStatus"));
    if (status.canConvert<QDBusVariant>())
        status = status.value<QDBusVariant>().variant();
    if (!status.toString().isEmpty())
        i.status = status.toString();
    QVariant meta = props.value(QStringLiteral("Metadata"));
    if (meta.canConvert<QDBusVariant>())
        meta = meta.value<QDBusVariant>().variant();
    fillFromMetadata(&i, meta);
    return i;
}

QString TrackWatcher::keyFor(const PlayerInfo &info)
{
    return info.title + QLatin1Char('\n') + info.artist;
}

bool TrackWatcher::poll()
{
    if (m_busy)
        return false;
    m_busy = true;
    m_infos.clear();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        QTimer::singleShot(0, this, [this] { finish(); });     // zawsze asynchronicznie, jak przy magistrali
        return true;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ListNames"));
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m, kTimeoutMs), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        const QDBusPendingReply<QStringList> reply = *watcher;
        QStringList names;
        if (!reply.isError()) {
            for (const QString &n : reply.value()) {
                if (n.size() > int(qstrlen(MPRIS_PREFIX)) && n.startsWith(QLatin1String(MPRIS_PREFIX)))
                    names << n;
            }
        }
        if (names.isEmpty()) {
            finish();
            return;
        }
        // Wszystkie odtwarzacze pytane naraz, ale jako jedno sprawdzenie (m_busy do końca).
        int idRequests = 0;
        for (const QString &n : std::as_const(names))
            idRequests += m_identity.contains(n) ? 0 : 1;
        m_remaining = int(names.size()) + idRequests;
        for (const QString &name : std::as_const(names)) {
            const int idx = int(m_infos.size());
            PlayerInfo placeholder;
            placeholder.name = name;
            m_infos << placeholder;
            QDBusMessage get = QDBusMessage::createMethodCall(
                name, kPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
            get << QString(kPlayerIface);
            get.setAutoStartService(false);
            auto *pw = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(get, kTimeoutMs), this);
            connect(pw, &QDBusPendingCallWatcher::finished, this, [this, idx, name](QDBusPendingCallWatcher *p) {
                p->deleteLater();
                const QDBusPendingReply<QVariantMap> r = *p;
                if (!r.isError())
                    m_infos[idx] = playerInfoFromProps(name, r.value());
                if (--m_remaining == 0)
                    finish();
            });
            if (!m_identity.contains(name)) {          // Identity (potrzebne do dopasowania strumienia) - raz na odtwarzacz
                QDBusMessage idGet = QDBusMessage::createMethodCall(
                    name, kPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
                idGet << QStringLiteral("org.mpris.MediaPlayer2");
                idGet.setAutoStartService(false);
                auto *iw = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(idGet, kTimeoutMs), this);
                connect(iw, &QDBusPendingCallWatcher::finished, this, [this, name](QDBusPendingCallWatcher *p) {
                    p->deleteLater();
                    const QDBusPendingReply<QVariantMap> r = *p;
                    QVariant v = r.isError() ? QVariant() : r.value().value(QStringLiteral("Identity"));
                    if (v.canConvert<QDBusVariant>())
                        v = v.value<QDBusVariant>().variant();
                    if (!r.isError())
                        m_identity.insert(name, v.toString());
                    if (--m_remaining == 0)
                        finish();
                });
            }
        }
    });
    return true;
}

void TrackWatcher::finish()
{
    for (PlayerInfo &i : m_infos)
        i.identity = m_identity.value(i.name);
    const int k = Mpris::bestIndex(m_infos);
    m_latest = k < 0 ? std::nullopt : std::optional<PlayerInfo>(m_infos[k]);
    m_infos.clear();
    m_busy = false;
    const bool playing = m_latest && m_latest->status == QLatin1String("Playing");
    emit polled(m_latest ? keyFor(*m_latest) : QString(), playing);
}
