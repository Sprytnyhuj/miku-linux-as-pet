#include "searchbubble.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QWindow>
#include <QVBoxLayout>

namespace {

// Wiersz wyniku: ikona, tytuł (pogrubiony) i podtytuł (przygaszony); kategoria po prawej.
class ResultDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return {SearchBubble::WIDTH - 40, SearchBubble::ROW_H};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString sub = index.data(Qt::UserRole).toString();
        const QString cat = index.data(Qt::UserRole + 1).toString();
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget *w = opt.widget;
        (w ? w->style() : QApplication::style())->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, p, w);

        const QRect r = opt.rect.adjusted(6, 2, -6, -2);
        const int isz = 24;
        icon.paint(p, QRect(r.left(), r.center().y() - isz / 2 + 1, isz, isz));
        const bool sel = opt.state & QStyle::State_Selected;
        const QPalette::ColorRole fg = sel ? QPalette::HighlightedText : QPalette::Text;
        const QPalette::ColorRole dim = sel ? QPalette::HighlightedText : QPalette::PlaceholderText;
        int right = r.right();
        QFont small = opt.font;
        small.setPointSizeF(small.pointSizeF() * 0.85);
        if (!cat.isEmpty() && !sub.isEmpty()) {
            p->setFont(small);
            p->setPen(opt.palette.color(dim));
            const int cw = std::min(90, QFontMetrics(small).horizontalAdvance(cat));
            p->drawText(QRect(right - cw, r.top(), cw, r.height()), Qt::AlignRight | Qt::AlignVCenter,
                        QFontMetrics(small).elidedText(cat, Qt::ElideRight, cw));
            right -= cw + 8;
        }
        const int tx = r.left() + isz + 8;
        const QString line2 = sub.isEmpty() ? cat : sub;
        QFont bold = opt.font;
        bold.setBold(true);
        const int hTitle = r.height() / 2 + 1;
        p->setFont(bold);
        p->setPen(opt.palette.color(fg));
        const QRect tr(tx, r.top(), right - tx, line2.isEmpty() ? r.height() : hTitle);
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(bold).elidedText(title, Qt::ElideRight, tr.width()));
        if (!line2.isEmpty()) {
            p->setFont(small);
            p->setPen(opt.palette.color(dim));
            const QRect sr(tx, r.top() + hTitle, right - tx, r.height() - hTitle);
            p->drawText(sr, Qt::AlignLeft | Qt::AlignTop, QFontMetrics(small).elidedText(line2, Qt::ElideRight, sr.width()));
        }
    }
};

}  // namespace

SearchBubble::SearchBubble(SearchEngine *engine)
    // Tool + WindowStaysOnTop zamiast Popup: Popup pod XWayland/KWin nie zawsze dostaje fokus klawiatury.
    : Bubble(Qt::Tool | Qt::WindowStaysOnTopHint)
{
    m_edit = new QLineEdit;
    m_edit->setPlaceholderText(QStringLiteral("Szukaj programów, plików, ustawień…"));
    m_edit->setClearButtonEnabled(true);
    m_edit->installEventFilter(this);
    m_note = new QLabel;
    m_note->setWordWrap(true);
    m_note->setForegroundRole(QPalette::PlaceholderText);
    m_note->hide();
    m_list = new QListWidget;
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setItemDelegate(new ResultDelegate(m_list));
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMouseTracking(true);
    m_list->hide();

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10 + TAIL);
    lay->setSpacing(6);
    lay->addWidget(m_edit);
    lay->addWidget(m_note);
    lay->addWidget(m_list);
    setFixedWidth(WIDTH);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(DEBOUNCE_MS);
    connect(m_debounce, &QTimer::timeout, this, &SearchBubble::sendQuery);
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(NO_RESULTS_SETTLE_MS);
    connect(m_settle, &QTimer::timeout, this, &SearchBubble::noResults);
    // KWin/XWayland bywa, że nie przyznaje fokusu od razu (focus stealing prevention). Ponawiamy prośbę o aktywację
    // kilka razy; nigdy nie chowamy okna z tego powodu - pole przejmie fokus przy kliknięciu.
    m_focusWatch = new QTimer(this);
    m_focusWatch->setInterval(FOCUS_RETRY_MS);
    connect(m_focusWatch, &QTimer::timeout, this, [this] {
        if (!isVisible() || isActiveWindow() || ++m_focusTries > FOCUS_RETRIES) {
            m_focusWatch->stop();
            return;
        }
        requestFocus();
    });
    connect(m_edit, &QLineEdit::textChanged, this, &SearchBubble::textChanged);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { launchRow(m_list->row(it)); });

    setEngine(engine);
}

void SearchBubble::setEngine(SearchEngine *engine)
{
    if (m_engine) {
        m_engine->disconnect(this);
        m_engine->deleteLater();
    }
    m_engine = engine;
    if (!m_engine)
        return;
    m_engine->setParent(this);
    connect(m_engine, &SearchEngine::resultsChanged, this, &SearchBubble::showResults);
    connect(m_engine, &SearchEngine::finished, this, &SearchBubble::finishedQuery);
    connect(m_engine, &SearchEngine::problem, this, [this](const QString &text) {
        m_note->setText(text);
        relayout();
    });
}

void SearchBubble::reset()
{
    m_note->clear();
    m_edit->clear();
    m_list->clear();
    m_list->hide();
    m_sentQuery.clear();
    m_navigated = false;
    m_wasActive = false;
    m_launchPending = false;
    relayout();
}

