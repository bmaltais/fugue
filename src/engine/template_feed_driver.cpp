#include "engine/template_feed_driver.h"

namespace fugue {

TemplateFeedDriver::TemplateFeedDriver(TemplateFeed *feed, Options options, QObject *parent)
    : QObject(parent), m_feed(feed), m_options(std::move(options)) {
    m_enabled = m_options.enabled();
    m_first.setSingleShot(true);
    connect(&m_first, &QTimer::timeout, this, &TemplateFeedDriver::maybeFetch);
    connect(&m_again, &QTimer::timeout, this, &TemplateFeedDriver::maybeFetch);
    connect(feed, &TemplateFeed::finished, this, [this](const FeedResult &result) {
        if (result.outcome == FeedResult::Outcome::Busy)
            return; // another Fugue is on it: nothing changed here, and nothing to say
        emit checked(result);
        if (result.outcome == FeedResult::Outcome::Updated)
            emit rootsChanged();
    });
}

void TemplateFeedDriver::start() {
    m_first.start(m_options.startDelayMs);
    m_again.start(m_options.recheckMs);
}

void TemplateFeedDriver::maybeFetch() {
    if (m_options.enabled() && m_options.allowed() && !m_feed->busy() && m_options.due())
        m_feed->check();
}

void TemplateFeedDriver::settingsChanged() {
    const bool enabled = m_options.enabled();
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    emit rootsChanged(); // off hides the fetched root at once, on shows what is there
    maybeFetch();
}

} // namespace fugue
