#include "searchhelper.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

// ---- klient (maskotka) ----

HelperEngine::HelperEngine(QObject *parent) : SearchEngine(parent)
{
    m_clock.start();
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(IDLE_STOP_MS);
    connect(m_idle, &QTimer::timeout, this, &HelperEngine::stopHelper);
    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    m_restartTimer->setInterval(RESTART_DELAY_MS);
    connect(m_restartTimer, &QTimer::timeout, this, [this] {
        if (!m_session)
            return;
        ensureRunning();                                 // wysyła begin
        if (!m_text.trimmed().isEmpty())
            query(m_text);                               // ponowienie przerwanego zapytania
    });
}

HelperEngine::~HelperEngine()
{
    stopHelper();
}

QString HelperEngine::helperProgram()
{
    const QByteArray env = qgetenv("DESKTOP_PET_HELPER");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    QString self = QCoreApplication::applicationFilePath();
    // Po aktualizacji (install zamienia plik) /proc/self/exe pokazuje "... (deleted)".
    if (self.endsWith(QLatin1String(" (deleted)")))
        self.chop(10);
    if (QFileInfo(self).fileName() == QLatin1String("desktop-pet"))
        return self;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/desktop-pet");   // np. binarka testów
}

void HelperEngine::ensureRunning()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        return;
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->deleteLater();
    }
    m_buf.clear();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::ForwardedErrorChannel);   // ślad stosu pomocnika trafia do dziennika
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &HelperEngine::readOutput);
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        helperGone(st == QProcess::CrashExit ? QStringLiteral("zawiesił się") : QStringLiteral("zakończył (kod %1)").arg(code));
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            helperGone(QStringLiteral("nie uruchomił się"));
    });
    m_stopping = false;
    m_proc->start(helperProgram(), {QStringLiteral("--search-helper")});
    if (m_session)
        send({{QStringLiteral("op"), QStringLiteral("begin")}});
}

void HelperEngine::send(const QJsonObject &obj)
{
    if (!m_proc || m_proc->state() == QProcess::NotRunning)
        return;
    m_proc->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void HelperEngine::begin()
{
    m_session = true;
    m_gaveUp = false;
    m_idle->stop();
    if (m_proc && m_proc->state() != QProcess::NotRunning)
        send({{QStringLiteral("op"), QStringLiteral("begin")}});
    else
        ensureRunning();
}

void HelperEngine::end()
{
    m_session = false;
    m_text.clear();
    m_count = 0;
    m_restartTimer->stop();
    send({{QStringLiteral("op"), QStringLiteral("end")}});
    m_idle->start();
}

void HelperEngine::query(const QString &text)
{
    m_text = text;
    if (text.trimmed().isEmpty())
        m_count = 0;
    if (m_gaveUp) {
        emit finished(text);
        return;
    }
    ensureRunning();
    send({{QStringLiteral("op"), QStringLiteral("query")}, {QStringLiteral("id"), ++m_seq}, {QStringLiteral("text"), text}});
}

bool HelperEngine::run(int index)
{
    if (!m_proc || m_proc->state() != QProcess::Running || index < 0 || index >= m_count)
        return false;
    send({{QStringLiteral("op"), QStringLiteral("run")}, {QStringLiteral("id"), ++m_seq}, {QStringLiteral("index"), index}});
    return true;
}

void HelperEngine::readOutput()
{
    m_buf += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString ev = o.value(QLatin1String("ev")).toString();
        const QString q = o.value(QLatin1String("query")).toString();
        if (ev == QLatin1String("results")) {
            QList<SearchResult> out;
            for (const QJsonValue &v : o.value(QLatin1String("results")).toArray()) {
                const QJsonObject r = v.toObject();
                SearchResult s;
                s.title = r.value(QLatin1String("title")).toString();
                s.subtitle = r.value(QLatin1String("sub")).toString();
                s.category = r.value(QLatin1String("cat")).toString();
                s.iconName = r.value(QLatin1String("icon")).toString();
                s.icon = s.iconName.isEmpty() ? QIcon() : QIcon::fromTheme(s.iconName);
                out.append(s);
            }
            if (q == m_text)
                m_count = int(out.size());
            emit resultsChanged(q, out);
        } else if (ev == QLatin1String("finished")) {
            emit finished(q);
        }
    }
}

