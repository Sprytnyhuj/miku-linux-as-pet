#include "pet.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int INFO_MS = 6000;      // odpowiedź na pytanie znika po ok. 6 s
constexpr qint64 FRESH_MS = 5000;  // odczyt z pamięci młodszy niż to nie wymaga nowego zapytania

QString tempsText(const GpuTemps &t)
{
    QString s;
    if (t.edge)
        s = QStringLiteral("GPU %1°C").arg(qRound(*t.edge));
    if (t.hotspot)
        s += (s.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("hotspot %1°C").arg(qRound(*t.hotspot));
    return s;
}

TempCard readingCard(const GpuTemps &t, const GpuAlertConfig &cfg)
{
    TempCard c;
    c.edgeLimit = cfg.edge;
    c.hotspotLimit = cfg.hotspot;
    c.kind = TempCard::Reading;
    c.temps = t;
    c.summary = tempsText(t);
    return c;
}

TempCard unavailableCard()
{
    TempCard c;
    c.kind = TempCard::Unavailable;
    c.summary = QStringLiteral("Nie mogę odczytać temperatury (LACT niedostępny)");
    return c;
}

}  // namespace


#include "animlog.h"
#include "config.h"
#include "searchhelper.h"

#include <QAction>
#include <cstdio>
#include <QActionGroup>
#include <QApplication>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegion>
#include <QRegularExpression>
#include <QScreen>
#include <QTransform>
#include <QJsonObject>
#include <algorithm>
#include <cmath>

const std::vector<Mode> &modes()
{
    static const std::vector<Mode> m = {
        {"sit", "Siedzi",
         {{"sit", 5, 250, 750}, {"sit_look", 2, 80, 160}, {"sit_legs", 2, 120, 300},
          {"sit_happy", 1, 100, 250}, {"sit_think", 1, 75, 150}}},
        {"lie", "Leży i śpi", {{"lie", 1, 1500, 3000}}},
        {"stand", "Stoi",
         {{"stand", 4, 1500, 3000}, {"ponder", 1, 75, 150}, {"shy", 1, 50, 100}, {"cheer", 1, 50, 100}}},
        {"auto", "Sama decyduje",
         {{"sit", 4, 250, 750}, {"sit_look", 2, 80, 160}, {"sit_legs", 2, 120, 300},
          {"lie", 2, 500, 1500}, {"stand", 1, 75, 200}, {"walk", 1, 50, 150},
          {"sit_happy", 1, 100, 250}, {"sit_think", 1, 75, 150}, {"shy", 1, 50, 100},
          {"ponder", 1, 75, 150}, {"cheer", 1, 50, 100}}},
        {"leek", "Macha porem",
         {{"leek", 10, 150, 400}, {"stand", 1, 50, 100}}},
        {"walk", "Spaceruje", {{"walk", 3, 80, 300}, {"stand", 2, 25, 150}}},
        // Pierwsza aktywność to główna czynność trybu (do niej wraca po upadku).
        {"happy", "Wesoła", {{"sit_happy", 4, 100, 250}, {"cheer", 2, 50, 100}, {"ponder", 1, 50, 100},
                            {"sit_legs", 2, 120, 300}}},
        {"shy", "Wstydliwa", {{"shy", 4, 75, 200}, {"sit", 3, 250, 500}}},
        {"dance", "Tańczy",
         {{"dance", 10, 100, 250}, {"stand", 1, 50, 100}}},
    };
    return m;
}

const Mode *findMode(const QString &key)
{
    for (const Mode &m : modes()) {
        if (key == QLatin1String(m.key))
            return &m;
    }
    return nullptr;
}

int GamesMenuStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    if (metric == QStyle::PM_SmallIconSize)
        return cfg::STEAM_ICON_PX;
    return QProxyStyle::pixelMetric(metric, option, widget);
}

std::optional<QRect> parseWorkarea(const QString &out)
{
    const QString tail = out.mid(out.indexOf(QLatin1Char('=')) + 1);   // jak partition("=")[2]
    static const QRegularExpression num(QStringLiteral("\\d+"));
    QList<int> n;
    auto it = num.globalMatch(tail);
    while (it.hasNext() && n.size() < 4)
        n << it.next().captured().toInt();
    if (n.size() == 4 && n[2] > 0 && n[3] > 0)
        return QRect(n[0], n[1], n[2], n[3]);
    return std::nullopt;
}

namespace {
int randint(int lo, int hi) { return int(QRandomGenerator::global()->bounded(lo, hi + 1)); }
}  // namespace

