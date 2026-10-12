// MPRIS przez QtDBus: odtwarzacze na magistrali sesji, metadane i sterowanie.
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <optional>

inline constexpr const char *MPRIS_PREFIX = "org.mpris.MediaPlayer2.";

struct PlayerInfo {
    QString name;                        // pełna nazwa na magistrali (org.mpris.MediaPlayer2.…)
    QString identity;
    QString status = QStringLiteral("Stopped");
    QString title;
    QString artist;                      // pierwszy wykonawca
    QString url;
};

class Mpris {
public:
    QStringList players() const;
    PlayerInfo info(const QString &name) const;
    // Najlepszy odtwarzacz: grający YouTube Music > grający > YT Music > pauza > pierwszy.
    std::optional<PlayerInfo> current() const;
    // Przycisk: Previous / PlayPause / Next (asynchronicznie, interfejs nie czeka).
    void call(const QString &name, const QString &method) const;

    // Ranking odtwarzaczy (osobno, żeby dało się go testować): indeks najlepszego albo -1.
    static int bestIndex(const QList<PlayerInfo> &infos);
};

// Metadane odtwarzacza z odpowiedzi Properties.GetAll(org.mpris.MediaPlayer2.Player):
// PlaybackStatus i Metadata (jako QDBusArgument albo już rozpakowana QVariantMap).
PlayerInfo playerInfoFromProps(const QString &name, const QVariantMap &props);

// Asynchroniczne sprawdzanie, co gra: ListNames + GetAll każdego odtwarzacza przez asyncCall,
// bez blokowania pętli zdarzeń (zawieszony odtwarzacz nie zatrzymuje animacji).
// Naraz leci najwyżej jedno sprawdzenie - poll() w trakcie poprzedniego zwraca false.
class TrackWatcher : public QObject {
    Q_OBJECT
public:
    explicit TrackWatcher(QObject *parent = nullptr) : QObject(parent) {}
    bool poll();                                   // false = poprzednie jeszcze trwa (tick pominięty)
    bool busy() const { return m_busy; }
    const std::optional<PlayerInfo> &latest() const { return m_latest; }
    // Klucz utworu (tytuł + wykonawca) do wykrywania zmiany.
    static QString keyFor(const PlayerInfo &info);

signals:
    // key: klucz utworu najlepszego odtwarzacza ("" = brak), playing: czy naprawdę gra.
    void polled(const QString &key, bool playing);

private:
    void finish();
    bool m_busy = false;
    int m_remaining = 0;
    QList<PlayerInfo> m_infos;
    QHash<QString, QString> m_identity;            // Identity odtwarzaczy (nie zmienia się w czasie życia)
    std::optional<PlayerInfo> m_latest;
};
