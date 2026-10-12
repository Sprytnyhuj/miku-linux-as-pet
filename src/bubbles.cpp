#include "bubbles.h"

#include "config.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr int SENSOR_PAD = 8;     // wcięcie treści dymka z temperaturą (tło wiersza ostrzeżenia wystaje poza nie)
}  // namespace

// ---------------------------------------------------------------- Bubble

Bubble::Bubble(Qt::WindowFlags flags)
    : QWidget(nullptr, flags | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
}

void Bubble::setTail(std::optional<double> x)
{
    if (x)
        x = std::max(24.0, std::min(width() - 24.0, *x));
    if (x.has_value() != m_tailX.has_value() || (x && std::abs(*x - *m_tailX) >= 1)) {
        m_tailX = x;
        update();
    }
}

void Bubble::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange)
        update();                           // zmiana motywu jasny/ciemny
    QWidget::changeEvent(e);
}

void Bubble::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(1, 1, -1, -1 - TAIL);
    QPainterPath path;
    path.addRoundedRect(body, RADIUS, RADIUS);
    if (m_tailX) {
        QPainterPath tail;
        tail.moveTo(*m_tailX - 10, body.bottom() - 1);
        tail.lineTo(*m_tailX, body.bottom() + TAIL - 1);
        tail.lineTo(*m_tailX + 10, body.bottom() - 1);
        tail.closeSubpath();
        path = path.united(tail);
    }
    // Tło jak okna systemu, ramka: akcent motywu zmieszany z Mid (delikatniejszy od gołego Highlight).
    const QPalette &pal = palette();
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor mid = pal.color(QPalette::Mid);
    const QColor border((accent.red() * 3 + mid.red()) / 4, (accent.green() * 3 + mid.green()) / 4,
                        (accent.blue() * 3 + mid.blue()) / 4);
    p.setPen(QPen(border, 1.5));
    p.setBrush(pal.color(QPalette::Window));
    p.drawPath(path);
}

// ---------------------------------------------------------------- MusicBubble