Pet::Pet() : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("desktop-pet"));
    m_sprites = loadSprites();
    // Okno ma stałą szerokość: postać + margines z obu stron na szersze klatki (por).
    // Zmiana rozmiaru okna w trakcie przeciągania zacinała animację, więc zmienia się
    // tylko maska (widoczny i klikalny obszar).
    const QSize body = m_sprites.anim(QStringLiteral("stand")).frames.first().size();
    m_bodyW = body.width();
    int widest = 0, tallest = body.height();
    for (const Anim &a : std::as_const(m_sprites.anims)) {
        for (const QPixmap &pm : a.frames) {
            widest = std::max(widest, pm.width());
            tallest = std::max(tallest, pm.height());
        }
    }
    m_pad = std::max(0, widest - m_bodyW);
    setFixedSize(m_bodyW + 2 * m_pad, tallest);      // klatki rysowane od dołu okna (frameY)

    m_mode = cfg::loadSettings().value(QStringLiteral("mode")).toString(QLatin1String(DEFAULT_MODE));
    if (!findMode(m_mode))
        m_mode = QLatin1String(DEFAULT_MODE);
    m_dir = QRandomGenerator::global()->bounded(2) ? 1 : -1;
    m_bubble = new MusicBubble(&m_mpris, &m_watcher, &m_volume);
    m_bubble->showPlayer(std::nullopt, std::nullopt);     // rozgrzewka układu (pierwsze odświeżenie przy kliknięciu nie zwleka)

    m_search = new SearchBubble(new HelperEngine);
    connect(m_search, &SearchBubble::launched, this, [this] {
        if (m_state == State::Floor)
            react({{QStringLiteral("cheer"), SEARCH_CHEER_TICKS}}, "search_launched");     // uruchomiła wynik - cieszy się
    });
    connect(m_search, &SearchBubble::noResults, this, [this] {
        if (m_state == State::Floor && m_search->isVisible())
            react({{QStringLiteral("shy"), SEARCH_SHY_TICKS}}, "search_noresults");         // nic nie znalazła (po ustabilizowaniu zapytania)
    });
    connect(m_search, &SearchBubble::typing, this, [this] {
        if (!m_reactions.empty() && m_reactions.front().first == QLatin1String("shy"))
            m_reactions.clear();                                          // pisze dalej - przestaje się wstydzić
    });
    connect(m_search, &SearchBubble::layoutChanged, this, [this] { placeBubble(); });

    // Ostrzeżenia GPU: dane z LACT w tle; dymek nie bierze fokusu i czeka, gdy nad głową jest inny dymek.
    m_alert = new AlertBubble;
    m_info = new AlertBubble(true);
    m_gpu = GpuAlertConfig::fromSettings(cfg::loadSettings());
    m_gpuLogic.setThresholds(m_gpu.edge, m_gpu.hotspot);
    m_gpuShowTimer = new QTimer(this);
    m_gpuShowTimer->setInterval(500);
    connect(m_gpuShowTimer, &QTimer::timeout, this, &Pet::tryShowGpuAlert);
    m_gpuShowTimer->start();
    connect(&m_lact, &LactClient::reading, this, &Pet::onGpuReading);
    connect(&m_lact, &LactClient::unavailable, this, [this] { m_gpuLogic.reset(); });
    connect(&m_lact, &LactClient::onceFinished, this, [this](bool ok, const GpuTemps &t, const QString &) {
        if (!m_tempWanted)
            return;
        m_tempWanted = false;
        if (ok)
            showTemperature(tempsText(t), readingCard(t, m_gpu));
        else
            showTemperature(QStringLiteral("Nie mogę odczytać temperatury (LACT niedostępny)"), unavailableCard());
    });
    m_lact.setEnabled(m_gpu.enabled);
    m_clickTimer = new QTimer(this);
    m_clickTimer->setSingleShot(true);
    connect(m_clickTimer, &QTimer::timeout, this, &Pet::singleClick);

    // Obszar roboczy: xprop zawsze w tle (start bez paneli to krótko availableGeometry), odświeżany co 10 s
    // i przy każdej zmianie geometrii ekranu.
    m_workareaTimer = new QTimer(this);
    m_workareaTimer->setInterval(10000);
    connect(m_workareaTimer, &QTimer::timeout, this, &Pet::refreshWorkarea);
    m_workareaTimer->start();
    auto watchScreen = [this](QScreen *s) {
        connect(s, &QScreen::availableGeometryChanged, this, [this] { refreshWorkarea(); });
    };
    for (QScreen *s : QGuiApplication::screens())
        watchScreen(s);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, watchScreen);
    refreshWorkarea();
    prefetchSteamHandler();
    m_steam.refresh();
    auto *steamTimer = new QTimer(this);
    steamTimer->setInterval(cfg::STEAM_RESCAN_S * 1000);
    connect(steamTimer, &QTimer::timeout, &m_steam, &SteamCache::refresh);
    steamTimer->start();
    m_watcher.poll();

    if (!restorePosition())
        placeCentered(QGuiApplication::primaryScreen());
    m_state = State::Floor;          // na starcie od razu na dole, bez spadania
    pickActivity();
    applyPos();

    m_timer = new QTimer(this);
    m_timer->setInterval(cfg::TICK_MS);
    connect(m_timer, &QTimer::timeout, this, &Pet::step);
    m_timer->start();                 // bez tego nie ma ani grawitacji, ani animacji
    m_clock.start();
    m_tickClock.start();
    m_trackTimer = new QTimer(this);
    m_trackTimer->setInterval(3000);          // lekkie odpytywanie MPRIS (limit czasu D-Bus jak w reszcie kodu)
    connect(m_trackTimer, &QTimer::timeout, this, &Pet::checkTrack);
    connect(&m_watcher, &TrackWatcher::polled, this, &Pet::onTrackPolled);
    m_trackTimer->start();
    // Zamknięcie (SIGTERM/SIGINT/"Zakończ"): ostatni zapis pozycji, zanim main() zrobi flushSettings().
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (!qApp->property("explicitQuit").toBool())
            std::fprintf(stderr, "desktop-pet: nieoczekiwane wyjście - ostatnie okno zamknięte?\n");
        savePosition();
    });
}

void Pet::refreshWorkarea()
{
    if (m_workareaBusy)
        return;
    m_workareaBusy = true;
    cfg::runAsync(this, QStringLiteral("xprop"), {QStringLiteral("-root"), QStringLiteral("_NET_WORKAREA")}, 1500,
                  [this](const cfg::ProcResult &r) {
                      m_workareaBusy = false;
                      if (r.finished) {
                          m_workarea = parseWorkarea(QString::fromUtf8(r.out));
                          clampToWorkarea();
                      }
                  });
}


// Po (kolejnym) odczycie obszaru roboczego: postać stojąca na podłodze nie zostaje pod panelem bocznym.
// Tylko x i tylko gdy wystaje poza obszar (mieszcząca się nie rusza); y poprawia najbliższy tick.
void Pet::clampToWorkarea()
{
    if (m_state != State::Floor)
        return;
    const QRect a = area(m_pos + QPointF(m_bodyW / 2.0, height() / 2.0));
    const double lo = a.left(), hi = a.right() + 1 - m_bodyW;
    if (hi < lo || (m_pos.x() >= lo && m_pos.x() <= hi))
        return;
    if (animlog::enabled())
        animlog::write(QStringLiteral("clamp x %1 -> %2 (lo=%3 hi=%4 dir=%5)").arg(m_pos.x()).arg(std::max(lo, std::min(hi, m_pos.x())))
                           .arg(lo).arg(hi).arg(m_dir));
    m_pos.setX(std::max(lo, std::min(hi, m_pos.x())));
    applyPos();
    placeBubble();
    update();
    savePosition();                            // zapisane miejsce nie może zostać pod panelem
}

