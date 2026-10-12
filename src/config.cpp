#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QThreadPool>
#include <QTimer>
#include <memory>

#include <cstdio>

namespace cfg {

QString configDir()
{
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                       : QString::fromLocal8Bit(xdg);
    return base + QStringLiteral("/desktop-pet");
}

QString spritesDir() { return configDir() + QStringLiteral("/sprites"); }
QString settingsFile() { return configDir() + QStringLiteral("/settings.json"); }

double spriteScale()
{
    bool ok = false;
    const double s = qEnvironmentVariable("DESKTOP_PET_SCALE", QStringLiteral("1.0")).toDouble(&ok);
    return ok && s > 0 ? s : 1.0;
}

QJsonObject loadSettings()
{
    QFile f(settingsFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void saveSettings(const QJsonObject &data)
{
    QDir().mkpath(configDir());
    QSaveFile f(settingsFile());
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(data).toJson(QJsonDocument::Indented)) < 0
        || !f.commit()) {
        std::fprintf(stderr, "desktop-pet: nie zapisano ustawień: %s\n", qPrintable(f.errorString()));
    }
}

ProcResult runCapture(const QString &program, const QStringList &args, int timeoutMs)
{
    ProcResult r;
    QProcess p;
    p.setStandardErrorFile(QProcess::nullDevice());
    p.start(program, args);
    p.closeWriteChannel();
    if (!p.waitForStarted(timeoutMs))
        return r;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(300);
        return r;
    }
    r.finished = p.exitStatus() == QProcess::NormalExit;
    r.exitCode = p.exitCode();
    r.out = p.readAllStandardOutput();
    return r;
}

namespace {
// Proces zabijany po cichu, gdy właściciel znika przed jego końcem (zamykanie programu).
class AsyncProcess : public QProcess {
public:
    using QProcess::QProcess;
    ~AsyncProcess() override
    {
        blockSignals(true);               // bez wywołań zwrotnych do właściciela, którego już nie ma
        if (state() != NotRunning) {
            kill();
            waitForFinished(300);
        }
    }
};
}  // namespace

void runAsync(QObject *ctx, const QString &program, const QStringList &args, int timeoutMs,
              std::function<void(const ProcResult &)> done)
{
    auto *p = new AsyncProcess(ctx);
    p->setStandardErrorFile(QProcess::nullDevice());
    auto finished = std::make_shared<bool>(false);
    auto finish = [p, finished, done = std::move(done)](bool ok) {
        if (*finished)
            return;
        *finished = true;
        ProcResult r;
        if (ok) {
            r.finished = p->exitStatus() == QProcess::NormalExit;
            r.exitCode = p->exitCode();
            r.out = p->readAllStandardOutput();
        }
        p->deleteLater();
        done(r);
    };
    QObject::connect(p, &QProcess::finished, p, [finish](int, QProcess::ExitStatus) { finish(true); });
    QObject::connect(p, &QProcess::errorOccurred, p, [finish, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(false);
    });
    auto *timer = new QTimer(p);                       // limit czasu: zabij i zgłoś porażkę
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, p, [finish, p] {
        p->kill();
        finish(false);
    });
    timer->start(timeoutMs);
    p->start(program, args);
    p->closeWriteChannel();
}

namespace {
QThreadPool &settingsPool()
{
    static QThreadPool *pool = [] {
        auto *tp = new QThreadPool;          // jeden wątek = zapisy po kolei
        tp->setMaxThreadCount(1);
        return tp;
    }();
    return *pool;
}
}  // namespace

void saveSettingKeyAsync(const QString &key, const QJsonValue &value)
{
    settingsPool().start([key, value] {
        QJsonObject s = loadSettings();
        s.insert(key, value);
        saveSettings(s);
    });
}

void flushSettings()
{
    settingsPool().waitForDone();
}

}  // namespace cfg
