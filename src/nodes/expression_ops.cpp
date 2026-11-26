#include "nodes/expression_ops.h"

#include <QHash>
#include <QStringList>

#include <cmath>
#include <vector>

namespace fugue::nodes {

struct Expression::Node {
    enum class Kind { Number, Boolean, Slot, Unary, Binary, Compare, Logic, Ternary, Call };
    Kind kind = Kind::Number;
    int column = 1;
    double number = 0;                // Number, Boolean (0 or 1)
    int slot = 0;                     // Slot
    QString op;                       // Unary (- + not), Binary, Logic (and or), Call (the function)
    std::vector<QString> compareOps;  // Compare: one per pair of neighbours
    std::vector<std::shared_ptr<const Node>> args;
    std::vector<int> argColumns;      // Call: the column of each argument
};

namespace {

using NodePtr = std::shared_ptr<const Expression::Node>;
using Node = Expression::Node;

struct Failure {
    int column;
    QString message;
};

// ---- the language's functions ------------------------------------------------------------------

struct Function {
    int minArgs;
    int maxArgs; // -1: any number
};

const QHash<QString, Function> &functions() {
    static const QHash<QString, Function> table{
        {"round", {1, 2}}, {"floor", {1, 1}}, {"ceil", {1, 1}}, {"abs", {1, 1}}, {"min", {1, -1}}, {"max", {1, -1}},
        {"clamp", {3, 3}}, {"sqrt", {1, 1}},  {"pow", {2, 2}},  {"log", {1, 1}},  {"log2", {1, 1}}, {"log10", {1, 1}},
        {"sin", {1, 1}},   {"cos", {1, 1}},   {"tan", {1, 1}},
    };
    return table;
}

// ---- parsing -----------------------------------------------------------------------------------

struct Token {
    enum class Kind { Number, Name, Op, End } kind = Kind::End;
    int column = 1;
    QString text;
    double number = 0;
};

class Parser {
public:
    explicit Parser(const QString &text) : m_text(text) {}

    NodePtr parse(Failure *failure) {
        try {
            tokenize();
            const NodePtr root = ternary();
            if (peek().kind != Token::Kind::End)
                fail(peek(), QStringLiteral("Unexpected %1.").arg(describe(peek())));
            return root;
        } catch (const Failure &f) {
            *failure = f;
            return nullptr;
        }
    }

private:
    [[noreturn]] void fail(const Token &at, const QString &message) { throw Failure{at.column, message}; }
    [[noreturn]] void fail(int column, const QString &message) { throw Failure{column, message}; }

    static QString describe(const Token &t) {
        if (t.kind == Token::Kind::End)
            return QStringLiteral("end of the formula");
        return QStringLiteral("\"%1\"").arg(t.text);
    }

    void tokenize() {
        const qsizetype n = m_text.size();
        qsizetype i = 0;
        while (true) {
            while (i < n && m_text.at(i).isSpace())
                ++i;
            Token t;
            t.column = int(i) + 1;
            if (i >= n) {
                m_tokens.push_back(t);
                return;
            }
            const QChar c = m_text.at(i);
            const bool digit = c >= '0' && c <= '9';
            if (digit || (c == '.' && i + 1 < n && m_text.at(i + 1) >= '0' && m_text.at(i + 1) <= '9')) {
                qsizetype j = i;
                while (j < n && ((m_text.at(j) >= '0' && m_text.at(j) <= '9') || m_text.at(j) == '.'))
                    ++j;
                if (j < n && (m_text.at(j) == 'e' || m_text.at(j) == 'E')) {
                    qsizetype k = j + 1;
                    if (k < n && (m_text.at(k) == '+' || m_text.at(k) == '-'))
                        ++k;
                    if (k < n && m_text.at(k) >= '0' && m_text.at(k) <= '9') {
                        while (k < n && m_text.at(k) >= '0' && m_text.at(k) <= '9')
                            ++k;
                        j = k;
                    }
                }
                t.kind = Token::Kind::Number;
                t.text = m_text.mid(i, j - i);
                bool ok = false;
                t.number = t.text.toDouble(&ok);
                if (!ok || !std::isfinite(t.number))
                    fail(t, QStringLiteral("\"%1\" is not a number.").arg(t.text));
                if (std::fabs(t.number) > kMaxMagnitude)
                    fail(t, QStringLiteral("%1 is over the limit of %2.").arg(t.text).arg(kMaxMagnitude, 0, 'g', 15));
                i = j;
            } else if (c.isLetter() || c == '_') {
                qsizetype j = i;
                while (j < n && (m_text.at(j).isLetterOrNumber() || m_text.at(j) == '_'))
                    ++j;
                t.kind = Token::Kind::Name;
                t.text = m_text.mid(i, j - i);
                i = j;
            } else {
                static const QStringList two{"**", "==", "!=", "<=", ">="};
                t.kind = Token::Kind::Op;
                const QString pair = m_text.mid(i, 2);
                if (two.contains(pair)) {
                    t.text = pair;
                    i += 2;
                } else if (QStringLiteral("+-*/%<>(),").contains(c)) {
                    t.text = QString(c);
                    ++i;
                } else {
                    t.text = QString(c);
                    fail(t, QStringLiteral("Unexpected character \"%1\".").arg(c));
                }
            }
            m_tokens.push_back(t);
        }
    }

