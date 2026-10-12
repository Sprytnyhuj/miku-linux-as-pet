#include "steam.h"

#include "config.h"
#include "vdf.h"

#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QProcess>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <sys/stat.h>

namespace {

const std::set<qint64> kToolAppIds = {228980, 1070560, 1391110, 1628350, 1493710, 2180100,
                                      2348590, 1826330, 1887720, 2805730, 3658110};

qint64 monotonicMs()
{
    static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
    return t.elapsed();
}

qint64 toInt(const VdfNode *n, bool *ok)
{
    if (!n || n->dict) {
        *ok = false;
        return 0;
    }
    return n->text.trimmed().toLongLong(ok);
}

qint64 toIntOr0(const VdfNode *n)
{
    bool ok = false;
    const qint64 v = toInt(n, &ok);
    return ok ? v : 0;
}

QString realDir(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();   // pusty, gdy nie istnieje
}

}  // namespace

bool isSteamTool(qint64 appid, const QString &name)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^Proton\b|Steam Linux Runtime|Steamworks Common|Steam Runtime)"),
        QRegularExpression::CaseInsensitiveOption);
    return kToolAppIds.count(appid) || re.match(name).hasMatch();
}

QStringList SteamLibrary::defaultRoots()
{
    const QString home = QDir::homePath();
    return {home + QStringLiteral("/.local/share/Steam"), home + QStringLiteral("/.steam/steam"),
            home + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam")};
}

SteamLibrary::SteamLibrary() : m_roots(defaultRoots()) {}
SteamLibrary::SteamLibrary(const QStringList &roots) : m_roots(roots) {}

QStringList SteamLibrary::steamRoots() const
{
    QStringList out;
    for (const QString &root : m_roots) {
        const QString real = realDir(root);
        if (!real.isEmpty() && !out.contains(real) && QFileInfo(real + QStringLiteral("/steamapps")).isDir())
            out << real;
    }
    return out;
}

QStringList SteamLibrary::libraryDirs() const
{
    QStringList out;
    for (const QString &root : steamRoots()) {
        QStringList paths{root};
        const VdfNode data = parseVdfFile(root + QStringLiteral("/steamapps/libraryfolders.vdf"));
        if (const VdfNode *lf = data.find(QStringLiteral("libraryfolders")); lf && lf->dict) {
            for (const VdfNode &entry : lf->vals) {
                const VdfNode *p = &entry;
                if (entry.dict)                    // nowy format: {"path": …, "apps": …}
                    p = entry.find(QStringLiteral("path"));
                // stary format: "1" "ścieżka"
                if (p && !p->dict && p->text.startsWith(QLatin1Char('/')))
                    paths << p->text;
            }
        }
        for (const QString &path : paths) {
            const QString real = realDir(path);
            if (!real.isEmpty() && !out.contains(real) && QFileInfo(real + QStringLiteral("/steamapps")).isDir())
                out << real;
        }
    }
    return out;
}

QList<QPair<QString, qint64>> SteamLibrary::dirStamp(const QStringList &libs) const
{
    QList<QPair<QString, qint64>> stamp;
    for (const QString &lib : libs) {
        struct stat st;
        if (::stat(QFile::encodeName(lib + QStringLiteral("/steamapps")).constData(), &st) == 0)
            stamp.append({lib, qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec});
    }
    return stamp;
}

const QList<SteamGame> &SteamLibrary::scan()
{
    const QStringList libs = libraryDirs();
    QHash<qint64, SteamGame> games;
    for (const QString &lib : libs) {
        const QDir apps(lib + QStringLiteral("/steamapps"));
        for (const QString &fn : apps.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files, QDir::Name)) {
            const VdfNode doc = parseVdfFile(apps.filePath(fn));
            const VdfNode *state = doc.find(QStringLiteral("appstate"));
            if (!state || !state->dict)
                continue;
            bool ok = false;
            const qint64 appid = toInt(state->find(QStringLiteral("appid")), &ok);
            const VdfNode *nm = state->find(QStringLiteral("name"));
            const QString name = (nm && !nm->dict) ? nm->text.trimmed() : QString();
            const qint64 flags = toIntOr0(state->find(QStringLiteral("stateflags")));
            if (!ok || name.isEmpty() || !(flags & cfg::STEAM_FULLY_INSTALLED))
                continue;
            if (isSteamTool(appid, name))
                continue;
            if (!games.contains(appid)) {
                SteamGame g;
                g.appid = appid;
                g.name = name;
                g.lastPlayed = toIntOr0(state->find(QStringLiteral("lastplayed")));
                g.updating = flags != cfg::STEAM_FULLY_INSTALLED;
                g.library = lib;
                games.insert(appid, g);
            }
        }
    }
    m_games = games.values();
    // Ostatnio grane na górze, potem alfabetycznie (bez rozróżniania wielkości liter).
    std::stable_sort(m_games.begin(), m_games.end(), [](const SteamGame &a, const SteamGame &b) {
        if (a.lastPlayed != b.lastPlayed)
            return a.lastPlayed > b.lastPlayed;
        const QString fa = a.name.toCaseFolded(), fb = b.name.toCaseFolded();
        return fa != fb ? fa < fb : a.appid < b.appid;
    });
    m_scannedMs = monotonicMs();
    m_stamp = dirStamp(libs);
    return m_games;
}

