#include "claudechat.h"

#include "config.h"

#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSslSocket>
#include <QTextDocument>
#include <QVBoxLayout>
#include <cstdio>

// ---------------------------------------------------------------- API

QString apiKey()
{
    QString key = qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
    if (key.isEmpty()) {
        QFile f(cfg::apiKeyFile());
        if (f.open(QIODevice::ReadOnly))
            key = QString::fromUtf8(f.readAll()).trimmed();
    }
    return key;
}

QByteArray buildClaudeBody(const QJsonArray &messages)
{
    const QJsonObject body{
        {"model", QLatin1String(cfg::CLAUDE_MODEL)},
        {"max_tokens", 16000},
        {"system", cfg::claudeSystem()},
        {"messages", messages},
        {"output_config", QJsonObject{{"effort", "low"}}},
        {"fallbacks", "default"},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

namespace {
ClaudeResult fail(const QString &text)
{
    ClaudeResult r;
    r.text = text;
    return r;
}
}  // namespace

ClaudeResult parseClaudeReply(int status, const QByteArray &body)
{
    if (status == 0)
        return fail(QStringLiteral("Brak połączenia z API."));
    if (status == 401)
        return fail(QStringLiteral("Nieprawidłowy klucz API."));
    if (status == 429)
        return fail(QStringLiteral("Za dużo zapytań - spróbuj za chwilę."));

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject();

    if (status < 200 || status >= 300) {
        QString msg = obj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (msg.isEmpty())
            msg = QString::fromUtf8(body).trimmed();
        if (msg.isEmpty())
            msg = QStringLiteral("nieznany błąd");
        return fail(QStringLiteral("Błąd API (%1): %2").arg(status).arg(msg));
    }
    if (obj.isEmpty() || !obj.value(QStringLiteral("content")).isArray())
        return fail(QStringLiteral("Błąd API (%1): nieprawidłowa odpowiedź.").arg(status));
    if (obj.value(QStringLiteral("stop_reason")).toString() == QLatin1String("refusal"))
        return fail(QStringLiteral("Claude odmówił odpowiedzi na to pytanie."));

    ClaudeResult r;
    r.ok = true;
    r.content = obj.value(QStringLiteral("content")).toArray();
    QString text;
    for (const QJsonValue &b : std::as_const(r.content)) {
        const QJsonObject bo = b.toObject();
        if (bo.value(QStringLiteral("type")).toString() == QLatin1String("text"))
            text += bo.value(QStringLiteral("text")).toString();
    }
    text = text.trimmed();
    r.text = text.isEmpty() ? QStringLiteral("(pusta odpowiedź)") : text;
    return r;
}

ClaudeClient::ClaudeClient(QObject *parent) : QObject(parent) {}

void ClaudeClient::send(const QJsonArray &messages)
{
    const QString key = apiKey();
    if (key.isEmpty()) {
        QTimer::singleShot(0, this, [this] {
            emit done(false, {}, QStringLiteral("Brak klucza API. Wpisz go do %1 albo ustaw ANTHROPIC_API_KEY.")
                                     .arg(cfg::apiKeyFile()));
        });
        return;
    }
    if (!QSslSocket::supportsSsl())
        std::fprintf(stderr, "desktop-pet: brak obsługi TLS w Qt - zapytanie do API się nie uda\n");
    QNetworkRequest req{QUrl(QLatin1String(cfg::CLAUDE_URL))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-api-key", key.toUtf8());
    req.setRawHeader("anthropic-version", "2023-06-01");
    req.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");
    req.setTransferTimeout(600000);                    // jak domyślny limit klienta Anthropic (10 min)
    QNetworkReply *reply = m_net.post(req, buildClaudeBody(messages));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        const ClaudeResult r = parseClaudeReply(status, body);
        emit done(r.ok, r.content, r.text);
    });
}

// ---------------------------------------------------------------- MessageBubble

MessageBubble::MessageBubble(Role role, const QString &text) : m_role(role)
{
    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::PlainText);             // treść z sieci - żadnego HTML
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_label->setMargin(0);
    m_label->setIndent(0);
    // Kolory tekstu z palety (rola, nie kolor) - zmiana motywu działa sama.
    m_label->setForegroundRole(role == User ? QPalette::HighlightedText
                               : role == System ? QPalette::WindowText : QPalette::Text);
    if (role == System) {
        QFont f = m_label->font();
        f.setPointSizeF(f.pointSizeF() * 0.95);
        m_label->setFont(f);
    }
    m_label->move(12, 8);
    setText(text);
}