    const Token &peek() const { return m_tokens[m_pos]; }
    Token take() { return m_tokens[m_pos++]; }
    bool isOp(const QString &text) const { return peek().kind == Token::Kind::Op && peek().text == text; }
    bool isWord(const QString &text) const { return peek().kind == Token::Kind::Name && peek().text == text; }

    void expectOp(const QString &text, const QString &why) {
        if (!isOp(text))
            fail(peek(), QStringLiteral("Expected \"%1\" %2, found %3.").arg(text, why, describe(peek())));
        ++m_pos;
    }

    struct DepthGuard {
        Parser &p;
        explicit DepthGuard(Parser &parser) : p(parser) {
            if (++p.m_depth > kMaxFormulaDepth)
                p.fail(p.peek(), QStringLiteral("The formula nests deeper than %1 levels.").arg(kMaxFormulaDepth));
        }
        ~DepthGuard() { --p.m_depth; }
    };

    static std::shared_ptr<Node> make(Node::Kind kind, int column) {
        auto n = std::make_shared<Node>();
        n->kind = kind;
        n->column = column;
        return n;
    }

    // x if c else y
    NodePtr ternary() {
        DepthGuard guard(*this);
        const NodePtr value = logicOr();
        if (!isWord("if"))
            return value;
        const Token iff = take();
        const NodePtr condition = logicOr();
        if (!isWord("else"))
            fail(peek(), QStringLiteral("Expected \"else\" after the condition of \"if\", found %1.").arg(describe(peek())));
        ++m_pos;
        const NodePtr otherwise = ternary();
        auto n = make(Node::Kind::Ternary, iff.column);
        n->args = {condition, value, otherwise};
        return n;
    }

    NodePtr logicOr() {
        NodePtr left = logicAnd();
        while (isWord("or")) {
            const Token t = take();
            auto n = make(Node::Kind::Logic, t.column);
            n->op = QStringLiteral("or");
            n->args = {left, logicAnd()};
            left = n;
        }
        return left;
    }

    NodePtr logicAnd() {
        NodePtr left = logicNot();
        while (isWord("and")) {
            const Token t = take();
            auto n = make(Node::Kind::Logic, t.column);
            n->op = QStringLiteral("and");
            n->args = {left, logicNot()};
            left = n;
        }
        return left;
    }

    NodePtr logicNot() {
        if (isWord("not")) {
            DepthGuard guard(*this);
            const Token t = take();
            auto n = make(Node::Kind::Unary, t.column);
            n->op = QStringLiteral("not");
            n->args = {logicNot()};
            return n;
        }
        return comparison();
    }

    NodePtr comparison() {
        const NodePtr first = sum();
        static const QStringList ops{"==", "!=", "<", ">", "<=", ">="};
        if (peek().kind != Token::Kind::Op || !ops.contains(peek().text))
            return first;
        auto n = make(Node::Kind::Compare, peek().column);
        n->args.push_back(first);
        while (peek().kind == Token::Kind::Op && ops.contains(peek().text)) {
            n->compareOps.push_back(take().text);
            n->args.push_back(sum());
        }
        return n;
    }

