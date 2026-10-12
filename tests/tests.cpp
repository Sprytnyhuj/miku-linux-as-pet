// Testy jednostkowe (QtTest): VDF, biblioteka Steama, ranking MPRIS,
// dopasowanie strumieni głośności, ustawienia.
#include "bubbles.h"
#include "gpualert.h"
#include "config.h"
#include "mpris.h"
#include "pet.h"
#include "search.h"
#include "searchhelper.h"
#include "searchbubble.h"
#include "reactions.h"
#include "sprites.h"
#include "steam.h"
#include "vdf.h"
#include "volume.h"

#include <QDialog>
#include <QSpinBox>
#include <QDir>
#include <csignal>
#include <QElapsedTimer>
#include <QMenu>
#include <QSignalSpy>
#include <KRunner/QueryMatch>
#include <KRunner/RunnerManager>
#include <KSycoca>
#include <QWheelEvent>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(text.toUtf8());
}

QString acf(int appid, const QString &name, int flags, qint64 lastPlayed = 0)
{
    return QStringLiteral("\"AppState\"\n{\n\t\"appid\"\t\t\"%1\"\n\t\"name\"\t\t\"%2\"\n"
                          "\t\"StateFlags\"\t\t\"%3\"\n\t\"LastPlayed\"\t\t\"%4\"\n}\n")
        .arg(appid).arg(name).arg(flags).arg(lastPlayed);
}

PlayerInfo player(const QString &n, const QString &status, const QString &title = {}, const QString &url = {})
{
    PlayerInfo i;
    i.name = n;
    i.status = status;
    i.title = title;
    i.url = url;
    return i;
}


// Środowisko dla testów postaci: własny HOME i XDG_CONFIG_HOME (prawdziwe sprite'y podlinkowane),
// fałszywe xprop/pactl/xdg-mime na początku PATH (wolne - udowadniają, że nic nie czeka na procesy)
// i fałszywa biblioteka Steama (150 gier z ikonkami).
struct PetEnv {
    QTemporaryDir tmp;
    QByteArray oldHome, oldCfg, oldPath;
    QString pactlLog;

    static void script(const QString &path, const QString &body)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(("#!/bin/sh\n" + body).toUtf8());
        f.close();
        f.setPermissions(QFile::permissions(path) | QFile::ExeUser);
    }

    PetEnv()
    {
        static const QString realSprites = QDir::homePath() + QStringLiteral("/.config/desktop-pet/sprites");
        static const QString realCfgSprites = cfg::spritesDir();
        oldHome = qgetenv("HOME");
        oldCfg = qgetenv("XDG_CONFIG_HOME");
        oldPath = qgetenv("PATH");
        const QString src = QFileInfo::exists(realCfgSprites) ? realCfgSprites : realSprites;
        QDir().mkpath(tmp.path() + QStringLiteral("/cfg/desktop-pet"));
        if (QFileInfo::exists(src))
            QFile::link(src, tmp.path() + QStringLiteral("/cfg/desktop-pet/sprites"));
        qputenv("HOME", tmp.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", (tmp.path() + QStringLiteral("/cfg")).toUtf8());
        const QString bin = tmp.path() + QStringLiteral("/bin");
        QDir().mkpath(bin);
        pactlLog = tmp.path() + QStringLiteral("/pactl.log");
        script(bin + QStringLiteral("/xprop"),
               QStringLiteral("sleep 1\necho '_NET_WORKAREA(CARDINAL) = 0, 0, 1280, 700'\n"));
        script(bin + QStringLiteral("/xdg-mime"), QStringLiteral("sleep 1\necho steam.desktop\n"));
        script(bin + QStringLiteral("/pactl"),
               QStringLiteral("if [ \"$3\" = list ]; then sleep 0.5; echo '[{\"index\":7,\"mute\":false,\"volume\":"
                              "{\"front-left\":{\"value\":1,\"value_percent\":\"50%\"}},\"properties\":"
                              "{\"application.name\":\"Chromium\"}}]'; else echo \"$@\" >> %1; fi\n").arg(pactlLog));
        qputenv("PATH", (bin + QLatin1Char(':')).toUtf8() + oldPath);
        // Fałszywy Steam w $HOME/.local/share/Steam.
        const QString steam = tmp.path() + QStringLiteral("/.local/share/Steam");
        QDir().mkpath(steam + QStringLiteral("/steamapps"));
        writeFile(steam + QStringLiteral("/steamapps/libraryfolders.vdf"), QStringLiteral("\"libraryfolders\"\n{\n}\n"));
        for (int i = 1; i <= 150; ++i) {
            writeFile(steam + QStringLiteral("/steamapps/appmanifest_%1.acf").arg(1000 + i),
                      acf(1000 + i, QStringLiteral("Game %1").arg(i), 4, i));
            const QString dir = steam + QStringLiteral("/appcache/librarycache/%1").arg(1000 + i);
            QDir().mkpath(dir);
            QImage im(184, 184, QImage::Format_RGB32);
            im.fill(QColor(i, 100, 200));
            im.save(dir + QStringLiteral("/%1%2.jpg").arg(QString(39, QLatin1Char('a'))).arg(i % 10));
        }
    }
    ~PetEnv()
    {
        cfg::flushSettings();
        qputenv("HOME", oldHome);
        if (oldCfg.isEmpty())
            qunsetenv("XDG_CONFIG_HOME");
        else
            qputenv("XDG_CONFIG_HOME", oldCfg);
        qputenv("PATH", oldPath);
    }
};


// Atrapa silnika wyszukiwania: odpowiada asynchronicznie (po 10 ms), niczego nie uruchamia.
// Zapytania zaczynające się od "zzz" nie mają wyników.
class FakeEngine : public SearchEngine {
public:
    QStringList queries;
    QList<int> runs;
    int begins = 0, ends = 0;
    void begin() override { ++begins; }
    void end() override { ++ends; }
    void query(const QString &text) override
    {
        queries << text;
        QList<SearchResult> out;
        if (!text.isEmpty() && !text.startsWith(QLatin1String("zzz")))
            for (const char *t : {"Alpha", "Beta", "Gamma"})
                out.append({QString::fromLatin1(t) + QLatin1Char(' ') + text, QStringLiteral("opis"), QStringLiteral("Aplikacje"), QIcon(), QString()});
        QTimer::singleShot(10, this, [this, text, out] {
            emit resultsChanged(text, out);
            emit finished(text);
        });
    }
    bool run(int index) override { runs << index; return true; }
};


// Podwójny klik tak, jak dostarcza go system: Press, Release, DblClick, Release
// (QTest::mouseDClick dla widżetów wysyła samo DblClick).
void dclick(QWidget &w, QPoint pos = QPoint(20, 20))
{
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseDClick(&w, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, pos);
}
template <class F> double timedMs(F &&f)
{
    QElapsedTimer t;
    t.start();
    f();
    return t.nsecsElapsed() / 1e6;
}

// Prawdziwe odpowiedzi LACT 0.10.1 z hosta (device_stats skrócone o pola nieistotne dla parsera).
const char LACT_LIST[] = R"JSON({"status":"ok","data":[{"id":"1002:164E-1458:D000-0000:12:00.0","name":"Raphael","device_type":"Integrated"},{"id":"1002:7550-1458:2437-0000:03:00.0","name":"AMD Radeon RX 9070 XT","device_type":"Dedicated"}]})JSON";
const char LACT_STATS[] = R"JSON({"status":"ok","data":{"fan":{"control_enabled":true,"control_mode":"curve","static_speed":0.42,"curve":{"40":0.3,"90":0.7},"pwm_current":76,"speed_current":1472,"speed_max":3200,"temperature_range":[25,100],"temperature_key":"edge"},"clockspeed":{"gpu_clockspeed":867,"vram_clockspeed":772},"vram":{"total":17095983104,"used":1701687296},"power":{"average":25.0,"cap_current":330.0},"temps":{"edge":{"current":29.0,"crit":110.0,"crit_hyst":-273.15,"primary":true,"display_only":false},"junction":{"current":31.0,"crit":110.0,"crit_hyst":-273.15,"primary":true,"display_only":false},"mem":{"current":54.0,"crit":108.0,"crit_hyst":-273.15,"primary":true,"display_only":false},"vrgfx":{"current":36.0,"crit":null,"crit_hyst":null,"primary":false,"display_only":true}},"busy_percent":6,"performance_level":"auto","throttle_info":{}}})JSON";
const char LACT_ID[] = "1002:7550-1458:2437-0000:03:00.0";

QAction *findAction(QMenu &m, const QString &text)
{
    for (QAction *a : m.actions())
        if (a->text() == text)
            return a;
    return nullptr;
}

QString none()
{
    static QTemporaryDir d;
    return d.filePath(QStringLiteral("brak.sock"));
}

QByteArray lactStats(double edge, double junction)
{
    return QByteArray("{\"status\":\"ok\",\"data\":{\"temps\":{\"edge\":{\"current\":") + QByteArray::number(edge)
        + ",\"crit\":110.0},\"junction\":{\"current\":" + QByteArray::number(junction) + ",\"crit\":110.0}}}}";
}

// Atrapa demona LACT na QLocalServer: zapisuje każdą komendę (test sprawdza, że są tylko odczytowe).
class FakeLact : public QObject {
public:
    QTemporaryDir dir;
    QLocalServer server;
    QStringList commands;          // nazwy komend w kolejności
    QStringList raw;               // surowe wiersze JSON, dokładnie jak przyszły
    int connections = 0;
    int open = 0;                  // aktualnie otwarte połączenia po stronie serwera
    enum Mode { Normal, Chunked, Burst, DropAfterRequest, HalfThenDrop } mode = Normal;
    bool silent = false;           // przyjmuje połączenia i nic nie odpowiada
    QByteArray stats = lactStats(62, 78);
    QByteArray list = QByteArray(LACT_LIST);

    FakeLact()
    {
        QLocalServer::removeServer(path());
        QVERIFY(server.listen(path()));
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (QLocalSocket *s = server.nextPendingConnection()) {
                ++connections;
                ++open;
                QObject::connect(s, &QLocalSocket::disconnected, s, [this] { --open; });
                QObject::connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QLocalSocket::readyRead, s, [this, s] {
                    while (s->canReadLine()) {
                        const QByteArray rawLine = s->readLine();
                        raw << QString::fromUtf8(rawLine.trimmed());
                        const QJsonObject req = QJsonDocument::fromJson(rawLine).object();
                        const QString c = req.value(QStringLiteral("command")).toString();
                        commands << c;
                        if (silent)
                            continue;
                        QByteArray resp;
                        if (c == QLatin1String("list_devices"))
                            resp = list + '\n';
                        else if (c == QLatin1String("device_stats"))
                            resp = stats + '\n';
                        else
                            resp = "{\"status\":\"error\",\"data\":{\"description\":\"nie\"}}\n";
                        reply(s, resp);
                    }
                });
            }
        });
    }
    void reply(QLocalSocket *s, const QByteArray &resp)
    {
        QPointer<QLocalSocket> sp(s);
        switch (mode) {
        case Normal:
            s->write(resp);
            break;
        case Burst:                // kilka odpowiedzi (i pusty wiersz) w jednym bloku
            s->write("\n" + resp + resp + "{\"status\":\"error\"}\n");
            break;
        case Chunked:              // odpowiedź w kawałkach po 7 bajtów, z przerwami
            for (int i = 0, n = 0; i < resp.size(); i += 7, ++n) {
                const QByteArray part = resp.mid(i, 7);
                QTimer::singleShot(n * 3, s, [sp, part] {
                    if (sp)
                        sp->write(part);
                });
            }
            break;
        case DropAfterRequest:
            s->disconnectFromServer();
            break;
        case HalfThenDrop:
            s->write(resp.left(resp.size() / 2));
            s->flush();
            s->disconnectFromServer();
            break;
        }
    }
    QString path() const { return dir.filePath(QStringLiteral("lactd.sock")); }
    // Surowe wiersze: wyłącznie {"command":"list_devices"} albo {"args":{"id":<ID>},"command":"device_stats"}.
    bool rawIsExactlyReadOnly() const
    {
        const QString list = QStringLiteral("{\"command\":\"list_devices\"}");
        const QString stats = QStringLiteral("{\"args\":{\"id\":\"") + QLatin1String(LACT_ID) + QStringLiteral("\"},\"command\":\"device_stats\"}");
        for (const QString &r : raw)
            if (r != list && r != stats)
                return false;
        return true;
    }
    bool onlyReadCommands() const
    {
        for (const QString &c : commands)
            if (c != QLatin1String("list_devices") && c != QLatin1String("device_stats"))
                return false;
        return true;
    }
};

}  // namespace

struct PetProbe {
    Pet &p;
    bool fall() const { return p.m_state == Pet::State::Fall; }
    bool floor() const { return p.m_state == Pet::State::Floor; }
    bool drag() const { return p.m_state == Pet::State::Drag; }
    QString anim() const { return p.m_anim; }
    bool timerActive() const { return p.m_timer->isActive(); }
    int timerInterval() const { return p.m_timer->interval(); }
    double y() const { return p.m_pos.y(); }
    double vy() const { return p.m_vy; }
    size_t reactions() const { return p.m_reactions.size(); }
    double floorY() const { return p.floorY(p.area(p.m_pos + QPointF(p.m_bodyW / 2.0, p.height() / 2.0))); }
    void step() { p.step(); }
    Mpris &mpris() { return p.m_mpris; }
    MusicBubble *bubble() { return p.m_bubble; }
    SearchBubble *search() { return p.m_search; }
    bool clickTimerActive() const { return p.m_clickTimer->isActive(); }
    SteamCache &steam() { return p.m_steam; }
    PlayerVolume &volume() { return p.m_volume; }
    TrackWatcher &watcher() { return p.m_watcher; }
    const std::vector<std::pair<QString, int>> &queue() const { return p.m_reactions; }
    void pushReaction(const QString &n, int t) { p.m_reactions.push_back({n, t}); }
    QString mode() const { return p.m_mode; }
    QString activity() const { return p.m_activity; }
    void setActivity(const QString &a, int left) { p.m_activity = a; p.m_activityLeft = left; }
    void buildMenu(QMenu &m) { p.buildMenu(m); }
    AlertBubble *alert() { return p.m_alert; }
    AlertBubble *info() { return p.m_info; }
    void showInfoNow(const QString &t) { p.showTemperature(t); }
    LactClient &lact() { return p.m_lact; }
    void gpuReading(double edge, double hot) { p.onGpuReading(GpuTemps{edge, hot}); }
    QString gpuPending() const { return p.m_gpuPending; }
    GpuAlertConfig gpuCfg() const { return p.m_gpu; }
    const GpuAlertLogic &gpuLogic() const { return p.m_gpuLogic; }
    void setStateDrag() { p.m_state = Pet::State::Drag; }
    void setStateFloor() { p.m_state = Pet::State::Floor; }
    void refreshWorkarea() { p.refreshWorkarea(); }
    std::optional<QRect> workarea() const { return p.m_workarea; }
    int frameIndex() const
    {
        const Anim &an = p.m_sprites.anim(p.m_anim);
        return (p.m_animTick / an.ticks) % int(an.frames.size());
    }
    int frameCount() const { return int(p.m_sprites.anim(p.m_anim).frames.size()); }
    bool mirroredCached() const
    {
        for (const Anim &a : std::as_const(p.m_sprites.anims))
            if (a.mirrored.size() != a.frames.size())
                return false;
        return true;
    }
    double x() const { return p.m_pos.x(); }
    int dir() const { return p.m_dir; }
    void setX(double x) { p.m_pos.setX(x); }
    int bodyW() const { return p.m_bodyW; }
    QRect areaNow() const { return p.area(p.m_pos + QPointF(p.m_bodyW / 2.0, p.height() / 2.0)); }
    double centerX() const { const QRect a = areaNow(); return a.center().x() - p.m_bodyW / 2.0; }
    void returnToCenter() { p.returnToCenter(); }
    void savePosition() { p.savePosition(); }
    void pick() { p.pickActivity(); }
    void clamp() { p.clampToWorkarea(); }
    int animTick() const { return p.m_animTick; }
    void setAnimNow(const QString &n) { p.setAnim(n); }
    void react(const QString &n, int t) { p.react({{n, t}}, "test"); }
    void trackPolled(const QString &key, bool playing) { p.onTrackPolled(key, playing); }
    void setDir(int d) { p.m_dir = d; }
    void paintOnce() { QPixmap pm(p.size()); p.render(&pm); }
    void setMode(const QString &m) { p.setMode(m); }
    void startFall(double above)
    {
        p.m_state = Pet::State::Fall;
        p.m_vy = 0;
        p.m_pos.setY(floorY() - above);
        p.m_reactions.clear();
    }
};