const QList<SteamGame> &SteamLibrary::games()
{
    QStringList stampLibs;
    for (const auto &s : std::as_const(m_stamp))
        stampLibs << s.first;
    if (m_scannedMs < 0 || monotonicMs() - m_scannedMs > qint64(cfg::STEAM_RESCAN_S) * 1000
        || dirStamp(stampLibs) != m_stamp)
        scan();
    return m_games;
}

QString SteamLibrary::iconPath(const SteamGame &game) const
{
    static const QRegularExpression hashed(QStringLiteral("^[0-9a-f]{40}\\.(jpg|png)$"));
    for (const QString &root : steamRoots()) {
        const QString cache = root + QStringLiteral("/appcache/librarycache");
        // Nowy układ librarycache/<appid>/<hash>.jpg - najmniejszy plik to ikonka.
        const QDir folder(cache + QLatin1Char('/') + QString::number(game.appid));
        qint64 bestSize = -1;
        QString best;
        for (const QFileInfo &fi : folder.entryInfoList(QDir::Files)) {
            if (hashed.match(fi.fileName()).hasMatch()
                && (bestSize < 0 || fi.size() < bestSize
                    || (fi.size() == bestSize && fi.absoluteFilePath() < best))) {
                bestSize = fi.size();
                best = fi.absoluteFilePath();
            }
        }
        if (!best.isEmpty())
            return best;
        // Stary układ librarycache/<appid>_icon.jpg.
        const QString old = cache + QStringLiteral("/%1_icon.jpg").arg(game.appid);
        if (QFileInfo(old).isFile())
            return old;
    }
    return {};
}

namespace {

// Wczytanie i zmniejszenie ikonki - bezpieczne w wątku roboczym (QImage, bez QPixmap).
QImage loadIconImage(const QString &path)
{
    QImage img = path.isEmpty() ? QImage() : QImage(path);
    const int limit = cfg::STEAM_ICON_PX * 2;       // zapas na HiDPI
    if (!img.isNull() && (img.width() > limit || img.height() > limit))
        img = img.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

}  // namespace

QIcon SteamLibrary::icon(const SteamGame &game)
{
    auto it = m_icons.constFind(game.appid);
    if (it == m_icons.constEnd()) {
        const QImage img = loadIconImage(iconPath(game));
        it = m_icons.insert(game.appid, img.isNull() ? QIcon() : QIcon(QPixmap::fromImage(img)));
    }
    return it.value();
}

SteamCache::SteamCache(const QStringList &roots, QObject *parent) : QObject(parent), m_roots(roots)
{
    m_pool.setMaxThreadCount(1);
}

SteamCache::~SteamCache()
{
    m_pool.waitForDone();
}

QIcon SteamCache::icon(const SteamGame &game)
{
    auto it = m_icons.constFind(game.appid);
    if (it == m_icons.constEnd()) {
        const QImage img = m_images.value(game.appid);
        it = m_icons.insert(game.appid, img.isNull() ? QIcon() : QIcon(QPixmap::fromImage(img)));
    }
    return it.value();
}

void SteamCache::refresh()
{
    if (m_running)
        return;
    m_running = true;
    const QStringList roots = m_roots;
    const QSet<qint64> known = QSet<qint64>(m_images.keyBegin(), m_images.keyEnd());
    auto *self = this;
    m_pool.start([self, roots, known] {
        SteamLibrary lib(roots);
        QList<SteamGame> games = lib.scan();
        QHash<qint64, QImage> images;
        for (const SteamGame &g : std::as_const(games)) {
            if (!known.contains(g.appid))
                images.insert(g.appid, loadIconImage(lib.iconPath(g)));
        }
        QMetaObject::invokeMethod(self, [self, games, images] {
            self->m_games = games;
            for (auto it = images.cbegin(); it != images.cend(); ++it) {
                self->m_images.insert(it.key(), it.value());
                self->m_icons.remove(it.key());
            }
            self->m_running = false;
            self->m_ready = true;
            emit self->updated();
        }, Qt::QueuedConnection);
    });
}

namespace {

// Program od steam:// według xdg-mime (np. "steam.desktop"), odczytany w tle przy starcie.
std::mutex g_handlerMutex;
std::optional<QString> g_handler;     // nullopt = jeszcze nieznany

}  // namespace

void prefetchSteamHandler()
{
    QThreadPool::globalInstance()->start([] {
        const QString h = QString::fromUtf8(
            cfg::runCapture(QStringLiteral("xdg-mime"),
                            {QStringLiteral("query"), QStringLiteral("default"),
                             QStringLiteral("x-scheme-handler/steam")}, 3000).out).trimmed();
        std::lock_guard<std::mutex> lock(g_handlerMutex);
        g_handler = h;
    });
}

bool openSteamUrl(const QString &url)
{
    QString handler;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(g_handlerMutex);
        known = g_handler.has_value();
        if (known)
            handler = *g_handler;
    }
    // Odczyt jeszcze trwa (tuż po starcie): próbujemy systemowej obsługi adresów jak przy znanym programie.
    if ((!known || !handler.isEmpty()) && QDesktopServices::openUrl(QUrl(url)))
        return true;
    if (QProcess::startDetached(QStringLiteral("steam"), {url}))
        return true;
    std::fprintf(stderr, "desktop-pet: nie uruchomiono Steama\n");
    return false;
}