MusicBubble::MusicBubble(Mpris *mpris, TrackWatcher *watcher, PlayerVolume *volume)
    // Qt::Popup: jak menu - kliknięcie gdziekolwiek poza dymkiem go zamyka.
    : Bubble(Qt::Popup), m_mpris(mpris), m_watcher(watcher), m_volume(volume)
{
    m_title = new QLabel;
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_artist = new QLabel;
    m_artist->setForegroundRole(QPalette::PlaceholderText);
    QFont small = m_artist->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    m_artist->setFont(small);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addStretch();
    const struct { const char *method; const char *glyph; } btns[] = {
        {"Previous", "⏮"}, {"PlayPause", "⏯"}, {"Next", "⏭"}};
    for (const auto &b : btns) {
        auto *btn = new QToolButton;
        btn->setText(QString::fromUtf8(b.glyph));
        QFont f = btn->font();
        f.setPointSizeF(f.pointSizeF() * 1.3);
        btn->setFont(f);
        btn->setMinimumSize(40, 32);
        btn->setCursor(Qt::PointingHandCursor);
        const QString method = QString::fromLatin1(b.method);
        connect(btn, &QToolButton::clicked, this, [this, method] { press(method); });
        m_buttons.insert(method, btn);
        row->addWidget(btn);
    }
    row->addStretch();

    auto *volRow = new QHBoxLayout;
    volRow->setSpacing(6);
    m_volDown = new QToolButton;
    m_volDown->setText(QStringLiteral("−"));
    m_volUp = new QToolButton;
    m_volUp->setText(QStringLiteral("+"));
    for (auto [b, delta] : {std::pair{m_volDown, -cfg::VOLUME_STEP}, std::pair{m_volUp, cfg::VOLUME_STEP}}) {
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setAutoRepeat(true);               // przytrzymanie = ciągła zmiana
        connect(b, &QToolButton::clicked, this, [this, d = delta] { changeVolume(d); });
    }
    m_volSlider = new QSlider(Qt::Horizontal);
    m_volSlider->setRange(0, 100);
    m_volSlider->setFixedWidth(140);
    connect(m_volSlider, &QSlider::valueChanged, this, &MusicBubble::sliderMoved);
    m_volLabel = new QLabel;
    m_volLabel->setFixedWidth(44);
    m_volLabel->setForegroundRole(QPalette::PlaceholderText);
    m_volLabel->setFont(small);
    volRow->addStretch();
    for (QWidget *w : {static_cast<QWidget *>(m_volDown), static_cast<QWidget *>(m_volSlider),
                       static_cast<QWidget *>(m_volUp), static_cast<QWidget *>(m_volLabel)})
        volRow->addWidget(w);
    volRow->addStretch();

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 10, 16, 10 + TAIL);
    lay->setSpacing(3);
    lay->addWidget(m_title, 0, Qt::AlignHCenter);
    lay->addWidget(m_artist, 0, Qt::AlignHCenter);
    lay->addSpacing(4);
    lay->addLayout(row);
    lay->addSpacing(2);
    lay->addLayout(volRow);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(cfg::BUBBLE_TIMEOUT_MS);
    connect(m_hideTimer, &QTimer::timeout, this, &QWidget::hide);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(2000);
    connect(m_refreshTimer, &QTimer::timeout, this, &MusicBubble::refresh);
    // Wyniki odczytów w tle odświeżają widoczny dymek (bez blokowania czegokolwiek).
    connect(m_watcher, &TrackWatcher::polled, this, [this] {
        if (!isVisible())
            return;
        const auto player = m_watcher->latest();
        showPlayer(player, m_volume->get(player));
        m_volume->refresh(player);
    });
    connect(m_volume, &PlayerVolume::changed, this, [this] {
        if (isVisible() && !m_volSlider->isSliderDown())
            showVolume(m_volume->get(m_player));
    });
}

void MusicBubble::popup()
{
    show();
    raise();
    m_hideTimer->start();
    m_refreshTimer->start();
}

void MusicBubble::hideEvent(QHideEvent *e)
{
    m_hiddenAt.start();
    m_refreshTimer->stop();
    QWidget::hideEvent(e);
}

bool MusicBubble::justClosed() const
{
    return m_hiddenAt.isValid() && m_hiddenAt.elapsed() < 400;
}

void MusicBubble::wheelEvent(QWheelEvent *e)       // przy otwartym dymku kółko zawsze zmienia głośność
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps != 0)
        changeVolume(cfg::pyRound(steps * cfg::VOLUME_STEP));
}

void MusicBubble::enterEvent(QEnterEvent *)        // nie znikaj, dopóki kursor jest nad dymkiem
{
    m_hideTimer->stop();
}

void MusicBubble::leaveEvent(QEvent *)
{
    m_hideTimer->start();
}

void MusicBubble::refresh()
{
    const auto player = m_watcher->latest();        // z pamięci, bez czekania na D-Bus i pactl
    showPlayer(player, m_volume->get(player));
    m_watcher->poll();                              // świeże dane przyjdą sygnałem polled
    m_volume->refresh(player);
}

void MusicBubble::showPlayer(const std::optional<PlayerInfo> &player, const std::optional<VolumeState> &vol)
{
    m_player = player;
    QString title, artist;
    if (player) {
        const QString icon = player->status == QLatin1String("Playing") ? QStringLiteral("♪ ")
                                                                         : QStringLiteral("⏸ ");
        title = icon + (player->title.isEmpty() ? QStringLiteral("(bez tytułu)") : player->title);
        artist = player->artist;
    } else {
        title = QStringLiteral("Nic nie gra");
        artist = QStringLiteral("włącz YouTube Music");
    }
    m_title->setText(m_title->fontMetrics().elidedText(title, Qt::ElideRight, TEXT_W));
    m_title->setToolTip(title.size() > 40 ? title : QString());
    m_artist->setText(m_artist->fontMetrics().elidedText(artist, Qt::ElideRight, TEXT_W));
    m_artist->setVisible(!artist.isEmpty());
    for (QToolButton *b : std::as_const(m_buttons))
        b->setEnabled(m_player.has_value());
    if (!m_volSlider->isSliderDown())
        showVolume(vol);
    adjustSize();
}

