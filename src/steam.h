// Gry ze Steama: libraryfolders.vdf i appmanifest_*.acf (tylko odczyt), ikonki, otwieranie steam://.
#pragma once

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QThreadPool>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <ctime>

struct SteamGame {
    qint64 appid = 0;
    QString name;
    qint64 lastPlayed = 0;
    bool updating = false;   // zainstalowana, ale ma też bity aktualizacji w StateFlags
    QString library;
};

// Narzędzia instalowane przez Steam jak gry (Proton, Steam Linux Runtime…) - nie do menu.
bool isSteamTool(qint64 appid, const QString &name);

class SteamLibrary {
public:
    SteamLibrary();                                  // domyślne korzenie (natywny / Flatpak)
    explicit SteamLibrary(const QStringList &roots);
    static QStringList defaultRoots();

    QStringList steamRoots() const;                  // istniejące instalacje, bez duplikatów
    QStringList libraryDirs() const;                 // wszystkie foldery bibliotek, bez duplikatów
    const QList<SteamGame> &scan();                  // wymusza skan od nowa
    // Lista z pamięci; skan od nowa po STEAM_RESCAN_S albo gdy zmienił się folder steamapps.
    const QList<SteamGame> &games();
    QString iconPath(const SteamGame &game) const;
    QIcon icon(const SteamGame &game);               // wczytywana przy pierwszym użyciu, potem z pamięci

private:
    QList<QPair<QString, qint64>> dirStamp(const QStringList &libs) const;

    QStringList m_roots;
    QList<SteamGame> m_games;
    qint64 m_scannedMs = -1;                         // monotoniczny czas ostatniego skanu
    QList<QPair<QString, qint64>> m_stamp;           // czasy modyfikacji folderów steamapps
    QHash<qint64, QIcon> m_icons;                    // pusta QIcon = brak obrazka
};

// Lista gier i ikonki w pamięci, odświeżane w tle (wątek roboczy): menu nigdy nie skanuje
// dysku i nie dekoduje obrazków. Pierwsze menu tuż po starcie może mieć jeszcze pustą listę.
class SteamCache : public QObject {
    Q_OBJECT
public:
    explicit SteamCache(const QStringList &roots = SteamLibrary::defaultRoots(), QObject *parent = nullptr);
    ~SteamCache() override;                     // czeka na zadanie w tle (nie wolno go zostawić z wiszącym this)
    const QList<SteamGame> &games() const { return m_games; }
    QIcon icon(const SteamGame &game);          // z pamięci (QImage z tła -> QIcon przy pierwszym użyciu)
    void refresh();                             // zlecenie odczytu w tle (jedno naraz)
    bool ready() const { return m_ready; }

signals:
    void updated();

private:
    QStringList m_roots;
    QList<SteamGame> m_games;
    QHash<qint64, QImage> m_images;             // z wątku roboczego (null = brak obrazka)
    QHash<qint64, QIcon> m_icons;
    bool m_running = false, m_ready = false;
    QThreadPool m_pool;                         // jeden wątek roboczy, własny dla obiektu
};

// Odczyt programu obsługującego steam:// w tle (raz); wołać przy starcie.
void prefetchSteamHandler();
// Otwiera steam://… przez systemową obsługę adresów, a bez niej - przez program steam.
bool openSteamUrl(const QString &url);
