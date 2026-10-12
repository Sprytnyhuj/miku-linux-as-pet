// Logika wykrywania zdarzeń do reakcji postaci (bez Qt Widgets, żeby dało się ją testować).
#pragma once

#include <QString>
#include <deque>
#include <utility>

// Szybkie kręcenie kółkiem: suma ząbków (wartości bezwzględnych) w oknie czasu >= próg.
// Po wykryciu licznik zeruje się, więc kolejna reakcja wymaga nowego rozkręcenia.
class WheelSpinDetector {
public:
    static constexpr double THRESHOLD = 6.0;     // ząbków
    static constexpr qint64 WINDOW_MS = 1500;    // w tym czasie

    // steps: przesunięcie w ząbkach (może być ułamkowe, ze znakiem), nowMs: zegar monotoniczny.
    bool add(double steps, qint64 nowMs)
    {
        const double amount = steps < 0 ? -steps : steps;
        if (amount <= 0)
            return false;
        m_events.emplace_back(nowMs, amount);
        double sum = 0;
        while (!m_events.empty() && nowMs - m_events.front().first > WINDOW_MS)
            m_events.pop_front();
        for (const auto &e : m_events)
            sum += e.second;
        if (sum >= THRESHOLD) {
            m_events.clear();
            return true;
        }
        return false;
    }

private:
    std::deque<std::pair<qint64, double>> m_events;
};

// Zmiana utworu: porównuje klucz (tytuł + wykonawca) z poprzednim odczytem. Fałszywe zmiany nie reagują:
// puste/niepełne metadane (ładowanie, reklama bez wykonawcy) są ignorowane, zmiana samego odtwarzacza
// tylko przestawia punkt odniesienia, nowy utwór musi się utrzymać przez STABLE_READS odczytów z rzędu,
// a reakcje są nie częściej niż co COOLDOWN_MS.
class TrackChangeDetector {
public:
    static constexpr qint64 COOLDOWN_MS = 60000;   // reakcje nie częściej niż co tyle
    static constexpr int STABLE_READS = 2;         // tyle kolejnych odczytów z tym samym nowym utworem

    // Klucz musi mieć niepusty tytuł i niepustego wykonawcę ("tytuł\nwykonawca").
    static bool validKey(const QString &key)
    {
        const int i = key.indexOf(QLatin1Char('\n'));
        return i > 0 && i < key.size() - 1;
    }

    // player: nazwa odtwarzacza, key: identyfikator utworu ("" = nic nie gra), allowed: czy postać może
    // zareagować (nie jest przeciągana ani nie spada). Zmiana w czasie, gdy allowed=false,
    // jest zapamiętana, ale bez reakcji.
    bool update(const QString &player, const QString &key, qint64 nowMs, bool allowed)
    {
        if (!validKey(key)) {
            m_cand.clear();
            m_candCount = 0;
            return false;
        }
        if (!m_have || player != m_player) {   // pierwszy odczyt albo inny odtwarzacz - tylko punkt odniesienia
            m_have = true;
            m_player = player;
            m_last = key;
            m_cand.clear();
            m_candCount = 0;
            return false;
        }
        if (key == m_last) {
            m_cand.clear();
            m_candCount = 0;
            return false;
        }
        if (key == m_cand) {
            ++m_candCount;
        } else {
            m_cand = key;
            m_candCount = 1;
        }
        if (m_candCount < STABLE_READS)
            return false;
        m_last = key;
        m_cand.clear();
        m_candCount = 0;
        if (!allowed)
            return false;
        if (m_reacted && nowMs - m_lastReaction < COOLDOWN_MS)
            return false;
        m_reacted = true;
        m_lastReaction = nowMs;
        return true;
    }

private:
    bool m_have = false, m_reacted = false;
    QString m_player, m_last, m_cand;
    int m_candCount = 0;
    qint64 m_lastReaction = 0;
}; 