void MessageBubble::setMaxWidth(int w)
{
    m_maxW = w;
    relayout();
}

void MessageBubble::setText(const QString &text, const QString &sizeText)
{
    m_label->setText(text);
    m_sizeText = sizeText.isEmpty() ? text : sizeText;
    relayout();
}

void MessageBubble::relayout()
{
    // Rozmiar liczymy sami z tej samej funkcji, której używa QLabel - bez heightForWidth.
    const int maxLabel = qMax(60, m_maxW - 24);
    const QFontMetrics fm(m_label->font());
    const int flags = Qt::TextWordWrap;
    int w = fm.boundingRect(QRect(0, 0, maxLabel, 100000), flags, m_sizeText).width() + 2;
    w = qMin(maxLabel, w);
    const int h = fm.boundingRect(QRect(0, 0, w, 100000), flags, m_label->text()).height();
    m_label->setFixedSize(w, h);
    setFixedSize(w + 24, h + 16);
}

void MessageBubble::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange
        || e->type() == QEvent::FontChange) {
        relayout();
        update();
    }
    QWidget::changeEvent(e);
}

void MessageBubble::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette &pal = palette();
    QColor fill, line;
    switch (m_role) {
    case User:
        fill = pal.color(QPalette::Highlight);
        line = fill;
        break;
    case Assistant:
        fill = pal.color(QPalette::AlternateBase);
        line = pal.color(QPalette::Mid);
        line.setAlpha(110);
        break;
    default:
        fill = pal.color(QPalette::Base);
        line = pal.color(QPalette::Highlight);
        line.setAlpha(160);
        break;
    }
    p.setPen(QPen(line, 1));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
}

// ---------------------------------------------------------------- ChatInput

ChatInput::ChatInput(QWidget *parent) : QPlainTextEdit(parent)
{
    setPlaceholderText(QStringLiteral("Napisz coś…"));
    setToolTip(QStringLiteral("Enter wysyła, Shift+Enter dodaje nową linię"));
    setTabChangesFocus(true);
    connect(this, &QPlainTextEdit::textChanged, this, &ChatInput::adjustHeight);
    adjustHeight();
}

QSize ChatInput::sizeHint() const
{
    return QSize(200, height());
}

void ChatInput::adjustHeight()
{
    const int lines = qBound(1, int(document()->size().height()), 5);
    const int h = lines * fontMetrics().lineSpacing() + int(document()->documentMargin()) * 2 + 2 * frameWidth();
    setFixedHeight(h + 4);
}

void ChatInput::keyPressEvent(QKeyEvent *e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        && !(e->modifiers() & Qt::ShiftModifier)) {
        emit submitted();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

// ---------------------------------------------------------------- ChatDialog

ChatDialog::ChatDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Czat z Claude"));
    resize(420, 480);
    m_client = new ClaudeClient(this);
    connect(m_client, &ClaudeClient::done, this, &ChatDialog::onReply);

    auto *container = new QWidget;
    container->setAutoFillBackground(true);
    container->setBackgroundRole(QPalette::Base);
    m_rows = new QVBoxLayout(container);
    m_rows->setContentsMargins(10, 10, 10, 10);
    m_rows->setSpacing(8);
    m_rows->addStretch();

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setWidget(container);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setBackgroundRole(QPalette::Base);
    m_scroll->viewport()->installEventFilter(this);
    connect(m_scroll->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { scrollToEnd(); });

    m_input = new ChatInput;
    m_sendBtn = new QPushButton(QStringLiteral("Wyślij"));
    m_sendBtn->setDefault(false);
    m_sendBtn->setAutoDefault(false);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_sendBtn, 0, Qt::AlignBottom);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_scroll, 1);
    lay->addLayout(bottom);

    m_typingTimer = new QTimer(this);
    m_typingTimer->setInterval(400);
    connect(m_typingTimer, &QTimer::timeout, this, [this] {
        if (!m_typing)
            return;
        m_dots = (m_dots + 1) % 4;
        m_typing->setText(QStringLiteral("Claude pisze") + QString(m_dots, QLatin1Char('.')),
                          QStringLiteral("Claude pisze..."));
    });

    connect(m_input, &ChatInput::submitted, this, &ChatDialog::send);
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatDialog::send);
    if (apiKey().isEmpty())
        appendMessage(MessageBubble::System,
                      QStringLiteral("⚠ Brak klucza API - wpisz go do %1.").arg(cfg::apiKeyFile()));
    m_input->setFocus();
}