    NodePtr binaryLevel(const QStringList &ops, NodePtr (Parser::*next)()) {
        NodePtr left = (this->*next)();
        while (peek().kind == Token::Kind::Op && ops.contains(peek().text)) {
            const Token t = take();
            auto n = make(Node::Kind::Binary, t.column);
            n->op = t.text;
            n->args = {left, (this->*next)()};
            left = n;
        }
        return left;
    }

    NodePtr sum() { return binaryLevel({"+", "-"}, &Parser::product); }
    NodePtr product() { return binaryLevel({"*", "/", "%"}, &Parser::unary); }

    NodePtr unary() {
        if (isOp("-") || isOp("+")) {
            DepthGuard guard(*this);
            const Token t = take();
            auto n = make(Node::Kind::Unary, t.column);
            n->op = t.text;
            n->args = {unary()};
            return n;
        }
        return power();
    }

    // The base binds tighter than a unary minus on its left; the exponent may carry its own sign.
    NodePtr power() {
        const NodePtr base = primary();
        if (!isOp("**"))
            return base;
        DepthGuard guard(*this);
        const Token t = take();
        auto n = make(Node::Kind::Binary, t.column);
        n->op = QStringLiteral("**");
        n->args = {base, unary()};
        return n;
    }

    NodePtr primary() {
        const Token t = take();
        if (t.kind == Token::Kind::Number) {
            auto n = make(Node::Kind::Number, t.column);
            n->number = t.number;
            return n;
        }
        if (t.kind == Token::Kind::Op && t.text == QLatin1String("(")) {
            DepthGuard guard(*this);
            const NodePtr inner = ternary();
            expectOp(QStringLiteral(")"), QStringLiteral("to close the \"(\" at column %1").arg(t.column));
            return inner;
        }
        if (t.kind == Token::Kind::Name)
            return name(t);
        fail(t, QStringLiteral("Expected a value, found %1.").arg(describe(t)));
    }

    NodePtr name(const Token &t) {
        if (isOp("(")) {
            const auto found = functions().constFind(t.text);
            if (found == functions().constEnd())
                fail(t, QStringLiteral("\"%1\" is not a function; the functions are %2.")
                            .arg(t.text, QStringList{"round", "floor", "ceil", "abs", "min", "max", "clamp", "sqrt", "pow",
                                                     "log", "log2", "log10", "sin", "cos", "tan"}.join(", ")));
            ++m_pos;
            auto n = make(Node::Kind::Call, t.column);
            n->op = t.text;
            if (isOp(")")) {
                ++m_pos;
            } else {
                while (true) {
                    n->argColumns.push_back(peek().column);
                    n->args.push_back(ternary());
                    if (isOp(",")) {
                        ++m_pos;
                        continue;
                    }
                    expectOp(QStringLiteral(")"), QStringLiteral("to close the call of %1").arg(t.text));
                    break;
                }
            }
            const int count = int(n->args.size());
            if (count < found->minArgs || (found->maxArgs >= 0 && count > found->maxArgs))
                fail(t, QStringLiteral("%1 takes %2, not %3.")
                            .arg(t.text,
                                 found->maxArgs < 0 ? QStringLiteral("at least %1 value(s)").arg(found->minArgs)
                                 : found->minArgs == found->maxArgs ? QStringLiteral("%1 value(s)").arg(found->minArgs)
                                                                   : QStringLiteral("%1 or %2 values").arg(found->minArgs).arg(found->maxArgs))
                            .arg(count));
            return n;
        }
        static const QStringList names{"a", "b", "c", "d", "e", "f"};
        const int slot = int(names.indexOf(t.text));
        if (slot >= 0) {
            auto n = make(Node::Kind::Slot, t.column);
            n->slot = slot;
            return n;
        }
        if (t.text == QLatin1String("true") || t.text == QLatin1String("false") || t.text == QLatin1String("True") ||
            t.text == QLatin1String("False")) {
            auto n = make(Node::Kind::Boolean, t.column);
            n->number = t.text.toLower() == QLatin1String("true") ? 1 : 0;
            return n;
        }
        if (t.text == QLatin1String("pi")) {
            auto n = make(Node::Kind::Number, t.column);
            n->number = M_PI;
            return n;
        }
        if (functions().contains(t.text))
            fail(t, QStringLiteral("%1 is a function; call it as %1(...).").arg(t.text));
        fail(t, QStringLiteral("\"%1\" is not a name this formula knows; the inputs are a to f, and there are true, false and pi.")
                    .arg(t.text));
    }

