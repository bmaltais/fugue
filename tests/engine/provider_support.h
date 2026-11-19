#pragma once

// Test helpers for providers: waiting for an asynchronous reply.

#include <QtTest>

#include <functional>
#include <optional>

#include "engine/provider.h"

namespace fugue::test {

// Calls an operation and spins the event loop until its reply arrives (or 5 seconds pass).
template <typename T> Outcome<T> await(std::function<void(Reply<T>)> call) {
    std::optional<Outcome<T>> got;
    call([&](const Outcome<T> &outcome) { got = outcome; });
    if (!QTest::qWaitFor([&] { return got.has_value(); }, 5000))
        return Outcome<T>{T{}, {providerError(QStringLiteral("internal"), QStringLiteral("No reply."))}};
    return *got;
}

} // namespace fugue::test