// -- zapamiętywanie pozycji
// settings.json: "position": {"screen": nazwa QScreen, "x": lewa krawędź ciała względem lewej krawędzi ekranu, "dir": 1/-1}.
// Y nie jest zapisywane (postać zawsze staje na podłodze obszaru roboczego).
QScreen *Pet::currentScreen() const
{
    QScreen *s = QGuiApplication::screenAt((m_pos + QPointF(m_bodyW / 2.0, height() / 2.0)).toPoint());
    return s ? s : QGuiApplication::primaryScreen();
}

void Pet::placeCentered(QScreen *screen)
{
    const QRect geo = area(screen->geometry().center());
    m_pos = QPointF(geo.center().x() - m_bodyW / 2.0, floorY(geo));
}

bool Pet::restorePosition()
{
    const QJsonValue v = cfg::loadSettings().value(QStringLiteral("position"));
    if (!v.isObject())
        return false;
    const QJsonObject o = v.toObject();
    const QJsonValue sv = o.value(QStringLiteral("screen")), xv = o.value(QStringLiteral("x"));
    if (!sv.isString() || !xv.isDouble() || !std::isfinite(xv.toDouble()))
        return false;
    QScreen *screen = nullptr;
    for (QScreen *s : QGuiApplication::screens()) {
        if (s->name() == sv.toString())
            screen = s;
    }
    if (!screen)
        return false;                                   // monitor odłączony - środek głównego ekranu
    const QRect a = area(screen->geometry().center());
    const double lo = a.left(), hi = a.right() + 1 - m_bodyW;
    const double x = screen->geometry().left() + xv.toDouble();
    m_pos = QPointF(std::max(lo, std::min(hi, x)), floorY(a));
    const int dir = o.value(QStringLiteral("dir")).toInt(0);
    if (dir == 1 || dir == -1)
        m_dir = dir;
    m_savedX = m_pos.x();
    m_savedDir = m_dir;
    return true;
}

void Pet::savePosition()
{
    if (m_state != State::Floor)
        return;
    const QScreen *screen = currentScreen();
    QJsonObject o;
    o.insert(QStringLiteral("screen"), screen->name());
    o.insert(QStringLiteral("x"), m_pos.x() - screen->geometry().left());
    o.insert(QStringLiteral("dir"), m_dir);
    m_savedX = m_pos.x();
    m_savedDir = m_dir;
    m_saveTicks = 0;
    cfg::saveSettingKeyAsync(QStringLiteral("position"), o);     // w tle, pozostałe klucze zostają
}

void Pet::returnToCenter()
{
    placeCentered(currentScreen());
    m_state = State::Floor;
    m_vy = 0;
    applyPos();
    placeBubble();
    update();
    savePosition();
}

// -- geometria
QRect Pet::area(const QPointF &point) const
{
    const QScreen *screen = QGuiApplication::screenAt(point.toPoint());
    QRect a = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    // Przy kilku monitorach Qt/xcb nie odejmuje paneli - robimy to sami.
    if (m_workarea && a.intersects(*m_workarea))
        a = a.intersected(*m_workarea);
    return a;
}

double Pet::floorY(const QRect &a) const
{
    return a.bottom() + 1 - height();
}

void Pet::applyPos()
{
    // m_pos to lewy górny róg postaci; okno zaczyna się o margines wcześniej.
    move(qRound(m_pos.x()) - m_pad, qRound(m_pos.y()));
}

// Gdzie w oknie narysować klatkę. Szersza klatka wystaje w stronę pora
// (nieodwrócona - w lewo), a postać zostaje na miejscu.
int Pet::frameX(const QPixmap &pm) const
{
    const bool flipped = (m_dir > 0) == m_sprites.facesLeft;
    if (pm.width() > m_bodyW && !flipped)
        return m_pad + m_bodyW - pm.width();
    return m_pad;
}

// -- logika
void Pet::pickActivity()
{
    if (m_activity == QLatin1String("walk") && (m_pos.x() != m_savedX || m_dir != m_savedDir))
        savePosition();                        // koniec spaceru
    const auto &opts = findMode(m_mode)->activities;
    int total = 0;
    for (const auto &o : opts)
        total += o.weight;
    int r = int(QRandomGenerator::global()->bounded(total));
    const ModeActivity *pick = &opts.back();
    for (const auto &o : opts) {
        if (r < o.weight) {
            pick = &o;
            break;
        }
        r -= o.weight;
    }
    // Dwa razy z rzędu ta sama poboczna aktywność (np. stand w trybie leek) to zbyt długi bezruch -
    // wtedy losujemy jeszcze raz (główna czynność może się powtarzać).
    // Przerywnik (poboczna aktywność) nigdy nie następuje po innym przerywniku.
    const bool prevSide = opts.size() == 2

                          && QLatin1String(opts.front().name) != m_activity
                          && std::any_of(opts.begin() + 1, opts.end(), [this](const ModeActivity &o) { return QLatin1String(o.name) == m_activity; });
    if (opts.size() > 1 && pick != &opts.front() && (QLatin1String(pick->name) == m_activity || prevSide))
        pick = &opts.front();
    if (QLatin1String(pick->name) == QLatin1String("walk"))
        m_dir = QRandomGenerator::global()->bounded(2) ? 1 : -1;
    m_activity = QLatin1String(pick->name);
    m_activityLeft = randint(pick->lo, pick->hi);
    if (animlog::enabled())
        animlog::write(QStringLiteral("pick_activity mode=%1 picked=%2 ticks=%3 main=%4")
                           .arg(m_mode, m_activity).arg(m_activityLeft).arg(pick == &opts.front() ? 1 : 0));
}

// Od razu główna czynność trybu (pierwsza aktywność), bez losowania postoju.
void Pet::startMainActivity(bool newDir)
{
    const ModeActivity &first = findMode(m_mode)->activities.front();
    if (newDir && QLatin1String(first.name) == QLatin1String("walk"))
        m_dir = QRandomGenerator::global()->bounded(2) ? 1 : -1;
    m_activity = QLatin1String(first.name);
    m_activityLeft = randint(first.lo, first.hi);
}

void Pet::setMode(const QString &mode)
{
    animlog::write(QStringLiteral("set_mode %1 -> %2").arg(m_mode, mode));
    m_mode = mode;
    cfg::saveSettingKeyAsync(QStringLiteral("mode"), mode);     // zapis w tle, pozostałe klucze zostają
    m_reactions.clear();                                        // bez dokańczania starej reakcji
    startMainActivity(true);                                    // od razu główna czynność nowego trybu
    if (m_state == State::Floor)
        setAnim(m_activity);
}