void MusicBubble::showVolume(const std::optional<VolumeState> &vol)
{
    for (QWidget *w : {static_cast<QWidget *>(m_volDown), static_cast<QWidget *>(m_volSlider),
                       static_cast<QWidget *>(m_volUp), static_cast<QWidget *>(m_volLabel)})
        w->setEnabled(vol.has_value());
    if (!vol) {
        m_volLabel->setText(QStringLiteral("–"));
        m_volSlider->setToolTip(QStringLiteral("Odtwarzacz nie gra teraz dźwięku"));
        return;
    }
    m_volSlider->setToolTip(QString());
    m_volSlider->blockSignals(true);
    m_volSlider->setValue(vol->percent);
    m_volSlider->blockSignals(false);
    m_volLabel->setText(vol->muted ? QStringLiteral("wycisz.") : QString::number(vol->percent) + QLatin1Char('%'));
}

void MusicBubble::sliderMoved(int percent)
{
    m_volume->set(m_player, percent);
    m_volLabel->setText(QString::number(percent) + QLatin1Char('%'));
    m_hideTimer->start();
}

void MusicBubble::changeVolume(int delta)
{
    // Od ostatniej znanej wartości, bez czekania na pactl; gdy jeszcze nieznana - po odczycie w tle.
    if (const auto now = m_volume->adjust(m_player, delta))
        showVolume(now);
    m_hideTimer->start();
}

void MusicBubble::press(const QString &method)
{
    if (m_player) {
        m_mpris->call(m_player->name, method);
        QTimer::singleShot(400, this, &MusicBubble::refresh);
    }
    m_hideTimer->start();
}

// ---------------------------------------------------------------- dymek z temperaturą

TempState tempState(double value, double limit)
{
    if (value > limit)
        return TempState::Over;
    return value >= limit - 10 ? TempState::Near : TempState::Ok;
}

namespace {

bool isDark(const QPalette &pal)
{
    return pal.color(QPalette::Window).lightness() < 128;
}

QColor mixed(const QColor &a, const QColor &b, double t)     // t = udział a
{
    return QColor::fromRgbF(a.redF() * t + b.redF() * (1 - t), a.greenF() * t + b.greenF() * (1 - t),
                            a.blueF() * t + b.blueF() * (1 - t));
}

QFont scaled(QFont f, double factor)
{
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * factor);
    else if (f.pixelSize() > 0)
        f.setPixelSize(std::max(1, int(std::lround(f.pixelSize() * factor))));
    return f;
}

QIcon themeIcon(const QStringList &names)
{
    for (const QString &n : names)
        if (QIcon::hasThemeIcon(n))
            return QIcon::fromTheme(n);
    return {};
}

// Ikona z motywu w rozmiarze względnym do czcionki, ostra także na HiDPI.
void setIcon(QLabel *l, const QIcon &icon, int px, const QColor &tint = QColor())
{
    l->setVisible(!icon.isNull());
    if (icon.isNull())
        return;
    const qreal dpr = l->devicePixelRatioF();
    QPixmap pm = icon.pixmap(QSize(px, px), dpr);
    if (tint.isValid()) {                         // ikony jednokolorowe (currentColor) - w kolorze tekstu motywu
        QPainter p(&pm);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(pm.rect(), tint);
    }
    l->setPixmap(pm);
    l->setFixedSize(px, px);
}