void ChatDialog::appendMessage(MessageBubble::Role role, const QString &text)
{
    auto *b = new MessageBubble(role, text);
    m_bubbles << b;
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    if (role != MessageBubble::Assistant)
        row->addStretch();
    row->addWidget(b);
    if (role != MessageBubble::User)
        row->addStretch();
    // Przed wskaźnikiem pisania (jeśli jest), zawsze przed końcowym rozciągnięciem.
    int at = m_rows->count() - 1;
    if (m_typing) {
        for (int i = 0; i < m_rows->count(); ++i) {
            QLayout *l = m_rows->itemAt(i)->layout();
            if (l && l->indexOf(m_typing) >= 0)
                at = i;
        }
    }
    m_rows->insertLayout(at, row);
    b->setMaxWidth(int(m_scroll->viewport()->width() * 0.8));
    scrollToEnd();
}

void ChatDialog::setTyping(bool on)
{
    if (on && !m_typing) {
        appendMessage(MessageBubble::Assistant, QString());
        m_typing = m_bubbles.last();
        m_dots = 0;
        m_typing->setText(QStringLiteral("Claude pisze"), QStringLiteral("Claude pisze..."));
        m_typingTimer->start();
    } else if (!on && m_typing) {
        m_typingTimer->stop();
        QWidget *w = m_typing;
        m_bubbles.removeAll(m_typing);
        for (int i = 0; i < m_rows->count(); ++i) {
            QLayout *l = m_rows->itemAt(i)->layout();
            if (l && l->indexOf(w) >= 0) {
                QLayoutItem *item = m_rows->takeAt(i);
                // Wiersz: dymek + rozciągnięcia - sprzątamy wszystko.
                while (QLayoutItem *c = l->takeAt(0)) {
                    delete c->widget();
                    delete c;
                }
                delete item;                      // usuwa układ wiersza
                break;
            }
        }
        m_typing.clear();
    }
}

void ChatDialog::send()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_busy)
        return;
    m_input->clear();
    appendMessage(MessageBubble::User, text);
    m_messages.append(QJsonObject{{"role", "user"}, {"content", text}});
    m_busy = true;
    m_sendBtn->setEnabled(false);
    setTyping(true);
    emit requestStarted();
    m_client->send(m_messages);
}

void ChatDialog::onReply(bool ok, const QJsonArray &content, const QString &text)
{
    setTyping(false);
    if (!ok) {
        m_messages.removeLast();               // nie zostawiaj pytania bez odpowiedzi w historii
        appendMessage(MessageBubble::System, QStringLiteral("⚠ ") + text);
    } else {
        m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});
        appendMessage(MessageBubble::Assistant, text);
    }
    m_busy = false;
    m_sendBtn->setEnabled(true);
    m_input->setFocus();
    emit requestFinished(ok);
}

void ChatDialog::applyBubbleWidth()
{
    const int w = int(m_scroll->viewport()->width() * 0.8);
    for (MessageBubble *b : std::as_const(m_bubbles))
        b->setMaxWidth(w);
}

void ChatDialog::scrollToEnd()
{
    QScrollBar *sb = m_scroll->verticalScrollBar();
    sb->setValue(sb->maximum());
}

bool ChatDialog::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_scroll->viewport() && e->type() == QEvent::Resize)
        applyBubbleWidth();
    return QDialog::eventFilter(obj, e);
}

void ChatDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    applyBubbleWidth();
}
