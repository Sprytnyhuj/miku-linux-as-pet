#include "volume.h"

#include "config.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

QJsonArray PlayerVolume::matchStreams(const QJsonArray &streams, const PlayerInfo &player)
{
    const QString prefix = QLatin1String(MPRIS_PREFIX);
    const QString busPart = player.name.mid(prefix.size()).split(QLatin1Char('.')).value(0);
    static const QRegularExpression nonWord(QStringLiteral("\\W+"),
                                            QRegularExpression::UseUnicodePropertiesOption);
    QSet<QString> tokens;
    for (const QString &t : (player.identity + QLatin1Char(' ') + busPart).toLower().split(nonWord)) {
        if (t.size() >= 4)
            tokens.insert(t);
    }
    tokens.remove(QStringLiteral("mozilla"));
    tokens.remove(QStringLiteral("google"));

    QJsonArray out;
    for (const QJsonValue &sv : streams) {
        const QJsonObject pr = sv.toObject().value(QStringLiteral("properties")).toObject();
        QStringList parts;
        for (const char *k : {"application.name", "application.process.binary",
                              "pipewire.access.portal.app_id"})
            parts << pr.value(QLatin1String(k)).toString();
        const QString hay = parts.join(QLatin1Char(' ')).toLower();
        const bool match = std::any_of(tokens.cbegin(), tokens.cend(),
                                       [&](const QString &t) { return hay.contains(t); });
        if (match)
            out.append(sv);
    }
    return out;
}

std::optional<PlayerVolume::Parsed> PlayerVolume::parse(const QByteArray &json, const PlayerInfo &player)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return std::nullopt;
    const QJsonArray st = matchStreams(doc.array(), player);
    if (st.isEmpty())
        return std::nullopt;
    const QJsonObject first = st.first().toObject();
    const QJsonObject vol = first.value(QStringLiteral("volume")).toObject();
    if (vol.isEmpty())
        return std::nullopt;
    double sum = 0;
    for (auto it = vol.begin(); it != vol.end(); ++it) {
        QString p = it.value().toObject().value(QStringLiteral("value_percent")).toString();
        p.remove(QLatin1Char('%'));
        sum += p.toInt();
    }
    Parsed out;
    const int percent = cfg::pyRound(sum / vol.size());
    out.state = VolumeState{std::min(percent, 100), first.value(QStringLiteral("mute")).toBool()};
    for (const QJsonValue &sv : st) {
        const QJsonObject o = sv.toObject();
        out.streams.append({o.value(QStringLiteral("index")).toInt(), o.value(QStringLiteral("mute")).toBool()});
    }
    return out;
}

std::optional<VolumeState> PlayerVolume::get(const std::optional<PlayerInfo> &player) const
{
    if (!player)
        return std::nullopt;
    const auto it = m_entries.constFind(player->name);
    if (it == m_entries.constEnd() || !it->parsed)
        return std::nullopt;
    return it->parsed->state;
}

void PlayerVolume::refresh(const std::optional<PlayerInfo> &player)
{
    if (!player)
        return;
    m_latestPlayer = player;
    if (m_running) {                 // jeden pactl naraz; po jego końcu odczyt zostanie powtórzony
        m_again = true;
        return;
    }
    m_running = true;
    const PlayerInfo info = *player;
    cfg::runAsync(this, QStringLiteral("pactl"),
                  {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sink-inputs")},
                  1500, [this, info](const cfg::ProcResult &r) { onResult(info, r.finished ? r.out : QByteArray(), r.finished); });
}

void PlayerVolume::onResult(const PlayerInfo &player, const QByteArray &json, bool ok)
{
    m_running = false;
    Entry &e = m_entries[player.name];
    if (ok) {
        auto fresh = parse(json, player);
        // Trwa zmiana głośności: odczyt mógł wystartować przed nią, więc zostaje wartość optymistyczna.
        if (fresh && e.parsed && (e.inflight > 0 || e.queued))
            fresh->state = e.parsed->state;
        e.parsed = fresh;
    }
    // Zmiany zlecone przed pierwszym odczytem liczymy teraz od świeżej wartości.
    const std::optional<int> pd = e.pendingDelta, ps = e.pendingSet;
    e.pendingDelta.reset();
    e.pendingSet.reset();
    if (e.parsed && (ps || pd)) {
        const int target = ps ? *ps : std::clamp(e.parsed->state.percent + *pd, 0, 100);
        set(player, target);                 // wysyła pactl i zapamiętuje nową wartość
    }
    emit changed();
    if (m_again && m_latestPlayer) {
        m_again = false;
        refresh(m_latestPlayer);
    }
}

// Koalescencja: jedna seria poleceń pactl naraz; zmiany w trakcie zapamiętują tylko ostatnią wartość.
// Szybkie kręcenie kółkiem nie zalewa systemu procesami pactl.
void PlayerVolume::send(const QString &key, int percent)
{
    Entry &e = m_entries[key];
    if (e.inflight > 0) {
        e.queued = std::clamp(percent, 0, 100);
        return;
    }
    launch(key, percent);
}

void PlayerVolume::launch(const QString &key, int percent)
{
    percent = std::clamp(percent, 0, 100);
    Entry &e = m_entries[key];
    if (!e.parsed)
        return;
    const auto streams = e.parsed->streams;          // kopia: callbacki mogą zmienić wpis
    auto run = [this, key, &e](const QStringList &args) {
        ++e.inflight;
        cfg::runAsync(this, QStringLiteral("pactl"), args, 2000, [this, key](const cfg::ProcResult &) {
            Entry &en = m_entries[key];
            if (--en.inflight == 0 && en.queued) {
                const int next = *en.queued;
                en.queued.reset();
                launch(key, next);
            }
        });
    };
    for (const Stream &st : streams) {
        const QString idx = QString::number(st.index);
        run({QStringLiteral("set-sink-input-volume"), idx, QString::number(percent) + QLatin1Char('%')});
        if (percent > 0 && st.muted)
            run({QStringLiteral("set-sink-input-mute"), idx, QStringLiteral("0")});
    }
}

void PlayerVolume::set(const std::optional<PlayerInfo> &player, int percent)
{
    if (!player)
        return;
    percent = std::clamp(percent, 0, 100);
    Entry &e = m_entries[player->name];
    if (!e.parsed) {
        e.pendingSet = percent;
        refresh(player);
        return;
    }
    send(player->name, percent);
    e.parsed->state.percent = percent;
    if (percent > 0)
        e.parsed->state.muted = false;
    for (Stream &s : e.parsed->streams)
        if (percent > 0)
            s.muted = false;
}

std::optional<VolumeState> PlayerVolume::adjust(const std::optional<PlayerInfo> &player, int delta)
{
    if (!player)
        return std::nullopt;
    Entry &e = m_entries[player->name];
    if (!e.parsed) {
        e.pendingDelta = e.pendingDelta.value_or(0) + delta;
        refresh(player);
        return std::nullopt;
    }
    const int now = std::clamp(e.parsed->state.percent + delta, 0, 100);
    const bool wasMuted = e.parsed->state.muted;
    set(player, now);
    VolumeState s = e.parsed->state;
    s.muted = now > 0 ? false : wasMuted;
    return s;
}