// Duża liczba z jednostką: wspólna linia bazowa, wyrównanie do prawej, cyfry o stałej szerokości.
class TempValue : public QWidget {
public:
    explicit TempValue(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    void setReading(const QString &number, TempState s)
    {
        m_number = number;
        m_state = s;
        update();
    }
    QSize sizeHint() const override
    {
        const QFontMetrics nm(numberFont()), um(unitFont());
        return {nm.horizontalAdvance(QStringLiteral("888")) + gap() + um.horizontalAdvance(unit()), nm.height()};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        const QFont nf = numberFont(), uf = unitFont();
        const QFontMetrics nm(nf), um(uf);
        const int uw = um.horizontalAdvance(unit());
        const int baseline = nm.ascent();
        p.setPen(tempStateColor(m_state, palette()));
        p.setFont(uf);
        p.drawText(width() - uw, baseline, unit());
        p.setFont(nf);
        p.drawText(width() - uw - gap() - nm.horizontalAdvance(m_number), baseline, m_number);
    }

private:
    static QString unit() { return QStringLiteral("°C"); }
    int gap() const { return std::max(1, QFontMetrics(font()).horizontalAdvance(QLatin1Char(' ')) / 4); }
    QFont numberFont() const
    {
        QFont f = scaled(font(), 1.75);
        f.setBold(true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        f.setFeature(QFont::Tag("tnum"), 1);                 // cyfry tabelaryczne: kolumny liczb równo
#endif
        return f;
    }
    QFont unitFont() const
    {
        QFont f = scaled(font(), 1.05);
        f.setBold(true);
        return f;
    }
    QString m_number;
    TempState m_state = TempState::Ok;
};

// Poziomy miernik od TEMP_METER_MIN do próg + TEMP_METER_HEADROOM, ze znacznikiem progu.
class TempMeter : public QWidget {
public:
    explicit TempMeter(QWidget *parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setReading(double value, double limit)
    {
        m_value = value;
        m_limit = limit;
        update();
    }
    QSize sizeHint() const override { return {100, trackH() + 2 * OVER}; }
    QSize minimumSizeHint() const override { return {40, trackH() + 2 * OVER}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette &pal = palette();
        const double lo = TEMP_METER_MIN, hi = m_limit + TEMP_METER_HEADROOM;
        auto frac = [&](double v) { return std::clamp((v - lo) / (hi - lo), 0.0, 1.0); };
        const double th = trackH();
        const QRectF track(0.5, (height() - th) / 2.0, width() - 1.0, th);
        const double r = th / 2.0;
        const QColor fg = pal.color(QPalette::WindowText), bg = pal.color(QPalette::Window);
        const TempState st = tempState(m_value, m_limit);
        const QColor col = tempStateColor(st, pal);
        const double limitX = track.left() + track.width() * frac(m_limit);

        QPainterPath clip;
        clip.addRoundedRect(track, r, r);
        p.setClipPath(clip);
        p.fillRect(track, mixed(fg, bg, 0.14));
        QColor zone = tempStateColor(TempState::Over, pal);       // strefa powyżej progu - ledwo widoczna
        zone.setAlphaF(0.22);
        p.fillRect(QRectF(limitX, track.top(), track.right() - limitX, th), zone);
        p.fillRect(QRectF(track.left(), track.top(), track.width() * frac(m_value), th), col);
        p.setClipping(false);

        QColor tick = fg;
        tick.setAlphaF(0.85);
        const double tw = std::max(1.5, th / 3.5);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);                                            // obrys w kolorze tła - znacznik widać na wypełnieniu
        p.drawRoundedRect(QRectF(limitX - tw, track.top() - OVER, 2 * tw, th + 2 * OVER), tw, tw);
        p.setBrush(tick);
        p.drawRoundedRect(QRectF(limitX - tw / 2, track.top() - OVER + 0.5, tw, th + 2 * OVER - 1), tw / 2, tw / 2);
    }

private:
    static constexpr int OVER = 2;           // o tyle znacznik wystaje ponad miernik
    int trackH() const { return std::max(6, int(std::lround(QFontMetrics(font()).height() * 0.42))); }
    double m_value = 0, m_limit = 100;
};

// Wiersz czujnika: etykieta, próg, duża liczba i miernik; przy przekroczeniu delikatne tło w kolorze negatywnym.
class SensorBox : public QWidget {
public:
    SensorBox(const QString &label, QWidget *parent = nullptr) : QWidget(parent)
    {
        m_name = new QLabel(label);
        m_limit = new QLabel;
        m_limit->setForegroundRole(QPalette::PlaceholderText);
        m_limit->setFont(scaled(font(), 0.85));
        m_value = new TempValue;
        m_meter = new TempMeter;
        auto *top = new QHBoxLayout;
        top->setSpacing(8);
        top->addWidget(m_name, 0, Qt::AlignBottom);
        top->addWidget(m_limit, 0, Qt::AlignBottom);
        top->addStretch();
        top->addWidget(m_value);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(PAD, 4, PAD, 6);
        lay->setSpacing(3);
        lay->addLayout(top);
        lay->addWidget(m_meter);
    }
    void setReading(double value, int limit, bool highlight)
    {
        m_highlight = highlight;
        m_limit->setText(QStringLiteral("próg %1°C").arg(limit));
        m_value->setReading(QString::number(std::lround(value)), tempState(value, limit));
        m_meter->setReading(value, limit);
        QFont f = font();
        f.setBold(highlight);
        m_name->setFont(f);
        update();
    }
    static constexpr int PAD = SENSOR_PAD;

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_highlight)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = tempStateColor(TempState::Over, palette());
        c.setAlphaF(0.13);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

private:
    QLabel *m_name, *m_limit;
    TempValue *m_value;
    TempMeter *m_meter;
    bool m_highlight = false;
};

}  // namespace

