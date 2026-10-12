#include "animlog.h"

#include "config.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace animlog {
namespace {
int g_state = -1;   // -1 = nie sprawdzono, 0 = wyłączony, 1 = włączony
}

QString logPath()
{
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/desktop-pet/anim.log");
}

bool enabled()
{
    if (g_state < 0) {
        g_state = (qEnvironmentVariable("DESKTOP_PET_DEBUG") == QLatin1String("1")
                   || QFileInfo::exists(cfg::configDir() + QStringLiteral("/debug"))) ? 1 : 0;
    }
    return g_state == 1;
}

void resetForTests() { g_state = -1; }

QString shortHash(const QString &text)
{
    if (text.isEmpty())
        return QStringLiteral("-");
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex().left(6));
}

void write(const QString &what)
{
    if (!enabled())
        return;
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (QFileInfo(path).size() > MAX_BYTES) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write((QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) + QLatin1Char(' ') + what
             + QLatin1Char('\n')).toUtf8());
}
}  // namespace animlog