// Krótka reakcja, np. react({{"surprised", 25}}) - potem wraca do trybu.
void Pet::react(std::vector<std::pair<QString, int>> steps, const char *why)
{
    // Reakcja krótsza niż MIN_REACTION_TICKS to tylko mignięcie innej klatki - wydłużamy do progu.
    for (auto &s : steps)
        s.second = std::max(s.second, MIN_REACTION_TICKS);
    if (animlog::enabled()) {
        QStringList names;
        for (const auto &s : steps)
            names << QStringLiteral("%1x%2").arg(s.first).arg(s.second);
        animlog::write(QStringLiteral("react src=%1 steps=%2 prev_reactions=%3 activity=%4 state=%5")
                           .arg(QLatin1String(why), names.join(QLatin1Char(',')))
                           .arg(m_reactions.size()).arg(m_activity).arg(int(m_state)));
    }
    m_reactions = std::move(steps);
}

void Pet::setAnim(const QString &name)
{
    if (name != m_anim) {
        if (animlog::enabled())
            animlog::write(QStringLiteral("anim %1->%2 (ticks_in_prev=%3 activity=%4 left=%5 reactions=%6 state=%7)")
                               .arg(m_anim, name).arg(m_animTick).arg(m_activity).arg(m_activityLeft)
                               .arg(m_reactions.size()).arg(int(m_state)));
        m_anim = name;
        m_animTick = 0;
    }
}

void Pet::step()
{
    // Pomiar: odstęp od poprzedniego ticku i czas samego step().
    const double gap = m_tickClock.nsecsElapsed() / 1e6;
    m_tickClock.start();
    if (m_tickStats.ticks > 0) {
        m_tickStats.maxGapMs = std::max(m_tickStats.maxGapMs, gap);
        if (gap > 2 * cfg::TICK_MS)
            ++m_tickStats.gapsOver80Ms;
        static const bool debug = qEnvironmentVariableIsSet("DESKTOP_PET_DEBUG");
        if (debug && gap > 2 * cfg::TICK_MS)
            std::fprintf(stderr, "desktop-pet: tick po %.1f ms\n", gap);
    }
    ++m_tickStats.ticks;
    stepImpl();
    m_tickStats.maxStepMs = std::max(m_tickStats.maxStepMs, m_tickClock.nsecsElapsed() / 1e6);
}

void Pet::stepImpl()
{
    ++m_animTick;
    if (m_state == State::Drag) {
        setAnim(QStringLiteral("drag"));
        return;
    }
    const QRect a = area(m_pos + QPointF(m_bodyW / 2.0, height() / 2.0));
    const double floor = floorY(a);

    if (m_state == State::Fall) {
        setAnim(QStringLiteral("fall"));
        m_vy = std::min(m_vy + cfg::GRAVITY, cfg::MAX_FALL);
        m_pos.setY(m_pos.y() + m_vy);
        if (m_pos.y() >= floor) {
            m_pos.setY(floor);
            const double landVy = m_vy;
            const bool hard = m_vy > 12;       // spadła z wysoka - "plask" i marudzenie
            m_vy = 0;
            m_state = State::Floor;
            animlog::write(QStringLiteral("state landed vy=%1 mode=%2").arg(landVy).arg(m_mode));
            // Po upadku od razu wraca do głównej czynności trybu (np. por), bez losowania postoju.
            startMainActivity();
            savePosition();                    // wylądowała: zapamiętaj miejsce
            const bool calmMode = m_mode == QLatin1String("leek") || m_mode == QLatin1String("dance");
            if (hard && m_mode == QLatin1String("leek"))
                react({{QStringLiteral("land"), 8}}, "land_hard_leek");            // z porem nie marudzi, macha dalej
            else if (hard)
                react({{QStringLiteral("land"), 12}, {QStringLiteral("grumpy"), 48}}, "land_hard");
            else if (landVy >= SOFT_LAND_VY && !calmMode) {      // wyraźny upadek: krótko (<= 0,3 s) "land" albo zawstydzenie
                if (randint(0, 1))
                    react({{QStringLiteral("land"), SOFT_LAND_TICKS}}, "land_soft");
                else
                    react({{QStringLiteral("shy"), SOFT_LAND_TICKS}}, "land_soft");
            }
        }
    } else {
        m_pos.setY(floor);
        if (m_chatWaiting && m_reactions.empty()) {
            setAnim(QStringLiteral("ponder"));       // czeka na odpowiedź Claude - zamyślona, stoi w miejscu
        } else if (m_search->isVisible() && m_reactions.empty()) {
            setAnim(QStringLiteral("ponder"));       // wyszukiwarka otwarta: zamyślona z ręką przy buzi
        } else if (!m_reactions.empty()) {
            auto &[name, left] = m_reactions.front();
            const QString n = name;
            setAnim(n);
            if (left <= 1)
                m_reactions.erase(m_reactions.begin());
            else
                --left;
        } else {
            const bool talking = bubbleOpen();
            if (talking && m_activity == QLatin1String("walk")) {
                m_activity = QStringLiteral("stand");              // przy chmurce nie ucieka
                animlog::write(QStringLiteral("talking_walk_to_stand"));
                m_activityLeft = 50;
            } else if (!talking) {
                if (--m_activityLeft <= 0)
                    pickActivity();
            }
            setAnim(m_activity);
            if (m_activity == QLatin1String("walk")) {
                double x = m_pos.x() + m_dir * cfg::WALK_SPEED;
                const double lo = a.left(), hi = a.right() + 1 - m_bodyW;
                if (x <= lo || x >= hi) {
                    x = std::max(lo, std::min(hi, x));
                    m_dir = -m_dir;
                }
                m_pos.setX(x);
            }
            if (++m_saveTicks >= SAVE_EVERY_TICKS && (m_pos.x() != m_savedX || m_dir != m_savedDir))
                savePosition();                // co ~10 s spaceru, gdy pozycja się zmieniła
        }
    }

    applyPos();
    placeBubble();
    update();
}

