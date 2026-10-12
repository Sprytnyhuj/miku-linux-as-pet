// Czat z Claude przez API Anthropic (QtNetwork) i okno czatu w stylu systemowym.
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>

class QLabel;
class QVBoxLayout;

// Klucz API: zmienna ANTHROPIC_API_KEY albo plik ~/.config/desktop-pet/api_key ("" = brak).
QString apiKey();

struct ClaudeResult {
    bool ok = false;          // true: content = bloki odpowiedzi, text = tekst; false: text = komunikat błędu
    QJsonArray content;
    QString text;
};

// Treść żądania POST /v1/messages (model, system, historia, effort, fallbacks).
QByteArray buildClaudeBody(const QJsonArray &messages);
// Odpowiedź API -> wynik. status 0 = brak połączenia (błąd sieci, timeout).
ClaudeResult parseClaudeReply(int status, const QByteArray &body);

class ClaudeClient : public QObject {
    Q_OBJECT
public:
    explicit ClaudeClient(QObject *parent = nullptr);
    void send(const QJsonArray &messages);
signals:
    void done(bool ok, const QJsonArray &content, const QString &text);

private:
    QNetworkAccessManager m_net;
};

// Pojedyncza wiadomość w oknie czatu: dymek w kolorach palety systemu.
class MessageBubble : public QWidget {
    Q_OBJECT
public:
    enum Role { User, Assistant, System };
    MessageBubble(Role role, const QString &text);
    // Maksymalna szerokość dymka; szerokość dopasowuje się do tekstu (sizeText - wzorzec szerokości).
    void setMaxWidth(int w);
    void setText(const QString &text, const QString &sizeText = QString());
    Role role() const { return m_role; }

protected:
    void paintEvent(QPaintEvent *) override;
    void changeEvent(QEvent *e) override;

private:
    void relayout();

    Role m_role;
    QLabel *m_label;
    QString m_sizeText;
    int m_maxW = 320;
};

// Pole wpisywania: Enter wysyła, Shift+Enter nowa linia; rośnie do kilku linii.
class ChatInput : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);
    QSize sizeHint() const override;
signals:
    void submitted();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    void adjustHeight();
};

class ChatDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChatDialog(QWidget *parent = nullptr);

    void appendMessage(MessageBubble::Role role, const QString &text);
    void setTyping(bool on);              // wskaźnik "Claude pisze…"
    void send();

signals:
    void requestStarted();                // pytanie poszło do API
    void requestFinished(bool ok);        // jest odpowiedź (ok) albo błąd

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void showEvent(QShowEvent *e) override;

private:
    void onReply(bool ok, const QJsonArray &content, const QString &text);
    void applyBubbleWidth();
    void scrollToEnd();

    QJsonArray m_messages;                // historia: pytania jako napisy, odpowiedzi jako pełne bloki content
    ClaudeClient *m_client;
    bool m_busy = false;
    QScrollArea *m_scroll;
    QVBoxLayout *m_rows;
    ChatInput *m_input;
    QPushButton *m_sendBtn;
    QPointer<MessageBubble> m_typing;
    QTimer *m_typingTimer;
    int m_dots = 0;
    QList<MessageBubble *> m_bubbles;
};
