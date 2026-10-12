// Głośność strumienia audio odtwarzacza (jak w mikserze KDE), przez pactl - asynchronicznie.
#pragma once

#include "mpris.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <optional>

struct VolumeState {
    int percent = 0;
    bool muted = false;
};

// Chromium ignoruje głośność z MPRIS, więc szukamy jego strumienia w PipeWire:
// nazwa odtwarzacza z MPRIS (np. "Chrome", "Firefox") musi pasować do nazwy
// aplikacji strumienia ("Google Chrome", "Firefox"). Przeglądarka miksuje
// wszystkie karty w jeden strumień, więc zmienia się głośność całej przeglądarki.
//
// Nic tu nie czeka na pactl: refresh() uruchamia go w tle, get() oddaje ostatnią znaną
// wartość, set()/adjust() liczą od niej i wysyłają polecenia bez czekania.
class PlayerVolume : public QObject {
    Q_OBJECT
public:
    struct Stream {
        int index = 0;
        bool muted = false;
    };
    // Wynik analizy `pactl -f json list sink-inputs` dla jednego odtwarzacza.
    struct Parsed {
        QList<Stream> streams;
        VolumeState state;
    };

    explicit PlayerVolume(QObject *parent = nullptr) : QObject(parent) {}

    // Pasujące strumienie z wyniku `pactl -f json list sink-inputs` (osobno - do testów).
    static QJsonArray matchStreams(const QJsonArray &streams, const PlayerInfo &player);
    // Cała analiza odpowiedzi pactl (osobno - do testów); nullopt, gdy odtwarzacz nie ma strumienia.
    static std::optional<Parsed> parse(const QByteArray &json, const PlayerInfo &player);

    // Ostatnio odczytana głośność odtwarzacza (nullopt = nieznana albo brak strumienia).
    std::optional<VolumeState> get(const std::optional<PlayerInfo> &player) const;
    // Odczyt w tle; po zakończeniu sygnał changed(). Gdy poprzedni jeszcze trwa, zostanie powtórzony.
    void refresh(const std::optional<PlayerInfo> &player);
    // Ustawia głośność (0-100): polecenia pactl idą w tle. Bez znanych strumieni najpierw odczyt.
    void set(const std::optional<PlayerInfo> &player, int percent);
    // Zmienia o delta od ostatniej znanej wartości; bez znanej wartości czeka na odczyt.
    // Zwraca nową wartość do pokazania albo nullopt, gdy jeszcze nieznana.
    std::optional<VolumeState> adjust(const std::optional<PlayerInfo> &player, int delta);

signals:
    void changed();

private:
    struct Entry {
        std::optional<Parsed> parsed;
        std::optional<int> pendingDelta;    // zmiana zlecona przed pierwszym odczytem
        std::optional<int> pendingSet;
        int inflight = 0;                   // trwające polecenia pactl (set-sink-input-*)
        std::optional<int> queued;          // ostatnia wartość czekająca na koniec trwających poleceń
    };
    void send(const QString &key, int percent);
    void launch(const QString &key, int percent);
    void onResult(const PlayerInfo &player, const QByteArray &json, bool ok);

    QHash<QString, Entry> m_entries;       // po nazwie odtwarzacza
    bool m_running = false;
    bool m_again = false;
    std::optional<PlayerInfo> m_latestPlayer;
};