QColor tempStateColor(TempState s, const QPalette &pal)
{
    // Kolory semantyczne w duchu Breeze (zielony / bursztynowy / czerwony), osobno dla jasnego i ciemnego tła.
    static const QColor light[] = {QColor(0x14, 0x7a, 0x3e), QColor(0xa8, 0x55, 0x00), QColor(0xc0, 0x1a, 0x2e)};
    static const QColor dark[] = {QColor(0x3d, 0xd6, 0x80), QColor(0xff, 0xb3, 0x47), QColor(0xff, 0x6e, 0x82)};
    return (isDark(pal) ? dark : light)[int(s)];
}

QString tempExtraLine(const GpuTemps &t)
{
    QStringList parts;
    if (t.powerW)
        parts << QStringLiteral("%1 W").arg(std::lround(*t.powerW));
    if (t.fanRpm)
        parts << QStringLiteral("wentylator %1 obr/min").arg(*t.fanRpm);
    if (t.busyPercent)
        parts << QStringLiteral("GPU %1%").arg(*t.busyPercent);
    return parts.join(QStringLiteral(" · "));
}

// Cała zawartość dymka z temperaturą: nagłówek, wiersze czujników, linia z dodatkami albo komunikat o braku LACT.
class TempCardView : public QWidget {
public:
    explicit TempCardView(QWidget *parent = nullptr) : QWidget(parent)
    {
        const QFont small = scaled(font(), 0.85);
        m_icon = new QLabel;
        m_title = new QLabel;
        QFont bold = font();
        bold.setBold(true);
        m_title->setFont(bold);
        m_when = new QLabel;
        m_when->setFont(small);
        m_when->setForegroundRole(QPalette::PlaceholderText);
        auto *head = new QHBoxLayout;
        head->setContentsMargins(SensorBox::PAD, 0, SensorBox::PAD, 0);
        head->setSpacing(6);
        head->addWidget(m_icon);
        head->addWidget(m_title);
        head->addStretch();
        head->addWidget(m_when);
        m_head = new QWidget;
        m_head->setLayout(head);

        m_edge = new SensorBox(QStringLiteral("GPU"));
        m_hot = new SensorBox(QStringLiteral("Hotspot"));
        m_extra = new QLabel;
        m_extra->setFont(small);
        m_extra->setForegroundRole(QPalette::PlaceholderText);
        m_extra->setContentsMargins(SensorBox::PAD, 0, SensorBox::PAD, 0);
        m_extra->setWordWrap(true);

        m_bigIcon = new QLabel;
        m_noTitle = new QLabel(QStringLiteral("Brak odczytu temperatury"));
        m_noTitle->setFont(bold);
        m_noTitle->setWordWrap(true);
        m_noText = new QLabel(QStringLiteral("LACT niedostępny.\nSprawdź, czy działa lactd."));
        m_noText->setFont(small);
        m_noText->setForegroundRole(QPalette::PlaceholderText);
        m_noText->setWordWrap(true);
        auto *noCol = new QVBoxLayout;
        noCol->setSpacing(2);
        noCol->addWidget(m_noTitle);
        noCol->addWidget(m_noText);
        auto *noRow = new QHBoxLayout;
        noRow->setContentsMargins(SensorBox::PAD, 4, SensorBox::PAD, 4);
        noRow->setSpacing(12);
        noRow->addWidget(m_bigIcon, 0, Qt::AlignTop);
        noRow->addLayout(noCol, 1);
        m_unavailable = new QWidget;
        m_unavailable->setLayout(noRow);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(2);
        lay->addWidget(m_head);
        lay->addSpacing(2);
        lay->addWidget(m_edge);
        lay->addWidget(m_hot);
        lay->addWidget(m_extra);
        lay->addWidget(m_unavailable);
    }

