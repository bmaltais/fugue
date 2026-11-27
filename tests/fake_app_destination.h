#pragma once

// The App view's side of a Model choice, played by a test: it is shown when `shownNow` says so, and counts the choices it takes
// (the real one is AppViewDestination over an AppView; tests/apps/test_app_view.cpp wires that one to Workflows).

#include "workflow/model_choice.h"

#include <functional>

namespace fugue {

class FakeAppDestination : public ChoiceDestination {
public:
    FakeAppDestination(std::function<bool()> shownNow, int &taken) : m_shown(std::move(shownNow)), m_taken(taken) {}
    bool shown(const QString &) const override { return m_shown(); }
    Uses inForce(const QString &) const override { return {}; }
    Take take(const QString &, const QList<OnDiskUse> &, const Uses &, bool, ChoiceRecord *) override {
        ++m_taken;
        return Take::Applied;
    }
    ChoiceRecord took(const QString &, const Document *) const override { return {}; }
    bool holds(const ChoiceRecord &) const override { return false; }
    bool revert(const ChoiceRecord &) override { return false; }
    bool undoInput(const QString &, const QString &) override { return false; }

private:
    std::function<bool()> m_shown;
    int &m_taken;
};

} // namespace fugue