const QPixmap &Pet::frame() const
{
    const Anim &an = m_sprites.anim(m_anim);
    const int i = (m_animTick / an.ticks) % int(an.frames.size());
    // Odbicie lustrzane jest policzone raz przy ładowaniu (Sprites::buildMirrored).
    if ((m_dir > 0) == m_sprites.facesLeft && i < an.mirrored.size())
        return an.mirrored[i];
    return an.frames[i];
}

void Pet::paintEvent(QPaintEvent *)
{
    const QPixmap &pm = frame();
    const int x = frameX(pm);
    const int y = frameY(height(), pm.height());     // stopy zawsze na dole okna = na podłodze
    const QRect drawn(x, y, pm.width(), std::min(pm.height(), height()));
    if (m_drawRect != drawn) {
        m_drawRect = drawn;
        setMask(QRegion(drawn));                     // maska = dokładnie narysowany obszar
    }
    QPainter(this).drawPixmap(x, y, pm);
}

// -- mysz: krótkie kliknięcie = chmurka, ruch o kilka px = przeciąganie
void Pet::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_pressPos = e->globalPosition();
        m_info->hide();                  // dymek z menu zwykle już zamknął Popup; to zabezpieczenie dla zdarzeń bez niego
        m_dragOffset = e->globalPosition() - m_pos;
        // Drugi klik w czasie interwału: to podwójny klik. Oczekujący pojedynczy klik (dymek z muzyką) przepada,
        // a wyszukiwarka otworzy się przy zwolnieniu przycisku (chyba że to początek przeciągania).
        if (m_clickTimer->isActive()) {
            m_clickTimer->stop();
            m_doubleClick = true;
        }
    } else if (e->button() == Qt::RightButton) {
        showMenu(e->globalPosition().toPoint());
    }
}

void Pet::mouseMoveEvent(QMouseEvent *e)
{
    if (m_pressPos && m_state != State::Drag) {
        if ((e->globalPosition() - *m_pressPos).manhattanLength() < cfg::DRAG_THRESHOLD)
            return;
        m_state = State::Drag;
        animlog::write(QStringLiteral("state drag_start"));
        m_reactions.clear();
        m_bubble->hide();
        hideInfo();
        m_search->hide();
        requeueGpuAlert();
        m_clickTimer->stop();
    }
    if (m_state == State::Drag) {
        const QPointF now = e->globalPosition() - m_dragOffset;
        const double dx = now.x() - m_pos.x();
        if (std::abs(dx) > 1)
            m_dir = dx > 0 ? 1 : -1;
        m_pos = now;
        applyPos();
        placeBubble();
        update();
    }
}

void Pet::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || !m_pressPos)
        return;
    m_pressPos.reset();
    if (m_state == State::Floor)
        m_alert->hide();                 // klik w postać chowa ostrzeżenie automatyczne (bez przechwytywania myszy)
    const bool dbl = std::exchange(m_doubleClick, false);
    if (m_state == State::Drag) {
        animlog::write(QStringLiteral("state release_fall"));
        m_state = State::Fall;
        m_vy = 0;
        update();
    } else if (m_state != State::Floor) {
        // Złapana w locie bez przeciągania (szybkie rzuty): to nie jest klik w stojącą postać.
    } else if (dbl) {
        // Pierwszy klik pary zamknął wyszukiwarkę (utrata aktywności albo zwolnienie przycisku): para ją zamyka, nie otwiera na nowo.
        const bool closedByFirst = m_searchClosedByClick.isValid()
            && m_searchClosedByClick.elapsed() < QApplication::doubleClickInterval() + 150;
        m_searchClosedByClick.invalidate();
        if (m_search->isVisible())
            m_search->hide();               // podwójny klik na otwartej wyszukiwarce ją zamyka
        else if (!closedByFirst)
            openSearch();
    } else if (m_bubble->isVisible()) {
        m_bubble->hide();
    } else if (m_search->isVisible()) {
        m_search->hide();
        m_searchClosedByClick.start();
    } else {
        if (m_search->justClosed())
            m_searchClosedByClick.start();
        else
            m_searchClosedByClick.invalidate();
        // Pojedynczy klik czeka na ewentualny drugi; dymek z muzyką otworzy się po interwale podwójnego kliku.
        // Klik, który właśnie zamknął dymek, nie otwiera go znowu, ale nadal może być pierwszą połową podwójnego.
        m_clickOpens = !m_bubble->justClosed() && !m_search->justClosed() && !m_info->justClosed();
        m_clickTimer->start(QApplication::doubleClickInterval());
    }
}

// Systemowy podwójny klik (zamiast drugiego mousePress): ta sama obsługa co zwykłe naciśnięcie.
void Pet::mouseDoubleClickEvent(QMouseEvent *e)
{
    mousePressEvent(e);
    if (e->button() == Qt::LeftButton) {
        m_clickTimer->stop();
        m_doubleClick = true;
    }
}

void Pet::singleClick()
{
    if (m_clickOpens && m_state == State::Floor && !m_search->isVisible() && !m_bubble->isVisible()
        && !m_info->isVisible())
        openBubble(true);
}

// Kółko myszy na postaci: głośniej / ciszej (i pokaż chmurkę).
void Pet::wheelEvent(QWheelEvent *e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0)
        return;
    if (!m_bubble->isVisible())
        openBubble();
    m_bubble->changeVolume(cfg::pyRound(steps * cfg::VOLUME_STEP));
    // Szybkie kręcenie: zamyślona -> wesoła -> radość (głośność zmieniona jak zawsze).
    // Nie nadpisuje trwającej sekwencji (np. marudzenia po upadku); własne "zaskoczenie" z openBubble() zastępuje.
    const bool free = m_reactions.empty() || m_reactions.front().first == QLatin1String("surprised");
    if (m_spin.add(steps, m_clock.elapsed()) && m_state == State::Floor && free)
        react({{QStringLiteral("ponder"), 30}, {QStringLiteral("sit_happy"), 32}, {QStringLiteral("cheer"), 36}}, "wheel_spin");
}

// Czy nad głową wisi dymek (wtedy postać nie odchodzi).
bool Pet::bubbleOpen() const
{
    return m_bubble->isVisible() || m_search->isVisible() || m_alert->isVisible() || m_info->isVisible();
}