void HelperEngine::helperGone(const QString &why)
{
    if (m_stopping)
        return;
    std::fprintf(stderr, "desktop-pet: pomocnik wyszukiwania %s\n", qPrintable(why));
    m_count = 0;
    if (!m_session)
        return;                                          // dymek zamknięty - następne begin() uruchomi nowego
    const qint64 now = m_clock.elapsed();
    while (!m_restartTimes.isEmpty() && now - m_restartTimes.first() > RESTART_WINDOW_MS)
        m_restartTimes.removeFirst();
    if (m_restartTimes.size() >= MAX_RESTARTS) {
        m_gaveUp = true;
        emit problem(QStringLiteral("Wyszukiwarka nie działa - spróbuj ponownie później."));
        emit finished(m_text);
        return;
    }
    m_restartTimes.append(now);
    ++m_totalRestarts;
    emit problem(QStringLiteral("Wyszukiwarka została uruchomiona ponownie…"));
    m_restartTimer->start();
}

void HelperEngine::stopHelper()
{
    if (!m_proc)
        return;
    m_stopping = true;
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->closeWriteChannel();                     // EOF na stdin = pomocnik kończy sam
        if (!m_proc->waitForFinished(1500)) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
    delete m_proc;
    m_proc = nullptr;
}

// ---- pomocnik (--search-helper) ----

namespace {

class HelperServer : public QObject {
public:
    HelperServer()
    {
        m_out = ::fdopen(::dup(1), "w");                 // protokół na prywatnym deskryptorze,
        ::dup2(2, 1);                                    // a zabłąkane wydruki idą na stderr
        ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
        auto *n = new QSocketNotifier(0, QSocketNotifier::Read, this);
        connect(n, &QSocketNotifier::activated, this, [this, n] { readInput(n); });
        connect(&m_engine, &SearchEngine::resultsChanged, this,
                [this](const QString &q, const QList<SearchResult> &results) {
            QJsonArray arr;
            for (const SearchResult &r : results)
                arr.append(QJsonObject{{QStringLiteral("title"), r.title}, {QStringLiteral("sub"), r.subtitle},
                                       {QStringLiteral("cat"), r.category}, {QStringLiteral("icon"), r.iconName}});
            emitEvent({{QStringLiteral("ev"), QStringLiteral("results")}, {QStringLiteral("query"), q},
                       {QStringLiteral("results"), arr}});
        });
        connect(&m_engine, &SearchEngine::finished, this, [this](const QString &q) {
            emitEvent({{QStringLiteral("ev"), QStringLiteral("finished")}, {QStringLiteral("query"), q}});
        });
    }

private:
    void emitEvent(const QJsonObject &o)
    {
        const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
        std::fwrite(line.constData(), 1, size_t(line.size()), m_out);
        std::fflush(m_out);
    }

    void readInput(QSocketNotifier *n)
    {
        char buf[4096];
        for (;;) {
            const ssize_t r = ::read(0, buf, sizeof buf);
            if (r > 0) {
                m_buf.append(buf, int(r));
                continue;
            }
            if (r == 0) {                                // rodzic zamknął rurę - kończymy
                n->setEnabled(false);
                std::fflush(m_out);
                ::_exit(0);                              // bez porządkowania: wątki runnerów mogłyby wysypać zwykłe zamknięcie
            }
            break;                                       // EAGAIN
        }
        int nl;
        while ((nl = m_buf.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(m_buf.left(nl)).object();
            m_buf.remove(0, nl + 1);
            handle(o);
        }
    }

    void handle(const QJsonObject &o)
    {
        const QString op = o.value(QLatin1String("op")).toString();
        if (op == QLatin1String("begin")) {
            m_engine.begin();
        } else if (op == QLatin1String("end")) {
            m_engine.end();
        } else if (op == QLatin1String("query")) {
            m_engine.query(o.value(QLatin1String("text")).toString());
        } else if (op == QLatin1String("run")) {
            const bool ok = m_engine.run(o.value(QLatin1String("index")).toInt(-1));
            emitEvent({{QStringLiteral("ev"), QStringLiteral("ran")}, {QStringLiteral("id"), o.value(QLatin1String("id"))},
                       {QStringLiteral("ok"), ok}});
        }
    }

    KRunnerEngine m_engine;
    QByteArray m_buf;
    FILE *m_out = nullptr;
};

}  // namespace

int runSearchHelper()
{
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);                  // rodzic zginął - pomocnik też
    HelperServer server;
    return QCoreApplication::exec();
}