void SearchBubble::open()
{
    reset();
    m_engine->begin();
    show();
    raise();
    m_focusTries = 0;
    requestFocus();
    m_focusWatch->start();
}

void SearchBubble::requestFocus()
{
    // QWindow::requestActivate pod xcb wysyła _NET_ACTIVE_WINDOW ze świeżym znacznikiem czasu ostatniej interakcji
    // (kliknięcie w postać), więc KWin traktuje to jako akcję użytkownika.
    if (QWindow *w = windowHandle())
        w->requestActivate();
    activateWindow();
    m_edit->setFocus(Qt::ActiveWindowFocusReason);
}

bool SearchBubble::justClosed() const
{
    return m_hiddenAt.isValid() && m_hiddenAt.elapsed() < 400;
}

QString SearchBubble::resultTitle(int row) const
{
    const QListWidgetItem *it = m_list->item(row);
    return it ? it->text() : QString();
}

void SearchBubble::textChanged()
{
    m_launchPending = false;
    m_settle->stop();
    emit typing();
    if (m_edit->text().trimmed().isEmpty()) {
        m_debounce->stop();
        m_sentQuery.clear();
        m_engine->query(QString());
        return;
    }
    m_debounce->start();                  // wyniki dopiero po chwili bez pisania
}

void SearchBubble::sendQuery()
{
    m_sentQuery = m_edit->text();
    m_navigated = false;
    m_engine->query(m_sentQuery);
}

void SearchBubble::showResults(const QString &query, const QList<SearchResult> &results)
{
    if (query != m_sentQuery && !(query.isEmpty() && m_sentQuery.isEmpty()))
        return;                           // spóźniona odpowiedź na starsze zapytanie
    if (!m_note->text().isEmpty()) {
        m_note->clear();
    }
    const QString keep = m_navigated && m_list->currentItem() ? m_list->currentItem()->text() : QString();
    m_list->setUpdatesEnabled(false);
    m_list->clear();
    int select = 0;
    for (const SearchResult &r : results) {
        auto *it = new QListWidgetItem(r.title, m_list);
        it->setIcon(r.icon);
        it->setData(Qt::UserRole, r.subtitle);
        it->setData(Qt::UserRole + 1, r.category);
        if (!keep.isEmpty() && r.title == keep)
            select = m_list->count() - 1;
    }
    if (m_list->count() > 0) {
        m_list->setCurrentRow(select);
        m_settle->stop();
    }
    m_list->setUpdatesEnabled(true);
    relayout();
    if (m_launchPending && m_list->count() > 0) {
        m_launchPending = false;
        launchRow(0);
    }
}

void SearchBubble::finishedQuery(const QString &query)
{
    if (query != m_sentQuery || query.trimmed().isEmpty())
        return;
    m_launchPending = false;
    if (m_list->count() == 0)
        m_settle->start();                // dopiero gdy przez chwilę nadal nic - nie migotamy przy każdym znaku
    if (!m_note->text().isEmpty()) {
        m_note->clear();
        relayout();
    }
}

void SearchBubble::relayout()
{
    m_note->setVisible(!m_note->text().isEmpty());
    const int n = m_list->count();
    m_list->setVisible(n > 0);
    m_list->setFixedHeight(std::min(n, VISIBLE_ROWS) * ROW_H + 2);
    adjustSize();
    emit layoutChanged();
}

void SearchBubble::move(int delta)
{
    const int n = m_list->count();
    if (n == 0)
        return;
    m_navigated = true;
    m_list->setCurrentRow(std::max(0, std::min(n - 1, m_list->currentRow() + delta)));
}

void SearchBubble::launchCurrent()
{
    if (m_debounce->isActive()) {         // Enter tuż po ostatnim znaku: wyślij zapytanie teraz, uruchom pierwszy wynik
        m_debounce->stop();
        m_launchPending = true;
        sendQuery();
        return;
    }
    launchRow(m_list->currentRow());
}

void SearchBubble::launchRow(int row)
{
    if (row < 0 || row >= m_list->count())
        return;
    const bool ok = m_engine->run(row);
    hide();
    if (ok)
        emit launched();
}

bool SearchBubble::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_edit && e->type() == QEvent::MouseButtonPress && !isActiveWindow())
        requestFocus();                   // kliknięcie w pole: weź fokus, jeśli WM nie dał go od razu
    if (obj == m_edit && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        switch (k->key()) {
        case Qt::Key_Down: move(1); return true;
        case Qt::Key_Up: move(-1); return true;
        case Qt::Key_PageDown: move(VISIBLE_ROWS); return true;
        case Qt::Key_PageUp: move(-VISIBLE_ROWS); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: launchCurrent(); return true;
        case Qt::Key_Escape: hide(); return true;
        default: break;
        }
    }
    return Bubble::eventFilter(obj, e);
}

void SearchBubble::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        hide();
    else
        Bubble::keyPressEvent(e);
}

void SearchBubble::hideEvent(QHideEvent *e)
{
    m_hiddenAt.start();
    m_launchPending = false;
    m_debounce->stop();
    m_settle->stop();
    m_focusWatch->stop();
    m_engine->end();
    Bubble::hideEvent(e);
    emit closed();
}

void SearchBubble::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && isVisible()) {
        if (isActiveWindow())
            m_wasActive = true;
        else if (m_wasActive)
            hide();                       // klik poza dymkiem / inne okno -> zamknij
    }
    Bubble::changeEvent(e);
}