    void setCard(const TempCard &c)
    {
        m_card = c;
        const bool ok = c.kind != TempCard::Unavailable;
        const GpuTemps &t = c.temps;
        const int px = std::max(16, QFontMetrics(font()).height());
        const int iconPx = std::max(20, px * 5 / 4);
        m_head->setVisible(ok);
        m_edge->setVisible(ok && t.edge.has_value());
        m_hot->setVisible(ok && t.hotspot.has_value());
        const QString extra = ok ? tempExtraLine(t) : QString();
        m_extra->setVisible(!extra.isEmpty());
        m_extra->setText(extra);
        m_unavailable->setVisible(!ok);
        if (!ok) {
            setIcon(m_bigIcon, themeIcon({QStringLiteral("dialog-warning"), QStringLiteral("dialog-error")}), px * 2);
            return;
        }
        const bool warn = c.kind == TempCard::Warning;
        const QString card = t.card.isEmpty() ? QStringLiteral("GPU") : t.card;
        const QFontMetrics fm(m_title->font());
        if (warn) {
            m_title->setText(QStringLiteral("Gorąco!"));
            m_when->setText(fm.elidedText(card, Qt::ElideRight, fm.averageCharWidth() * 14) + QStringLiteral(" · teraz"));
            setIcon(m_icon, themeIcon({QStringLiteral("temperature-warm"), QStringLiteral("dialog-warning")}), iconPx);
        } else {
            m_title->setText(fm.elidedText(card, Qt::ElideRight, fm.averageCharWidth() * 22));
            m_when->setText(QStringLiteral("teraz"));
            setIcon(m_icon, themeIcon({QStringLiteral("temperature-normal"), QStringLiteral("video-display")}), iconPx,
                    palette().color(QPalette::WindowText));
        }
        m_warn = warn;
        QPalette pal = m_title->palette();
        pal.setColor(QPalette::WindowText, warn ? tempStateColor(TempState::Over, palette()) : palette().color(QPalette::WindowText));
        m_title->setPalette(pal);
        if (t.edge)
            m_edge->setReading(*t.edge, c.edgeLimit, warn && c.edgeOver);
        if (t.hotspot)
            m_hot->setReading(*t.hotspot, c.hotspotLimit, warn && c.hotspotOver);
    }

protected:
    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::PaletteChange && m_title) {           // kolor nagłówka ostrzeżenia zależy od motywu
            if (m_card)
                setCard(*m_card);
            QPalette pal = m_title->palette();
            pal.setColor(QPalette::WindowText,
                         m_warn ? tempStateColor(TempState::Over, palette()) : palette().color(QPalette::WindowText));
            m_title->setPalette(pal);
        }
        QWidget::changeEvent(e);
    }

