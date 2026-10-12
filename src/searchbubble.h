// Dymek wyszukiwarki nad głową: pole tekstowe i lista wyników (jak KRunner).
#pragma once

#include "bubbles.h"
#include "search.h"

#include <QElapsedTimer>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QTimer>

class SearchBubble : public Bubble {
    Q_OBJECT
public:
    static constexpr int VISIBLE_ROWS = 8;
    static constexpr int ROW_H = 40;
    static constexpr int WIDTH = 380;
    static constexpr int DEBOUNCE_MS = 120;       // od ostatniego znaku do zapytania
    static constexpr int FOCUS_RETRY_MS = 150;     // ponawianie prośby o fokus
    static constexpr int FOCUS_RETRIES = 6;
    static constexpr int NO_RESULTS_SETTLE_MS = 350;   // tyle braku wyników = "naprawdę nic nie ma"

    explicit SearchBubble(SearchEngine *engine);
    void setEngine(SearchEngine *engine);         // przejmuje własność (stary silnik jest usuwany)
    void reset();                                 // czyści pole i listę (przed ustawieniem pozycji)
    void open();                                  // pokazuje, bierze fokus
    bool justClosed() const;
    QString text() const { return m_edit->text(); }
    QLineEdit *edit() const { return m_edit; }
    int resultCount() const { return m_list->count(); }
    int currentRow() const { return m_list->currentRow(); }
    QString resultTitle(int row) const;
    void launchCurrent();                         // Enter

signals:
    void launched();                              // wynik uruchomiony (dymek się zamyka)
    void noResults();                             // zapytanie ustabilizowane i nic nie znaleziono
    void typing();                                // użytkownik wpisuje (zapytanie jeszcze się nie ustabilizowało)
    void closed();
    void layoutChanged();                         // zmienił się rozmiar (lista wyników)

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void textChanged();
    void sendQuery();
    void showResults(const QString &query, const QList<SearchResult> &results);
    void finishedQuery(const QString &query);
    void move(int delta);
    void launchRow(int row);
    void relayout();
    void requestFocus();

    SearchEngine *m_engine = nullptr;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QLabel *m_note;                               // krótki komunikat (np. restart pomocnika wyszukiwania)
    QTimer *m_debounce, *m_settle, *m_focusWatch;
    int m_focusTries = 0;
    QElapsedTimer m_hiddenAt;
    QString m_sentQuery;                          // ostatnie zapytanie wysłane do silnika
    bool m_navigated = false;                     // użytkownik ruszał zaznaczeniem strzałkami
    bool m_wasActive = false;
    bool m_launchPending = false;                 // Enter w trakcie debounce: uruchom pierwszy wynik, gdy przyjdzie
};
