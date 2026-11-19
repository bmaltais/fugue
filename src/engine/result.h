#pragma once

// How engine calls report failure (spec 13.3). There is one way, in two shapes:
//
//   - A call that makes a value returns Result<T>: the value, with any warnings that
//     came with it, or the errors that kept it from being made. Calls that check many
//     things report all of them at once (4.7), so a failure can hold several errors,
//     and warnings among them.
//   - A call that makes no value returns Errors: empty when all went well. It may
//     hold only warnings; hasErrors() tells a failure from that.
//
// Public headers do not take an `Errors &` to fill, and do not return a value beside
// a separate error list. Inside a .cpp, code that gathers problems as it goes may
// pass an Errors along and turn it into a Result at the end with collected().

#include <optional>
#include <utility>

#include "engine/error.h"

namespace fugue {

template <typename T>
class Result {
public:
    Result(T value) : m_value(std::move(value)) {}
    // A value and the warnings that came with it.
    Result(T value, Errors warnings) : m_value(std::move(value)), m_problems(std::move(warnings)) {}
    Result(Error error) : m_problems{std::move(error)} {}
    // A failure with everything that was reported (say one error per bad entry, and the
    // warnings found on the way); not empty. error() is the first.
    Result(Errors errors) : m_problems(std::move(errors)) {}

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
    const Error &error() const {
        static const Error none;
        return m_problems.isEmpty() ? none : m_problems.first();
    }
    // Everything a failure reported (at least error()); empty when ok().
    Errors errors() const { return ok() ? Errors() : m_problems; }
    // What a success reported beside its value; empty when !ok().
    Errors warnings() const { return ok() ? m_problems : Errors(); }
    // Either of the two: all that was reported, in the order it was found.
    const Errors &problems() const { return m_problems; }

private:
    std::optional<T> m_value;
    Errors m_problems;
};

// An error with severity error, for the common case.
inline Error makeError(const QString &code, const QString &message, const QString &hint = {}) {
    Error error;
    error.code = code;
    error.message = message;
    error.hint = hint;
    return error;
}

// An error about one input of a node (`input` set), as kernels and checks report it.
inline Error makeInputError(const QString &code, const QString &input, const QString &message, const QString &hint = {}) {
    Error error = makeError(code, message, hint);
    error.input = input;
    return error;
}

// The Result of code that gathered `problems` while it made, or failed to make, a value.
template <typename T>
Result<T> collected(std::optional<T> value, Errors problems) {
    if (value)
        return Result<T>(std::move(*value), std::move(problems));
    return Result<T>(std::move(problems));
}

} // namespace fugue