private:
    QLabel *m_icon, *m_title, *m_when, *m_extra, *m_bigIcon, *m_noTitle, *m_noText;
    QWidget *m_head, *m_unavailable;
    SensorBox *m_edge, *m_hot;
    bool m_warn = false;
    std::optional<TempCard> m_card;
};

// ---------------------------------------------------------------- AlertBubble

AlertBubble::AlertBubble(bool popup)
    : Bubble(popup ? Qt::Popup : (Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)), m_popup(popup)
{
    if (!popup)
        setAttribute(Qt::WA_ShowWithoutActivating);   // ostrzeżenie automatyczne: żadnego fokusu ani przechwytywania myszy
    setCursor(Qt::PointingHandCursor);
    m_text = new QLabel;
    m_text->setTextFormat(Qt::PlainText);
    m_text->setWordWrap(true);
    m_text->setFixedWidth(TEXT_W);
    m_text->setMargin(SENSOR_PAD);
    QFont bold = m_text->font();
    bold.setBold(true);
    m_text->setFont(bold);
    m_view = new TempCardView;
    m_view->setFixedWidth(TEXT_W + 2 * SENSOR_PAD);
    m_view->hide();
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 10, 6, 10 + TAIL);
    lay->addWidget(m_text);
    lay->addWidget(m_view);
    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(TIMEOUT_MS);
    connect(m_hideTimer, &QTimer::timeout, this, [this] {
        hide();
        emit dismissed();
    });
}

void AlertBubble::setMessage(const QString &text)
{
    m_message = text;
    m_card.reset();
    m_text->setText(text);
    m_text->show();
    m_view->hide();
    layout()->activate();
    adjustSize();
}

int AlertBubble::meterCount() const
{
    int n = 0;
    for (const QWidget *w : findChildren<QWidget *>())
        n += (dynamic_cast<const SensorBox *>(w) && w->isVisibleTo(this)) ? 1 : 0;
    return n;
}

void AlertBubble::setCard(const TempCard &card)
{
    m_message = card.summary;
    m_card = card;
    m_view->setCard(card);
    m_text->hide();
    m_view->show();
    m_view->layout()->invalidate();
    m_view->layout()->activate();
    layout()->activate();
    resize(sizeHint());
}

void AlertBubble::showEvent(QShowEvent *e)
{
    m_hideTimer->start();
    Bubble::showEvent(e);
}

void AlertBubble::mousePressEvent(QMouseEvent *)
{
    m_hideTimer->stop();
    hide();
    emit dismissed();
}

void AlertBubble::hideEvent(QHideEvent *e)
{
    m_hiddenAt.start();
    m_hideTimer->stop();
    Bubble::hideEvent(e);
}

bool AlertBubble::justClosed() const
{
    return m_hiddenAt.isValid() && m_hiddenAt.elapsed() < 400;
}

void AlertBubble::keyPressEvent(QKeyEvent *e)
{
    if (m_popup && e->key() == Qt::Key_Escape) {
        hide();
        emit dismissed();
        return;
    }
    Bubble::keyPressEvent(e);
}

void AlertBubble::enterEvent(QEnterEvent *e)       // dymek z menu: nie znikaj, dopóki kursor jest nad nim
{
    if (m_popup)
        m_hideTimer->stop();
    Bubble::enterEvent(e);
}

void AlertBubble::leaveEvent(QEvent *e)
{
    if (m_popup && isVisible())
        m_hideTimer->start();
    Bubble::leaveEvent(e);
}
