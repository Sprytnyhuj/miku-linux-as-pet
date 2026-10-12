#include "search.h"

#include <QTimer>

#include <KRunner/QueryMatch>
#include <KSycoca>
#include <KRunner/RunnerManager>

KRunnerEngine::KRunnerEngine(QObject *parent)
    : SearchEngine(parent), m_matches(new QList<KRunner::QueryMatch>)
{
}

// Tylko stabilne, bezpieczne runnery. Pominięte celowo: krunner_shell (uruchamia dowolne polecenie powłoki),
// krunner_kill (zabija procesy), krunner_sessions (wylogowanie/wyłączenie), krunner_powerdevil (uśpienie),
// krunner_keys, krunner_kwin, krunner_plasma-desktop - pojedynczy Enter nie powinien robić nic destrukcyjnego.
// krunner_recentdocuments też wyłączony: jego init w wątku runnera (KActivities::Stats::ResultModel -> Consumer) wysypał się
// ze SIGSEGV w QObject::thread() (zrzut z pomocnika wyszukiwania).
// Identyfikatory zweryfikowane na hoście (runnerMetaDataList): nazwa pliku .so, a dla D-Bus nazwa wpisu (baloosearch).
QStringList KRunnerEngine::allowedRunners()
{
    return {QStringLiteral("krunner_services"),         // aplikacje (.desktop)
            QStringLiteral("krunner_systemsettings"),   // ustawienia systemu
            QStringLiteral("calculator"),
            QStringLiteral("unitconverter"),
            QStringLiteral("krunner_placesrunner"),
            QStringLiteral("org.kde.datetime"),
            QStringLiteral("baloosearch")};             // pliki (Baloo, D-Bus)
}

KRunnerEngine::~KRunnerEngine()
{
    if (m_manager) {                      // anuluje trwające zapytanie, zanim zniknie zarządca
        m_manager->reset();
        delete m_manager;
        m_manager = nullptr;
    }
    delete m_matches;
}

KRunner::RunnerManager *KRunnerEngine::manager()
{
    // Runnery ładują usługi w wątkach; pusty/nieaktualny cache ksycoca przebudowany równolegle potrafi
    // wysypać libKF6Service. Budujemy go tu, w głównym wątku, zanim ruszą wątki runnerów.
    if (!m_manager && !m_sycocaChecked) {
        m_sycocaChecked = true;
        KSycoca::self()->ensureCacheValid();
    }
    if (!m_manager) {
        m_manager = new KRunner::RunnerManager(this);
        m_manager->setAllowedRunners(allowedRunners());
        m_manager->setHistoryEnabled(false);          // nie dopisujemy do historii prawdziwego KRunnera
        connect(m_manager, &KRunner::RunnerManager::matchesChanged, this,
                [this](const QList<KRunner::QueryMatch> &matches) {
            *m_matches = matches.mid(0, MAX_RESULTS);
            QList<SearchResult> out;
            out.reserve(m_matches->size());
            for (const KRunner::QueryMatch &m : std::as_const(*m_matches)) {
                SearchResult r;
                r.title = m.text();
                r.subtitle = m.subtext();
                r.category = m.matchCategory();
                r.iconName = m.iconName().isEmpty() ? m.icon().name() : m.iconName();
                r.icon = m.icon().isNull() ? QIcon::fromTheme(r.iconName) : m.icon();
                out.append(r);
            }
            emit resultsChanged(m_query, out);
        });
        connect(m_manager, &KRunner::RunnerManager::queryFinished, this, [this] { emit finished(m_query); });
        if (m_session)
            m_manager->setupMatchSession();
    }
    return m_manager;
}

void KRunnerEngine::begin()
{
    m_session = true;
    if (m_manager)
        m_manager->setupMatchSession();
    else                                  // ładowanie runnerów od razu po otwarciu, nie dopiero przy pierwszym znaku
        QTimer::singleShot(0, this, [this] { if (m_session) manager(); });
}

void KRunnerEngine::end()
{
    m_session = false;
    m_matches->clear();
    m_query.clear();
    if (m_manager) {
        m_manager->reset();
        m_manager->matchSessionComplete();
    }
}

void KRunnerEngine::query(const QString &text)
{
    m_query = text;
    if (text.trimmed().isEmpty()) {
        m_matches->clear();
        if (m_manager)
            m_manager->reset();
        emit resultsChanged(text, {});
        emit finished(text);
        return;
    }
    manager()->launchQuery(text);
}

bool KRunnerEngine::run(int index)
{
    if (!m_manager || index < 0 || index >= m_matches->size())
        return false;
    return m_manager->run(m_matches->at(index));
}
