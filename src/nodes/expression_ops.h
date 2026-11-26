#pragma once

// A small formula language over six values (Expression, spec 14.1): parse once, evaluate many times.
//
//   - Numbers (`12`, `1.5`, `.5`, `1e3`), the names a to f (the six inputs), `true`, `false`, `pi`.
//   - Operators, tightest first: `**` (right to left; binds tighter than a unary minus on its left,
//     so -2 ** 2 is -4), unary `+ -`, `* / %`, `+ -`, comparisons `== != < > <= >=` (they chain as in
//     Python: a < b < c is a < b and b < c), `not`, `and`, `or`, `x if c else y`.
//   - Functions: round floor ceil abs min max clamp sqrt pow log log2 log10 sin cos tan.
//
// A value is a number or a boolean. A boolean is 1 or 0 in arithmetic and comparisons; a number is true
// when it is not 0. `and`, `or` and `not` always give a boolean (Python gives an operand), and `x if c
// else y` evaluates only the branch it takes, as do `and` and `or` (so `1 / a if a else 0` is safe).
// Every result is finite and at most kMaxMagnitude; dividing by zero, a modulo by zero, a square root or
// logarithm of what it cannot take, and a power beyond kMaxExponent are errors, never NaN or inf.
// There is no loop, no variable and no call of anything but the functions above.

#include <QString>

#include <array>
#include <memory>
#include <optional>

namespace fugue::nodes {

constexpr double kMaxMagnitude = 1e15;
constexpr double kMaxExponent = 1024;
constexpr int kMaxFormulaLength = 2000;
constexpr int kMaxFormulaDepth = 64;
constexpr int kExpressionSlots = 6;

struct ExpressionValue {
    double number = 0;
    bool isBoolean = false; // a comparison, `not`, `and`, `or`, true or false, or a boolean input

    bool truthy() const { return number != 0; }
};

struct ExpressionError {
    int column = 0; // 1-based, in the formula text
    QString message;
};

// The slots a to f in order; an unset slot (nothing connected) is empty, and using it is an error.
using ExpressionSlots = std::array<std::optional<ExpressionValue>, kExpressionSlots>;

class Expression {
public:
    // Parses `text`. On failure the result is not valid() and `error` (when given) says where and why.
    static Expression parse(const QString &text, ExpressionError *error = nullptr);

    bool valid() const { return m_root != nullptr; }

    // Evaluates with the six values; false with `error` set when the formula cannot give a result
    // for them. Only valid() expressions may be evaluated.
    bool evaluate(const ExpressionSlots &inputs, ExpressionValue *result, ExpressionError *error = nullptr) const;

    struct Node;

private:
    std::shared_ptr<const Node> m_root;
};

} // namespace fugue::nodes
