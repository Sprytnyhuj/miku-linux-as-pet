// Wyszukiwarka jak KRunner: interfejs silnika (do podmiany atrapą w testach) i silnik na KF6 Runner.
#pragma once

#include <QIcon>
#include <QList>
#include <QObject>
#include <QString>

namespace KRunner {
class RunnerManager;
class QueryMatch;
}

struct SearchResult {
    QString title, subtitle, category;
    QIcon icon;
    QString iconName;          // nazwa ikony motywu (przekazywana między procesami zamiast QIcon)
};

// Silnik wyszukiwania. Zapytania są asynchroniczne: wyniki przychodzą sygnałem resultsChanged
// (możliwe wielokrotnie, w miarę jak odpowiadają kolejne runnery), koniec - sygnałem finished.
Q_DECLARE_METATYPE(SearchResult)

class SearchEngine : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void begin() {}                          // dymek się otworzył (sesja dopasowań)
    virtual void end() {}                            // dymek się zamknął
    virtual void query(const QString &text) = 0;     // puste = wyczyść
    virtual bool run(int index) = 0;                 // uruchamia wynik o danym numerze z ostatnio zgłoszonej listy
signals:
    void problem(const QString &text);               // krótki komunikat dla użytkownika (np. restart pomocnika)
    void resultsChanged(const QString &query, const QList<SearchResult> &results);
    void finished(const QString &query);
};

// Ten sam silnik, którego używa Plasma (KRunner::RunnerManager) - w procesie programu, ładowany
// dopiero przy pierwszym użyciu.
class KRunnerEngine : public SearchEngine {
    Q_OBJECT
public:
    explicit KRunnerEngine(QObject *parent = nullptr);
    ~KRunnerEngine() override;
    // Runnery dozwolone w wyszukiwarce (lista dozwolonych; bez powłoki, kill, sesji, zasilania itd.).
    static QStringList allowedRunners();
    void begin() override;
    void end() override;
    void query(const QString &text) override;
    bool run(int index) override;

    static constexpr int MAX_RESULTS = 40;

private:
    KRunner::RunnerManager *manager();
    KRunner::RunnerManager *m_manager = nullptr;
    QString m_query;
    QList<KRunner::QueryMatch> *m_matches;
    bool m_session = false;
    bool m_sycocaChecked = false;
};