// Podwójny klik: wyszukiwarka nad głową (zastępuje dymek z muzyką).
void Pet::openSearch()
{
    m_clickTimer->stop();
    hideInfo();
    requeueGpuAlert();
    m_bubble->hide();
    m_search->reset();
    placeBubble(m_search);
    m_search->open();
}

// fromClick: klik w postać losuje reakcję (najczęściej zaskoczenie); kółko zawsze zaskakuje.
void Pet::openBubble(bool fromClick)
{
    if (m_state == State::Floor && m_reactions.empty()) {
        const int r = fromClick ? randint(0, 5) : 0;     // 0-3 zaskoczenie, 4 zawstydzenie, 5 radość
        if (r == 4)
            react({{QStringLiteral("shy"), 40}}, "click_shy");
        else if (r == 5)
            react({{QStringLiteral("cheer"), 36}}, "click_cheer");
        else
            react({{QStringLiteral("surprised"), 20}}, "bubble_open");
    }
    hideInfo();
    requeueGpuAlert();
    m_search->hide();
    m_bubble->refresh();
    placeBubble(m_bubble);
    m_bubble->popup();
}

void Pet::placeBubble(Bubble *which)
{
    Bubble *b = which ? which : m_search->isVisible() ? static_cast<Bubble *>(m_search) : m_bubble;
    if (which || b->isVisible())
        placeOne(b);
    if (which != m_alert && m_alert->isVisible())
        placeOne(m_alert);
    if (which != m_info && m_info->isVisible())
        placeOne(m_info);
}

void Pet::placeOne(Bubble *b)
{
    const QRect a = area(m_pos + QPointF(m_bodyW / 2.0, height() / 2.0));
    double x = m_pos.x() + m_bodyW / 2.0 - b->width() / 2.0;
    x = std::max<double>(a.left(), std::min<double>(a.right() + 1 - b->width(), x));
    double y = m_pos.y() + cfg::BUBBLE_OVERLAP - b->height();
    if (y < a.top()) {                    // postać wysoko (np. przeciągana) - dymek obok, bez ogonka
        x = m_pos.x() + m_bodyW;
        if (x + b->width() > a.right() + 1)
            x = m_pos.x() - b->width();
        y = std::max<double>(a.top(), m_pos.y());
        b->setTail(std::nullopt);
    } else {
        b->setTail(m_pos.x() + m_bodyW / 2.0 - x);
    }
    b->move(qRound(x), qRound(y));
}

// -- menu
void Pet::showMenu(const QPoint &pos)
{
    QMenu menu(this);
    buildMenu(menu);
    m_watcher.poll();                 // świeższe dane na następny raz (w tle)
    m_steam.refresh();
    menu.exec(pos);
}

// Menu z danych w pamięci (MPRIS i Steam odświeżane w tle) - bez D-Bus, pactl i dysku.
void Pet::buildMenu(QMenu &menu)
{
    const auto player = m_watcher.latest();
    if (player) {
        QString song = player->title.isEmpty() ? QStringLiteral("(bez tytułu)") : player->title;
        if (!player->artist.isEmpty())
            song = player->artist + QStringLiteral(" – ") + song;
        const QString icon = player->status == QLatin1String("Playing") ? QStringLiteral("▶")
                                                                         : QStringLiteral("⏸");
        menu.addAction(icon + QStringLiteral("  ") + song)->setEnabled(false);
        const QString name = player->name;
        const struct { const char *label; const char *method; } items[] = {
            {"⏮  Poprzedni", "Previous"}, {"⏯  Odtwórz / pauza", "PlayPause"}, {"⏭  Następny", "Next"}};
        for (const auto &it : items) {
            const QString method = QString::fromLatin1(it.method);
            menu.addAction(QString::fromUtf8(it.label), this, [this, name, method] { m_mpris.call(name, method); });
        }
    } else {
        menu.addAction(QStringLiteral("Nic nie gra"))->setEnabled(false);
    }
    menu.addSeparator();
    QMenu *modeMenu = menu.addMenu(QStringLiteral("Co ma robić?"));
    auto *group = new QActionGroup(modeMenu);
    for (const Mode &m : modes()) {
        QAction *act = modeMenu->addAction(QString::fromUtf8(m.label));
        act->setCheckable(true);
        act->setChecked(m_mode == QLatin1String(m.key));
        group->addAction(act);
        const QString key = QString::fromLatin1(m.key);
        connect(act, &QAction::triggered, this, [this, key] { setMode(key); });
    }
    menu.addAction(QStringLiteral("Jaka jest temperatura?"), this, &Pet::askTemperature);
    addGamesMenu(&menu);
    addGpuMenu(&menu);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Wróć na środek ekranu"), this, &Pet::returnToCenter);
    menu.addAction(QStringLiteral("Porozmawiaj z Claude…"), this, &Pet::openChat);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Zakończ"), qApp, [] { markExplicitQuit(); QApplication::quit(); });
}

// Podmenu "Gry": zainstalowane gry ze Steama, ostatnio grane na górze.
void Pet::addGamesMenu(QMenu *menu)
{
    QMenu *gamesMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("input-gaming")), QStringLiteral("Gry"));
    if (!m_gamesStyle)
        m_gamesStyle = std::make_unique<GamesMenuStyle>(QApplication::style()->name());
    gamesMenu->setStyle(m_gamesStyle.get());
    const QList<SteamGame> games = m_steam.games();
    for (const SteamGame &game : games) {
        QString label = game.name;
        label.replace(QLatin1Char('&'), QStringLiteral("&&"));          // "&" w QAction to skrót klawiszowy
        if (game.updating)
            label += QStringLiteral(" (aktualizacja)");
        QAction *act = gamesMenu->addAction(label);
        const QIcon icon = m_steam.icon(game);
        if (!icon.isNull())
            act->setIcon(icon);
        connect(act, &QAction::triggered, this, [this, game] { launchGame(game); });
    }
    if (games.isEmpty()) {
        // Tuż po starcie lista jeszcze się wczytuje w tle - nie udajemy, że gier nie ma.
        const QString text = m_steam.ready() ? QStringLiteral("Brak gier ze Steama") : QStringLiteral("Wczytywanie…");
        gamesMenu->addAction(text)->setEnabled(false);
    }
    gamesMenu->addSeparator();
    gamesMenu->addAction(QStringLiteral("Otwórz bibliotekę Steam"), this,
                         [] { openSteamUrl(QLatin1String(cfg::STEAM_LIBRARY_URL)); });
}

