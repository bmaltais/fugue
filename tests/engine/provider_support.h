#pragma once

// Test helpers for providers: waiting for an asynchronous reply.

#include <QtTest>

#include <functional>
#include <optional>

#include "engine/provider.h"

namespace fugue::test {

// Calls an operation and spins the event loop until its reply arrives (or 5 seconds pass).
template <typename T> Result<T> await(std::function<void(Reply<T>)> call) {
    std::optional<Result<T>> got;
    call([&](const Result<T> &outcome) { got = outcome; });
    if (!QTest::qWaitFor([&] { return got.has_value(); }, 5000))
        return Result<T>(makeError(QStringLiteral("internal"), QStringLiteral("No reply.")));
    return *got;
}

} // namespace fugue::test