    QString m_text;
    std::vector<Token> m_tokens;
    size_t m_pos = 0;
    int m_depth = 0;
};

// ---- evaluation --------------------------------------------------------------------------------

ExpressionValue number(double value) { return {value, false}; }
ExpressionValue boolean(bool value) { return {value ? 1.0 : 0.0, true}; }

// A result that is not finite or beyond the limit is an error naming the operation's column.
double checked(double value, int column) {
    if (!std::isfinite(value))
        throw Failure{column, QStringLiteral("The result is not a finite number.")};
    if (std::fabs(value) > kMaxMagnitude)
        throw Failure{column, QStringLiteral("The result is beyond the limit of %1.").arg(kMaxMagnitude, 0, 'g', 15)};
    return value;
}

double pythonModulo(double a, double b) {
    double r = std::fmod(a, b);
    if (r != 0 && (r < 0) != (b < 0))
        r += b;
    return r;
}

double power(double base, double exponent, int column) {
    if (std::fabs(exponent) > kMaxExponent)
        throw Failure{column, QStringLiteral("The exponent %1 is beyond the limit of %2.").arg(exponent).arg(kMaxExponent)};
    if (base == 0 && exponent < 0)
        throw Failure{column, QStringLiteral("Cannot raise 0 to a negative power.")};
    if (base < 0 && exponent != std::floor(exponent))
        throw Failure{column, QStringLiteral("Cannot raise a negative number to a fractional power.")};
    return checked(std::pow(base, exponent), column);
}

class Evaluator {
public:
    explicit Evaluator(const ExpressionSlots &inputs) : m_slots(inputs) {}

    ExpressionValue eval(const Node &n) const {
        switch (n.kind) {
        case Node::Kind::Number:
            return number(n.number);
        case Node::Kind::Boolean:
            return boolean(n.number != 0);
        case Node::Kind::Slot: {
            const std::optional<ExpressionValue> &v = m_slots[size_t(n.slot)];
            if (!v)
                throw Failure{n.column, QStringLiteral("The input %1 has no value; connect it or leave it out of the formula.")
                                            .arg(QChar(u'a' + n.slot))};
            return *v;
        }
        case Node::Kind::Unary: {
            const ExpressionValue v = eval(*n.args[0]);
            if (n.op == QLatin1String("not"))
                return boolean(!v.truthy());
            return number(n.op == QLatin1String("-") ? -v.number : v.number);
        }
        case Node::Kind::Binary:
            return binary(n);
        case Node::Kind::Compare:
            return compare(n);
        case Node::Kind::Logic: {
            const bool left = eval(*n.args[0]).truthy();
            if (n.op == QLatin1String("and"))
                return boolean(left && eval(*n.args[1]).truthy());
            return boolean(left || eval(*n.args[1]).truthy());
        }
        case Node::Kind::Ternary:
            return eval(eval(*n.args[0]).truthy() ? *n.args[1] : *n.args[2]);
        case Node::Kind::Call:
            return call(n);
        }
        return {};
    }

private:
    ExpressionValue binary(const Node &n) const {
        const double a = eval(*n.args[0]).number;
        const double b = eval(*n.args[1]).number;
        const QString &op = n.op;
        if (op == QLatin1String("+"))
            return number(checked(a + b, n.column));
        if (op == QLatin1String("-"))
            return number(checked(a - b, n.column));
        if (op == QLatin1String("*"))
            return number(checked(a * b, n.column));
        if (op == QLatin1String("**"))
            return number(power(a, b, n.column));
        if (b == 0)
            throw Failure{n.column, op == QLatin1String("/") ? QStringLiteral("Cannot divide by zero.")
                                                           : QStringLiteral("Cannot take a modulo by zero.")};
        return number(checked(op == QLatin1String("/") ? a / b : pythonModulo(a, b), n.column));
    }