void Pet::launchGame(const SteamGame &game)
{
    if (openSteamUrl(QString::fromLatin1(cfg::STEAM_RUN_URL).arg(game.appid)))
        react({{QStringLiteral("surprised"), 20}}, "launch_game");
    else
        react({{QStringLiteral("grumpy"), 48}}, "launch_game_fail");
}

void Pet::openChat()
{
    if (!m_chat) {
        m_chat = new ChatDialog;
        // Podłączone sygnałami: czat nic nie wie o postaci.
        connect(m_chat, &ChatDialog::requestStarted, this, [this] { m_chatWaiting = true; });
        connect(m_chat, &ChatDialog::requestFinished, this, [this](bool ok) {
            m_chatWaiting = false;
            if (ok && m_state == State::Floor)
                react({{QStringLiteral("cheer"), 36}}, "chat_done");     // przy błędzie API bez radości
        });
    }
    m_chat->show();
    m_chat->raise();
    m_chat->activateWindow();
}

// Co 3 s: zapytanie o utwór (asynchronicznie). Radość przy zmianie tytułu/wykonawcy, tylko gdy postać stoi na podłodze.
void Pet::checkTrack()
{
    m_watcher.poll();      // asynchronicznie; gdy poprzednie trwa, tick jest pomijany
}

void Pet::onTrackPolled(const QString &key, bool playing)
{
    const QString player = m_watcher.latest() ? m_watcher.latest()->name : QString();
    const QString useKey = playing ? key : QString();
    const bool cheer = m_track.update(player, useKey, m_clock.elapsed(), m_state == State::Floor);
    if (animlog::enabled()) {
        const QString h = animlog::shortHash(useKey);
        if (h != m_logTrack || player != m_logPlayer) {      // wpis tylko przy zmianie odczytu (bez treści utworu)
            animlog::write(QStringLiteral("mpris_poll player=%1 key=%2 (was %3) valid=%4 playing=%5 cheer=%6")
                               .arg(player.mid(int(qstrlen(MPRIS_PREFIX))), h, m_logTrack)
                               .arg(TrackChangeDetector::validKey(useKey) ? 1 : 0).arg(playing ? 1 : 0).arg(cheer ? 1 : 0));
            m_logTrack = h;
            m_logPlayer = player;
        }
    }
    if (cheer && m_reactions.empty())
        react({{QStringLiteral("cheer"), 36}}, "mpris_change");
}

void Pet::setupApplication()
{
    QApplication::setQuitOnLastWindowClosed(false);   // okna Qt::Tool (dymki, czat) nie mogą kończyć programu
    // KJob/KIO w runnerach KDE trzymają QEventLoopLocker; zwolnienie ostatniego kończyło pętlę zdarzeń (ciche wyjście).
    QCoreApplication::setQuitLockEnabled(false);
}

void Pet::markExplicitQuit()
{
    qApp->setProperty("explicitQuit", true);
}

// -- ostrzeżenia o temperaturze GPU (LACT)
void Pet::showTemperature(const QString &text, std::optional<TempCard> card)
{
    m_infoPending = text;
    m_infoCard = card;
    tryShowGpuAlert();
}

// Z pamięci, gdy odczyt jest świeży; inaczej jeden asynchroniczny device_stats (menu się nie blokuje).
void Pet::askTemperature()
{
    if (m_lact.enabled() && m_lact.latest() && m_lact.latestAgeMs() <= FRESH_MS) {
        showTemperature(tempsText(*m_lact.latest()), readingCard(*m_lact.latest(), m_gpu));
        return;
    }
    m_tempWanted = true;
    m_lact.readOnce();
}

void Pet::saveGpuConfig()
{
    cfg::saveSettingKeyAsync(QStringLiteral("gpu_alert"), m_gpu.toJson());    // w tle, pozostałe klucze zostają
}

void Pet::setGpuAlertEnabled(bool on)
{
    m_gpu.enabled = on;
    saveGpuConfig();
    m_lact.setEnabled(on);              // wyłączone = żadnego odpytywania
    if (!on) {
        m_gpuPending.clear();
        m_gpuPendingCard.reset();
        m_alert->hide();
        m_gpuLogic.reset();
    }
}

void Pet::setGpuThresholds(int edge, int hotspot)
{
    m_gpu.edge = std::clamp(edge, GPU_EDGE_MIN, GPU_EDGE_MAX);   // zakresy jak w oknie "Własny…"
    m_gpu.hotspot = std::clamp(hotspot, GPU_HOT_MIN, GPU_HOT_MAX);
    m_gpuLogic.setThresholds(m_gpu.edge, m_gpu.hotspot);
    saveGpuConfig();
}

void Pet::onGpuReading(const GpuTemps &t)
{
    if (!m_gpu.enabled)
        return;
    if (const auto alert = m_gpuLogic.update(t, m_clock.elapsed())) {
        m_gpuPending = alert->text;       // nowsze ostrzeżenie zastępuje czekające
        m_gpuPendingSevere = alert->severe;
        TempCard c;
        c.kind = TempCard::Warning;
        c.temps = t;
        c.edgeLimit = m_gpu.edge;
        c.hotspotLimit = m_gpu.hotspot;
        c.edgeOver = alert->edgeOver;
        c.hotspotOver = alert->hotOver;
        c.summary = alert->text;
        m_gpuPendingCard = c;
        tryShowGpuAlert();
    }
}

