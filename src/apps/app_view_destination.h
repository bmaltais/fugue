#pragma once

// The App view's side of the Model choice seam (workflow/model_choice.h): a file chosen in place of a missing model is the app's own,
// kept by AppView with the form's values; the app's workflow is never written.

#include "apps/app_view.h"
#include "workflow/model_choice.h"

namespace fugue {

class AppViewDestination : public ChoiceDestination {
public:
    explicit AppViewDestination(AppView &view) : m_view(view) {}
    bool shown(const QString &tab) const override { return m_view.shown() && m_view.tab() == tab; }
    Uses inForce(const QString &tab) const override { return m_view.usesInForce(tab); }
    Take take(const QString &tab, const QList<OnDiskUse> &chosen, const Uses &resolved, bool, ChoiceRecord *) override {
        m_view.takeUses(tab, chosen, resolved); // whichever view shows now
        return Take::Applied;
    }
    ChoiceRecord took(const QString &, const Document *) const override { return {}; } // its Undo is per input, its own
    bool holds(const ChoiceRecord &) const override { return false; }
    bool revert(const ChoiceRecord &) override { return false; }

private:
    AppView &m_view;
};

} // namespace fugue