    ExpressionValue compare(const Node &n) const {
        double left = eval(*n.args[0]).number;
        for (size_t i = 0; i < n.compareOps.size(); ++i) {
            const double right = eval(*n.args[i + 1]).number;
            const QString &op = n.compareOps[i];
            const bool holds = op == QLatin1String("==")   ? left == right
                               : op == QLatin1String("!=") ? left != right
                               : op == QLatin1String("<")  ? left < right
                               : op == QLatin1String(">")  ? left > right
                               : op == QLatin1String("<=") ? left <= right
                                                           : left >= right;
            if (!holds)
                return boolean(false);
            left = right;
        }
        return boolean(true);
    }

    ExpressionValue call(const Node &n) const {
        std::vector<double> v;
        for (const NodePtr &arg : n.args)
            v.push_back(eval(*arg).number);
        const QString &f = n.op;
        const int col = n.column;
        auto domain = [&](bool ok, const QString &what) {
            if (!ok)
                throw Failure{col, what};
        };
        if (f == QLatin1String("round")) {
            if (v.size() == 1)
                return number(std::nearbyint(v[0])); // halves to even, as Python's round
            domain(v[1] == std::floor(v[1]) && v[1] >= 0 && v[1] <= 15, QStringLiteral("round takes 0 to 15 digits."));
            const double scale = std::pow(10.0, v[1]);
            return number(checked(std::nearbyint(v[0] * scale) / scale, col));
        }
        if (f == QLatin1String("floor"))
            return number(std::floor(v[0]));
        if (f == QLatin1String("ceil"))
            return number(std::ceil(v[0]));
        if (f == QLatin1String("abs"))
            return number(std::fabs(v[0]));
        if (f == QLatin1String("min") || f == QLatin1String("max")) {
            double r = v[0];
            for (double x : v)
                r = f == QLatin1String("min") ? std::min(r, x) : std::max(r, x);
            return number(r);
        }
        if (f == QLatin1String("clamp")) {
            domain(v[1] <= v[2], QStringLiteral("clamp needs its low limit (%1) not above its high limit (%2).").arg(v[1]).arg(v[2]));
            return number(std::min(std::max(v[0], v[1]), v[2]));
        }
        if (f == QLatin1String("sqrt")) {
            domain(v[0] >= 0, QStringLiteral("Cannot take the square root of %1.").arg(v[0]));
            return number(std::sqrt(v[0]));
        }
        if (f == QLatin1String("pow"))
            return number(power(v[0], v[1], col));
        if (f == QLatin1String("log") || f == QLatin1String("log2") || f == QLatin1String("log10")) {
            domain(v[0] > 0, QStringLiteral("Cannot take the logarithm of %1; it must be above 0.").arg(v[0]));
            return number(f == QLatin1String("log") ? std::log(v[0]) : f == QLatin1String("log2") ? std::log2(v[0]) : std::log10(v[0]));
        }
        const double r = f == QLatin1String("sin") ? std::sin(v[0]) : f == QLatin1String("cos") ? std::cos(v[0]) : std::tan(v[0]);
        return number(checked(r, col));
    }

    const ExpressionSlots &m_slots;
};

} // namespace

Expression Expression::parse(const QString &text, ExpressionError *error) {
    Expression out;
    Failure failure{0, {}};
    if (text.trimmed().isEmpty()) {
        failure = {1, QStringLiteral("The formula is empty.")};
    } else if (text.size() > kMaxFormulaLength) {
        failure = {kMaxFormulaLength + 1, QStringLiteral("The formula is longer than %1 characters.").arg(kMaxFormulaLength)};
    } else {
        out.m_root = Parser(text).parse(&failure);
    }
    if (!out.m_root && error)
        *error = {failure.column, failure.message};
    return out;
}

bool Expression::evaluate(const ExpressionSlots &inputs, ExpressionValue *result, ExpressionError *error) const {
    try {
        *result = Evaluator(inputs).eval(*m_root);
        // A result is as finite and as small as every step of it was, but a bare input is not checked.
        if (!std::isfinite(result->number) || std::fabs(result->number) > kMaxMagnitude)
            throw Failure{1, QStringLiteral("The result is beyond the limit of %1.").arg(kMaxMagnitude, 0, 'g', 15)};
        return true;
    } catch (const Failure &f) {
        if (error)
            *error = {f.column, f.message};
        return false;
    }
}

} // namespace fugue::nodes