// Pokazuje czekające dymki, gdy nad głową jest wolne miejsce: postać stoi (nie spada, nie jest
// przeciągana), nie ma dymka z muzyką, wyszukiwarki ani dymka z menu.
// Odpowiedź usera (menu) ma pierwszeństwo przed ostrzeżeniem automatycznym: ostrzeżenie ustępuje i wraca później.
void Pet::tryShowGpuAlert()
{
    const bool warn = !m_gpuPending.isEmpty() && m_gpu.enabled;
    if (!warn && m_infoPending.isEmpty())
        return;
    if (m_state != State::Floor || m_pressPos || m_bubble->isVisible() || m_search->isVisible()
        || m_info->isVisible() || m_infoScheduled)
        return;
    if (!m_infoPending.isEmpty()) {       // odpowiedź na pytanie o temperaturę (też przy wyłączonych ostrzeżeniach)
        showInfoBubble();
        return;
    }
    if (m_alert->isVisible())
        return;
    m_alert->setTimeoutMs(AlertBubble::TIMEOUT_MS);
    if (m_gpuPendingCard)
        m_alert->setCard(*m_gpuPendingCard);
    else
        m_alert->setMessage(m_gpuPending);
    m_gpuPending.clear();
    m_gpuPendingCard.reset();
    placeBubble(m_alert);
    m_alert->show();
    react({{QStringLiteral("surprised"), 20},
           {QLatin1String(m_gpuPendingSevere ? "grumpy" : "shy"), 40}}, "gpu_alert");
}

// Dymek z menu to Qt::Popup: pokazany w tym samym obiegu co zamknięcie QMenu mógłby złapać to samo kliknięcie
// i od razu się zamknąć, więc pokazujemy go w następnym obiegu pętli.
void Pet::showInfoBubble()
{
    m_infoScheduled = true;
    requeueGpuAlert();                    // ostrzeżenie automatyczne ustępuje (wróci po zamknięciu odpowiedzi)
    m_info->setTimeoutMs(INFO_MS);
    if (m_infoCard)
        m_info->setCard(*m_infoCard);
    else
        m_info->setMessage(m_infoPending);
    m_infoPending.clear();
    m_infoCard.reset();
    QTimer::singleShot(0, this, [this] {
        m_infoScheduled = false;
        if (m_state != State::Floor || m_pressPos || m_bubble->isVisible() || m_search->isVisible()) {
            m_infoPending = m_info->message();       // nie zgubi się: wróci, gdy będzie wolne miejsce
            m_infoCard = m_info->card();
            return;
        }
        placeBubble(m_info);
        m_info->show();
        m_info->raise();
        react({{QStringLiteral("ponder"), 30}}, "temp_info");
    });
}

// Dymek z menu zniknie bez wracania (klik w postać, przeciąganie, inny dymek).
void Pet::hideInfo()
{
    m_info->hide();
}

void Pet::requeueGpuAlert()
{
    if (!m_alert->isVisible())
        return;
    if (m_gpuPending.isEmpty()) {
        m_gpuPending = m_alert->message();      // nie zgubi się: wróci po zamknięciu tamtego dymka
        m_gpuPendingCard = m_alert->card();
    }
    m_alert->hide();
}

// Prawy przycisk: przełącznik, progi i (gdy włączone) ostatni odczyt albo powód niedostępności - z pamięci.
void Pet::addGpuMenu(QMenu *menu)
{
    menu->addSeparator();
    QAction *toggle = menu->addAction(QStringLiteral("Ostrzeżenia o temperaturze GPU"));
    toggle->setCheckable(true);
    toggle->setChecked(m_gpu.enabled);
    connect(toggle, &QAction::triggered, this, [this](bool on) { setGpuAlertEnabled(on); });
    if (m_gpu.enabled) {
        QString info;
        if (const auto &t = m_lact.latest()) {
            info = tempsText(*t);
        } else if (!m_lact.reason().isEmpty()) {
            info = QStringLiteral("LACT niedostępny (%1)").arg(m_lact.reason());
        } else {
            info = QStringLiteral("LACT: łączenie…");
        }
        menu->addAction(info)->setEnabled(false);
    }
    QMenu *thr = menu->addMenu(QStringLiteral("Progi"));
    auto addGroup = [this, thr](const QString &title, std::initializer_list<int> values, int current, bool isEdge) {
        thr->addSection(title);
        auto *group = new QActionGroup(thr);
        auto addValue = [&](int v, const QString &label) {
            QAction *act = thr->addAction(label);
            act->setCheckable(true);
            act->setChecked(v == current);
            group->addAction(act);
            connect(act, &QAction::triggered, this, [this, v, isEdge] {
                setGpuThresholds(isEdge ? v : m_gpu.edge, isEdge ? m_gpu.hotspot : v);
            });
        };
        bool listed = false;
        for (int v : values) {
            addValue(v, QStringLiteral("%1°C").arg(v));
            listed = listed || v == current;
        }
        if (!listed)                       // próg spoza listy (własny albo ręcznie wpisany w settings.json)
            addValue(current, QStringLiteral("%1 °C (własny)").arg(current));
        QAction *custom = thr->addAction(QStringLiteral("Własny…"));
        connect(custom, &QAction::triggered, this, [this, isEdge] { askCustomThreshold(isEdge); });
    };
    addGroup(QStringLiteral("GPU (edge)"), {60, 65, 70, 75, 80, 85, 90, 95}, m_gpu.edge, true);
    addGroup(QStringLiteral("Hotspot (junction)"), {75, 80, 85, 90, 95, 100, 105, 110, 115}, m_gpu.hotspot, false);
}

// "Własny…": małe okno z QSpinBox (edge 40-110, hotspot 50-125), zapis jak przy progach z listy.
void Pet::askCustomThreshold(bool isEdge)
{
    auto *dlg = new QDialog;
    dlg->setObjectName(QStringLiteral("gpuThresholdDialog"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(isEdge ? QStringLiteral("Własny próg GPU (edge)") : QStringLiteral("Własny próg hotspotu"));
    auto *spin = new QSpinBox;
    spin->setObjectName(QStringLiteral("gpuThresholdSpin"));
    spin->setRange(isEdge ? GPU_EDGE_MIN : GPU_HOT_MIN, isEdge ? GPU_EDGE_MAX : GPU_HOT_MAX);
    spin->setSingleStep(1);
    spin->setSuffix(QStringLiteral(" °C"));
    spin->setValue(isEdge ? m_gpu.edge : m_gpu.hotspot);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *lay = new QVBoxLayout(dlg);
    lay->addWidget(new QLabel(QStringLiteral("Ostrzegaj powyżej:")));
    lay->addWidget(spin);
    lay->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(dlg, &QDialog::accepted, this, [this, spin, isEdge] {
        const int v = spin->value();
        setGpuThresholds(isEdge ? v : m_gpu.edge, isEdge ? m_gpu.hotspot : v);
    });
    dlg->open();
}