class Tests : public QObject {
    Q_OBJECT
private slots:
    // Regresja: postać utknęła w spadaniu (timer nigdy nie wystartował, brak grawitacji).
    void petFallsAndLands()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        struct Flush { ~Flush() { cfg::flushSettings(); } } flushAtEnd;     // zapis w tle przed usunięciem katalogu
        const QString real = QDir::homePath() + QStringLiteral("/.config/desktop-pet/sprites");
        QDir().mkpath(tmp.path() + QStringLiteral("/desktop-pet"));
        if (QFileInfo::exists(real))
            QVERIFY(QFile::link(real, tmp.path() + QStringLiteral("/desktop-pet/sprites")));
        qputenv("XDG_CONFIG_HOME", tmp.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        QVERIFY2(pr.timerActive(), "timer ticków nie działa");
        QCOMPARE(pr.timerInterval(), cfg::TICK_MS);
        QVERIFY(pr.floor());

        // Mysz: wciśnięcie, ruch, puszczenie -> spadanie, y rośnie, ląduje.
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&pet, QPoint(120, -300));
        QVERIFY(pr.drag());
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(120, -300));
        QVERIFY(pr.fall());
        pr.step();
        QVERIFY(pr.vy() > 0);
        // Prawdziwy timer: bez ręcznego step() stan się zmienia.
        const double y0 = pr.y();
        QTRY_VERIFY_WITH_TIMEOUT(pr.y() > y0 || pr.floor(), 1000);

        const QStringList keys = [] { QStringList k; for (const Mode &m : modes()) k << QString::fromLatin1(m.key); return k; }();
        QVERIFY(keys.contains(QStringLiteral("happy")) && keys.contains(QStringLiteral("dance")));
        for (const QString &mode : keys) {
            for (const double above : {6.0, 400.0}) {            // miękki (vy >= 4) i twardy (vy > 12) upadek
                for (int rep = 0; rep < 6; ++rep) {
                    const QByteArray tag = (mode + QStringLiteral(" h=%1 #%2").arg(above).arg(rep)).toUtf8();
                    pr.setMode(mode);
                    pr.startFall(above);
                    int ticks = 0;
                    while (pr.fall() && ticks < 200) { pr.step(); ++ticks; }
                    QVERIFY2(pr.floor(), tag.constData());
                    QCOMPARE(pr.y(), pr.floorY());
                    // Po lądowaniu: kolejka reakcji się opróżnia, animacja nie jest "fall".
                    for (int i = 0; i < 400; ++i) {
                        pr.step();
                        QVERIFY2(pr.floor() && pr.anim() != QLatin1String("fall"), tag.constData());
                    }
                    QCOMPARE(pr.reactions(), size_t(0));
                    QCOMPARE(pr.y(), pr.floorY());
                }
            }
        }
    }

    // ---------------------------------------------------------- lagi: przestoje logiczne
    // Zmiana trybu (także przez prawdziwą akcję menu): w następnym ticku animacja = główna czynność
    // nowego trybu, kolejka reakcji wyczyszczona, nawet w środku starej reakcji i przy losowym postoju.
    void modeChangeStartsMainActivityAtOnce()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        QVERIFY(pr.floor());
        for (const Mode &m : modes()) {
            const QString key = QString::fromLatin1(m.key), main = QString::fromLatin1(m.activities.front().name);
            for (const QString &stale : {QStringLiteral("grumpy"), QStringLiteral("surprised"), QStringLiteral("shy")}) {
                const QByteArray tag = (key + QLatin1Char('/') + stale).toUtf8();
                pr.setMode(key == QLatin1String("sit") ? QStringLiteral("lie") : QStringLiteral("sit"));
                pr.pushReaction(stale, 50);
                pr.pushReaction(QStringLiteral("land"), 50);
                pr.setActivity(QStringLiteral("stand"), 1);          // stary tryb miał właśnie wylosować postój
                // Prawdziwa akcja z menu "Co ma robić?".
                QMenu menu;
                pr.buildMenu(menu);
                QAction *sub = nullptr;
                for (QAction *a : menu.actions())
                    if (a->menu() && a->text() == QStringLiteral("Co ma robić?"))
                        sub = a;
                QVERIFY2(sub, tag.constData());
                QAction *pick = nullptr;
                for (QAction *a : sub->menu()->actions())
                    if (a->text() == QString::fromUtf8(m.label))
                        pick = a;
                QVERIFY2(pick, tag.constData());
                pick->trigger();
                QCOMPARE(pr.mode(), key);
                QCOMPARE(pr.reactions(), size_t(0));
                pr.step();
                QVERIFY2(pr.anim() == main, tag.constData());
                // Dalej też główna czynność (aktywność ma co najmniej lo ticków).
                for (int i = 0; i < m.activities.front().lo - 2; ++i) {
                    pr.step();
                    QVERIFY2(pr.anim() == main, tag.constData());
                }
            }
        }
        cfg::flushSettings();
        QCOMPARE(cfg::loadSettings().value(QStringLiteral("mode")).toString(), QStringLiteral("dance"));   // ostatni tryb zapisany
    }

    // 5 szybkich rzutów (press -> drag -> release) w ciągu ~1 s, w każdym trybie: po ostatnim lądowaniu
    // główna czynność rusza najpóźniej 1 tick po końcu reakcji, a krótkie lądowanie trwa <= 0,3 s.
    void rapidThrowsResumeMainActivity()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        const Sprites sprites = loadSprites();
        for (const Mode &m : modes()) {
            const QString key = QString::fromLatin1(m.key), main = QString::fromLatin1(m.activities.front().name);
            for (const int height : {-300, 20, -900}) {       // y myszy w trakcie rzutu: wysoko / nisko / bardzo wysoko
                const QByteArray tag = (key + QStringLiteral(" y=%1").arg(height)).toUtf8();
                pr.setMode(key);
                pr.startFall(0);
                for (int i = 0; i < 3; ++i)
                    pr.step();
                QVERIFY2(pr.floor(), tag.constData());
                for (int throwNo = 0; throwNo < 5; ++throwNo) {
                    QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
                    QTest::mouseMove(&pet, QPoint(60 + throwNo * 30, height));
                    QVERIFY2(pr.drag(), tag.constData());
                    pr.step();                                   // przeciąganie
                    QCOMPARE(pr.anim(), QStringLiteral("drag"));
                    QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(60 + throwNo * 30, height));
                    QVERIFY2(pr.fall(), tag.constData());
                    QCOMPARE(pr.reactions(), size_t(0));
                    if (throwNo < 4) {
                        for (int i = 0; i < 4 + throwNo; ++i)    // chwila lotu, potem złapana w locie
                            pr.step();
                        QVERIFY2(pr.fall() || pr.floor(), tag.constData());
                        if (pr.fall()) {
                            // Złapanie bez ruchu (klik w locie) nie otwiera dymka ani nie odpala reakcji.
                            QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
                            QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
                            QVERIFY2(!pr.bubble()->isVisible(), tag.constData());
                            QCOMPARE(pr.reactions(), size_t(0));
                        }
                    }
                }
                int ticks = 0;
                while (pr.fall() && ticks < 300) { pr.step(); ++ticks; }
                QVERIFY2(pr.floor(), tag.constData());
                // Reakcja po lądowaniu: suma ticków w kolejce.
                int reactionTicks = 0;
                for (const auto &r : pr.queue())
                    reactionTicks += r.second;
                const bool hard = height != 20 ? true : false;
                if (!hard || key == QLatin1String("leek"))
                    QVERIFY2(reactionTicks <= (key == QLatin1String("leek") && hard ? 8 : SOFT_LAND_TICKS), tag.constData());
                if (key == QLatin1String("dance") && !hard)
                    QCOMPARE(reactionTicks, 0);
                int t = 0;
                while (pr.anim() != main && t < 200) { pr.step(); ++t; }
                QVERIFY2(t <= reactionTicks + 1, qPrintable(QString::fromUtf8(tag) + QStringLiteral(" t=%1 r=%2").arg(t).arg(reactionTicks)));
                // Klatki główną czynnością idą dalej (brak "zawieszenia" na pierwszej klatce).
                QSet<int> seen;
                for (int i = 0; i < 400 && pr.anim() == main; ++i) {
                    seen.insert(pr.frameIndex());
                    pr.step();
                }
                const Anim &an = sprites.anim(main);
                if (an.frames.size() > 1 && m.activities.front().lo >= int(an.frames.size()) * an.ticks)
                    QVERIFY2(seen.size() > 1, tag.constData());
            }
        }
    }

    // Miękkie lądowanie z kilkunastu px nie daje reakcji; wyraźny upadek (vy >= 8) - krótką (<= 0,3 s).
    void softLandingIsShortOrNone()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        for (const Mode &m : modes()) {
            const QString key = QString::fromLatin1(m.key);
            pr.setMode(key);
            pr.startFall(15);                 // ok. 15 px: vy ~4,8
            int n = 0;
            while (pr.fall() && n++ < 100) pr.step();
            QVERIFY2(pr.floor() && pr.reactions() == 0, qPrintable(key));
            for (int rep = 0; rep < 12; ++rep) {
                pr.setMode(key);
                pr.startFall(40);             // ok. 40 px: vy ~8,4 - wyraźny, ale nie "twardy"
                n = 0;
                while (pr.fall() && n++ < 100) pr.step();
                QVERIFY(pr.floor());
                int total = 0;
                for (const auto &r : pr.queue())
                    total += r.second;
                QVERIFY2(total <= SOFT_LAND_TICKS, qPrintable(key));
                if (key == QLatin1String("leek") || key == QLatin1String("dance"))
                    QCOMPARE(total, 0);
            }
        }
    }

    // Przejścia między aktywnościami: poboczne "stand" w leek/dance krótkie i nigdy dwa razy z rzędu.
    void noUnnaturallyLongStandstill()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        for (const QString &key : {QStringLiteral("leek"), QStringLiteral("dance"), QStringLiteral("walk")}) {
            pr.setMode(key);
            const QString main = QString::fromLatin1(findMode(key)->activities.front().name);
            int run = 0, longest = 0;
            for (int i = 0; i < 40000; ++i) {
                pr.step();
                if (pr.anim() == QLatin1String("stand")) {
                    longest = std::max(longest, ++run);
                } else {
                    run = 0;
                }
            }
            const int limit = key == QLatin1String("walk") ? 150 : 100 + 1;
            qInfo("%s: najdłuższy bezruch %d ticków (limit %d)", qPrintable(key), longest, limit);
            QVERIFY2(longest <= limit, qPrintable(key));
        }
    }

    // ---------------------------------------------------------- lagi: nic nie blokuje wątku UI
    void mirroredFramesCached()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        QVERIFY(pr.mirroredCached());
        QVERIFY(placeholderSprites().anim(QStringLiteral("walk")).mirrored.size() == 4);
        pr.setDir(1);
        pr.paintOnce();
        pr.setDir(-1);
        pr.paintOnce();
    }

    void workareaParsing()
    {
        QCOMPARE(parseWorkarea(QStringLiteral("_NET_WORKAREA(CARDINAL) = 0, 0, 1920, 1040")), std::optional<QRect>(QRect(0, 0, 1920, 1040)));
        QVERIFY(!parseWorkarea(QString()).has_value());
        QVERIFY(!parseWorkarea(QStringLiteral("xprop: unable to open display")).has_value());
    }

    void volumeAsyncNoBlocking()
    {
        PetEnv env;
        PlayerVolume vol;
        PlayerInfo pi = player(QStringLiteral("org.mpris.MediaPlayer2.chromium.instance1"), QStringLiteral("Playing"));
        pi.identity = QStringLiteral("Chromium");
        QSignalSpy spy(&vol, &PlayerVolume::changed);
        QVERIFY(!vol.get(pi).has_value());
        const double refreshMs = timedMs([&] { vol.refresh(pi); });             // pactl śpi 0,5 s
        QVERIFY2(refreshMs < 100, "refresh blokuje");
        // Zmiana przed pierwszym odczytem: nic nie blokuje, wartość nieznana, zlecona po odczycie.
        double adjustMs = timedMs([&] { QVERIFY(!vol.adjust(pi, 5).has_value()); });
        QVERIFY2(adjustMs < 100, "adjust blokuje");
        QVERIFY(spy.wait(5000));
        QVERIFY(vol.get(pi).has_value());
        QCOMPARE(vol.get(pi)->percent, 55);                      // 50% z pactl + 5 z kółka
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(env.pactlLog).size() > 0, 3000);
        QFile logFile(env.pactlLog);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(logFile.readAll())
                    .contains(QStringLiteral("set-sink-input-volume 7 55%")));
        // Kolejne kółko: od ostatniej znanej wartości, od razu.
        std::optional<VolumeState> s;
        const double wheelMs = timedMs([&] { s = vol.adjust(pi, 5); });     // + startDetached pactl set-sink-input-volume
        qInfo("głośność: refresh() %.2f ms, adjust przed odczytem %.2f ms, kółko po odczycie %.2f ms", refreshMs, adjustMs, wheelMs);
        QVERIFY2(wheelMs < 100, "adjust blokuje");
        QVERIFY(s && s->percent == 60);
        QVERIFY2(timedMs([&] { vol.adjust(pi, -5); vol.adjust(pi, -5); vol.set(pi, 30); }) < 100, "set blokuje");
        QCOMPARE(vol.get(pi)->percent, 30);
        QTest::qWait(300);                                          // dokończenie pactl set-sink-input-volume (startDetached)
    }

    // Szybkie kręcenie kółkiem: jedna seria pactl naraz, w locie tylko ostatnia wartość (bez zalewu procesami).
    void volumeCoalescesRapidWheel()
    {
        PetEnv env;
        PlayerVolume vol;
        PlayerInfo pi = player(QStringLiteral("org.mpris.MediaPlayer2.chromium.instance1"), QStringLiteral("Playing"));
        pi.identity = QStringLiteral("Chromium");
        QSignalSpy spy(&vol, &PlayerVolume::changed);
        vol.refresh(pi);
        QVERIFY(spy.wait(5000));
        QCOMPARE(vol.get(pi)->percent, 50);
        for (int i = 0; i < 30; ++i)
            vol.adjust(pi, 1);                                  // 30 kroków naraz, od ostatniej znanej wartości
        QCOMPARE(vol.get(pi)->percent, 80);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(env.pactlLog).size() > 0, 3000);
        QString log;
        auto readLog = [&] {
            QFile f(env.pactlLog);
            if (f.open(QIODevice::ReadOnly))
                log = QString::fromUtf8(f.readAll());
            return log;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readLog().contains(QStringLiteral("set-sink-input-volume 7 80%")), 5000);
        QTest::qWait(200);
        readLog();
        QVERIFY2(log.count(QLatin1Char('\n')) <= 3, qPrintable(log));    // pierwsza + ostatnia wartość, nie 30 procesów
        QVERIFY(log.trimmed().endsWith(QStringLiteral("80%")));
    }

    // Pierwsze menu tuż po starcie: lista gier jeszcze się wczytuje w tle - pozycja "Wczytywanie…", potem gry.
    void firstMenuShowsLoadingEntry()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        auto gamesTexts = [&] {
            QMenu menu;
            pr.buildMenu(menu);
            QStringList out;
            for (QAction *a : menu.actions()) {
                if (a->menu() && a->text() == QStringLiteral("Gry")) {
                    for (QAction *g : a->menu()->actions())
                        out << g->text();
                }
            }
            return out;
        };
        if (!pr.steam().ready())
            QVERIFY(gamesTexts().contains(QStringLiteral("Wczytywanie…")));
        QTRY_VERIFY_WITH_TIMEOUT(pr.steam().ready(), 5000);
        const QStringList after = gamesTexts();
        QVERIFY(!after.contains(QStringLiteral("Wczytywanie…")));
        QVERIFY(after.contains(QStringLiteral("Game 1")));
    }

    // Typowe operacje przy żywym timerze ticków i wolnych xprop/pactl/xdg-mime: żadna nie trwa > 15 ms,
    // a tick nie zwleka.
    void uiThreadNeverBlocks()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTest::qWait(300);                                   // start: workarea, Steam i inne odczyty w tle
        QTRY_VERIFY_WITH_TIMEOUT(pr.workarea().has_value(), 3000);   // xprop (1 s) skończony - kolejne wywołania naprawdę startują proces
        const double limit = 100.0;   // blokada na xprop/pactl/xdg-mime trwałaby >= 500 ms; zapas na wolną maszynę
        QMap<QString, double> worst;
        auto measure = [&](const QString &what, auto &&f) {
            const double ms = timedMs(f);
            worst[what] = std::max(worst.value(what), ms);
            QVERIFY2(ms < limit, qPrintable(QStringLiteral("%1: %2 ms").arg(what).arg(ms)));
        };
        // Steam: odświeżenie w tle (150 gier z ikonkami), potem menu z gotowych danych.
        measure(QStringLiteral("steam.refresh()"), [&] { pr.steam().refresh(); });
        QTRY_VERIFY_WITH_TIMEOUT(pr.steam().games().size() == 150, 5000);
        for (int round = 0; round < 3; ++round) {
            measure(QStringLiteral("buildMenu (150 gier)"), [&] {
                QMenu menu;
                pr.buildMenu(menu);
            });
            measure(QStringLiteral("workarea refresh (xprop 1 s)"), [&] { pr.refreshWorkarea(); });
            measure(QStringLiteral("odświeżenie dymka"), [&] { pr.bubble()->refresh(); });
            measure(QStringLiteral("watcher.poll()"), [&] { pr.watcher().poll(); });
            for (const Mode &m : modes())
                measure(QStringLiteral("zmiana trybu"), [&] { pr.setMode(QString::fromLatin1(m.key)); });
            for (int d : {120, -120, 240}) {                 // kółko na postaci (otwiera dymek) i na dymku
                QWheelEvent we(QPointF(20, 20), pet.mapToGlobal(QPointF(20, 20)), QPoint(), QPoint(0, d),
                               Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                measure(QStringLiteral("kółko (wheelEvent)"), [&] { QApplication::sendEvent(&pet, &we); });
            }
            measure(QStringLiteral("zmiana głośności"), [&] { pr.bubble()->changeVolume(5); });
            measure(QStringLiteral("step()"), [&] { pr.step(); });
            pr.bubble()->hide();
            QTest::qWait(250);
        }
        // Ikonki Steama: z pamięci, 150 sztuk
        measure(QStringLiteral("ikonki Steama x150"), [&] {
            for (const SteamGame &g : pr.steam().games())
                pr.steam().icon(g);
        });
        QTRY_VERIFY_WITH_TIMEOUT(pr.workarea().has_value(), 3000);
        QCOMPARE(*pr.workarea(), QRect(0, 0, 1280, 700));    // wynik z wolnego xprop doszedł asynchronicznie
        // Cały czas działał prawdziwy timer: odstępy między tickami.
        const TickStats st = pet.tickStats();
        for (auto it = worst.cbegin(); it != worst.cend(); ++it)
            qInfo("najdłużej: %-32s %.2f ms", qPrintable(it.key()), it.value());
        qInfo("ticki: %lld, najdłuższy odstęp %.1f ms (nominalnie %d), odstępów > 80 ms: %d, najdłuższe step() %.2f ms",
              static_cast<long long>(st.ticks), st.maxGapMs, cfg::TICK_MS, st.gapsOver80Ms, st.maxStepMs);
        QVERIFY(st.ticks > 20);
        QVERIFY2(st.maxStepMs < limit, "step() trwa za długo");
        QVERIFY2(st.maxGapMs < 300.0, "tick zwlekał o ponad 2 ticki");
    }

    // ---------------------------------------------------------- wyszukiwarka
    // Pojedynczy klik: dymek z muzyką dopiero po interwale podwójnego kliku; podwójny: wyszukiwarka,
    // muzyka się nie otwiera.
    void searchSingleVersusDoubleClick()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        auto *fake = new FakeEngine;
        pr.search()->setEngine(fake);
        const int interval = QApplication::doubleClickInterval();
        QVERIFY(interval > 0);
        QTest::qWait(50);
        QVERIFY(pr.floor());

        QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(pr.clickTimerActive());
        QVERIFY(!pr.bubble()->isVisible());               // jeszcze czeka na ewentualny drugi klik
        QTest::qWait(interval / 2);
        QVERIFY(!pr.bubble()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(pr.bubble()->isVisible(), interval + 1000);
        QVERIFY(!pr.search()->isVisible());
        pr.bubble()->hide();
        QTest::qWait(450);                                // justClosed() wygasa

        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QVERIFY(!pr.bubble()->isVisible());
        QVERIFY(!pr.clickTimerActive());
        QTest::qWait(interval + 300);
        QVERIFY2(!pr.bubble()->isVisible(), "podwójny klik nie może otworzyć dymka z muzyką");
        QVERIFY(pr.search()->isVisible());
        QCOMPARE(fake->begins, 1);

        // Dwa wolne kliki (poza interwałem) to dwa pojedyncze, nie podwójny.
        pr.search()->hide();
        QTest::qWait(450);
        QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTRY_VERIFY_WITH_TIMEOUT(pr.bubble()->isVisible(), interval + 1000);
        QVERIFY(!pr.search()->isVisible());
    }

    // Przeciąganie po pierwszym kliku nie otwiera żadnego dymka; drugie naciśnięcie + ruch to przeciąganie.
    void searchDoesNotBreakDrag()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        QTest::qWait(50);
        QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(pr.clickTimerActive());
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(!pr.clickTimerActive());
        QTest::mouseMove(&pet, QPoint(80, -100));
        QVERIFY(pr.drag());
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(80, -100));
        QVERIFY(pr.fall());
        QTest::qWait(600);
        QVERIFY(!pr.search()->isVisible());
        QVERIFY(!pr.bubble()->isVisible());
        // Zwykłe przeciąganie z działającym timerem pojedynczego kliku też go anuluje.
        QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
    }

    // Tylko jeden dymek nad głową: wyszukiwarka chowa muzykę i na odwrót.
    void searchAndMusicBubbleExclusive()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QWheelEvent we(QPointF(20, 20), pet.mapToGlobal(QPointF(20, 20)), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&pet, &we);               // kółko otwiera dymek z muzyką
        QVERIFY(pr.bubble()->isVisible());
        QVERIFY(!pr.search()->isVisible());
        QTest::qWait(450);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QVERIFY(!pr.bubble()->isVisible());
    }

    // Dymek stoi nad głową (ogonek do głowy), postać przy otwartej wyszukiwarce nie odchodzi.
    void searchPlacementAndStaysPut()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        pr.setMode(QStringLiteral("walk"));
        pr.setActivity(QStringLiteral("walk"), 500);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        const double x0 = pr.x();
        for (int i = 0; i < 60; ++i)
            pr.step();
        QCOMPARE(pr.x(), x0);                             // _bubble_open: nie spaceruje
        QVERIFY(pr.search()->geometry().bottom() <= pr.y() + cfg::BUBBLE_OVERLAP + Bubble::TAIL + 1);
        const QRect g = pr.search()->geometry();
        QVERIFY(g.center().x() >= pr.x() - 1 && g.left() <= pr.x() + pr.bodyW());
    }

    // Klawiatura: strzałki, Enter uruchamia zaznaczony (domyślnie pierwszy), Esc zamyka bez uruchamiania.
    void searchKeyboardNavigation()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        auto *fake = new FakeEngine;
        pr.search()->setEngine(fake);
        SearchBubble *s = pr.search();
        QSignalSpy launched(s, &SearchBubble::launched);

        dclick(pet);
        QVERIFY(s->isVisible());
        QVERIFY(s->edit()->hasFocus() || !s->isActiveWindow());   // pole tekstowe ma fokus od razu (gdy okno aktywne)
        QTest::keyClicks(s->edit(), QStringLiteral("ab"));
        QVERIFY(s->resultCount() == 0);                   // debounce: jeszcze bez zapytania
        QVERIFY(fake->queries.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 3, 2000);
        QCOMPARE(fake->queries, QStringList{QStringLiteral("ab")});   // jedno zapytanie, nie po jednym na znak
        QCOMPARE(s->currentRow(), 0);                     // domyślnie pierwszy
        QTest::keyClick(s->edit(), Qt::Key_Down);
        QTest::keyClick(s->edit(), Qt::Key_Down);
        QTest::keyClick(s->edit(), Qt::Key_Down);         // dalej niż koniec listy: zostaje na ostatnim
        QCOMPARE(s->currentRow(), 2);
        QTest::keyClick(s->edit(), Qt::Key_Up);
        QCOMPARE(s->currentRow(), 1);
        QTest::keyClick(s->edit(), Qt::Key_Return);
        QCOMPARE(fake->runs, QList<int>{1});
        QCOMPARE(launched.count(), 1);
        QVERIFY(!s->isVisible());
        QCOMPARE(fake->ends, 1);

        // Enter bez strzałek: pierwszy wynik; Enter tuż po ostatnim znaku (w trakcie debounce) też.
        QTest::qWait(450);
        pr.setMode(QStringLiteral("sit"));
        dclick(pet);
        QVERIFY(s->isVisible());
        QTest::keyClicks(s->edit(), QStringLiteral("q"));
        QTest::keyClick(s->edit(), Qt::Key_Enter);
        QTRY_COMPARE_WITH_TIMEOUT(fake->runs.size(), 2, 2000);
        QCOMPARE(fake->runs.last(), 0);
        QVERIFY(!s->isVisible());

        // Esc: zamyka, nic nie uruchamia.
        QTest::qWait(450);
        dclick(pet);
        QTest::keyClicks(s->edit(), QStringLiteral("x"));
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 3, 2000);
        QTest::keyClick(s->edit(), Qt::Key_Escape);
        QVERIFY(!s->isVisible());
        QCOMPARE(fake->runs.size(), 2);
        QCOMPARE(launched.count(), 2);

        // Enter na pustej liście nic nie robi.
        QTest::qWait(450);
        dclick(pet);
        QTest::keyClick(s->edit(), Qt::Key_Return);
        QVERIFY(s->isVisible());
        QCOMPARE(fake->runs.size(), 2);
        s->hide();
    }

    // Klik w wynik uruchamia go; max 8 wierszy widocznych.
    void searchClickLaunchesAndRowLimit()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        auto *fake = new FakeEngine;
        pr.search()->setEngine(fake);
        SearchBubble *s = pr.search();
        dclick(pet);
        QTest::keyClicks(s->edit(), QStringLiteral("k"));
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 3, 2000);
        QListWidget *list = s->findChild<QListWidget *>();
        QVERIFY(list && list->isVisible());
        QCOMPARE(list->height(), 3 * SearchBubble::ROW_H + 2);
        const QRect r = list->visualItemRect(list->item(2));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
        QCOMPARE(fake->runs, QList<int>{2});
        QVERIFY(!s->isVisible());
        // Wiele wyników: lista nie rośnie ponad 8 wierszy.
        struct Many : SearchEngine {
            void query(const QString &t) override
            {
                QList<SearchResult> out;
                for (int i = 0; i < 30; ++i)
                    out.append({QStringLiteral("Wynik %1").arg(i), QString(), QString(), QIcon(), QString()});
                QTimer::singleShot(0, this, [this, t, out] { emit resultsChanged(t, out); emit finished(t); });
            }
            bool run(int) override { return true; }
        };
        s->setEngine(new Many);
        QTest::qWait(450);
        dclick(pet);
        QTest::keyClicks(s->edit(), QStringLiteral("m"));
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 30, 2000);
        QCOMPARE(list->height(), SearchBubble::VISIBLE_ROWS * SearchBubble::ROW_H + 2);
        s->hide();
    }

    // Animacje: ponder przy otwartej, cheer po uruchomieniu, shy przy braku wyników (dopiero po ustabilizowaniu),
    // powrót do trybu po zamknięciu bez uruchomienia.
    void searchAnimations()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        auto *fake = new FakeEngine;
        pr.search()->setEngine(fake);
        SearchBubble *s = pr.search();
        pr.setMode(QStringLiteral("sit"));
        const QString main = QStringLiteral("sit");
        for (int i = 0; i < 5; ++i)
            pr.step();
        QCOMPARE(pr.anim(), main);

        dclick(pet);
        QVERIFY(s->isVisible());
        for (int i = 0; i < 5; ++i) {
            pr.step();
            QCOMPARE(pr.anim(), QStringLiteral("ponder"));
        }
        QTest::keyClicks(s->edit(), QStringLiteral("ab"));         // podczas pisania nadal ponder
        pr.step();
        QCOMPARE(pr.anim(), QStringLiteral("ponder"));
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 3, 2000);
        pr.step();
        QCOMPARE(pr.anim(), QStringLiteral("ponder"));

        // Zamknięcie bez uruchomienia: wraca do trybu.
        s->hide();
        pr.step();
        QCOMPARE(pr.anim(), main);
        QCOMPARE(pr.reactions(), size_t(0));

        // Uruchomienie: radość przez 1-1,5 s, potem tryb.
        QTest::qWait(450);
        dclick(pet);
        QTest::keyClicks(s->edit(), QStringLiteral("c"));
        QTRY_COMPARE_WITH_TIMEOUT(s->resultCount(), 3, 2000);
        QTest::keyClick(s->edit(), Qt::Key_Return);
        QVERIFY(!s->isVisible());
        int cheerTicks = 0;
        for (int i = 0; i < 100; ++i) {
            pr.step();
            if (pr.anim() != QStringLiteral("cheer"))
                break;
            ++cheerTicks;
        }
        QVERIFY2(cheerTicks * cfg::TICK_MS >= 1000 && cheerTicks * cfg::TICK_MS <= 1500, qPrintable(QString::number(cheerTicks)));
        QCOMPARE(pr.anim(), main);

        // Brak wyników: wstyd dopiero po ustabilizowaniu zapytania, nie przy każdym znaku.
        QTest::qWait(450);
        dclick(pet);
        for (const QChar c : QStringLiteral("zzzzzz")) {
            QTest::keyClick(s->edit(), c.toLatin1());
            QTest::qWait(60);                                       // wolniej niż odpowiedź silnika, szybciej niż stabilizacja
            pr.step();
            QVERIFY2(pr.anim() != QStringLiteral("shy"), "shy w trakcie pisania");
            QCOMPARE(pr.reactions(), size_t(0));
        }
        QTRY_VERIFY_WITH_TIMEOUT(pr.reactions() > 0, 3000);
        pr.step();
        QCOMPARE(pr.anim(), QStringLiteral("shy"));
        int shyTicks = 1;
        while (shyTicks < 200) {
            pr.step();
            if (pr.anim() != QStringLiteral("shy"))
                break;
            ++shyTicks;
        }
        QVERIFY2(shyTicks * cfg::TICK_MS <= 2000, qPrintable(QString::number(shyTicks)));
        QCOMPARE(pr.anim(), QStringLiteral("ponder"));              // okno nadal otwarte
        // Dalsze wpisywanie (są wyniki) - bez ponownego wstydu.
        QTest::keyClicks(s->edit(), QStringLiteral("\b\b\b\b\b\b"));
        QTRY_COMPARE_WITH_TIMEOUT(s->text(), QString(), 1000);
        QTest::qWait(500);
        QCOMPARE(pr.reactions(), size_t(0));
        s->hide();
    }

    // Wpisywanie nie blokuje pętli zdarzeń (>100 ms) ani animacji; silnik działa asynchronicznie.
    void searchTypingDoesNotBlock()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        SearchBubble *s = pr.search();
        QTest::qWait(300);
        QVERIFY(timedMs([&] { dclick(pet); }) < 100);
        QVERIFY(s->isVisible());
        for (const QChar c : QStringLiteral("firefox krunner")) {
            const double ms = timedMs([&] { QTest::keyClick(s->edit(), c.toLatin1()); });
            QVERIFY2(ms < 100, qPrintable(QString::number(ms)));
            QTest::qWait(30);
        }
        QTRY_VERIFY_WITH_TIMEOUT(s->resultCount() == 3, 2000);
        s->hide();
        const TickStats st = pet.tickStats();
        QVERIFY2(st.maxGapMs < 300.0, "tick zwlekał o ponad 2 ticki");
    }

    // Bez fokusu okno NIE chowa się samo (zostaje otwarte, pole przejmie fokus przy kliknięciu).
    void searchStaysOpenWithoutFocus()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QTest::qWait(2500);                               // dłużej niż ponawianie aktywacji i dawne 1,5 s
        QVERIFY(pr.search()->isVisible());
    }

    // Druga połowa pary, której pierwszy klik zamknął wyszukiwarkę (utrata aktywności), nie otwiera jej od nowa.
    void searchDoubleClickClosesOpenSearch()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        dclick(pet);
        QVERIFY(!pr.search()->isVisible());
        QTest::qWait(450);
        // Pierwszy klik pary zamyka okno przez utratę aktywności (jak w KWin), zanim przyjdzie zwolnienie przycisku.
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        pr.search()->hide();
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseDClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY(!pr.search()->isVisible());
    }

    // Podwójny klik podczas spadania nie otwiera wyszukiwarki.
    void searchNotOpenedWhileFalling()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(new FakeEngine);
        QTest::qWait(50);
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&pet, QPoint(80, -150));
        QVERIFY(pr.drag());
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(80, -150));
        QVERIFY(pr.fall());
        dclick(pet, QPoint(80, -150));
        QVERIFY(!pr.search()->isVisible());
        QVERIFY(!pr.bubble()->isVisible());
    }

    // Zestaw runnerów: tylko bezpieczne (bez powłoki, kill, sesji, zasilania).
    void searchRunnerAllowList()
    {
        const QStringList allowed = KRunnerEngine::allowedRunners();
        for (const char *bad : {"krunner_shell", "shell", "krunner_kill", "krunner_sessions", "krunner_powerdevil",
                                "krunner_keys", "krunner_kwin", "krunner_plasma-desktop"})
            QVERIFY2(!allowed.contains(QLatin1String(bad)), bad);
        QVERIFY2(!allowed.contains(QStringLiteral("krunner_recentdocuments")), "SIGSEGV w KActivities::Consumer przy init");
        QVERIFY(allowed.contains(QStringLiteral("krunner_services")));
        QVERIFY(allowed.contains(QStringLiteral("calculator")));
    }

    // Prawdziwy silnik (KF6 Runner): "2+2" daje wynik kalkulatora, bez uruchamiania czegokolwiek.
    // Bez pluginów runnerów (kontener budujący) test jest pomijany.
    void searchKRunnerIntegration()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        QStringList ids;
        for (const KPluginMetaData &m : KRunner::RunnerManager::runnerMetaDataList())
            ids << m.pluginId();
        qInfo("runnery w systemie: %s", qPrintable(ids.join(QLatin1Char(' '))));
        for (const QString &a : KRunnerEngine::allowedRunners())
            qInfo("dozwolony %s: %s", qPrintable(a), ids.contains(a) ? "jest" : "brak");
        PetEnv env;
        KRunnerEngine engine;
        QSignalSpy results(&engine, &SearchEngine::resultsChanged);
        QSignalSpy finished(&engine, &SearchEngine::finished);
        engine.begin();
        double worst = timedMs([&] { engine.query(QStringLiteral("2+2")); });
        QVERIFY2(worst < 500, qPrintable(QString::number(worst)));    // pierwsze zapytanie ładuje runnery
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 10000);
        QList<SearchResult> last = results.last().at(1).value<QList<SearchResult>>();
        QVERIFY2(!last.isEmpty(), "brak wyników dla 2+2");
        bool hasFour = false;
        for (const SearchResult &r : last)
            hasFour = hasFour || r.title.contains(QLatin1Char('4'));
        QVERIFY2(hasFour, qPrintable(last.first().title));
        qInfo("2+2 -> %s [%s], pierwsze zapytanie %.1f ms", qPrintable(last.first().title),
              qPrintable(last.first().category), worst);
        // "konsole": wynik (programy) tylko gdy zainstalowane - samo uruchomienie nie jest wołane.
        results.clear();
        finished.clear();
        engine.query(QStringLiteral("konsole"));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 10000);
        qInfo("konsole -> %lld wyników", static_cast<long long>(results.isEmpty() ? 0 : results.last().at(1).value<QList<SearchResult>>().size()));
        engine.end();
    }

    // Prawdziwy silnik w dymku: pisanie "2+2" nie blokuje UI, a wyniki mają ikony i tytuły.
    void searchKRunnerBubbleNoBlocking()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        SearchBubble *s = pr.search();
        QTest::qWait(200);
        dclick(pet);
        double worst = 0;
        for (const QChar c : QStringLiteral("2+2")) {
            worst = std::max(worst, timedMs([&] { QTest::keyClick(s->edit(), c.toLatin1()); }));
            QTest::qWait(40);
        }
        QElapsedTimer t;
        t.start();
        double lastEvent = 0;
        while (s->resultCount() == 0 && t.elapsed() < 10000) {
            const double ms = timedMs([] { QTest::qWait(10); });
            lastEvent = std::max(lastEvent, ms - 10);
        }
        QVERIFY(s->resultCount() > 0);
        qInfo("KRunner w dymku: najdłuższe pisanie %.1f ms, najdłuższa zwłoka pętli %.1f ms, wynik: %s",
              worst, lastEvent, qPrintable(s->resultTitle(0)));
        QVERIFY2(worst < 100, qPrintable(QString::number(worst)));
        s->hide();
        QVERIFY2(pet.tickStats().maxStepMs < 100, "step() za długo");
    }

    // Stress: pisanie znak po znaku (30-80 ms), backspace i dopisywanie w trakcie zapytania, prawdziwe runnery.
    // Regresja crasha przy wpisywaniu. Liczba rund: DESKTOP_PET_STRESS_ROUNDS (domyślnie 2).
    void searchKRunnerTypingStress()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        // Prawdziwy HOME (ostatnie dokumenty, miejsca, ksycoca) i kopia prawdziwej konfiguracji KDE - tak jak na pulpicie.
        const QByteArray realHome = env.oldHome;
        qputenv("HOME", realHome);
        for (const char *name : {"kdeglobals", "baloofilerc", "mimeapps.list", "user-dirs.dirs", "krunnerrc"})
            QFile::copy(QString::fromUtf8(realHome) + QStringLiteral("/.config/") + QString::fromUtf8(name),
                        env.tmp.path() + QStringLiteral("/cfg/") + QString::fromUtf8(name));
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        SearchBubble *s = pr.search();
        QTest::qWait(200);
        dclick(pet);
        const QStringList words = {QStringLiteral("firefox"), QStringLiteral("steam"), QStringLiteral("2+2"),
                                   QStringLiteral("konsole"), QStringLiteral("dolphin"), QStringLiteral("10 km"),
                                   QStringLiteral("ustawienia"), QStringLiteral("usta"), QStringLiteral("2+"),
                                   QStringLiteral("date"), QStringLiteral("5 usd in eur"), QStringLiteral("doc")};
        bool okRounds = false;
        const int rounds = qEnvironmentVariableIntValue("DESKTOP_PET_STRESS_ROUNDS") > 0
                               ? qEnvironmentVariableIntValue("DESKTOP_PET_STRESS_ROUNDS") : 2;
        int queries = 0;
        quint32 seed = 12345;
        auto rnd = [&](int lo, int hi) { seed = seed * 1664525u + 1013904223u; return lo + int((seed >> 8) % quint32(hi - lo + 1)); };
        for (int round = 0; round < rounds; ++round) {
            for (const QString &w : words) {
                s->edit()->clear();
                for (const QChar c : w) {
                    QTest::keyClick(s->edit(), c.toLatin1());
                    ++queries;
                    QTest::qWait(rnd(30, 80));
                }
                // backspace w trakcie zapytania i szybkie dopisywanie
                const int back = rnd(1, std::min<int>(3, int(w.size())));
                for (int i = 0; i < back; ++i) {
                    QTest::keyClick(s->edit(), Qt::Key_Backspace);
                    QTest::qWait(rnd(30, 80));
                }
                for (int i = int(w.size()) - back; i < w.size(); ++i) {
                    QTest::keyClick(s->edit(), w.at(i).toLatin1());
                    QTest::qWait(rnd(5, 40));
                }
                QTest::qWait(rnd(0, 400));
                // zamknięcie dymka (utrata aktywności) w trakcie zapytania i ponowne otwarcie
                if (rnd(0, 2) == 0) {
                    s->hide();
                    QTest::qWait(rnd(0, 150));
                    s->open();
                    QTest::qWait(rnd(0, 100));
                }
            }
            okRounds = true;
        }
        QVERIFY(okRounds);
        QTest::qWait(1500);
        qInfo("stress: %d naciśnięć klawiszy, %d rund", queries, rounds);
        s->hide();
        QTest::qWait(300);
    }

    // Każdy dozwolony runner osobno (RunnerManager z jednym runnerem): krótkie zapytania, także anulowane w locie.
    void searchEachRunnerShortQueries()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        qputenv("HOME", env.oldHome);
        for (const char *name : {"kdeglobals", "baloofilerc", "mimeapps.list", "user-dirs.dirs", "krunnerrc"})
            QFile::copy(QString::fromUtf8(env.oldHome) + QStringLiteral("/.config/") + QString::fromUtf8(name),
                        env.tmp.path() + QStringLiteral("/cfg/") + QString::fromUtf8(name));
        KSycoca::self()->ensureCacheValid();
        for (const QString &id : KRunnerEngine::allowedRunners()) {
            KRunner::RunnerManager mgr;
            mgr.setAllowedRunners({id});
            mgr.setHistoryEnabled(false);
            int total = 0;
            QObject::connect(&mgr, &KRunner::RunnerManager::matchesChanged, &mgr,
                             [&](const QList<KRunner::QueryMatch> &m) {
                for (const KRunner::QueryMatch &q : m) {          // dotykamy wszystkiego, co robi nasz silnik
                    total += q.text().size() + q.subtext().size() + q.matchCategory().size();
                    const QIcon ic = q.icon().isNull() ? QIcon::fromTheme(q.iconName()) : q.icon();
                    total += ic.pixmap(QSize(24, 24)).width();
                    total += q.actions().size();
                }
            });
            for (const QString &w : {QStringLiteral("c"), QStringLiteral("ch"), QStringLiteral("chr"),
                                     QStringLiteral("st"), QStringLiteral("s"), QStringLiteral("a"),
                                     QStringLiteral("k"), QStringLiteral("d"), QStringLiteral("f")}) {
                for (int i = 0; i < 3; ++i) {
                    mgr.launchQuery(w);
                    QTest::qWait(i == 0 ? 20 : 400);               // raz anulowane w trakcie
                }
                QTest::qWait(500);
            }
            mgr.reset();
            qInfo("runner %s: ok (%d)", qPrintable(id), total);
        }
    }

    // Pomocnik wyszukiwania: wyniki przychodzą z osobnego procesu (prawdziwe runnery, "ch" i krótkie prefiksy).
    void helperEngineReturnsResultsForShortQueries()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        qputenv("HOME", env.oldHome);
        for (const char *name : {"kdeglobals", "baloofilerc", "mimeapps.list", "user-dirs.dirs", "krunnerrc"})
            QFile::copy(QString::fromUtf8(env.oldHome) + QStringLiteral("/.config/") + QString::fromUtf8(name),
                        env.tmp.path() + QStringLiteral("/cfg/") + QString::fromUtf8(name));
        KSycoca::self()->ensureCacheValid();             // cache usług gotowy przed startem pomocnika (bez przebudowy w jego wątkach)
        HelperEngine eng;
        QSignalSpy results(&eng, &SearchEngine::resultsChanged), fin(&eng, &SearchEngine::finished);
        eng.begin();
        for (const QString &w : {QStringLiteral("c"), QStringLiteral("ch"), QStringLiteral("chr"), QStringLiteral("st"),
                                 QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("k"), QStringLiteral("d"),
                                 QStringLiteral("f"), QStringLiteral("2+2")}) {
            fin.clear();
            eng.query(w);
            QTRY_VERIFY_WITH_TIMEOUT(fin.count() > 0, 15000);
            QCOMPARE(fin.last().at(0).toString(), w);
        }
        QVERIFY(eng.helperPid() > 0);
        bool sawFour = false;
        for (const auto &r : results)
            for (const SearchResult &s : r.at(1).value<QList<SearchResult>>())
                sawFour = sawFour || (r.at(0).toString() == QLatin1String("2+2") && s.title.contains(QLatin1Char('4')));
        QVERIFY(sawFour);
        eng.end();
    }

    // Śmierć pomocnika (SIGKILL w trakcie zapytania) nie zabija maskotki: restart, komunikat, ponowione zapytanie.
    void helperCrashDoesNotKillPetAndQueryIsRetried()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        qputenv("HOME", env.oldHome);
        KSycoca::self()->ensureCacheValid();
        auto *eng = new HelperEngine;
        QSignalSpy problems(eng, &SearchEngine::problem), fin(eng, &SearchEngine::finished);
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.search()->setEngine(eng);
        QTest::qWait(100);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QTest::keyClicks(pr.search()->edit(), QStringLiteral("ch"));
        QTRY_VERIFY_WITH_TIMEOUT(fin.count() > 0, 15000);
        const qint64 pid = eng->helperPid();
        QVERIFY(pid > 0);
        fin.clear();
        QTest::keyClicks(pr.search()->edit(), QStringLiteral("r"));       // "chr"
        QTest::qWait(SearchBubble::DEBOUNCE_MS + 60);
        QCOMPARE(::kill(pid_t(pid), SIGKILL), 0);                       // pomocnik pada w trakcie zapytania
        QTRY_VERIFY_WITH_TIMEOUT(problems.count() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(eng->helperPid() > 0 && eng->helperPid() != pid, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(fin.count() > 0, 15000);               // zapytanie ponowione na nowym pomocniku
        QVERIFY(pr.search()->isVisible());
        QCOMPARE(eng->restarts(), 1);
        // po kolejnej śmierci też; maskotka żyje i dymek działa
        QCOMPARE(::kill(pid_t(eng->helperPid()), SIGKILL), 0);
        QTRY_VERIFY_WITH_TIMEOUT(eng->restarts() == 2 && eng->helperPid() > 0, 5000);
        pr.search()->hide();
    }

    // Przyczyna cichego znikania maskotki: biblioteki KDE (KJob/KIO w runnerach) trzymają QEventLoopLocker, a gdy ostatni
    // zostanie zwolniony, a nie ma "prawdziwego" okna (dymki i postać to Qt::Tool), Qt kończy pętlę (QEvent::Quit)
    // - bez śladu stosu i zrzutu. Pet::setupApplication() wyłącza blokadę wyjścia.
    void quitLockReleaseNeverQuits()
    {
        Pet::setupApplication();
        PetEnv env;
        Pet pet;
        pet.show();
        bool survived = false;
        QTimer::singleShot(0, [] { QEventLoopLocker locker; });          // blokada założona i zaraz zwolniona
        QTimer::singleShot(400, [&survived] { survived = true; QCoreApplication::exit(0); });
        QCoreApplication::exec();
        QVERIFY2(survived, "zwolnienie QEventLoopLocker zakończyło program");
    }

    // Krótkie prefiksy ("ch", "c", "chr", ...) z prawdziwymi runnerami: znak po znaku i od razu.
    void searchKRunnerShortPrefixes()
    {
        if (KRunner::RunnerManager::runnerMetaDataList().isEmpty())
            QSKIP("brak pluginów KRunner w tym środowisku");
        PetEnv env;
        const QByteArray realHome = env.oldHome;
        qputenv("HOME", realHome);
        for (const char *name : {"kdeglobals", "baloofilerc", "mimeapps.list", "user-dirs.dirs", "krunnerrc"})
            QFile::copy(QString::fromUtf8(realHome) + QStringLiteral("/.config/") + QString::fromUtf8(name),
                        env.tmp.path() + QStringLiteral("/cfg/") + QString::fromUtf8(name));
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        SearchBubble *s = pr.search();
        QTest::qWait(200);
        dclick(pet);
        const QStringList words = {QStringLiteral("c"), QStringLiteral("ch"), QStringLiteral("chr"),
                                   QStringLiteral("st"), QStringLiteral("s"), QStringLiteral("a"),
                                   QStringLiteral("k"), QStringLiteral("d"), QStringLiteral("f"),
                                   QStringLiteral("chrome"), QStringLiteral("ch ")};
        quint32 seed = 777;
        auto rnd = [&](int lo, int hi) { seed = seed * 1664525u + 1013904223u; return lo + int((seed >> 8) % quint32(hi - lo + 1)); };
        for (int round = 0; round < 3; ++round) {
            for (const QString &w : words) {
                s->edit()->clear();
                for (const QChar c : w) {                              // znak po znaku
                    QTest::keyClick(s->edit(), c.toLatin1());
                    QTest::qWait(rnd(30, 80));
                }
                QTest::qWait(rnd(300, 900));
                s->edit()->clear();
                QTest::qWait(rnd(0, 150));
                s->edit()->setText(w);                                  // od razu całość
                QTest::qWait(rnd(300, 900));
            }
        }
        s->hide();
        QTest::qWait(300);
    }

    // Zamknięcie dymków (Esc, utrata aktywności, close(), X) nie może zakończyć programu:
    // okna Qt::Tool nie liczą się jako okna główne, więc bez setQuitOnLastWindowClosed(false) program by się wyłączył.
    void closingBubblesNeverQuits()
    {
        Pet::setupApplication();                          // to samo, co robi main()
        QVERIFY(!QApplication::quitOnLastWindowClosed());
        PetEnv env;
        QSignalSpy quitSpy(qApp, &QCoreApplication::aboutToQuit);
        QSignalSpy lastSpy(qApp, &QGuiApplication::lastWindowClosed);
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QVERIFY(pet.windowFlags().testFlag(Qt::Tool));
        pr.search()->setEngine(new FakeEngine);
        QTest::qWait(100);
        // wyszukiwarka: Esc, close(), utrata aktywności (changeEvent), podwójny klik
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        QTest::keyClick(pr.search()->edit(), Qt::Key_Escape);
        QVERIFY(!pr.search()->isVisible());
        QTest::qWait(450);
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        pr.search()->close();
        QVERIFY(!pr.search()->isVisible());
        QTest::qWait(450);
        dclick(pet);
        QEvent act(QEvent::ActivationChange);
        QApplication::sendEvent(pr.search(), &act);
        pr.search()->hide();
        // dymek z muzyką
        QWheelEvent we(QPointF(20, 20), pet.mapToGlobal(QPointF(20, 20)), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&pet, &we);
        QVERIFY(pr.bubble()->isVisible());
        pr.bubble()->close();
        QVERIFY(!pr.bubble()->isVisible());
        QTest::qWait(300);
        QVERIFY(pet.isVisible());
        QCOMPARE(quitSpy.count(), 0);
        QCOMPARE(lastSpy.count(), 0);
    }

    // ---------------------------------------------------------- VDF
    void vdfBasic()
    {
        const VdfNode r = parseVdf(QStringLiteral("\"AppState\" { \"appid\" \"42\" \"Name\" \"Gra\" }"));
        const VdfNode *s = r.find(QStringLiteral("appstate"));
        QVERIFY(s && s->dict);
        QCOMPARE(s->find(QStringLiteral("APPID"))->text, QStringLiteral("42"));
        QCOMPARE(s->find(QStringLiteral("name"))->text, QStringLiteral("Gra"));
    }
    void vdfCommentsEscapesConditions()
    {
        const QString src = QStringLiteral(
            "// komentarz\n\"a\" // koniec linii\n{\n"
            "  \"k1\" \"w\\\"artosc\\\\z\\n\\t\\q\"\n"
            "  \"k2\" \"http://x\"   // url w napisie to nie komentarz\n"
            "  \"k3\" bez_cudzyslowow\n"
            "  \"k4\" \"[$WIN32]\"\n"
            "  \"k5\" [$WIN32] \"po warunku\"\n"
            "}\n");
        const VdfNode r = parseVdf(src);
        const VdfNode *a = r.find(QStringLiteral("a"));
        QVERIFY(a);
        QCOMPARE(a->find(QStringLiteral("k1"))->text, QStringLiteral("w\"artosc\\z\n\t\\q"));
        QCOMPARE(a->find(QStringLiteral("k2"))->text, QStringLiteral("http://x"));
        QCOMPARE(a->find(QStringLiteral("k3"))->text, QStringLiteral("bez_cudzyslowow"));
        QCOMPARE(a->find(QStringLiteral("k4"))->text, QStringLiteral("[$WIN32]"));   // w cudzysłowie zostaje
        QCOMPARE(a->find(QStringLiteral("k5"))->text, QStringLiteral("po warunku"));
    }
    void vdfExtraBraceAndBroken()
    {
        const VdfNode r = parseVdf(QStringLiteral("\"a\" { \"x\" \"1\" } } } \"b\" \"2\" \"c\" { \"y\" \"3\""));
        QCOMPARE(r.find(QStringLiteral("a"))->find(QStringLiteral("x"))->text, QStringLiteral("1"));
        QCOMPARE(r.find(QStringLiteral("b"))->text, QStringLiteral("2"));
        QCOMPARE(r.find(QStringLiteral("c"))->find(QStringLiteral("y"))->text, QStringLiteral("3"));
        QVERIFY(parseVdf(QString()).vals.empty());
        QVERIFY(parseVdf(QStringLiteral("\"niezamkniety")).find(QStringLiteral("niezamkniety")) == nullptr);
    }
    void vdfDuplicateKeyOverwrites()
    {
        const VdfNode r = parseVdf(QStringLiteral("\"a\" \"1\" \"a\" \"2\""));
        QCOMPARE(r.keys.size(), size_t(1));
        QCOMPARE(r.find(QStringLiteral("a"))->text, QStringLiteral("2"));
    }

    // ---------------------------------------------------------- Steam (pliki testowe)
    void steamLibraryFormats()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString root = tmp.filePath(QStringLiteral("Steam"));
        const QString lib2 = tmp.filePath(QStringLiteral("lib2"));
        const QString lib3 = tmp.filePath(QStringLiteral("lib3"));
        // Nowy format (zagnieżdżone sekcje z "path") + stary ("1" "ścieżka") + śmieci.
        writeFile(root + QStringLiteral("/steamapps/libraryfolders.vdf"),
                  QStringLiteral("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\"%1\"\n\t\t\"apps\" { \"1\" \"2\" }\n\t}\n"
                                 "\t\"1\"\n\t{\n\t\t\"path\"\t\"%2\"\n\t}\n"
                                 "\t\"2\" \"%3\"\n\t\"TimeNextStatsReport\" \"123\"\n\t\"3\" \"/nie/ma/takiego\"\n}\n")
                      .arg(root, lib2, lib3));
        writeFile(root + QStringLiteral("/steamapps/appmanifest_100.acf"), acf(100, QStringLiteral("Gra Stara"), 4, 5));
        writeFile(lib2 + QStringLiteral("/steamapps/appmanifest_200.acf"), acf(200, QStringLiteral("gra z Dysku 2"), 4, 50));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_300.acf"), acf(300, QStringLiteral("Aktualizowana"), 1028, 50));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_301.acf"), acf(301, QStringLiteral("Nieinstalowana"), 2));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_302.acf"), acf(302, QStringLiteral("Proton 9.0"), 4));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_303.acf"), acf(303, QStringLiteral("Steam Linux Runtime 4.0"), 4));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_228980.acf"), acf(228980, QStringLiteral("Cokolwiek"), 4));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_304.acf"), QStringLiteral("to nie jest vdf {{{"));
        writeFile(lib3 + QStringLiteral("/steamapps/appmanifest_305.acf"), acf(305, QString(), 4));
        // Dowiązanie do tego samego korzenia nie może dublować biblioteki.
        QVERIFY(QFile::link(root, tmp.filePath(QStringLiteral("link"))));

        SteamLibrary lib({tmp.filePath(QStringLiteral("link")), root, tmp.filePath(QStringLiteral("brak"))});
        QCOMPARE(lib.steamRoots().size(), 1);
        QCOMPARE(lib.libraryDirs().size(), 3);
        const QList<SteamGame> games = lib.games();
        QStringList names;
        for (const auto &g : games)
            names << g.name;
        // Ostatnio grane na górze; remis -> alfabetycznie bez wielkości liter.
        QCOMPARE(names, (QStringList{QStringLiteral("Aktualizowana"), QStringLiteral("gra z Dysku 2"),
                                     QStringLiteral("Gra Stara")}));
        QVERIFY(games[0].updating);
        QVERIFY(!games[1].updating);
    }
    void steamToolFilter()
    {
        QVERIFY(isSteamTool(1493710, QStringLiteral("cokolwiek")));
        QVERIFY(isSteamTool(1, QStringLiteral("Proton Experimental")));
        QVERIFY(isSteamTool(1, QStringLiteral("proton 9.0")));
        QVERIFY(isSteamTool(1, QStringLiteral("Steam Linux Runtime 3.0 (sniper)")));
        QVERIFY(isSteamTool(1, QStringLiteral("Steamworks Common Redistributables")));
        QVERIFY(!isSteamTool(1, QStringLiteral("Protonmail Simulator")));   // \b po "Proton"
        QVERIFY(!isSteamTool(899770, QStringLiteral("Last Epoch")));
    }
    void steamRealFiles()
    {
        SteamLibrary lib;
        if (lib.steamRoots().isEmpty())
            QSKIP("brak instalacji Steama na tej maszynie");
        QStringList names;
        for (const auto &g : lib.games())
            names << g.name;
        qInfo() << "gry ze Steama:" << names;
        QVERIFY2(names.contains(QStringLiteral("Last Epoch")), qPrintable(names.join(QLatin1Char(','))));
        QVERIFY2(names.contains(QStringLiteral("Path of Exile")), qPrintable(names.join(QLatin1Char(','))));
        QCOMPARE(names.size(), 2);
        for (const auto &n : names) {
            QVERIFY(!n.contains(QLatin1String("Proton")));
            QVERIFY(!n.contains(QLatin1String("Runtime")));
            QVERIFY(!n.contains(QLatin1String("Redistributables")));
        }
    }

    // ---------------------------------------------------------- MPRIS
    void mprisRanking()
    {
        QCOMPARE(Mpris::bestIndex({}), -1);
        // grający wygrywa z pauzą i zatrzymanym
        QCOMPARE(Mpris::bestIndex({player("a", "Paused"), player("b", "Playing"), player("c", "Stopped")}), 1);
        // grający YouTube Music > grający inny
        QCOMPARE(Mpris::bestIndex({player("a", "Playing", "x"),
                                   player("b", "Playing", "x", "https://music.youtube.com/watch?v=1")}), 1);
        QCOMPARE(Mpris::bestIndex({player("a", "Playing", "Kawałek - YouTube Music"), player("b", "Playing")}), 0);
        // grający inny > YT Music w pauzie
        QCOMPARE(Mpris::bestIndex({player("a", "Paused", "x", "https://music.youtube.com/"), player("b", "Playing", "x")}), 1);
        // YT Music w pauzie > zwykła pauza > zatrzymany
        QCOMPARE(Mpris::bestIndex({player("a", "Stopped"), player("b", "Paused"),
                                   player("c", "Paused", "x", "https://music.youtube.com/")}), 2);
        QCOMPARE(Mpris::bestIndex({player("a", "Stopped"), player("b", "Paused")}), 1);
        // remis -> pierwszy
        QCOMPARE(Mpris::bestIndex({player("a", "Playing"), player("b", "Playing")}), 0);
        QCOMPARE(Mpris::bestIndex({player("a", "Stopped"), player("b", "Stopped")}), 0);
    }

    // Na żywej magistrali sesji (pomijany, gdy nic nie gra / brak D-Bus).
    void mprisLive()
    {
        Mpris m;
        const QStringList names = m.players();
        if (names.isEmpty())
            QSKIP("brak odtwarzaczy MPRIS na magistrali sesji");
        const auto cur = m.current();
        QVERIFY(cur.has_value());
        QVERIFY(cur->name.startsWith(QLatin1String(MPRIS_PREFIX)));
        QVERIFY(!cur->identity.isEmpty());
        qInfo() << "MPRIS:" << cur->name << cur->identity << cur->status << "|" << cur->title << "|" << cur->artist;
        PlayerVolume pv;
        QSignalSpy spy(&pv, &PlayerVolume::changed);
        pv.refresh(cur);
        QVERIFY(spy.wait(5000));
        const auto vol = pv.get(cur);
        qInfo() << "głośność:" << (vol ? QString::number(vol->percent) : QStringLiteral("brak strumienia"));
    }

    // ---------------------------------------------------------- głośność
    void volumeStreamMatching()
    {
        const QByteArray json = R"([
          {"index": 7, "mute": false, "volume": {"front-left": {"value": 1, "value_percent": "50%"}},
           "properties": {"application.name": "Google Chrome", "application.process.binary": "chrome"}},
          {"index": 9, "mute": false, "volume": {},
           "properties": {"application.name": "Firefox", "application.process.binary": "firefox"}},
          {"index": 11, "mute": false, "volume": {},
           "properties": {"application.name": "Telegram Desktop"}}
        ])";
        const QJsonArray all = QJsonDocument::fromJson(json).array();
        PlayerInfo chrome;
        chrome.name = QStringLiteral("org.mpris.MediaPlayer2.chromium.instance123");
        chrome.identity = QStringLiteral("Google Chrome");
        const QJsonArray m = PlayerVolume::matchStreams(all, chrome);
        QCOMPARE(m.size(), 1);
        QCOMPARE(m[0].toObject().value("index").toInt(), 7);   // "google" odrzucone, "chrome" pasuje
        PlayerInfo ff;
        ff.name = QStringLiteral("org.mpris.MediaPlayer2.firefox.instance_1_42");
        ff.identity = QStringLiteral("Mozilla Firefox");
        const QJsonArray f = PlayerVolume::matchStreams(all, ff);
        QCOMPARE(f.size(), 1);
        QCOMPARE(f[0].toObject().value("index").toInt(), 9);
        PlayerInfo none;
        none.name = QStringLiteral("org.mpris.MediaPlayer2.vlc");
        none.identity = QStringLiteral("VLC");   // token "vlc" ma 3 znaki -> pomijany
        QVERIFY(PlayerVolume::matchStreams(all, none).isEmpty());
    }

    // ---------------------------------------------------------- ustawienia, sprite'y, tryby
    void settingsKeepOtherKeys()
    {
        QTemporaryDir tmp;
        qputenv("XDG_CONFIG_HOME", tmp.path().toUtf8());
        writeFile(cfg::settingsFile(), QStringLiteral("{\"mode\":\"auto\",\"obce\":[1,2],\"x\":true}"));
        QJsonObject s = cfg::loadSettings();
        QCOMPARE(s.value("mode").toString(), QStringLiteral("auto"));
        s.insert(QStringLiteral("mode"), QStringLiteral("leek"));
        cfg::saveSettings(s);
        const QJsonObject back = cfg::loadSettings();
        QCOMPARE(back.value("mode").toString(), QStringLiteral("leek"));
        QCOMPARE(back.value("obce").toArray().size(), 2);
        QVERIFY(back.value("x").toBool());
        writeFile(cfg::settingsFile(), QStringLiteral("to nie json"));
        QVERIFY(cfg::loadSettings().isEmpty());
        qunsetenv("XDG_CONFIG_HOME");
    }
    void modesTable()
    {
        QCOMPARE(int(modes().size()), 9);
        for (const char *k : {"sit", "lie", "stand", "auto", "leek", "walk", "happy", "shy", "dance"})
            QVERIFY(findMode(QString::fromLatin1(k)));
        QVERIFY(!findMode(QStringLiteral("nie ma")));
        QCOMPARE(findMode(QStringLiteral("leek"))->activities.front().name, "leek");
    }
    // Każdy tryb: niepuste aktywności, wagi i czasy > 0, animacje istnieją (postać zastępcza i defs paczki).
    void modesActivitiesValid()
    {
        const Sprites ph = placeholderSprites();
        const QStringList names = shimejiAnimNames();
        for (const Mode &m : modes()) {
            QVERIFY2(!m.activities.empty(), m.key);
            for (const ModeActivity &a : m.activities) {
                const QString n = QString::fromLatin1(a.name);
                QVERIFY2(a.weight > 0, qPrintable(QLatin1String(m.key) + QLatin1Char('/') + n));
                QVERIFY2(a.lo > 0 && a.hi >= a.lo, qPrintable(n));
                QVERIFY2(names.contains(n), qPrintable(n));
                QVERIFY2(ph.anims.contains(n), qPrintable(n));
            }
        }
        // Główna czynność (po upadku) nowych trybów.
        QCOMPARE(findMode(QStringLiteral("happy"))->activities.front().name, "sit_happy");
        QCOMPARE(findMode(QStringLiteral("shy"))->activities.front().name, "shy");
        QCOMPARE(findMode(QStringLiteral("dance"))->activities.front().name, "dance");
        // Główne czynności dominują w sit i stand.
        QCOMPARE(findMode(QStringLiteral("sit"))->activities.front().weight, 5);
        QCOMPARE(findMode(QStringLiteral("stand"))->activities.front().weight, 4);
    }

    // Nowe animacje ładują się z prawdziwej paczki (pomijane, gdy jej nie ma na tym koncie).
    void newAnimsFromRealPack()
    {
        const QString root = QDir::homePath() + QStringLiteral("/.config/desktop-pet/sprites");
        const QString folder = findShimejiDir(root);
        if (folder.isEmpty())
            QSKIP("brak prawdziwej paczki sprite'ów");
        Sprites sp;
        QString missing;
        QVERIFY2(loadShimeji(folder, 1.0, &sp, &missing), qPrintable(missing));
        const int h = sp.anim(QStringLiteral("stand")).frames.first().height();
        for (const char *c : {"sit_happy", "sit_think", "shy", "ponder", "cheer", "dance"}) {
            const QString n = QString::fromLatin1(c);
            QVERIFY2(sp.anims.contains(n), c);
            const Anim &a = sp.anims[n];
            QCOMPARE(int(a.frames.size()), int(shimejiAnimFrames(n).size()));   // prawdziwe klatki, nie zastępstwo stand
            QVERIFY2(a.ticks > 0, c);
            for (const QPixmap &f : a.frames) {
                QVERIFY2(!f.isNull(), c);
                QVERIFY2(f.height() <= h, c);          // nie wyższa niż okno
            }
        }
        QCOMPARE(sp.anims[QStringLiteral("dance")].frames.size(), 6);
    }

    void wheelSpinDetector()
    {
        WheelSpinDetector d;
        for (int i = 0; i < 5; ++i)
            QVERIFY(!d.add(1, i * 100));          // 5 ząbków - jeszcze nie
        QVERIFY(d.add(-1, 500));                  // 6. (kierunek bez znaczenia)
        QVERIFY(!d.add(1, 600));                  // po wykryciu licznik od zera
        WheelSpinDetector slow;                   // wolno: 1 ząbek na 400 ms nigdy nie dobije do 6 w 1,5 s
        for (int i = 0; i < 30; ++i)
            QVERIFY(!slow.add(1, i * 400));
        WheelSpinDetector fine;                   // ułamki (touchpad) też się sumują
        bool hit = false;
        for (int i = 0; i < 60 && !hit; ++i)
            hit = fine.add(0.125, i * 10);
        QVERIFY(hit);
        WheelSpinDetector zero;
        QVERIFY(!zero.add(0, 0));
    }

    void trackChangeDetector()
    {
        TrackChangeDetector t;
        const QString p = QStringLiteral("p1");
        const QString a = QStringLiteral("A\nx"), b = QStringLiteral("B\nx"), c = QStringLiteral("C\nx");
        QVERIFY(!t.update(p, QString(), 0, true));          // nic nie gra
        QVERIFY(!t.update(p, a, 100, true));                // pierwszy odczyt - bez reakcji
        QVERIFY(!t.update(p, a, 3100, true));               // bez zmiany
        QVERIFY(!t.update(p, b, 6100, true));               // nowy utwór dopiero raz - czeka na potwierdzenie
        QVERIFY(t.update(p, b, 9100, true));                // utrzymał się 2 odczyty - reakcja
        QVERIFY(!t.update(p, c, 12000, true));
        QVERIFY(!t.update(p, c, 15000, true));              // zmiana potwierdzona, ale cooldown 60 s
        QVERIFY(!t.update(p, c, 18000, true));              // i nie wraca po cooldownie, bo to ten sam utwór
        QVERIFY(!t.update(p, a, 70000, false));             // przeciągana/spada - zapamiętane, bez reakcji
        QVERIFY(!t.update(p, a, 73000, false));
        QVERIFY(!t.update(p, a, 76000, true));              // ta sama zmiana nie wraca po ruchu
        QVERIFY(!t.update(p, b, 80000, true));
        QVERIFY(t.update(p, b, 83000, true));               // po cooldownie znowu reaguje
        QVERIFY(!t.update(p, QString(), 90000, true));      // pusty klucz ignorowany
        QVERIFY(!t.update(p, b, 91000, true));              // ten sam utwór po pauzie - bez reakcji
    }

    // Fałszywe zmiany utworu (YouTube Music w przeglądarce) nie wywołują radości.
    void trackChangeIgnoresFalseChanges()
    {
        const QString p = QStringLiteral("p1"), a = QStringLiteral("A\nx"), b = QStringLiteral("B\ny");
        auto fresh = [&] { auto *t = new TrackChangeDetector; t->update(p, a, 0, true); return std::unique_ptr<TrackChangeDetector>(t); };
        {   // puste i niepełne metadane (ładowanie, reklama bez wykonawcy) w trakcie utworu
            auto t = fresh();
            qint64 now = 3000;
            for (const QString &k : {QStringLiteral("\n"), QStringLiteral("Reklama\n"), QStringLiteral("\nx"), QString(), QStringLiteral("A")})
                QVERIFY(!t->update(p, k, now += 3000, true));
            QVERIFY(!t->update(p, a, now += 3000, true));            // ten sam utwór po przerwie - bez zmiany
        }
        {   // jednorazowy błysk innego tytułu (jeden odczyt) nie liczy się
            auto t = fresh();
            QVERIFY(!t->update(p, b, 3000, true));
            QVERIFY(!t->update(p, a, 6000, true));
            QVERIFY(!t->update(p, b, 9000, true));
            QVERIFY(!t->update(p, a, 12000, true));
        }
        {   // zmiana samego odtwarzacza (inna karta/okno) tylko przestawia punkt odniesienia
            auto t = fresh();
            QVERIFY(!t->update(QStringLiteral("p2"), b, 3000, true));
            QVERIFY(!t->update(QStringLiteral("p2"), b, 6000, true));
            QVERIFY(!t->update(p, a, 9000, true));
            QVERIFY(!t->update(p, a, 12000, true));
        }
        {   // przełączanie tam i z powrotem między dwoma odtwarzaczami przez 10 min - zero reakcji
            auto t = fresh();
            int hits = 0;
            for (int i = 0; i < 200; ++i) {
                const bool one = i % 2 == 0;
                hits += t->update(one ? p : QStringLiteral("p2"), one ? a : b, 3000 + i * 3000, true);
            }
            QCOMPARE(hits, 0);
        }
        {   // prawdziwe zmiany co 10 s przez 10 min: reakcje nie częściej niż co minutę
            auto t = fresh();
            int hits = 0;
            qint64 last = -1000000;
            for (int i = 1; i <= 200; ++i) {
                const qint64 now = i * 3000;
                if (t->update(p, QStringLiteral("T%1\nx").arg(i / 4), now, true)) {
                    ++hits;
                    QVERIFY(now - last >= TrackChangeDetector::COOLDOWN_MS);
                    last = now;
                }
            }
            QVERIFY(hits >= 1 && hits <= 10);
        }
    }

    void frameAlignedToBottom()
    {
        QCOMPARE(frameY(128, 125), 3);       // 125 px w oknie 128 px: stopy na dole okna
        QCOMPARE(frameY(128, 128), 0);
        QCOMPARE(frameY(128, 140), 0);       // wyższa niż okno - przycięta od góry, nigdy ujemne
        for (int h : {100, 125, 127, 128})
            QCOMPARE(frameY(128, h) + h, 128);
        // Wszystkie klatki prawdziwej paczki: dolna krawędź = dół okna.
        const QString folder = findShimejiDir(QDir::homePath() + QStringLiteral("/.config/desktop-pet/sprites"));
        Sprites sp = folder.isEmpty() ? placeholderSprites() : Sprites();
        QString missing;
        if (!folder.isEmpty())
            QVERIFY2(loadShimeji(folder, 1.0, &sp, &missing), qPrintable(missing));
        int win = 0;
        for (const Anim &a : std::as_const(sp.anims))
            for (const QPixmap &f : a.frames)
                win = std::max(win, f.height());
        for (const Anim &a : std::as_const(sp.anims))
            for (const QPixmap &f : a.frames)
                QCOMPARE(frameY(win, f.height()) + f.height(), win);
    }

    void playerPropsParsing()
    {
        QVariantMap meta{{"xesam:title", "Tytuł"}, {"xesam:artist", QStringList{"A", "B"}},
                         {"xesam:url", "https://music.youtube.com/x"}};
        const PlayerInfo p = playerInfoFromProps(QStringLiteral("org.mpris.MediaPlayer2.x"),
                                                 {{"PlaybackStatus", "Playing"}, {"Metadata", meta}});
        QCOMPARE(p.status, QStringLiteral("Playing"));
        QCOMPARE(p.title, QStringLiteral("Tytuł"));
        QCOMPARE(p.artist, QStringLiteral("A"));
        QCOMPARE(TrackWatcher::keyFor(p), QStringLiteral("Tytuł\nA"));
        const PlayerInfo e = playerInfoFromProps(QStringLiteral("n"), {});     // brak pól -> wartości domyślne
        QCOMPARE(e.status, QStringLiteral("Stopped"));
        QVERIFY(e.title.isEmpty());
    }

    // Jedno sprawdzenie naraz, wynik przychodzi asynchronicznie (pętla zdarzeń nie jest blokowana),
    // a po zakończeniu można zacząć następne. Działa z magistralą i bez niej.
    void trackWatcherAsync()
    {
        TrackWatcher w;
        QSignalSpy spy(&w, &TrackWatcher::polled);
        QVERIFY(!w.busy());
        QVERIFY(w.poll());
        QVERIFY(w.busy());
        QCOMPARE(spy.count(), 0);              // nic synchronicznie
        QVERIFY(!w.poll());                    // tick pominięty, gdy poprzednie trwa
        QVERIFY(!w.poll());
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 1);              // dokładnie jeden wynik mimo trzech wywołań
        QVERIFY(!w.busy());
        QVERIFY(w.poll());                     // po zakończeniu znowu wolno
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.count(), 2);
        // Wynik zasila detektor tak samo jak wcześniej synchroniczny odczyt.
        TrackChangeDetector d;
        const QList<QVariant> args = spy.last();
        QVERIFY(!d.update(QStringLiteral("p"), args.at(1).toBool() ? args.at(0).toString() : QString(), 0, true));
    }

    // ---------------------------------------------------------- zapamiętywanie pozycji
    static QString primaryName() { return QGuiApplication::primaryScreen()->name(); }
    static QJsonObject writePos(const QString &screen, double x, int dir)
    {
        QJsonObject s = cfg::loadSettings();
        s.insert(QStringLiteral("position"), QJsonObject{{QStringLiteral("screen"), screen},
                                                          {QStringLiteral("x"), x}, {QStringLiteral("dir"), dir}});
        cfg::saveSettings(s);
        return s;
    }

    void positionRoundTrip()
    {
        PetEnv env;
        double x;
        {
            Pet pet;
            PetProbe pr{pet};
            pr.setX(pr.centerX() - 123);
            pr.setDir(-1);
            pr.savePosition();
            x = pr.x();
        }
        cfg::flushSettings();
        const QJsonObject pos = cfg::loadSettings().value(QStringLiteral("position")).toObject();
        QCOMPARE(pos.value(QStringLiteral("screen")).toString(), primaryName());
        QVERIFY(!pos.contains(QStringLiteral("y")));
        for (int dir : {-1, 1}) {
            {
                Pet pet;
                PetProbe pr{pet};
                pr.setDir(dir);
                pr.setX(x);
                pr.savePosition();
            }
            cfg::flushSettings();
            Pet pet2;
            PetProbe p2{pet2};
            QVERIFY(p2.floor());                        // bez spadania na starcie
            QCOMPARE(p2.x(), x);
            QCOMPARE(p2.dir(), dir);
            QCOMPARE(p2.y(), p2.floorY());
        }
    }

    void positionFallbackToCenter()
    {
        PetEnv env;
        double center;
        {
            Pet pet;
            center = PetProbe{pet}.centerX();
        }
        const QList<QByteArray> bad = {"to nie json", "[1,2]", "{\"position\":5}", "{\"position\":{}}",
                                       "{\"position\":{\"screen\":3,\"x\":10}}",
                                       "{\"position\":{\"screen\":\"offscreen\",\"x\":\"abc\"}}"};
        for (const QByteArray &b : bad) {
            writeFile(cfg::settingsFile(), QString::fromUtf8(b));
            Pet pet;
            PetProbe pr{pet};
            QVERIFY2(pr.floor(), b.constData());
            QCOMPARE(pr.x(), center);
        }
        QFile::remove(cfg::settingsFile());                    // brak pliku
        { Pet pet; QCOMPARE(PetProbe{pet}.x(), center); }
        writePos(QStringLiteral("odlaczony-monitor"), 40, -1);   // ekranu nie ma
        Pet pet;
        QCOMPARE(PetProbe{pet}.x(), center);
    }

    void positionClampedToWorkarea()
    {
        PetEnv env;
        double lo, hi;
        {
            Pet pet;
            PetProbe pr{pet};
            const QRect a = pr.areaNow();
            lo = a.left();
            hi = a.right() + 1 - pr.bodyW();
        }
        const double left = QGuiApplication::primaryScreen()->geometry().left();
        writePos(primaryName(), 1e6, 1);
        { Pet pet; QCOMPARE(PetProbe{pet}.x(), hi); }
        writePos(primaryName(), -1e6, 1);
        { Pet pet; QCOMPARE(PetProbe{pet}.x(), lo); }
        writePos(primaryName(), (lo + hi) / 2 - left, 1);
        { Pet pet; QCOMPARE(PetProbe{pet}.x(), (lo + hi) / 2); }
    }

    void landingAfterThrowSavesPosition()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        cfg::flushSettings();
        QVERIFY(!cfg::loadSettings().contains(QStringLiteral("position")));     // sam start nie zapisuje
        pr.setX(pr.centerX() - 200);
        // Rzut myszą: złap, przeciągnij, puść.
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&pet, QPoint(60, -200));
        QVERIFY(pr.drag());
        cfg::flushSettings();
        QVERIFY(!cfg::loadSettings().contains(QStringLiteral("position")));     // w locie nie zapisuje
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(60, -200));
        QVERIFY(pr.fall());
        for (int i = 0; i < 2000 && !pr.floor(); ++i)
            pr.step();
        QVERIFY(pr.floor());
        cfg::flushSettings();
        const QJsonObject pos = cfg::loadSettings().value(QStringLiteral("position")).toObject();
        QCOMPARE(pos.value(QStringLiteral("x")).toDouble() + QGuiApplication::primaryScreen()->geometry().left(), pr.x());
        QCOMPARE(int(pos.value(QStringLiteral("dir")).toInt()), pr.dir());
    }

    void positionSaveKeepsOtherKeysAndIsPeriodic()
    {
        PetEnv env;
        writeFile(cfg::settingsFile(), QStringLiteral("{\"mode\":\"auto\",\"obce\":[1,2]}"));
        Pet pet;
        PetProbe pr{pet};
        pr.setMode(QStringLiteral("walk"));
        pr.savePosition();
        cfg::flushSettings();
        QJsonObject s = cfg::loadSettings();
        QCOMPARE(s.value(QStringLiteral("mode")).toString(), QStringLiteral("walk"));
        QCOMPARE(s.value(QStringLiteral("obce")).toArray().size(), 2);
        QVERIFY(s.contains(QStringLiteral("position")));
        // Spacer: zapis co ~10 s (250 ticków), a nie co tick.
        QFile::remove(cfg::settingsFile());
        pr.setActivity(QStringLiteral("walk"), 100000);
        for (int i = 0; i < 100; ++i) pr.step();
        cfg::flushSettings();
        QVERIFY(!QFileInfo::exists(cfg::settingsFile()));
        for (int i = 0; i < 200; ++i) pr.step();
        cfg::flushSettings();
        QVERIFY(cfg::loadSettings().contains(QStringLiteral("position")));
    }

    void returnToCenterMenuAction()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pr.setX(pr.centerX() - 300);
        QMenu menu;
        pr.buildMenu(menu);
        QAction *act = nullptr;
        for (QAction *a : menu.actions())
            if (a->text() == QStringLiteral("Wróć na środek ekranu"))
                act = a;
        QVERIFY(act);
        act->trigger();
        QCOMPARE(pr.x(), pr.centerX());
        QVERIFY(pr.floor());
        cfg::flushSettings();
        const QJsonObject pos = cfg::loadSettings().value(QStringLiteral("position")).toObject();
        QCOMPARE(pos.value(QStringLiteral("x")).toDouble() + QGuiApplication::primaryScreen()->geometry().left(), pr.x());
    }

    // Odczyt obszaru roboczego (xprop, async) przycina x stojącej postaci; bez skoku, gdy x się mieści.
    void workareaClampsFloorX()
    {
        PetEnv env;
        const QString bin = env.tmp.path() + QStringLiteral("/bin/xprop");
        const QString log = env.tmp.path() + QStringLiteral("/xprop.log");
        const QRect g = QGuiApplication::primaryScreen()->geometry();
        auto setPanels = [&](int l, int r) {       // panel z lewej o szerokości l, z prawej r
            PetEnv::script(bin, QStringLiteral("echo x >> %1\necho '_NET_WORKAREA(CARDINAL) = %2, %3, %4, %5'\n")
                                    .arg(log).arg(g.left() + l).arg(g.top()).arg(g.width() - l - r).arg(g.height()));
        };
        auto calls = [&] { QFile f(log); return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0; };
        setPanels(0, 0);
        Pet pet;
        PetProbe pr{pet};
        QTRY_VERIFY_WITH_TIMEOUT(pr.workarea().has_value(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(calls(), 1, 3000);
        QTest::qWait(100);                              // callback pierwszego odczytu już wykonany
        const double left = g.left();
        // Panel z lewej 150 px, postać przy krawędzi ekranu -> przesunięta za panel (i zapisana).
        setPanels(150, 0);
        pr.setX(left);
        pr.refreshWorkarea();
        QTRY_COMPARE_WITH_TIMEOUT(pr.x(), left + 150, 3000);
        QVERIFY(pr.floor());
        cfg::flushSettings();
        QCOMPARE(cfg::loadSettings().value(QStringLiteral("position")).toObject().value(QStringLiteral("x")).toDouble(), 150.0);
        // Postać mieści się -> bez skoku.
        const double mid = left + g.width() / 2.0;
        pr.setX(mid);
        const int n = calls();
        pr.refreshWorkarea();
        QTRY_COMPARE_WITH_TIMEOUT(calls(), n + 1, 3000);
        QTest::qWait(150);
        QCOMPARE(pr.x(), mid);
        // Panel z prawej 200 px, postać przy prawej krawędzi.
        setPanels(0, 200);
        pr.setX(left + g.width() - pr.bodyW());
        pr.refreshWorkarea();
        QTRY_COMPARE_WITH_TIMEOUT(pr.x(), left + g.width() - 200 - pr.bodyW(), 3000);
        // W locie nic się nie przycina.
        setPanels(150, 0);
        pr.startFall(300);
        pr.setX(left);
        const int m = calls();
        pr.refreshWorkarea();
        QTRY_COMPARE_WITH_TIMEOUT(calls(), m + 1, 3000);
        QTest::qWait(150);
        QCOMPARE(pr.x(), left);
        QVERIFY(pr.fall());
    }

    // ---------------------------------------------------------- przerywanie animacji
    // Fałszywe zmiany tytułu (puste metadane, reklama bez wykonawcy, przełączanie odtwarzaczy) nie dają radości.
    void petIgnoresFalseTrackChanges()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pr.queue();
        const QString a = QStringLiteral("A\nx"), b = QStringLiteral("B\ny");
        pr.trackPolled(a, true);
        for (const QString &k : {QStringLiteral("\n"), QStringLiteral("Reklama\n"), QString(), QStringLiteral("\nx"), a, b, a, b, a})
            pr.trackPolled(k, true);
        pr.trackPolled(b, false);                                  // pauza - klucz ignorowany
        pr.trackPolled(b, false);
        QCOMPARE(pr.reactions(), size_t(0));
        pr.trackPolled(b, true);                                   // prawdziwa zmiana potrzebuje 2 odczytów
        QCOMPARE(pr.reactions(), size_t(0));
        pr.trackPolled(b, true);
        QCOMPARE(pr.reactions(), size_t(1));
        QCOMPARE(pr.queue().front().first, QStringLiteral("cheer"));
        // Kolejna prawdziwa zmiana w cooldownie (60 s) - bez reakcji.
        pr.step();
        while (pr.reactions() > 0)
            pr.step();
        const QString c = QStringLiteral("C\nz");
        pr.trackPolled(c, true);
        pr.trackPolled(c, true);
        QCOMPARE(pr.reactions(), size_t(0));
    }

    // clampToWorkarea nie rusza postaci mieszczącej się w obszarze: x, kierunek, animacja i jej licznik bez zmian.
    void clampInsideAreaChangesNothing()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pr.setMode(QStringLiteral("leek"));
        for (int i = 0; i < 30; ++i)
            pr.step();
        for (const int dir : {1, -1}) {
            pr.setDir(dir);
            pr.setX(pr.centerX());
            const double x = pr.x();
            const int tick = pr.animTick();
            const QString anim = pr.anim();
            pr.clamp();
            QCOMPARE(pr.x(), x);
            QCOMPARE(pr.dir(), dir);
            QCOMPARE(pr.animTick(), tick);
            QCOMPARE(pr.anim(), anim);
        }
        // Poza obszarem przycina tylko x - kierunek zostaje.
        pr.setDir(-1);
        pr.setX(pr.areaNow().right() + 500.0);
        pr.clamp();
        QVERIFY(pr.x() <= pr.areaNow().right() + 1 - pr.bodyW());
        QCOMPARE(pr.dir(), -1);
    }

    // Ponowne ustawienie tej samej aktywności/animacji (zapis pozycji, losowanie tej samej czynności) nie resetuje animacji.
    void sameActivityDoesNotResetAnimTick()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pr.setMode(QStringLiteral("leek"));
        for (int i = 0; i < 40; ++i)
            pr.step();
        QCOMPARE(pr.anim(), QStringLiteral("leek"));
        int tick = pr.animTick();
        pr.setAnimNow(QStringLiteral("leek"));
        QCOMPARE(pr.animTick(), tick);
        pr.savePosition();
        pr.step();
        QCOMPARE(pr.animTick(), tick + 1);
        // Losowanie, które znowu daje główną czynność, też nie resetuje (pętla aż do trafienia).
        for (int tries = 0; tries < 500; ++tries) {
            const bool wasLeek = pr.anim() == QLatin1String("leek");
            pr.setActivity(QStringLiteral("leek"), 1);
            tick = pr.animTick();
            pr.step();                                // activityLeft -> 0 -> pickActivity()
            if (wasLeek && pr.activity() == QLatin1String("leek")) {
                QCOMPARE(pr.animTick(), tick + 1);
                QCOMPARE(pr.anim(), QStringLiteral("leek"));
            }
        }
    }

    // Reakcje krótsze niż próg są wydłużane do progu (żadnych jednoklatkowych mignięć).
    void shortReactionsAreStretched()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pr.react(QStringLiteral("cheer"), 1);
        QCOMPARE(pr.queue().front().second, MIN_REACTION_TICKS);
        int n = 0;
        while (pr.reactions() > 0 && n < 100) { pr.step(); ++n; }
        QCOMPARE(n, MIN_REACTION_TICKS);
        // Stałe reakcji w kodzie nie schodzą poniżej progu.
        QVERIFY(SOFT_LAND_TICKS >= MIN_REACTION_TICKS);
    }

    // Przerywniki w trybach z jedną główną czynnością są rzadkie i nigdy dwa pod rząd.
    void sideActivitiesAreRareAndNeverConsecutive()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        for (const QString &key : {QStringLiteral("leek"), QStringLiteral("dance")}) {
            pr.setMode(key);
            const Mode *m = findMode(key);
            const QString main = QLatin1String(m->activities.front().name);
            int side = 0;
            bool prevSide = false;
            const int N = 4000;
            for (int i = 0; i < N; ++i) {
                pr.pick();
                const bool isSide = pr.activity() != main;
                QVERIFY2(!(isSide && prevSide), qPrintable(key));
                side += isSide;
                prevSide = isSide;
            }
            QVERIFY2(side < N * 12 / 100, qPrintable(QStringLiteral("%1: %2 przerywników na %3").arg(key).arg(side).arg(N)));
            QVERIFY(side > 0);
        }
        // W żadnym trybie poboczna aktywność nie powtarza się dwa razy z rzędu.
        for (const Mode &m : modes()) {
            pr.setMode(QString::fromLatin1(m.key));
            if (m.activities.size() < 2)
                continue;
            const QString main = QLatin1String(m.activities.front().name);
            QString prev = pr.activity();
            for (int i = 0; i < 2000; ++i) {
                pr.pick();
                if (pr.activity() != main)
                    QVERIFY2(pr.activity() != prev, m.key);
                prev = pr.activity();
            }
        }
    }

    // 10 minut w każdym trybie: brak spontanicznych reakcji, żadnych mignięć (< MIN_REACTION_TICKS), przerywniki w granicach.
    void tenMinutesPerModeInterruptions()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        for (const Mode &m : modes()) {
            const QString key = QString::fromLatin1(m.key);
            pr.setMode(key);
            const QString main = QLatin1String(m.activities.front().name);
            QString cur = pr.anim();
            int len = 0, segments = 0, sideSegments = 0, maxSide = 0, shortSeg = 0, firstSeg = 1;
            for (int t = 0; t < 25 * 600; ++t) {
                pr.step();
                QVERIFY2(pr.reactions() == 0 && pr.floor(), qPrintable(key));
                if (pr.anim() != cur) {
                    if (!firstSeg && len < MIN_REACTION_TICKS)
                        ++shortSeg;
                    firstSeg = 0;
                    ++segments;
                    if (cur != main)
                        maxSide = std::max(maxSide, len), ++sideSegments;
                    cur = pr.anim();
                    len = 0;
                }
                ++len;
            }
            QVERIFY2(shortSeg == 0, qPrintable(key));
            const bool calm = key == QLatin1String("leek") || key == QLatin1String("dance");
            if (calm) {
                QVERIFY2(sideSegments <= 20, qPrintable(QStringLiteral("%1: %2 przerywników").arg(key).arg(sideSegments)));
                QVERIFY2(maxSide <= 100, qPrintable(QStringLiteral("%1: przerywnik %2 ticków").arg(key).arg(maxSide)));
            }
            QVERIFY2(segments <= 25 * 600 / 25, qPrintable(key));        // aktywności trwają >= 1 s
        }
        cfg::flushSettings();
    }

    // ---------------------------------------------------------- ostrzeżenia GPU (LACT)
    void initTestCase()
    {
        // Żaden test nie dotyka prawdziwego gniazda /run/lactd.sock.
        qRegisterMetaType<GpuTemps>();
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    void lactParseDeviceList()
    {
        QString err;
        QCOMPARE(parseLactDeviceId(LACT_LIST, &err), QString::fromLatin1(LACT_ID));
        // Jedna karta (nawet zintegrowana) - ta.
        QCOMPARE(parseLactDeviceId("{\"status\":\"ok\",\"data\":[{\"id\":\"x\",\"name\":\"Raphael\",\"device_type\":\"Integrated\"}]}"),
                 QStringLiteral("x"));
        // Kilka: dyskretna AMD przed dyskretną innej firmy, nawet gdy jest dalej.
        QCOMPARE(parseLactDeviceId("{\"status\":\"ok\",\"data\":[{\"id\":\"n\",\"name\":\"NVIDIA X\",\"device_type\":\"Dedicated\"},"
                                   "{\"id\":\"a\",\"name\":\"AMD Radeon\",\"device_type\":\"Dedicated\"}]}"),
                 QStringLiteral("a"));
        QCOMPARE(parseLactDeviceId("{\"status\":\"ok\",\"data\":[]}", &err), QString());
        QVERIFY(!err.isEmpty());
        QCOMPARE(parseLactDeviceId("{\"status\":\"error\",\"data\":{\"description\":\"x\"}}", &err), QString());
        QCOMPARE(parseLactDeviceId("to nie json", &err), QString());
    }

    void lactParseStats()
    {
        QString err;
        const auto t = parseLactStats(LACT_STATS, &err);
        QVERIFY(t);
        QCOMPARE(*t->edge, 29.0);
        QCOMPARE(*t->hotspot, 31.0);          // junction
        QVERIFY(!parseLactStats("{\"status\":\"error\",\"data\":{\"description\":\"x\"}}", &err));
        QVERIFY(!parseLactStats("{\"status\":\"ok\",\"data\":{\"temps\":{}}}", &err));
        QVERIFY(!parseLactStats("", &err));
        const auto only = parseLactStats("{\"status\":\"ok\",\"data\":{\"temps\":{\"edge\":{\"current\":50}}}}");
        QVERIFY(only && only->edge && !only->hotspot);
    }

    void gpuConfigDefaultsAndRoundTrip()
    {
        QVERIFY(GpuAlertConfig::fromSettings({}).enabled);
        QCOMPARE(GpuAlertConfig::fromSettings({}).edge, 85);
        QCOMPARE(GpuAlertConfig::fromSettings({}).hotspot, 100);
        const QJsonObject bad{{"gpu_alert", QJsonObject{{"enabled", "tak"}, {"edge", "x"}, {"hotspot", 9999}}}};
        const GpuAlertConfig c = GpuAlertConfig::fromSettings(bad);
        QVERIFY(c.enabled);
        QCOMPARE(c.edge, 85);
        QCOMPARE(c.hotspot, 150);
        GpuAlertConfig d;
        d.enabled = false;
        d.edge = 75;
        d.hotspot = 110;
        const GpuAlertConfig back = GpuAlertConfig::fromSettings(QJsonObject{{"gpu_alert", d.toJson()}});
        QVERIFY(!back.enabled);
        QCOMPARE(back.edge, 75);
        QCOMPARE(back.hotspot, 110);
    }

    // Progi: 2 odczyty z rzędu, histereza 5 °C, 5 minut przy stale wysokiej, edge + hotspot razem.
    void gpuAlertLogic()
    {
        const qint64 S = 1000, MIN = 60 * 1000;
        GpuAlertLogic l;
        l.setThresholds(85, 100);
        qint64 now = 0;
        auto up = [&](double e, double h) { now += 3 * S; return l.update(GpuTemps{e, h}, now); };
        QVERIFY(!up(70, 80));
        QVERIFY(!up(86, 80));                      // pierwszy odczyt ponad progiem - jeszcze nic
        const auto a = up(87, 80);                 // drugi z rzędu
        QVERIFY(a);
        QCOMPARE(a->text, QStringLiteral("GPU ma 87°C!"));
        QVERIFY(!a->severe);
        QVERIFY(!up(88, 80));                      // stale wysoka - cisza
        QVERIFY(!up(89, 80));
        // Pojedyncze odczyty ponad progiem między normalnymi nie liczą się.
        GpuAlertLogic blip;
        blip.setThresholds(85, 100);
        for (int i = 0; i < 6; ++i)
            QVERIFY(!blip.update(GpuTemps{i % 2 ? 60.0 : 95.0, 80}, i * 3000));
        // Spadek o mniej niż 5 °C i powrót: bez nowego ostrzeżenia (jeszcze nie minęło 5 min).
        now += 10 * S;
        QVERIFY(!up(81, 80));                      // 81 > 85-5? nie: 81 > 80, więc nie uzbraja
        QVERIFY(!up(90, 80));
        QVERIFY(!up(90, 80));
        // Spadek o >= 5 °C (<= 80) i ponowne przekroczenie: ostrzeżenie po dwóch odczytach.
        QVERIFY(!up(80, 80));
        QVERIFY(!up(86, 80));
        const auto b = up(86, 80);
        QVERIFY(b);
        QCOMPARE(b->text, QStringLiteral("GPU ma 86°C!"));
        // Stale wysoka: powtórka dopiero po 5 minutach.
        for (int i = 0; i < 90; ++i)               // 90 * 3 s = 4,5 min
            QVERIFY(!up(88, 80));
        std::optional<GpuAlertLogic::Alert> again;
        for (int i = 0; i < 20 && !again; ++i)
            again = up(88, 80);
        QVERIFY(again);
        QVERIFY(now - 0 > 5 * MIN);
        // Tylko hotspot, z oznaczeniem poważnego (>= próg + 10).
        GpuAlertLogic h;
        h.setThresholds(85, 100);
        QVERIFY(!h.update(GpuTemps{60, 105}, 0));
        const auto hs = h.update(GpuTemps{60, 111}, 3000);
        QVERIFY(hs);
        QCOMPARE(hs->text, QStringLiteral("Hotspot 111°C – gorąco!"));
        QVERIFY(hs->severe);
        // Oba naraz: jeden tekst.
        GpuAlertLogic both;
        both.setThresholds(85, 100);
        QVERIFY(!both.update(GpuTemps{90, 105}, 0));
        const auto ab = both.update(GpuTemps{91, 106}, 3000);
        QVERIFY(ab);
        QCOMPARE(ab->text, QStringLiteral("GPU ma 91°C, hotspot 106°C – gorąco!"));
        QVERIFY(!both.update(GpuTemps{91, 106}, 6000));
        // Hotspot przekracza później niż edge (który już ostrzegł i nadal jest wysoki): ostrzeżenie o hotspocie
        // niesie też aktualny edge, w jednym dymku.
        GpuAlertLogic lag;
        lag.setThresholds(85, 100);
        QVERIFY(!lag.update(GpuTemps{90, 80}, 0));
        QVERIFY(lag.update(GpuTemps{90, 80}, 3000));
        QVERIFY(!lag.update(GpuTemps{90, 101}, 6000));
        const auto lh = lag.update(GpuTemps{90, 101}, 9000);
        QVERIFY(lh);
        QCOMPARE(lh->text, QStringLiteral("GPU ma 90°C, hotspot 101°C – gorąco!"));
        // Brak odczytu (LACT zniknął) przerywa serię "z rzędu".
        GpuAlertLogic gap;
        gap.setThresholds(85, 100);
        QVERIFY(!gap.update(GpuTemps{90, 50}, 0));
        gap.reset();
        QVERIFY(!gap.update(GpuTemps{90, 50}, 3000));
        QVERIFY(gap.update(GpuTemps{90, 50}, 6000));
    }

    void lactClientTalksReadOnly()
    {
        FakeLact srv;
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(60, 200, 1000);
        QSignalSpy spy(&c, &LactClient::reading);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 3000);
        const GpuTemps t = qvariant_cast<GpuTemps>(spy.first().first());
        QCOMPARE(*t.edge, 62.0);
        QCOMPARE(*t.hotspot, 78.0);
        QVERIFY(c.available());
        QVERIFY(srv.onlyReadCommands());
        QCOMPARE(srv.commands.first(), QStringLiteral("list_devices"));   // lista tylko raz, potem same odczyty
        QCOMPARE(srv.commands.count(QStringLiteral("list_devices")), 1);
        QVERIFY(srv.commands.count(QStringLiteral("device_stats")) >= 3);
        // Wyłączenie zatrzymuje odpytywanie.
        c.setEnabled(false);
        const int n = srv.commands.size();
        QTest::qWait(300);
        QCOMPARE(srv.commands.size(), n);
        QVERIFY(!c.latest());
    }

    void lactClientTimeoutDoesNotBlock()
    {
        FakeLact srv;
        srv.silent = true;
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(60, 5000, 300);
        QSignalSpy bad(&c, &LactClient::unavailable);
        // Pętla zdarzeń cały czas żyje: odstępy między tykami timera 10 ms.
        QElapsedTimer clock;
        clock.start();
        qint64 last = 0, worst = 0;
        QTimer ticker;
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            worst = std::max(worst, now - last);
            last = now;
        });
        ticker.start();
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() == 1, 3000);
        QCOMPARE(bad.first().first().toString(), QStringLiteral("brak odpowiedzi"));
        QVERIFY2(worst < 100, qPrintable(QString::number(worst)));
        QVERIFY(!c.available());
        QVERIFY(srv.onlyReadCommands());
    }

    void lactClientMissingSocket()
    {
        QTemporaryDir dir;
        LactClient c;
        c.setSocketPath(dir.filePath(QStringLiteral("nie-ma.sock")));
        c.setIntervals(60, 200, 500);
        QSignalSpy bad(&c, &LactClient::unavailable);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() >= 2, 3000);     // ponawia w interwale ponowień
        QVERIFY(c.reason().contains(QStringLiteral("gniazda")));
        // Gniazdo pojawia się później: sam się podłącza.
        FakeLact srv;
        c.setSocketPath(srv.path());
        QSignalSpy ok(&c, &LactClient::reading);
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 1, 3000);
        QVERIFY(c.reason().isEmpty());
    }

    void lactClientBadResponses()
    {
        FakeLact srv;
        srv.list = "{\"status\":\"ok\",\"data\":[]}";
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(60, 150, 500);
        QSignalSpy bad(&c, &LactClient::unavailable);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() >= 1, 3000);
        QVERIFY(c.reason().contains(QStringLiteral("GPU")));
        srv.list = QByteArray(LACT_LIST);
        srv.stats = "to nie json";
        const int before = bad.count();
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() > before + 1, 3000);
        QVERIFY(srv.onlyReadCommands());
    }

    // Zegar przypomnienia należy do czujnika, który sam ostrzega: świeże przekroczenie hotspotu po edge nie
    // przesuwa przypomnienia o edge o kolejne 5 minut, a przypomnienie niesie oba odczyty i zeruje oba zegary.
    void gpuAlertLogicJoinedSensorKeepsClock()
    {
        const qint64 S = 1000, MIN = 60 * S;
        GpuAlertLogic l;
        l.setThresholds(85, 100);
        QVERIFY(!l.update(GpuTemps{90, 70}, 0));
        QVERIFY(l.update(GpuTemps{90, 70}, 3 * S));                       // edge: ostrzeżenie w t=3 s
        QVERIFY(!l.update(GpuTemps{90, 101}, 60 * S));
        const auto hs = l.update(GpuTemps{90, 101}, 63 * S);              // hotspot dołącza po minucie
        QVERIFY(hs);
        QCOMPARE(hs->text, QStringLiteral("GPU ma 90°C, hotspot 101°C – gorąco!"));
        // Nic wcześniej niż 5 min po ostrzeżeniu o edge (t=3 s), nawet mimo ostrzeżenia o hotspocie w t=63 s.
        for (qint64 t = 66 * S; t < 3 * S + 5 * MIN; t += 3 * S)
            QVERIFY2(!l.update(GpuTemps{90, 101}, t), qPrintable(QString::number(t)));
        const auto rep = l.update(GpuTemps{90, 101}, 3 * S + 5 * MIN);    // przypomnienie o edge (z hotspotem)
        QVERIFY(rep);
        QCOMPARE(rep->text, QStringLiteral("GPU ma 90°C, hotspot 101°C – gorąco!"));
        // Hotspot został uwzględniony w przypomnieniu, więc nie ostrzega osobno minutę później.
        for (qint64 t = 3 * S + 5 * MIN + 3 * S; t < 3 * S + 10 * MIN; t += 3 * S)
            QVERIFY2(!l.update(GpuTemps{90, 101}, t), qPrintable(QString::number(t)));
        QVERIFY(l.update(GpuTemps{90, 101}, 3 * S + 10 * MIN));
    }

    void lactClientChunkedAndBurstResponses()
    {
        for (FakeLact::Mode m : {FakeLact::Chunked, FakeLact::Burst}) {
            FakeLact srv;
            srv.mode = m;
            LactClient c;
            c.setSocketPath(srv.path());
            c.setIntervals(40, 150, 1000);
            QSignalSpy ok(&c, &LactClient::reading);
            QSignalSpy bad(&c, &LactClient::unavailable);
            c.setEnabled(true);
            QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 3, 5000);
            QCOMPARE(bad.count(), 0);
            const GpuTemps t = qvariant_cast<GpuTemps>(ok.last().first());
            QCOMPARE(*t.edge, 62.0);
            QCOMPARE(*t.hotspot, 78.0);
            QCOMPARE(srv.commands.count(QStringLiteral("list_devices")), 1);
            QVERIFY(srv.rawIsExactlyReadOnly());
        }
    }

    void lactClientDroppedConnectionsRecover()
    {
        FakeLact srv;
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(40, 80, 1000);
        QSignalSpy ok(&c, &LactClient::reading);
        QSignalSpy bad(&c, &LactClient::unavailable);
        for (FakeLact::Mode m : {FakeLact::DropAfterRequest, FakeLact::HalfThenDrop}) {
            srv.mode = m;
            bad.clear();
            c.setEnabled(false);
            c.setEnabled(true);
            QTRY_VERIFY_WITH_TIMEOUT(bad.count() >= 2, 4000);        // bez zawieszenia, ponawia
            QVERIFY(!c.available());
            QVERIFY(c.reason().contains(QStringLiteral("LACT")));
        }
        srv.mode = FakeLact::Normal;
        ok.clear();
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 2, 4000);             // wraca sam
        QVERIFY(c.available());
        QVERIFY(srv.onlyReadCommands());
    }

    // Restart lactd: gniazdo znika (ServerNotFound), potem wraca - klient sam wznawia i zaczyna od list_devices.
    void lactClientSurvivesDaemonRestart()
    {
        FakeLact srv;
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(40, 80, 1000);
        QSignalSpy ok(&c, &LactClient::reading);
        QSignalSpy bad(&c, &LactClient::unavailable);
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 2, 4000);
        srv.server.close();
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() >= 1, 4000);
        QVERIFY(!c.available());
        const int lists = srv.commands.count(QStringLiteral("list_devices"));
        QVERIFY(srv.server.listen(srv.path()));
        ok.clear();
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 2, 4000);
        QCOMPARE(srv.commands.count(QStringLiteral("list_devices")), lists + 1);   // lista od nowa po restarcie
        QVERIFY(srv.rawIsExactlyReadOnly());
    }

    // Długie działanie: 1000 odczytów, potem 200 zerwanych połączeń - ani gniazd Qt, ani deskryptorów, ani
    // połączeń po stronie serwera nie przybywa; wyłączenie zamyka wszystko.
    void lactClientNoSocketLeakOver1000Cycles()
    {
        auto fds = [] { return QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot).size(); };
        FakeLact srv;
        LactClient c;
        c.setSocketPath(srv.path());
        c.setIntervals(1, 1, 1000);
        QSignalSpy ok(&c, &LactClient::reading);
        QSignalSpy bad(&c, &LactClient::unavailable);
        const int fdBase = fds();
        c.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(ok.count() >= 1000, 60000);
        QVERIFY(c.findChildren<QLocalSocket *>().size() <= 3);
        QVERIFY(srv.open <= 3);
        QVERIFY2(fds() <= fdBase + 8, qPrintable(QString::number(fds() - fdBase)));
        QCOMPARE(srv.commands.count(QStringLiteral("list_devices")), 1);
        srv.mode = FakeLact::DropAfterRequest;
        bad.clear();
        QTRY_VERIFY_WITH_TIMEOUT(bad.count() >= 200, 60000);
        QVERIFY(c.findChildren<QLocalSocket *>().size() <= 3);
        QVERIFY(srv.open <= 3);
        QVERIFY2(fds() <= fdBase + 8, qPrintable(QString::number(fds() - fdBase)));
        c.setEnabled(false);
        QTRY_COMPARE_WITH_TIMEOUT(srv.open, 0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(c.findChildren<QLocalSocket *>().isEmpty(), 2000);
        QVERIFY2(fds() <= fdBase + 2, qPrintable(QString::number(fds() - fdBase)));
        QVERIFY(srv.rawIsExactlyReadOnly());
    }

    // Pełny cykl w programie: start, odpytywanie, menu, wyłączenie, ponowne włączenie, brak gniazda, wznowienie.
    // Wszystkie wysłane do (atrapy) LACT komendy muszą należeć do listy dozwolonych (odczytowych).
    void gpuFullCycleOnlyAllowedCommands()
    {
        PetEnv env;
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        {
            Pet pet;
            PetProbe pr{pet};
            pr.lact().setIntervals(40, 80, 1000);
            QTRY_VERIFY_WITH_TIMEOUT(srv.commands.count(QStringLiteral("device_stats")) >= 3, 4000);   // start + odpytywanie
            auto find = [](QMenu &m, const QString &text) -> QAction * {
                for (QAction *a : m.actions())
                    if (a->text() == text)
                        return a;
                return nullptr;
            };
            const int connsBefore = srv.connections;
            for (int i = 0; i < 20; ++i) {                          // menu: dane z cache, żadnych połączeń
                QMenu m;
                pr.buildMenu(m);
                QVERIFY(find(m, QStringLiteral("Ostrzeżenia o temperaturze GPU")));
            }
            QCOMPARE(srv.connections, connsBefore);
            // Progi w menu nic nie wysyłają do LACT.
            {
                QMenu m;
                pr.buildMenu(m);
                QAction *thr = find(m, QStringLiteral("Progi"));
                QVERIFY(thr);
                for (QAction *a : thr->menu()->actions())
                    if (a->text() == QStringLiteral("90°C"))
                        a->trigger();
            }
            // Wyłączenie: cisza i zamknięte połączenia.
            {
                QMenu m;
                pr.buildMenu(m);
                find(m, QStringLiteral("Ostrzeżenia o temperaturze GPU"))->trigger();
            }
            QTRY_COMPARE_WITH_TIMEOUT(srv.open, 0, 3000);
            const int n = srv.commands.size();
            QTest::qWait(300);
            QCOMPARE(srv.commands.size(), n);
            // "Jaka jest temperatura?" przy wyłączonych ostrzeżeniach: jeden jednorazowy odczyt, bez odpytywania.
            {
                QMenu m;
                pr.buildMenu(m);
                find(m, QStringLiteral("Jaka jest temperatura?"))->trigger();
            }
            QTRY_COMPARE_WITH_TIMEOUT(srv.commands.size(), n + 2, 3000);
            QTest::qWait(300);
            QCOMPARE(srv.commands.size(), n + 2);
            const int n2 = srv.commands.size();
            Q_UNUSED(n2)
            // Ponowne włączenie.
            {
                QMenu m;
                pr.buildMenu(m);
                find(m, QStringLiteral("Ostrzeżenia o temperaturze GPU"))->trigger();
            }
            QTRY_VERIFY_WITH_TIMEOUT(srv.commands.size() >= n + 5, 4000);
            // Brak gniazda (demon padł) i wznowienie.
            srv.server.close();
            QTRY_VERIFY_WITH_TIMEOUT(!pr.lact().reason().isEmpty(), 4000);
            const int m1 = srv.commands.size();
            QTest::qWait(200);
            QCOMPARE(srv.commands.size(), m1);
            QVERIFY(srv.server.listen(srv.path()));
            QTRY_VERIFY_WITH_TIMEOUT(srv.commands.size() >= m1 + 3 && pr.lact().available(), 4000);
            cfg::flushSettings();
        }
        static const QStringList allowed{QStringLiteral("list_devices"), QStringLiteral("device_stats"),
                                         QStringLiteral("device_info")};
        QVERIFY(!srv.commands.isEmpty());
        for (const QString &c : srv.commands)
            QVERIFY2(allowed.contains(c), qPrintable(c));
        QVERIFY(srv.rawIsExactlyReadOnly());
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    // "Jaka jest temperatura?": tuż pod "Co ma robić?"; świeży odczyt z pamięci, bez nowego zapytania.
    void gpuAskTemperatureFromCache()
    {
        PetEnv env;
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(pr.lact().available(), 3000);
        QMenu menu;
        pr.buildMenu(menu);
        const QList<QAction *> acts = menu.actions();
        const int mode = int(acts.indexOf(findAction(menu, QStringLiteral("Co ma robić?"))));
        QAction *ask = findAction(menu, QStringLiteral("Jaka jest temperatura?"));
        QVERIFY(mode >= 0 && ask);
        QCOMPARE(int(acts.indexOf(ask)), mode + 1);
        const int n = srv.commands.size();
        ask->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);   // Popup: w następnym obiegu pętli
        QCOMPARE(pr.info()->message(), QStringLiteral("GPU 62°C · hotspot 78°C"));
        QCOMPARE(pr.queue().back().first, QStringLiteral("ponder"));
        QVERIFY(pr.info()->card() && pr.info()->card()->kind == TempCard::Reading);
        QCOMPARE(pr.info()->card()->temps.card, QStringLiteral("RX 9070 XT"));
        QCOMPARE(pr.info()->meterCount(), 2);
        QVERIFY(pr.info()->timeoutMs() >= 5500 && pr.info()->timeoutMs() <= 6500);
        QCOMPARE(srv.commands.size(), n);                      // z pamięci: nic nie wysłano
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    // ---- progi GPU: nowe wartości i "Własny…"

    // Etykiety jednej grupy ("GPU (edge)" albo "Hotspot (junction)") w podmenu Progi.
    static QList<QAction *> thresholdGroup(QMenu &thr, const QString &section)
    {
        QList<QAction *> out;
        bool in = false;
        for (QAction *a : thr.actions()) {
            if (a->isSeparator()) {
                in = (a->text() == section);
                continue;
            }
            if (in)
                out << a;
        }
        return out;
    }

    static QStringList labelsOf(const QList<QAction *> &l)
    {
        QStringList r;
        for (QAction *a : l)
            r << a->text();
        return r;
    }

    void gpuThresholdMenuValuesAndCustom()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        auto groups = [&](QMenu &m, QList<QAction *> &edge, QList<QAction *> &hot) {
            pr.buildMenu(m);
            QAction *thr = findAction(m, QStringLiteral("Progi"));
            QVERIFY(thr && thr->menu());
            edge = thresholdGroup(*thr->menu(), QStringLiteral("GPU (edge)"));
            hot = thresholdGroup(*thr->menu(), QStringLiteral("Hotspot (junction)"));
        };
        QMenu m1;
        QList<QAction *> edge, hot;
        groups(m1, edge, hot);
        QCOMPARE(labelsOf(edge), (QStringList{QStringLiteral("60°C"), QStringLiteral("65°C"), QStringLiteral("70°C"),
            QStringLiteral("75°C"), QStringLiteral("80°C"), QStringLiteral("85°C"), QStringLiteral("90°C"),
            QStringLiteral("95°C"), QStringLiteral("Własny…")}));
        QCOMPARE(labelsOf(hot), (QStringList{QStringLiteral("75°C"), QStringLiteral("80°C"), QStringLiteral("85°C"),
            QStringLiteral("90°C"), QStringLiteral("95°C"), QStringLiteral("100°C"), QStringLiteral("105°C"),
            QStringLiteral("110°C"), QStringLiteral("115°C"), QStringLiteral("Własny…")}));
        QCOMPARE(pr.gpuCfg().edge, 85);                         // domyślne
        QCOMPARE(pr.gpuCfg().hotspot, 100);
        QVERIFY(findAction(m1, QStringLiteral("Progi")) != nullptr);
        for (QAction *a : edge)
            QCOMPARE(a->isChecked(), a->text() == QStringLiteral("85°C"));
        for (QAction *a : hot)
            QCOMPARE(a->isChecked(), a->text() == QStringLiteral("100°C"));
        // Nowe wartości zapisują się do settings.json.
        edge[0]->trigger();                                     // 60
        hot.at(8)->trigger();                                   // 115
        QCOMPARE(pr.gpuCfg().edge, 60);
        QCOMPARE(pr.gpuCfg().hotspot, 115);
        cfg::flushSettings();
        QJsonObject g = cfg::loadSettings().value("gpu_alert").toObject();
        QCOMPARE(g.value("edge").toInt(), 60);
        QCOMPARE(g.value("hotspot").toInt(), 115);
        // "Własny…" edge: okno z QSpinBox, wartość startowa = bieżąca, zakres 40-110.
        QMenu m2;
        groups(m2, edge, hot);
        QVERIFY(edge.at(0)->isChecked());
        edge.last()->trigger();
        auto dialog = [&]() -> QDialog * {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (w->objectName() == QLatin1String("gpuThresholdDialog") && w->isVisible())
                    return static_cast<QDialog *>(w);
            return nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(dialog(), 2000);
        QSpinBox *spin = dialog()->findChild<QSpinBox *>();
        QVERIFY(spin);
        QCOMPARE(spin->value(), 60);
        QCOMPARE(spin->minimum(), 40);
        QCOMPARE(spin->maximum(), 110);
        QCOMPARE(spin->singleStep(), 1);
        spin->setValue(500);
        QCOMPARE(spin->value(), 110);                           // zakres wymuszony
        spin->setValue(-3);
        QCOMPARE(spin->value(), 40);
        spin->setValue(87);
        dialog()->accept();
        QCOMPARE(pr.gpuCfg().edge, 87);
        QCOMPARE(pr.gpuCfg().hotspot, 115);
        cfg::flushSettings();
        g = cfg::loadSettings().value("gpu_alert").toObject();
        QCOMPARE(g.value("edge").toInt(), 87);
        QCOMPARE(g.value("hotspot").toInt(), 115);
        // Własny próg jest w menu zaznaczoną pozycją.
        QMenu m3;
        groups(m3, edge, hot);
        QVERIFY(labelsOf(edge).contains(QStringLiteral("87 °C (własny)")));
        for (QAction *a : edge)
            QCOMPARE(a->isChecked(), a->text() == QStringLiteral("87 °C (własny)"));
        QCOMPARE(labelsOf(edge).last(), QStringLiteral("Własny…"));
        // Anulowanie niczego nie zmienia; hotspot 50-125.
        hot.last()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(dialog(), 2000);
        spin = dialog()->findChild<QSpinBox *>();
        QCOMPARE(spin->value(), 115);
        QCOMPARE(spin->minimum(), 50);
        QCOMPARE(spin->maximum(), 125);
        spin->setValue(999);
        QCOMPARE(spin->value(), 125);
        spin->setValue(1);
        QCOMPARE(spin->value(), 50);
        spin->setValue(52);
        dialog()->reject();
        QCOMPARE(pr.gpuCfg().hotspot, 115);
        hot.last()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(dialog(), 2000);
        dialog()->findChild<QSpinBox *>()->setValue(118);
        dialog()->accept();
        QCOMPARE(pr.gpuCfg().hotspot, 118);
        QMenu m4;
        groups(m4, edge, hot);
        QVERIFY(labelsOf(hot).contains(QStringLiteral("118 °C (własny)")));
        // Wartość z pliku spoza listy też jest pokazana jako własna.
        QTRY_VERIFY_WITH_TIMEOUT(!dialog(), 2000);
    }

    void gpuThresholdCustomFromSettingsShownChecked()
    {
        PetEnv env;
        writeFile(cfg::settingsFile(), QStringLiteral("{\"gpu_alert\":{\"enabled\":true,\"edge\":73,\"hotspot\":101}}"));
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QMenu m;
        pr.buildMenu(m);
        QMenu *thr = findAction(m, QStringLiteral("Progi"))->menu();
        QAction *e = findAction(*thr, QStringLiteral("73 °C (własny)"));
        QAction *h = findAction(*thr, QStringLiteral("101 °C (własny)"));
        QVERIFY(e && h && e->isChecked() && h->isChecked());
    }

    // ---- zamykanie dymka temperatury

    // Dymek z menu to Qt::Popup pokazany w następnym obiegu pętli (nie w tym samym co zamknięcie QMenu).
    void gpuInfoBubbleIsPopupShownNextLoop()
    {
        PetEnv env;
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(pr.lact().available(), 3000);
        QVERIFY(pr.info()->isPopup());
        QCOMPARE(pr.info()->windowFlags() & Qt::WindowType_Mask, Qt::Popup);
        QVERIFY(!pr.alert()->isPopup());
        QMenu menu;
        pr.buildMenu(menu);
        findAction(menu, QStringLiteral("Jaka jest temperatura?"))->trigger();
        QVERIFY(!pr.info()->isVisible());                      // dopiero w następnym obiegu pętli
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
        QCOMPARE(QApplication::activePopupWidget(), static_cast<QWidget *>(pr.info()));
        QVERIFY(pr.info()->timeoutMs() >= 5500 && pr.info()->timeoutMs() <= 6500);
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    // Klik gdziekolwiek poza dymkiem (zdarzenie systemu okien na oknie offscreen) zamyka go.
    void gpuInfoBubbleClosesOnOutsideClick()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.showInfoNow(QStringLiteral("GPU 50°C · hotspot 60°C"));
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
        QWindow *outside = pet.windowHandle();
        QVERIFY(outside);
        QTest::mouseClick(outside, Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QVERIFY2(!pr.info()->isVisible(), "klik poza dymkiem musi go zamknąć");
        QVERIFY(pr.info()->justClosed());
    }

    void gpuInfoBubbleClosesOnEscAndOwnClick()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.showInfoNow(QStringLiteral("GPU 50°C · hotspot 60°C"));
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
        QTest::keyClick(pr.info(), Qt::Key_Escape);
        QVERIFY(!pr.info()->isVisible());
        pr.showInfoNow(QStringLiteral("GPU 51°C · hotspot 61°C"));
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
        QTest::mouseClick(pr.info(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(!pr.info()->isVisible());
    }

    // Auto-zamknięcie i wstrzymanie, dopóki kursor jest nad dymkiem.
    void gpuInfoBubbleTimeoutAndHover()
    {
        AlertBubble b(true);
        b.setMessage(QStringLiteral("GPU 50°C"));
        b.setTimeoutMs(200);
        b.show();
        QEnterEvent in(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QApplication::sendEvent(&b, &in);
        QTest::qWait(500);
        QVERIFY2(b.isVisible(), "kursor nad dymkiem wstrzymuje zamknięcie");
        QEvent out(QEvent::Leave);
        QApplication::sendEvent(&b, &out);
        QTRY_VERIFY_WITH_TIMEOUT(!b.isVisible(), 1500);
    }

    // Klik w Miku, który zamyka dymek, nie otwiera muzyki ani wyszukiwarki (też po upływie interwału podwójnego kliku).
    void gpuInfoCloseClickOnPetOpensNothing()
    {
        for (int viaWindow = 0; viaWindow < 2; ++viaWindow) {
            PetEnv env;
            Pet pet;
            PetProbe pr{pet};
            pet.show();
            QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
            pr.showInfoNow(QStringLiteral("GPU 50°C · hotspot 60°C"));
            QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
            if (viaWindow)
                QTest::mouseClick(pet.windowHandle(), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
            else
                QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
            QVERIFY(!pr.info()->isVisible());
            QTest::qWait(QApplication::doubleClickInterval() + 400);
            QVERIFY2(!pr.bubble()->isVisible(), "klik zamykający dymek nie otwiera muzyki");
            QVERIFY2(!pr.search()->isVisible(), "ani wyszukiwarki");
            // Następny, osobny klik otwiera muzykę jak zawsze.
            QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
            QTRY_VERIFY_WITH_TIMEOUT(pr.bubble()->isVisible(), QApplication::doubleClickInterval() + 1500);
        }
    }

    // Ostrzeżenie automatyczne: bez Popup, bez fokusu, bez przechwytywania myszy i klawiatury.
    void gpuAutoWarningGrabsNothing()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.gpuReading(90, 60);
        pr.gpuReading(91, 60);
        AlertBubble *a = pr.alert();
        QVERIFY(a->isVisible());
        QVERIFY(!a->isPopup());
        QVERIFY((a->windowFlags() & Qt::WindowType_Mask) != Qt::Popup);
        QVERIFY(a->windowFlags() & Qt::WindowDoesNotAcceptFocus);
        QVERIFY(a->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(!a->isActiveWindow());
        QVERIFY(QApplication::activePopupWidget() == nullptr);
        QVERIFY(QWidget::mouseGrabber() == nullptr);
        QVERIFY(QWidget::keyboardGrabber() == nullptr);
        QCOMPARE(a->timeoutMs(), 10000);
    }

    void gpuAutoWarningHidesOnPetClickAndOtherBubble()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.gpuReading(90, 60);
        pr.gpuReading(91, 60);
        QVERIFY(pr.alert()->isVisible());
        QTest::mouseClick(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QVERIFY2(!pr.alert()->isVisible(), "klik w Miku chowa ostrzeżenie");
        pr.bubble()->hide();
        QTest::qWait(QApplication::doubleClickInterval() + 500);
        pr.bubble()->hide();
        // Inny dymek (odpowiedź z menu) chowa ostrzeżenie; wraca po jego zamknięciu.
        pr.gpuReading(30, 30);
        pr.gpuReading(30, 30);
        pr.gpuReading(120, 60);
        pr.gpuReading(121, 60);
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 2000);
        pr.showInfoNow(QStringLiteral("GPU 50°C · hotspot 60°C"));
        QVERIFY(!pr.alert()->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 1000);
        QVERIFY(!pr.alert()->isVisible());
        pr.info()->hide();
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 2000);
    }

    void gpuAskTemperatureStaleReadsOnce()
    {
        PetEnv env;
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(pr.lact().available(), 3000);
        pr.lact().setIntervals(60000, 60000, 1000);           // brak odświeżania w tle
        QTest::qWait(1500);
        QTRY_VERIFY_WITH_TIMEOUT(pr.lact().latestAgeMs() > 5000, 8000);
        srv.stats = lactStats(71, 83);
        const int n = srv.commands.count(QStringLiteral("device_stats"));
        QMenu menu;
        pr.buildMenu(menu);
        findAction(menu, QStringLiteral("Jaka jest temperatura?"))->trigger();
        QVERIFY(!pr.info()->isVisible());                     // asynchronicznie - menu nie czeka
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 3000);
        QCOMPARE(pr.info()->message(), QStringLiteral("GPU 71°C · hotspot 83°C"));
        QCOMPARE(srv.commands.count(QStringLiteral("device_stats")), n + 1);
        QVERIFY(srv.rawIsExactlyReadOnly());
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    // ---- wygląd dymka z temperaturą

    void tempStateByThresholds()
    {
        QCOMPARE(tempState(60, 85), TempState::Ok);
        QCOMPARE(tempState(74.9, 85), TempState::Ok);
        QCOMPARE(tempState(75, 85), TempState::Near);       // >= próg - 10
        QCOMPARE(tempState(85, 85), TempState::Near);       // próg włącznie: jeszcze nie "powyżej"
        QCOMPARE(tempState(85.1, 85), TempState::Over);
        QCOMPARE(tempState(104, 100), TempState::Over);
        for (bool dark : {false, true}) {
            QPalette pal;
            pal.setColor(QPalette::Window, dark ? QColor(0x20, 0x23, 0x26) : QColor(0xef, 0xf0, 0xf1));
            const QColor ok = tempStateColor(TempState::Ok, pal), near = tempStateColor(TempState::Near, pal),
                         over = tempStateColor(TempState::Over, pal);
            QVERIFY(ok != near && near != over && ok != over);
            QVERIFY(ok.green() > ok.red() && ok.green() > ok.blue());          // zielony
            QVERIFY(over.red() > over.green() && over.red() > over.blue());    // czerwony
            QVERIFY(near.red() > near.blue() && near.green() > near.blue());   // bursztynowy
            // kontrast z tłem (różnica jasności): ciemny tekst na jasnym, jasny na ciemnym
            for (const QColor &c : {ok, near, over})
                QVERIFY(qAbs(c.lightnessF() - pal.color(QPalette::Window).lightnessF()) > 0.3);
        }
        QPalette light, dark;
        light.setColor(QPalette::Window, Qt::white);
        dark.setColor(QPalette::Window, Qt::black);
        QVERIFY(tempStateColor(TempState::Ok, light) != tempStateColor(TempState::Ok, dark));
    }

    void tempExtraLineOnlyWithData()
    {
        QCOMPARE(tempExtraLine(GpuTemps{50, 60}), QString());
        GpuTemps t{50, 60};
        t.powerW = 212.4;
        t.fanRpm = 1450;
        t.busyPercent = 97;
        QCOMPARE(tempExtraLine(t), QStringLiteral("212 W · wentylator 1450 obr/min · GPU 97%"));
        GpuTemps p{50, 60};
        p.powerW = 30;
        QCOMPARE(tempExtraLine(p), QStringLiteral("30 W"));

        AlertBubble b;
        TempCard c;
        c.temps = GpuTemps{50, 60};
        b.setCard(c);
        b.show();
        auto extraShown = [&b] {
            for (const QLabel *l : b.findChildren<QLabel *>())
                if (l->isVisibleTo(&b) && l->text().contains(QStringLiteral(" W")))
                    return true;
            return false;
        };
        QVERIFY(!extraShown());
        const int h0 = b.height();
        c.temps = t;
        b.setCard(c);
        QVERIFY(extraShown());
        QVERIFY(b.height() > h0);
        c.temps = GpuTemps{50, 60};
        b.setCard(c);
        QVERIFY(!extraShown());
        QCOMPARE(b.height(), h0);
    }

    void lactStatsExtrasAndCardName()
    {
        const auto t = parseLactStats(LACT_STATS);
        QVERIFY(t);
        QCOMPARE(*t->powerW, 25.0);
        QCOMPARE(*t->fanRpm, 1472);
        QCOMPARE(*t->busyPercent, 6);
        const auto bare = parseLactStats("{\"status\":\"ok\",\"data\":{\"temps\":{\"edge\":{\"current\":50}}}}");
        QVERIFY(bare && !bare->powerW && !bare->fanRpm && !bare->busyPercent);
        QCOMPARE(parseLactDeviceName(LACT_LIST, QString::fromLatin1(LACT_ID)), QStringLiteral("RX 9070 XT"));
        QCOMPARE(shortGpuName(QStringLiteral("AMD Radeon RX 7800 XT")), QStringLiteral("RX 7800 XT"));
        QCOMPARE(shortGpuName(QStringLiteral("NVIDIA GeForce RTX 4070")), QStringLiteral("NVIDIA GeForce RTX 4070"));
        QCOMPARE(shortGpuName(QStringLiteral("AMD")), QStringLiteral("AMD"));
    }

    // Żaden tekst nie jest przycięty, a dymek mieści się w ~260-300 px, także z najdłuższymi tekstami i cyframi.
    void tempCardNoClipping()
    {
        for (bool dark : {false, true}) {
            AlertBubble b;
            QPalette pal = b.palette();
            pal.setColor(QPalette::Window, dark ? QColor(0x20, 0x23, 0x26) : QColor(0xef, 0xf0, 0xf1));
            pal.setColor(QPalette::WindowText, dark ? QColor(0xfc, 0xfc, 0xfc) : QColor(0x23, 0x26, 0x29));
            b.setPalette(pal);
            for (TempCard::Kind kind : {TempCard::Reading, TempCard::Warning, TempCard::Unavailable}) {
                TempCard c;
                c.kind = kind;
                c.temps = GpuTemps{188, 199};
                c.temps.card = QStringLiteral("Radeon Pro WX 9100 Super Duper Edition XTX");
                c.temps.powerW = 1234;
                c.temps.fanRpm = 12345;
                c.temps.busyPercent = 100;
                c.edgeLimit = 150;
                c.hotspotLimit = 150;
                c.edgeOver = c.hotspotOver = true;
                b.setCard(c);
                b.show();
                QApplication::processEvents();
                QVERIFY2(b.width() >= 260 && b.width() <= 300, qPrintable(QString::number(b.width())));
                QVERIFY(b.sizeHint().width() <= b.width() && b.sizeHint().height() <= b.height());
                for (const QLabel *l : b.findChildren<QLabel *>()) {
                    if (!l->isVisibleTo(&b))
                        continue;
                    const QString what = l->text();
                    QVERIFY2(l->sizeHint().width() <= l->width() || l->wordWrap(), qPrintable(what));
                    if (l->wordWrap())
                        QVERIFY2(l->heightForWidth(l->width()) <= l->height(), qPrintable(what));
                    else
                        QVERIFY2(l->sizeHint().height() <= l->height(), qPrintable(what));
                    const QRect inBubble(l->mapTo(&b, QPoint(0, 0)), l->size());
                    QVERIFY2(b.rect().adjusted(0, 0, 0, -Bubble::TAIL).contains(inBubble), qPrintable(what));
                }
            }
        }
    }

    void tempCardUnavailableHasNoMeters()
    {
        AlertBubble b;
        TempCard c;
        c.temps = GpuTemps{62, 78};
        c.summary = QStringLiteral("x");
        b.setCard(c);
        b.show();
        QCOMPARE(b.meterCount(), 2);
        c.temps = GpuTemps{62, std::nullopt};
        b.setCard(c);
        QCOMPARE(b.meterCount(), 1);
        c.kind = TempCard::Unavailable;
        c.temps = GpuTemps{};
        b.setCard(c);
        QCOMPARE(b.meterCount(), 0);
        bool titleShown = false;
        for (const QLabel *l : b.findChildren<QLabel *>())
            titleShown |= l->isVisibleTo(&b) && l->text().contains(QStringLiteral("LACT"));
        QVERIFY(titleShown);
        b.setMessage(QStringLiteral("zwykły tekst"));
        QCOMPARE(b.meterCount(), 0);
        QCOMPARE(b.message(), QStringLiteral("zwykły tekst"));
        QVERIFY(!b.card());
    }

    void gpuAskTemperatureUnavailable()
    {
        PetEnv env;
        Pet pet;                                               // gniazdo z initTestCase nie istnieje
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!pr.lact().reason().isEmpty(), 3000);
        QMenu menu;
        pr.buildMenu(menu);
        findAction(menu, QStringLiteral("Jaka jest temperatura?"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 3000);
        QCOMPARE(pr.info()->message(), QStringLiteral("Nie mogę odczytać temperatury (LACT niedostępny)"));
        QVERIFY(pr.info()->card() && pr.info()->card()->kind == TempCard::Unavailable);
        QCOMPARE(pr.info()->meterCount(), 0);
    }

    void gpuAskTemperatureWithWarningsOffIsOneShot()
    {
        PetEnv env;
        writeFile(cfg::settingsFile(), QStringLiteral("{\"gpu_alert\":{\"enabled\":false,\"edge\":85,\"hotspot\":100}}"));
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.lact().setIntervals(40, 80, 1000);
        QTest::qWait(300);
        QCOMPARE(srv.connections, 0);
        QMenu menu;
        pr.buildMenu(menu);
        findAction(menu, QStringLiteral("Jaka jest temperatura?"))->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(pr.info()->isVisible(), 3000);
        QCOMPARE(pr.info()->message(), QStringLiteral("GPU 62°C · hotspot 78°C"));
        QTest::qWait(500);                                     // brak cyklicznego odpytywania
        QCOMPARE(srv.commands, (QStringList{QStringLiteral("list_devices"), QStringLiteral("device_stats")}));
        QVERIFY(!pr.lact().enabled());
        QVERIFY(!pr.lact().latest());
        QTRY_COMPARE_WITH_TIMEOUT(srv.open, 0, 3000);
        QVERIFY(srv.rawIsExactlyReadOnly());
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    void gpuMenuToggleAndThresholdsPersist()
    {
        PetEnv env;
        writeFile(cfg::settingsFile(), QStringLiteral("{\"mode\":\"leek\",\"obce\":[1,2]}"));
        FakeLact srv;
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        QVERIFY(pr.gpuCfg().enabled);                      // domyślnie włączone
        auto find = [](QMenu &m, const QString &text) -> QAction * {
            for (QAction *a : m.actions())
                if (a->text() == text)
                    return a;
            return nullptr;
        };
        QMenu menu;
        pr.buildMenu(menu);
        QAction *toggle = find(menu, QStringLiteral("Ostrzeżenia o temperaturze GPU"));
        QVERIFY(toggle);
        QVERIFY(toggle->isCheckable() && toggle->isChecked());
        // Informacja z ostatniego odczytu (w tle), nieaktywna.
        QTRY_VERIFY_WITH_TIMEOUT(pr.lact().available(), 3000);
        QMenu menu2;
        pr.buildMenu(menu2);
        QAction *info = find(menu2, QStringLiteral("GPU 62°C · hotspot 78°C"));
        QVERIFY(info);
        QVERIFY(!info->isEnabled());
        // Progi
        QAction *thr = find(menu2, QStringLiteral("Progi"));
        QVERIFY(thr && thr->menu());
        QStringList labels;
        QAction *e80 = nullptr, *h95 = nullptr;
        bool seenEdgeChecked = false, seenHotChecked = false;
        for (QAction *a : thr->menu()->actions()) {
            if (a->isSeparator())
                continue;
            labels << a->text();
            if (a->text() == QStringLiteral("80°C") && !e80)
                e80 = a;
            if (a->text() == QStringLiteral("95°C"))
                h95 = a;
            if (a->isChecked() && a->text() == QStringLiteral("85°C"))
                seenEdgeChecked = true;
            if (a->isChecked() && a->text() == QStringLiteral("100°C"))
                seenHotChecked = true;
        }
        QVERIFY(seenEdgeChecked && seenHotChecked);
        for (const char *v : {"75°C", "80°C", "85°C", "90°C", "95°C", "100°C", "105°C", "110°C"})
            QVERIFY2(labels.contains(QString::fromUtf8(v)), v);
        QVERIFY(e80 && h95);
        e80->trigger();
        h95->trigger();
        QCOMPARE(pr.gpuCfg().edge, 80);
        QCOMPARE(pr.gpuCfg().hotspot, 95);
        cfg::flushSettings();
        QJsonObject s = cfg::loadSettings();
        QCOMPARE(s.value("gpu_alert").toObject().value("edge").toInt(), 80);
        QCOMPARE(s.value("gpu_alert").toObject().value("hotspot").toInt(), 95);
        QVERIFY(s.value("gpu_alert").toObject().value("enabled").toBool());
        QCOMPARE(s.value("mode").toString(), QStringLiteral("leek"));       // pozostałe klucze zostają
        QCOMPARE(s.value("obce").toArray().size(), 2);
        // Wyłączenie: zapis i koniec odpytywania.
        toggle = find(menu2, QStringLiteral("Ostrzeżenia o temperaturze GPU"));
        toggle->trigger();
        QVERIFY(!pr.gpuCfg().enabled);
        QVERIFY(!pr.lact().enabled());
        cfg::flushSettings();
        s = cfg::loadSettings();
        QVERIFY(!s.value("gpu_alert").toObject().value("enabled").toBool());
        QCOMPARE(s.value("gpu_alert").toObject().value("edge").toInt(), 80);
        const int n = srv.commands.size();
        QTest::qWait(400);
        QCOMPARE(srv.commands.size(), n);
        QMenu menu3;
        pr.buildMenu(menu3);
        QVERIFY(!find(menu3, QStringLiteral("GPU 62°C · hotspot 78°C")));
        for (QAction *a : menu3.actions())
            QVERIFY(!a->text().startsWith(QStringLiteral("LACT")));
        QVERIFY(srv.onlyReadCommands());
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    void gpuMenuShowsUnavailable()
    {
        PetEnv env;
        Pet pet;                                           // gniazdo z initTestCase nie istnieje
        PetProbe pr{pet};
        QTRY_VERIFY_WITH_TIMEOUT(!pr.lact().reason().isEmpty(), 3000);
        QMenu menu;
        pr.buildMenu(menu);
        QAction *info = nullptr;
        for (QAction *a : menu.actions())
            if (a->text().startsWith(QStringLiteral("LACT niedostępny")))
                info = a;
        QVERIFY(info);
        QVERIFY(!info->isEnabled());
        QVERIFY(info->text().contains(QStringLiteral("brak gniazda")));
    }

    void gpuAlertBubbleNoFocus()
    {
        AlertBubble b;
        QVERIFY(b.windowFlags() & Qt::Tool);
        QVERIFY(b.windowFlags() & Qt::WindowDoesNotAcceptFocus);
        QVERIFY(b.windowFlags() & Qt::WindowStaysOnTopHint);
        QVERIFY(b.testAttribute(Qt::WA_ShowWithoutActivating));
        b.setMessage(QStringLiteral("GPU ma 87°C!"));
        QSignalSpy dis(&b, &AlertBubble::dismissed);
        b.show();
        QVERIFY(b.isVisible());
        QVERIFY(!b.isActiveWindow());
        QVERIFY(!b.hasFocus());
        QTest::mouseClick(&b, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(!b.isVisible());                       // klik zamyka
        QCOMPARE(dis.count(), 1);
        b.setTimeoutMs(100);
        b.show();
        QTRY_VERIFY_WITH_TIMEOUT(!b.isVisible(), 1500); // sam znika
        QCOMPARE(dis.count(), 2);
        QCOMPARE(AlertBubble::TIMEOUT_MS, 10000);
    }

    void gpuAlertShowsAndReacts()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        pr.gpuReading(90, 60);
        QVERIFY(!pr.alert()->isVisible());
        pr.gpuReading(91, 60);
        QVERIFY(pr.alert()->isVisible());
        QCOMPARE(pr.alert()->message(), QStringLiteral("GPU ma 91°C!"));
        QVERIFY(pr.alert()->card() && pr.alert()->card()->kind == TempCard::Warning);
        QVERIFY(pr.alert()->card()->edgeOver && !pr.alert()->card()->hotspotOver);
        QVERIFY(pr.gpuPending().isEmpty());
        QVERIFY(pr.queue().size() >= 1);
        QCOMPARE(pr.queue().front().first, QStringLiteral("surprised"));
        QCOMPARE(pr.queue().back().first, QStringLiteral("shy"));
        // Nad głową postaci (ogonek do niej), a postać przy ostrzeżeniu nie odchodzi.
        QVERIFY(pr.alert()->y() < pet.y());
        QVERIFY(!pr.alert()->isActiveWindow());
        QTest::mouseClick(pr.alert(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(!pr.alert()->isVisible());
        // Dalsze odczyty tej samej serii (stale wysoka) nie ostrzegają ponownie.
        pr.gpuReading(120, 60);
        pr.gpuReading(120, 60);
        QVERIFY(!pr.alert()->isVisible());              // stale wysoka, ta sama seria - cisza
    }

    void gpuAlertWaitsForSearchAndMusic()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        pr.search()->setEngine(new FakeEngine);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        // Wyszukiwarka otwarta: ostrzeżenie czeka.
        dclick(pet);
        QVERIFY(pr.search()->isVisible());
        pr.gpuReading(95, 60);
        pr.gpuReading(95, 60);
        QVERIFY(!pr.alert()->isVisible());
        QCOMPARE(pr.gpuPending(), QStringLiteral("GPU ma 95°C!"));
        QTest::qWait(700);
        QVERIFY(!pr.alert()->isVisible());              // nadal czeka
        QVERIFY(pr.search()->isVisible());
        pr.search()->hide();
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 2000);   // po zamknięciu wyszukiwarki - bez gubienia
        QCOMPARE(pr.alert()->message(), QStringLiteral("GPU ma 95°C!"));
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 1000);
        // Dymek z muzyką wypiera widoczne ostrzeżenie, które wraca po jego zamknięciu.
        QWheelEvent we(QPointF(20, 20), pet.mapToGlobal(QPointF(20, 20)), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&pet, &we);
        QVERIFY(pr.bubble()->isVisible());
        QVERIFY(!pr.alert()->isVisible());
        QCOMPARE(pr.gpuPending(), QStringLiteral("GPU ma 95°C!"));
        QTest::qWait(700);
        QVERIFY(!pr.alert()->isVisible());
        pr.bubble()->hide();
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 2000);
        QVERIFY(pr.gpuPending().isEmpty());
        // Nowe ostrzeżenie w trakcie otwartej muzyki też czeka.
        pr.alert()->hide();
        pr.bubble()->show();
        pr.gpuReading(60, 112);
        pr.gpuReading(60, 112);
        QVERIFY(!pr.alert()->isVisible());
        QVERIFY(pr.gpuPending().contains(QStringLiteral("Hotspot 112")));
        pr.bubble()->hide();
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 2000);
    }

    void gpuAlertNotDuringDrag()
    {
        PetEnv env;
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        pr.lact().setEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&pet, QPoint(80, -100));
        QVERIFY(pr.drag());
        pr.gpuReading(95, 60);
        pr.gpuReading(95, 60);
        QVERIFY(!pr.alert()->isVisible());
        QVERIFY(!pr.gpuPending().isEmpty());
        QTest::qWait(700);
        QVERIFY(!pr.alert()->isVisible());              // przeciąganie trwa
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(80, -100));
        QTRY_VERIFY_WITH_TIMEOUT(pr.floor(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 3000);   // po wylądowaniu
        // Widoczne ostrzeżenie chowa się przy chwyceniu postaci i wraca.
        QTest::mousePress(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseMove(&pet, QPoint(80, -100));
        QVERIFY(pr.drag());
        QVERIFY(!pr.alert()->isVisible());
        QVERIFY(!pr.gpuPending().isEmpty());
        QTest::mouseRelease(&pet, Qt::LeftButton, Qt::NoModifier, QPoint(80, -100));
        QTRY_VERIFY_WITH_TIMEOUT(pr.alert()->isVisible(), 5000);
    }

    void gpuAlertDisabledNeverShows()
    {
        PetEnv env;
        writeFile(cfg::settingsFile(), QStringLiteral("{\"gpu_alert\":{\"enabled\":false,\"edge\":80,\"hotspot\":95}}"));
        FakeLact srv;
        srv.stats = lactStats(99, 120);
        qputenv("DESKTOP_PET_LACT_SOCKET", srv.path().toUtf8());
        Pet pet;
        PetProbe pr{pet};
        pet.show();
        QVERIFY(!pr.gpuCfg().enabled);
        QCOMPARE(pr.gpuCfg().edge, 80);
        QTest::qWait(500);
        QCOMPARE(srv.connections, 0);                   // nawet nie łączy się z LACT
        pr.gpuReading(99, 120);
        pr.gpuReading(99, 120);
        QVERIFY(!pr.alert()->isVisible());
        QVERIFY(pr.gpuPending().isEmpty());
        qputenv("DESKTOP_PET_LACT_SOCKET", none().toUtf8());
    }

    void spritesDirAndPlaceholder()
    {
        QTemporaryDir tmp;
        writeFile(tmp.filePath(QStringLiteral("a/b/c/shime1.png")), QString());
        writeFile(tmp.filePath(QStringLiteral("z/shime1.png")), QString());
        QCOMPARE(findShimejiDir(tmp.path()), tmp.filePath(QStringLiteral("z")));   // płytszy wygrywa
        QCOMPARE(findShimejiDir(tmp.filePath(QStringLiteral("brak"))), QString());
        const Sprites ph = placeholderSprites();
        for (const char *n : {"stand", "walk", "fall", "drag", "sit", "sit_look", "sit_legs", "lie",
                              "surprised", "grumpy", "land", "leek", "sit_happy", "sit_think", "shy", "ponder",
                              "cheer", "dance"})
            QVERIFY2(!ph.anim(QString::fromLatin1(n)).frames.isEmpty(), n);
        for (const QString &n : shimejiAnimNames())     // każda animacja paczki ma odpowiednik w postaci zastępczej
            QVERIFY2(ph.anims.contains(n), qPrintable(n));
        // Pusty folder / brakujące klatki obowiązkowe -> false.
        Sprites out;
        QString missing;
        QVERIFY(!loadShimeji(tmp.filePath(QStringLiteral("z")), 1.0, &out, &missing));
        QVERIFY(missing.endsWith(QStringLiteral("shime1.png")));
    }
};

QTEST_MAIN(Tests)
#include "tests.moc"
