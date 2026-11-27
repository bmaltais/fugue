#pragma once

// When the window fetches new templates (spec 11.10, #699): once shortly after it shows, again every 6 hours while it stays open,
// and when the "Get new templates" switch is turned on; never while the switch is off or the feed may not run on its own
// (TemplateFeed::automaticAllowed). A check is only made when the last try is more than 6 hours old (TemplateFeed::due), so
// opening many windows does not ask GitHub many times. The window follows `rootsChanged` (the fetched root appeared, went away
// or has new files) and `checked` (the Settings line reads again).

#include <QObject>
#include <QTimer>

#include <functional>

#include "engine/template_feed.h"

namespace fugue {

class TemplateFeedDriver : public QObject {
    Q_OBJECT
public:
    struct Options {
        std::function<bool()> enabled;                          // settings.json's fetch_templates, now
        std::function<bool()> allowed = &TemplateFeed::automaticAllowed;
        std::function<bool()> due = [] { return TemplateFeed::due(); };
        int startDelayMs = 2000;
        int recheckMs = 6 * 3600 * 1000;
    };
    TemplateFeedDriver(TemplateFeed *feed, Options options, QObject *parent = nullptr);

    // Arms the first check and the 6-hourly one.
    void start();
    // The settings changed: reacts only when the switch moved.
    void settingsChanged();

signals:
    void rootsChanged();
    void checked(const fugue::FeedResult &result);

private:
    void maybeFetch();

    TemplateFeed *m_feed;
    Options m_options;
    QTimer m_first;
    QTimer m_again;
    bool m_enabled = false;
};

} // namespace fugue
