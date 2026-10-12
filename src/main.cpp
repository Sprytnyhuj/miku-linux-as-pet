// desktop-pet - chibi maskotka na pulpit (C++/Qt6). Opis w README.md.
#include "bubbles.h"
#include "claudechat.h"
#include "config.h"
#include "pet.h"
#include "searchhelper.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QPainter>
#include <QSettings>
#include <QSocketNotifier>
#include <QStyleFactory>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <execinfo.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

// Blokada pliku - druga kopia programu od razu się kończy. Zwraca deskryptor (-1 = już działa).
int singleInstanceLock()
{
    QString runDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runDir.isEmpty())
        runDir = cfg::configDir();
    QDir().mkpath(runDir);
    const int fd = ::open(QFile::encodeName(runDir + QStringLiteral("/desktop-pet.lock")).constData(),
                          O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {     // nie da się utworzyć pliku blokady - działamy bez blokady, a nie "już działa"
        std::fprintf(stderr, "desktop-pet: nie utworzono pliku blokady, pomijam\n");
        return -2;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;        // trzymamy otwarty plik do końca działania programu
}

// Czyste wyjście na SIGINT/SIGTERM: handler tylko zapisuje bajt do rury (async-signal-safe),
// a QSocketNotifier w pętli Qt wywołuje quit() - Qt nie jest ruszane z handlera.
int g_sigPipe[2] = {-1, -1};

void onSignal(int)
{
    const int saved = errno;
    const char b = 1;
    if (::write(g_sigPipe[1], &b, 1) < 0) {}
    errno = saved;
}

// Diagnostyka: przy SIGSEGV/SIGABRT/SIGBUS/SIGFPE wypisz ślad stosu do stderr (dziennik użytkownika), potem
// przywróć domyślną obsługę i ponów sygnał, więc coredump powstaje jak dotąd.
void onFatal(int sig)
{
    static const char msg[] = "desktop-pet: sygnał krytyczny, ślad stosu:\n";
    if (::write(2, msg, sizeof msg - 1) < 0) {}
    void *frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    ::raise(sig);
}

void installCrashHandlers()
{
    // Zerwane połączenie (D-Bus, gniazdo, rura) nie może po cichu zabić procesu przez SIGPIPE.
    signal(SIGPIPE, SIG_IGN);
    struct sigaction fa {};
    fa.sa_handler = onFatal;
    sigemptyset(&fa.sa_mask);
    fa.sa_flags = SA_RESETHAND | SA_NODEFER;
    for (const int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
        sigaction(s, &fa, nullptr);
}

void installSignalHandlers()
{
    if (::pipe2(g_sigPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    installCrashHandlers();
    auto *n = new QSocketNotifier(g_sigPipe[0], QSocketNotifier::Read, qApp);
    QObject::connect(n, &QSocketNotifier::activated, qApp, [n] {
        char buf[16];
        if (::read(g_sigPipe[0], buf, sizeof buf) < 0) {}
        n->setEnabled(false);
        Pet::markExplicitQuit();
        std::fprintf(stderr, "desktop-pet: SIGINT/SIGTERM - kończę\n");
        QApplication::quit();
    });
}

// Ciemna paleta do zrzutów (--dark), żeby sprawdzić kolory z palety w obu motywach.
void applyDarkPalette()
{
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, QColor(0x2a, 0x2e, 0x32));
    p.setColor(QPalette::WindowText, QColor(0xfc, 0xfc, 0xfc));
    p.setColor(QPalette::Base, QColor(0x1b, 0x1e, 0x20));
    p.setColor(QPalette::AlternateBase, QColor(0x2a, 0x2e, 0x32));
    p.setColor(QPalette::Text, QColor(0xfc, 0xfc, 0xfc));
    p.setColor(QPalette::Button, QColor(0x31, 0x36, 0x3b));
    p.setColor(QPalette::ButtonText, QColor(0xfc, 0xfc, 0xfc));
    p.setColor(QPalette::PlaceholderText, QColor(0xa1, 0xa9, 0xb1));
    p.setColor(QPalette::Mid, QColor(0x4d, 0x54, 0x5c));
    p.setColor(QPalette::Highlight, QColor(0x3d, 0xae, 0xe9));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    QApplication::setPalette(p);
}

// Zrzut widżetu do PNG (ukryta opcja do sprawdzania wyglądu bez przeglądarki): widżet
// z przezroczystym tłem składamy na neutralnym tle "pulpitu".
bool saveShot(QWidget *w, const QString &file)
{
    w->show();
    QApplication::processEvents();
    const qreal dpr = w->devicePixelRatioF();
    QImage img((w->size() * dpr).expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(QColor(0x7a, 0x84, 0x8f));
    QPainter p(&img);
    w->render(&p);
    p.end();
    return img.save(file);
}

int screenshotBubble(const QString &file)
{
    Mpris mpris;
    TrackWatcher watcher;
    PlayerVolume vol;
    MusicBubble bubble(&mpris, &watcher, &vol);
    PlayerInfo pi;
    pi.name = QStringLiteral("org.mpris.MediaPlayer2.chromium.instance1");
    pi.status = QStringLiteral("Playing");
    pi.title = QStringLiteral("Tell Your World");
    pi.artist = QStringLiteral("livetune feat. Hatsune Miku");
    bubble.showPlayer(pi, VolumeState{65, false});
    bubble.setTail(bubble.width() / 2.0);
    return saveShot(&bubble, file) ? 0 : 1;
}

// Paleta z pliku schematu kolorów KDE (np. /usr/share/color-schemes/BreezeDark.colors) - zrzuty jak na pulpicie.
bool applyColorScheme(const QString &path)
{
    QSettings s(path, QSettings::IniFormat);
    auto col = [&s](const QString &group, const QString &key, const QColor &def) {
        const QStringList v = s.value(group + QLatin1Char('/') + key).toStringList();
        return v.size() == 3 ? QColor(v[0].toInt(), v[1].toInt(), v[2].toInt()) : def;
    };
    const QColor win = col(QStringLiteral("Colors:Window"), QStringLiteral("BackgroundNormal"), QColor());
    if (!win.isValid())
        return false;
    const QColor btn = col(QStringLiteral("Colors:Button"), QStringLiteral("BackgroundNormal"), win);
    QPalette p(btn, win);
    p.setColor(QPalette::WindowText, col(QStringLiteral("Colors:Window"), QStringLiteral("ForegroundNormal"), Qt::black));
    p.setColor(QPalette::Base, col(QStringLiteral("Colors:View"), QStringLiteral("BackgroundNormal"), Qt::white));
    p.setColor(QPalette::AlternateBase, col(QStringLiteral("Colors:View"), QStringLiteral("BackgroundAlternate"), Qt::white));
    p.setColor(QPalette::Text, col(QStringLiteral("Colors:View"), QStringLiteral("ForegroundNormal"), Qt::black));
    p.setColor(QPalette::PlaceholderText, col(QStringLiteral("Colors:View"), QStringLiteral("ForegroundInactive"), Qt::gray));
    p.setColor(QPalette::ButtonText, col(QStringLiteral("Colors:Button"), QStringLiteral("ForegroundNormal"), Qt::black));
    p.setColor(QPalette::Highlight, col(QStringLiteral("Colors:Selection"), QStringLiteral("BackgroundNormal"), Qt::blue));
    p.setColor(QPalette::HighlightedText, col(QStringLiteral("Colors:Selection"), QStringLiteral("ForegroundNormal"), Qt::white));
    QApplication::setPalette(p);
    QIcon::setThemeName(win.lightness() < 128 ? QStringLiteral("breeze-dark") : QStringLiteral("breeze"));
    return true;
}

// Dymek z temperaturą z danymi testowymi: a = norma, b = blisko progu, c = ostrzeżenie, d = LACT niedostępny,
// e = norma bez dodatkowych danych.
int screenshotTemp(const QString &file, const QString &variant)
{
    TempCard c;
    c.temps.card = QStringLiteral("RX 9070 XT");
    c.temps.powerW = 212.4;
    c.temps.fanRpm = 1450;
    c.temps.busyPercent = 97;
    c.summary = QStringLiteral("test");
    if (variant == QLatin1String("a")) {
        c.temps.edge = 45;
        c.temps.hotspot = 58;
        c.temps.powerW = 38;
        c.temps.fanRpm.reset();
        c.temps.busyPercent = 12;
    } else if (variant == QLatin1String("b")) {
        c.temps.edge = 80;
        c.temps.hotspot = 95;
    } else if (variant == QLatin1String("c")) {
        c.kind = TempCard::Warning;
        c.temps.edge = 89;
        c.temps.hotspot = 104;
        c.edgeOver = c.hotspotOver = true;
    } else if (variant == QLatin1String("e")) {
        c.temps = GpuTemps{62, 78};
    } else {
        c.kind = TempCard::Unavailable;
    }
    AlertBubble bubble;
    bubble.setCard(c);
    bubble.setTail(bubble.width() / 2.0);
    return saveShot(&bubble, file) ? 0 : 1;
}

int screenshotChat(const QString &file)
{
    ChatDialog dlg;
    dlg.resize(420, 480);
    dlg.show();
    dlg.appendMessage(MessageBubble::User, QStringLiteral("Cześć! Co dziś robisz na moim pulpicie?"));
    dlg.appendMessage(MessageBubble::Assistant,
                      QStringLiteral("Cześć! Siedzę sobie nad panelem, machamy porem i pilnuję, żeby "
                                     "muzyka nie grała za głośno. A Ty nad czym pracujesz?"));
    dlg.appendMessage(MessageBubble::User, QStringLiteral("Piszę maskotkę w C++.\nZ Qt6!"));
    dlg.appendMessage(MessageBubble::System,
                      QStringLiteral("⚠ Brak klucza API - wpisz go do ~/.config/desktop-pet/api_key."));
    dlg.setTyping(true);
    QApplication::processEvents();
    const QImage img = dlg.grab().toImage();
    return img.save(file) ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    // Pomocnik wyszukiwania (proces potomny maskotki): bez blokady instancji i bez okien.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--search-helper") == 0) {
            installCrashHandlers();
            QApplication helperApp(argc, argv);
            QApplication::setQuitOnLastWindowClosed(false);
            QCoreApplication::setQuitLockEnabled(false);       // KJob/KIO nie mogą zakończyć pomocnika
            helperApp.setApplicationName(QStringLiteral("desktop-pet"));
            return runSearchHelper();
        }
    }
    // Ukryte opcje do zrzutów ekranu (zwykle z QT_QPA_PLATFORM=offscreen); bez blokady instancji.
    QString shotBubble, shotChat, shotTemp, tempVariant = QStringLiteral("a"), scheme;
    bool dark = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a = argv[i];
        if (a == "--screenshot-bubble" && i + 1 < argc)
            shotBubble = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--screenshot-chat" && i + 1 < argc)
            shotChat = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--screenshot-temp" && i + 1 < argc)
            shotTemp = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--temp-variant" && i + 1 < argc)
            tempVariant = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--scheme" && i + 1 < argc)
            scheme = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--dark")
            dark = true;
    }

    int lockFd = -1;
    if (shotBubble.isEmpty() && shotChat.isEmpty() && shotTemp.isEmpty()) {
        lockFd = singleInstanceLock();
        if (lockFd == -1) {
            std::fprintf(stderr, "desktop-pet: już działa\n");
            return 0;
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("desktop-pet"));
    Pet::setupApplication();
    if (dark)
        applyDarkPalette();
    if (!scheme.isEmpty() && !applyColorScheme(scheme))
        std::fprintf(stderr, "desktop-pet: nie wczytano schematu kolorów\n");

    if (!shotBubble.isEmpty())
        return screenshotBubble(shotBubble);
    if (!shotChat.isEmpty())
        return screenshotChat(shotChat);
    if (!shotTemp.isEmpty())
        return screenshotTemp(shotTemp, tempVariant);

    installSignalHandlers();
    Pet pet;
    pet.show();
    const int rc = app.exec();
    std::fprintf(stderr, "desktop-pet: pętla zdarzeń zakończona (kod %d)\n", rc);
    cfg::flushSettings();            // dokończ zapis ustawień z tła
    if (lockFd >= 0)
        ::close(lockFd);
    return rc;
}
