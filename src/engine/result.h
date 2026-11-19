#pragma once

// A value or the Error that kept it from being made. Engine calls that can fail
// return one instead of throwing, so a failure always reaches the caller as a
// structured error (spec 13.3).

#include <optional>
#include <utility>

#include "engine/error.h"

namespace fugue {

template <typename T>
class Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    Result(Error error) : m_error(std::move(error)) {}
    // A failure with several errors (say one per bad entry); not empty. error() is the first.
    Result(Errors errors) : m_error(errors.value(0)), m_errors(std::move(errors)) {}

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }

    // Only when ok().
    T &value() { return *m_value; }
    const T &value() const { return *m_value; }
    T &operator*() { return *m_value; }
    const T &operator*() const { return *m_value; }
    T *operator->() { return &*m_value; }
    const T *operator->() const { return &*m_value; }

    // Only when !ok().
    const Error &error() const { return m_error; }
    // Every error of a failure (at least error()); empty when ok().
    Errors errors() const {
        if (ok())
            return {};
        return m_errors.isEmpty() ? Errors{m_error} : m_errors;
    }

private:
    std::optional<T> m_value;
    Error m_error;
    Errors m_errors;
};

// An error with severity error, for the common case.
inline Error makeError(const QString &code, const QString &message, const QString &hint = {}) {
    Error error;
    error.code = code;
    error.message = message;
    error.hint = hint;
    return error;
}

} // namespace fugue
