// Wyszukiwarka w osobnym procesie: ta sama binarka z --search-helper. Biblioteki i runnery KDE (KF6 Runner,
// KService, wtyczki, D-Bus) działają tylko tam, więc ich awaria nie zabija maskotki.
// Protokół: po jednym obiekcie JSON w wierszu, stdin/stdout.
//   do pomocnika: {"op":"begin"} {"op":"end"} {"op":"query","id":N,"text":"..."} {"op":"run","id":N,"index":I}
//   z pomocnika:  {"ev":"results","id":N,"query":"...","results":[{"title","sub","cat","icon"}]}
//                 {"ev":"finished","id":N,"query":"..."} {"ev":"ran","id":N,"ok":true}
#pragma once

#include "search.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

// Strona maskotki: silnik wyszukiwania na pomocniku, z automatycznym restartem.
class HelperEngine : public SearchEngine {
    Q_OBJECT
public:
    static constexpr int IDLE_STOP_MS = 120000;     // po tylu ms od zamknięcia dymka pomocnik się kończy
    static constexpr int RESTART_DELAY_MS = 300;
    static constexpr int MAX_RESTARTS = 5;           // na RESTART_WINDOW_MS; potem poddajemy się do następnego otwarcia
    static constexpr int RESTART_WINDOW_MS = 60000;

    explicit HelperEngine(QObject *parent = nullptr);
    ~HelperEngine() override;
    void begin() override;
    void end() override;
    void query(const QString &text) override;
    bool run(int index) override;

    // Ścieżka binarki pomocnika (DESKTOP_PET_HELPER, własna binarka albo desktop-pet obok pliku wykonywalnego).
    static QString helperProgram();
    qint64 helperPid() const { return m_proc && m_proc->state() == QProcess::Running ? m_proc->processId() : 0; }
    int restarts() const { return m_totalRestarts; }

private:
    void ensureRunning();
    void send(const QJsonObject &obj);
    void readOutput();
    void helperGone(const QString &why);
    void stopHelper();

    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    QTimer *m_idle, *m_restartTimer;
    QString m_text;                     // zapytanie, na które czekamy (pusty = brak)
    int m_seq = 0;
    int m_count = 0;                    // ile wyników w ostatniej liście
    bool m_session = false;
    bool m_stopping = false;
    bool m_gaveUp = false;
    int m_totalRestarts = 0;
    QList<qint64> m_restartTimes;       // znaczniki czasu restartów (ms od startu)
    QElapsedTimer m_clock;
};

// Strona pomocnika (--search-helper): pętla zdarzeń z KRunnerEngine, rozmowa przez stdin/stdout.
int runSearchHelper();
