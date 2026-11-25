#include "engine/dynamic_prompt.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <QDateTime>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/project.h"

namespace fugue::dynprompt {

namespace {

// Problems end the work by exception inside this file and become an Error at the API's edge.
struct Problem {
    QString message;
    QString hint;
};

[[noreturn]] void raise(const QString &message, const QString &hint = {}) { throw Problem{message, hint}; }

Error toError(const Problem &problem) {
    return makeInputError(QStringLiteral("template_invalid"), QStringLiteral("template"), problem.message, problem.hint);
}

QString lineColumn(const QString &text, int pos) {
    int line = 1;
    int start = 0;
    for (int i = 0; i < pos && i < text.size(); ++i)
        if (text.at(i) == QLatin1Char('\n')) {
            ++line;
            start = i + 1;
        }
    return QStringLiteral("line %1, column %2").arg(line).arg(pos - start + 1);
}

enum class Sampler { Default, Random, Cyclic, Combinatorial };

struct Source {
    QString text;
    QString origin; // empty: the node's template
    QString where(int pos) const {
        const QString place = lineColumn(text, pos);
        if (origin.isEmpty())
            return QStringLiteral("%1 of the template").arg(place);
        return text.contains(QLatin1Char('\n')) ? QStringLiteral("%1, %2").arg(origin, place)
                                                : QStringLiteral("%1, column %2").arg(origin).arg(pos + 1);
    }
};
using SourceP = std::shared_ptr<const Source>;

struct Seq;
using SeqP = std::shared_ptr<const Seq>;

struct Option {
    double weight = 1;
    SeqP seq;
};

struct Node {
    enum class Kind { Literal, Variant, Wildcard, VarSet, VarUse, Wrap };
    Kind kind = Kind::Literal;
    int pos = 0;
    QString text; // the literal, or the variable's or wildcard's name
    Sampler sampler = Sampler::Default;
    int lo = 1;
    int hi = 1;
    bool hiOpen = false;
    QString sep = QStringLiteral(", ");
    std::vector<Option> options;
    std::vector<std::pair<QString, QString>> params; // wildcard parameters
    bool immediate = false;
    bool keep = false;
    SeqP a; // variable value or default, the wrap's wrapper
    SeqP b; // the wrap's inner prompt
};

struct Seq {
    SourceP src;
    std::vector<Node> nodes;
};

// What a wildcard name may be: a relative path of plain names inside the wildcard folders.
QString badName(const QString &name) {
    if (name.isEmpty())
        return QStringLiteral("it is empty");
    if (name.contains(QLatin1Char('\\')))
        return QStringLiteral("it has a backslash (use / between folders)");
    if (name.contains(QLatin1Char(':')))
        return QStringLiteral("it has a colon (no drive letters)");
    if (name.startsWith(QLatin1Char('/')))
        return QStringLiteral("it is an absolute path");
    for (const QChar &c : name)
        if (c.unicode() < 0x20)
            return QStringLiteral("it has a control character");
    const QStringList parts = name.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part == QLatin1String(".."))
            return QStringLiteral("it has .. (a wildcard cannot leave the wildcard folders)");
        if (part == QLatin1String("."))
            return QStringLiteral("it has a . folder");
        if (part.isEmpty())
            return QStringLiteral("it has an empty folder name");
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// The parser

class Parser {
public:
    Parser(SourceP source, const Limits &limits) : m_src(std::move(source)), m_t(m_src->text), m_limits(limits) {}

    SeqP parseTop() {
        SeqP seq = parseSeq(0);
        if (m_i < m_t.size())
            fail(m_i, QStringLiteral("A } closes nothing; write \\} for a literal one"));
        return seq;
    }

private:
    enum Stop { StopBar = 1, StopClose = 2, StopDollar = 4 };

    SourceP m_src;
    const QString &m_t;
    const Limits &m_limits;
    int m_i = 0;
    int m_depth = 0;

    [[noreturn]] void fail(int pos, const QString &message, const QString &hint = {}) const {
        raise(QStringLiteral("%1 (%2).").arg(message, m_src->where(pos)), hint);
    }

    QChar at(int i) const { return i < m_t.size() ? m_t.at(i) : QChar(); }

    void skipSpace() {
        while (m_i < m_t.size() && m_t.at(m_i).isSpace())
            ++m_i;
    }

    bool isCommentStart(int i) const {
        const QChar next = at(i + 1);
        if (i + 1 >= m_t.size() || next == QLatin1Char(' ') || next == QLatin1Char('\t') || next == QLatin1Char('\n') ||
            next == QLatin1Char('\r'))
            return true;
        for (int k = i - 1; k >= 0; --k) {
            const QChar c = m_t.at(k);
            if (c == QLatin1Char('\n'))
                return true;
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t') && c != QLatin1Char('\r'))
                return false;
        }
        return true;
    }

    static QString tidy(QString text) {
        static const QRegularExpression newline(QStringLiteral("[ \\t]*\\r?\\n\\s*"));
        text.replace(newline, QStringLiteral(" "));
        return text;
    }

    static void trimEnds(std::vector<Node> &nodes) {
        if (!nodes.empty() && nodes.front().kind == Node::Kind::Literal) {
            QString &t = nodes.front().text;
            qsizetype k = 0;
            while (k < t.size() && t.at(k).isSpace())
                ++k;
            t.remove(0, k);
            if (t.isEmpty())
                nodes.erase(nodes.begin());
        }
        if (!nodes.empty() && nodes.back().kind == Node::Kind::Literal) {
            QString &t = nodes.back().text;
            while (!t.isEmpty() && t.back().isSpace())
                t.chop(1);
            if (t.isEmpty())
                nodes.pop_back();
        }
    }

    SeqP parseSeq(int stops) {
        auto seq = std::make_shared<Seq>();
        seq->src = m_src;
        QString buf;
        int bufStart = m_i;
        const auto flush = [&] {
            if (!buf.isEmpty()) {
                Node literal;
                literal.kind = Node::Kind::Literal;
                literal.pos = bufStart;
                literal.text = tidy(buf);
                seq->nodes.push_back(std::move(literal));
                buf.clear();
            }
        };
        while (m_i < m_t.size()) {
            const QChar c = m_t.at(m_i);
            const QChar next = at(m_i + 1);
            if (buf.isEmpty())
                bufStart = m_i;
            if (c == QLatin1Char('\\') && QStringLiteral("#~!@{}|\\$%_").contains(next) && !next.isNull()) {
                buf += next;
                m_i += 2;
            } else if (c == QLatin1Char('#') && isCommentStart(m_i)) {
                while (m_i < m_t.size() && m_t.at(m_i) != QLatin1Char('\n'))
                    ++m_i;
            } else if (c == QLatin1Char('}')) {
                if (stops & StopClose)
                    break;
                fail(m_i, QStringLiteral("A } closes nothing"), QStringLiteral("Write \\} for a literal }."));
            } else if (c == QLatin1Char('|') && (stops & StopBar)) {
                break;
            } else if (c == QLatin1Char('$') && next == QLatin1Char('$') && (stops & StopDollar)) {
                break;
            } else if (c == QLatin1Char('$') && next == QLatin1Char('{')) {
                flush();
                seq->nodes.push_back(parseVariable());
            } else if (c == QLatin1Char('{')) {
                flush();
                seq->nodes.push_back(parseVariant());
            } else if (c == QLatin1Char('%') && next == QLatin1Char('{')) {
                flush();
                seq->nodes.push_back(parseWrap());
            } else if (c == QLatin1Char('_') && next == QLatin1Char('_')) {
                std::optional<Node> wildcard = tryWildcard();
                if (wildcard) {
                    flush();
                    seq->nodes.push_back(std::move(*wildcard));
                } else {
                    while (at(m_i) == QLatin1Char('_')) { // a run of underscores is text
                        buf += QLatin1Char('_');
                        ++m_i;
                    }
                }
            } else {
                buf += c;
                ++m_i;
            }
        }
        flush();
        trimEnds(seq->nodes);
        return seq;
    }

    void enter(int pos) {
        if (++m_depth > m_limits.maxDepth)
            fail(pos, QStringLiteral("Braces are nested more than %1 deep").arg(m_limits.maxDepth));
    }

    Node parseVariant() {
        Node node;
        node.kind = Node::Kind::Variant;
        node.pos = m_i;
        const int start = m_i;
        enter(start);
        ++m_i;
        skipSpace();
        const QChar mark = at(m_i);
        if (mark == QLatin1Char('~') || mark == QLatin1Char('@') || mark == QLatin1Char('!')) {
            node.sampler = mark == QLatin1Char('~') ? Sampler::Random : mark == QLatin1Char('@') ? Sampler::Cyclic : Sampler::Combinatorial;
            ++m_i;
            skipSpace();
        }
        static const QRegularExpression header(QStringLiteral("\\G\\s*(\\d+)?\\s*(-)?\\s*(\\d+)?\\s*\\$\\$"));
        const QRegularExpressionMatch range = header.match(m_t, m_i);
        if (range.hasMatch() && (!range.captured(1).isEmpty() || !range.captured(2).isEmpty() || !range.captured(3).isEmpty())) {
            const auto number = [](const QString &s) {
                bool ok = false;
                const qlonglong v = s.toLongLong(&ok);
                return ok ? int(std::min<qlonglong>(v, 1000000000)) : 1000000000;
            };
            if (range.captured(2).isEmpty()) {
                node.lo = node.hi = number(range.captured(1));
            } else {
                node.lo = range.captured(1).isEmpty() ? 1 : number(range.captured(1));
                node.hiOpen = range.captured(3).isEmpty();
                node.hi = node.hiOpen ? node.lo : number(range.captured(3));
                if (!node.hiOpen && node.lo > node.hi)
                    fail(start, QStringLiteral("The range %1-%2 is backwards").arg(node.lo).arg(node.hi));
            }
            m_i += range.capturedLength(0);
            // A separator is the text up to a second $$, when no | { } or __ comes first.
            const qsizetype close = m_t.indexOf(QStringLiteral("$$"), m_i);
            if (close >= 0) {
                const QString between = m_t.mid(m_i, close - m_i);
                if (!between.contains(QLatin1Char('|')) && !between.contains(QLatin1Char('{')) && !between.contains(QLatin1Char('}')) &&
                    !between.contains(QStringLiteral("__"))) {
                    node.sep = between;
                    m_i = int(close) + 2;
                }
            }
        }
        for (;;) {
            Option option;
            // A weight "0.5::" (by hand: a regex match at an offset of a long template is slow).
            int k = m_i;
            while (k < m_t.size() && m_t.at(k).isSpace())
                ++k;
            const int digits = k;
            while (k < m_t.size() && (m_t.at(k).isDigit() || m_t.at(k) == QLatin1Char('.')))
                ++k;
            if (k > digits && at(k) == QLatin1Char(':') && at(k + 1) == QLatin1Char(':')) {
                bool ok = false;
                const double weight = m_t.mid(digits, k - digits).toDouble(&ok);
                if (ok) {
                    option.weight = weight;
                    m_i = k + 2;
                }
            }
            option.seq = parseSeq(StopBar | StopClose);
            node.options.push_back(std::move(option));
            if (m_i >= m_t.size())
                fail(start, QStringLiteral("The { is never closed"), QStringLiteral("Add the missing }, or write \\{ for a literal brace."));
            if (m_t.at(m_i) == QLatin1Char('|')) {
                ++m_i;
                continue;
            }
            ++m_i; // the }
            break;
        }
        --m_depth;
        return node;
    }

    Node parseVariable() {
        Node node;
        node.pos = m_i;
        const int start = m_i;
        enter(start);
        m_i += 2;
        skipSpace();
        static const QRegularExpression name(QStringLiteral("\\G[A-Za-z_][A-Za-z0-9_\\-]*"));
        const QRegularExpressionMatch found = name.match(m_t, m_i);
        if (!found.hasMatch())
            fail(start, QStringLiteral("A variable needs a name after ${"));
        node.text = found.captured(0);
        m_i += found.capturedLength(0);
        skipSpace();
        const auto closeOrFail = [&] {
            if (m_i >= m_t.size())
                fail(start, QStringLiteral("The ${%1 is never closed").arg(node.text), QStringLiteral("Add the missing }."));
            ++m_i;
        };
        const QChar c = at(m_i);
        if (c == QLatin1Char('}')) {
            node.kind = Node::Kind::VarUse;
            ++m_i;
        } else if (c == QLatin1Char(':')) {
            node.kind = Node::Kind::VarUse;
            ++m_i;
            node.a = parseSeq(StopClose);
            closeOrFail();
        } else if (c == QLatin1Char('=') || (c == QLatin1Char('?') && at(m_i + 1) == QLatin1Char('='))) {
            node.kind = Node::Kind::VarSet;
            node.keep = c == QLatin1Char('?');
            m_i += node.keep ? 2 : 1;
            if (at(m_i) == QLatin1Char('!')) {
                node.immediate = true;
                ++m_i;
            }
            node.a = parseSeq(StopClose);
            closeOrFail();
        } else if (m_i >= m_t.size()) {
            fail(start, QStringLiteral("The ${%1 is never closed").arg(node.text), QStringLiteral("Add the missing }."));
        } else {
            fail(m_i, QStringLiteral("After ${%1 expected }, :default}, =value} or ?=value}").arg(node.text));
        }
        --m_depth;
        return node;
    }

    Node parseWrap() {
        Node node;
        node.kind = Node::Kind::Wrap;
        node.pos = m_i;
        const int start = m_i;
        enter(start);
        m_i += 2;
        node.a = parseSeq(StopDollar | StopClose);
        if (at(m_i) != QLatin1Char('$') || at(m_i + 1) != QLatin1Char('$')) {
            if (m_i >= m_t.size())
                fail(start, QStringLiteral("The %{ is never closed"), QStringLiteral("Add the missing }."));
            fail(start, QStringLiteral("A wrap needs $$ between the wrapper and the prompt: %{wrapper ...$$prompt}"));
        }
        m_i += 2;
        node.b = parseSeq(StopClose);
        if (m_i >= m_t.size())
            fail(start, QStringLiteral("The %{ is never closed"), QStringLiteral("Add the missing }."));
        ++m_i;
        --m_depth;
        return node;
    }

    // At "__": a wildcard, or nothing (the caller keeps the underscore as text).
    std::optional<Node> tryWildcard() {
        const int start = m_i;
        int j = m_i + 2;
        Node node;
        node.kind = Node::Kind::Wildcard;
        node.pos = start;
        const QChar mark = at(j);
        if (mark == QLatin1Char('~') || mark == QLatin1Char('@') || mark == QLatin1Char('!')) {
            node.sampler = mark == QLatin1Char('~') ? Sampler::Random : mark == QLatin1Char('@') ? Sampler::Cyclic : Sampler::Combinatorial;
            ++j;
        }
        const int nameStart = j;
        bool closed = false;
        while (j < m_t.size()) {
            const QChar ch = m_t.at(j);
            if (ch == QLatin1Char('_') && at(j + 1) == QLatin1Char('_')) {
                closed = true;
                break;
            }
            if (ch == QLatin1Char('(')) {
                closed = true;
                break;
            }
            if (ch.isSpace() || QStringLiteral("{}|$%#").contains(ch))
                break;
            ++j;
        }
        if (j == nameStart && !(closed && at(j) == QLatin1Char('(')))
            return std::nullopt; // "____" or "__ ": plain underscores
        node.text = m_t.mid(nameStart, j - nameStart);
        if (!closed)
            return std::nullopt; // "cute ^__^ face", "a__b c": underscores that close nothing are text
        if (at(j) == QLatin1Char('(')) {
            const qsizetype close = m_t.indexOf(QLatin1Char(')'), j);
            if (close < 0)
                fail(start, QStringLiteral("The ( of the wildcard __%1 is never closed").arg(node.text));
            for (const QString &item : m_t.mid(j + 1, close - j - 1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const qsizetype eq = item.indexOf(QLatin1Char('='));
                const QString key = eq < 0 ? QString() : item.left(eq).trimmed();
                static const QRegularExpression keyPattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_\\-]*$"));
                if (!keyPattern.match(key).hasMatch())
                    fail(start, QStringLiteral("A parameter of __%1 must be name=value, not \"%2\"").arg(node.text, item.trimmed()));
                QString value = item.mid(eq + 1).trimmed();
                if (value.size() >= 2 && (value.front() == QLatin1Char('"') || value.front() == QLatin1Char('\'')) && value.back() == value.front())
                    value = value.mid(1, value.size() - 2);
                for (const auto &given : node.params)
                    if (given.first == key)
                        fail(start, QStringLiteral("The parameter %1 of __%2(...)__ is given twice").arg(key, node.text));
                node.params.emplace_back(key, value);
            }
            j = int(close) + 1;
            if (at(j) != QLatin1Char('_') || at(j + 1) != QLatin1Char('_'))
                fail(start, QStringLiteral("The wildcard __%1(...) is never closed with __").arg(node.text));
        }
        if (const QString why = badName(node.text); !why.isEmpty())
            fail(start, QStringLiteral("The wildcard name \"%1\" is not allowed: %2").arg(node.text, why));
        m_i = j + 2;
        return node;
    }
};

SeqP parseText(const QString &text, const QString &origin, const Limits &limits) {
    auto source = std::make_shared<Source>();
    source->text = text;
    source->origin = origin;
    Parser parser(source, limits);
    return parser.parseTop();
}

// ---------------------------------------------------------------------------------------------
// Wildcard collections

struct Entry {
    QString text;
    double weight = 1;
    QString origin;
};

Entry makeEntry(QString text, const QString &origin) {
    static const QRegularExpression weight(QStringLiteral("^\\s*(\\d+(?:\\.\\d+)?|\\.\\d+)::"));
    Entry entry;
    entry.origin = origin;
    const QRegularExpressionMatch w = weight.match(text);
    if (w.hasMatch()) {
        entry.weight = w.captured(1).toDouble();
        text = text.mid(w.capturedLength(0));
    }
    entry.text = text;
    return entry;
}

// The YAML a wildcard collection uses: nested mappings that end in lists of strings, a string as a
// one-item list, numbers and nulls ignored, block scalars (| and >), quoted and plain strings,
// comments. Anything else (anchors, tags, flow lists, a mapping inside a list) is reported by line.
class YamlReader {
public:
    using Leaves = QMap<QString, QList<Entry>>;

    YamlReader(const QString &text, QString rel, Leaves &out, int maxDepth) : m_rel(std::move(rel)), m_out(out), m_maxDepth(maxDepth) {
        const QStringList raw = text.split(QLatin1Char('\n'));
        for (int k = 0; k < raw.size(); ++k) {
            QString line = raw.at(k);
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            Line l;
            l.no = k + 1;
            l.raw = line;
            while (l.indent < line.size() && line.at(l.indent) == QLatin1Char(' '))
                ++l.indent;
            if (l.indent < line.size() && line.at(l.indent) == QLatin1Char('\t'))
                fail(l.no, QStringLiteral("a tab in the indentation"));
            l.body = line.mid(l.indent);
            l.blank = l.body.trimmed().isEmpty() || l.body.startsWith(QLatin1Char('#'));
            m_lines.push_back(std::move(l));
        }
    }

    void read() {
        int idx = 0;
        skipBlank(idx);
        if (idx < int(m_lines.size()) && m_lines[idx].body.trimmed() == QLatin1String("---")) {
            ++idx;
            skipBlank(idx);
        }
        if (idx >= int(m_lines.size()))
            return;
        parseBlock(idx, m_lines[idx].indent, QString());
        skipBlank(idx);
        if (idx < int(m_lines.size()))
            fail(m_lines[idx].no, QStringLiteral("this line does not fit the structure above it"));
    }

private:
    struct Line {
        int no = 0;
        int indent = 0;
        QString raw;
        QString body;
        bool blank = false;
    };
    QString m_rel;
    Leaves &m_out;
    int m_maxDepth;
    int m_depth = 0;
    std::vector<Line> m_lines;

    [[noreturn]] void fail(int line, const QString &why) const {
        raise(QStringLiteral("The wildcard file %1, line %2: %3. Fugue reads YAML that is nested names ending in lists of text; "
                             "put the text in quotes or use a .txt or .json file.")
                  .arg(m_rel).arg(line).arg(why));
    }

    void skipBlank(int &idx) const {
        while (idx < int(m_lines.size()) && m_lines[idx].blank)
            ++idx;
    }
    static bool isDash(const QString &body) { return body == QLatin1String("-") || body.startsWith(QLatin1String("- ")); }

    void add(const QString &path, const QString &text, int line) {
        if (text.trimmed().isEmpty())
            return;
        m_out[path.isEmpty() ? m_rel.left(m_rel.lastIndexOf(QLatin1Char('.'))) : path] << makeEntry(text, QStringLiteral("%1, line %2").arg(m_rel).arg(line));
    }

    void parseBlock(int &idx, int indent, const QString &path) {
        skipBlank(idx);
        if (idx >= int(m_lines.size()) || m_lines[idx].indent < indent)
            return;
        if (m_lines[idx].indent > indent)
            fail(m_lines[idx].no, QStringLiteral("unexpected indentation"));
        if (++m_depth > m_maxDepth)
            fail(m_lines[idx].no, QStringLiteral("names nested more than %1 deep").arg(m_maxDepth));
        if (isDash(m_lines[idx].body))
            parseList(idx, indent, path);
        else
            parseMap(idx, indent, path);
        --m_depth;
    }

    // The text of a block scalar whose header is at `idx - 1`, indented past `parent`.
    QString blockScalar(int &idx, int parent) {
        int first = idx;
        while (first < int(m_lines.size()) && m_lines[first].raw.trimmed().isEmpty())
            ++first;
        if (first >= int(m_lines.size()) || m_lines[first].indent <= parent)
            return {};
        const int indent = m_lines[first].indent;
        QStringList out;
        int k = idx;
        for (; k < int(m_lines.size()); ++k) {
            const Line &l = m_lines[k];
            if (l.raw.trimmed().isEmpty()) {
                out << QString();
                continue;
            }
            if (l.indent <= parent)
                break;
            out << l.raw.mid(std::min(indent, l.indent));
        }
        while (!out.isEmpty() && out.last().isEmpty())
            out.removeLast();
        idx = k;
        return out.join(QLatin1Char('\n'));
    }

    // A scalar's text (without the comment), or nothing for a number, a null or an empty value.
    std::optional<QString> scalar(QString rest, int no) const {
        rest = rest.trimmed();
        if (rest.isEmpty())
            return std::nullopt;
        const QChar first = rest.front();
        if (first == QLatin1Char('"') || first == QLatin1Char('\'')) {
            QString out;
            int k = 1;
            for (; k < rest.size(); ++k) {
                const QChar c = rest.at(k);
                if (first == QLatin1Char('"') && c == QLatin1Char('\\') && k + 1 < rest.size()) {
                    const QChar e = rest.at(++k);
                    out += e == QLatin1Char('n') ? QLatin1Char('\n') : e == QLatin1Char('t') ? QLatin1Char('\t') : e;
                } else if (first == QLatin1Char('\'') && c == QLatin1Char('\'') && k + 1 < rest.size() && rest.at(k + 1) == QLatin1Char('\'')) {
                    out += QLatin1Char('\'');
                    ++k;
                } else if (c == first) {
                    break;
                } else {
                    out += c;
                }
            }
            if (k >= rest.size())
                fail(no, QStringLiteral("a quoted string that is not closed on this line"));
            const QString tail = rest.mid(k + 1).trimmed();
            if (!tail.isEmpty() && !tail.startsWith(QLatin1Char('#')))
                fail(no, QStringLiteral("text after the closing quote"));
            return out;
        }
        if (QStringLiteral("[&*!?%@`").contains(first))
            fail(no, QStringLiteral("a value starting with %1 (a flow list, anchor, alias or tag)").arg(first));
        const qsizetype comment = rest.indexOf(QStringLiteral(" #"));
        if (comment >= 0)
            rest = rest.left(comment).trimmed();
        bool number = false;
        rest.toDouble(&number);
        if (number || rest == QLatin1String("~") || rest == QLatin1String("null") || rest == QLatin1String("true") || rest == QLatin1String("false"))
            return std::nullopt;
        return rest;
    }

    void parseList(int &idx, int indent, const QString &path) {
        for (skipBlank(idx); idx < int(m_lines.size()) && m_lines[idx].indent == indent && isDash(m_lines[idx].body); skipBlank(idx)) {
            const Line line = m_lines[idx];
            const QString rest = line.body.mid(1).trimmed();
            if (rest.isEmpty())
                fail(line.no, QStringLiteral("a list inside a list, or a mapping under a dash"));
            ++idx;
            if (rest.front() == QLatin1Char('|') || rest.front() == QLatin1Char('>')) {
                add(path, blockScalar(idx, indent), line.no);
            } else if (const std::optional<QString> value = scalar(rest, line.no)) {
                add(path, *value, line.no);
            }
        }
        skipBlank(idx);
        if (idx < int(m_lines.size()) && m_lines[idx].indent > indent)
            fail(m_lines[idx].no, QStringLiteral("a value that continues on several lines (use a | block or quotes)"));
    }

    void parseMap(int &idx, int indent, const QString &path) {
        for (skipBlank(idx); idx < int(m_lines.size()) && m_lines[idx].indent == indent; skipBlank(idx)) {
            const Line line = m_lines[idx];
            if (isDash(line.body))
                fail(line.no, QStringLiteral("a list item among names"));
            QString key;
            QString rest;
            if (line.body.front() == QLatin1Char('"') || line.body.front() == QLatin1Char('\'')) {
                const qsizetype close = line.body.indexOf(line.body.front(), 1);
                if (close < 0 || line.body.mid(close + 1).trimmed().isEmpty() || line.body.at(close + 1) != QLatin1Char(':'))
                    fail(line.no, QStringLiteral("a quoted name must be followed by a colon"));
                key = line.body.mid(1, close - 1);
                rest = line.body.mid(close + 2);
            } else {
                qsizetype colon = -1;
                for (qsizetype k = 0; k < line.body.size(); ++k)
                    if (line.body.at(k) == QLatin1Char(':') && (k + 1 == line.body.size() || line.body.at(k + 1) == QLatin1Char(' '))) {
                        colon = k;
                        break;
                    }
                if (colon <= 0)
                    fail(line.no, QStringLiteral("expected a name followed by a colon"));
                key = line.body.left(colon).trimmed();
                rest = line.body.mid(colon + 1);
            }
            const QString keyPath = path.isEmpty() ? key : path + QLatin1Char('/') + key;
            ++idx;
            QString value = rest.trimmed();
            if (value.startsWith(QLatin1Char('#')))
                value.clear();
            if (value.isEmpty()) {
                skipBlank(idx);
                if (idx < int(m_lines.size())) {
                    const Line &next = m_lines[idx];
                    if (next.indent > indent || (next.indent == indent && isDash(next.body)))
                        parseBlock(idx, next.indent, keyPath);
                }
            } else if (value.front() == QLatin1Char('|') || value.front() == QLatin1Char('>')) {
                add(keyPath, blockScalar(idx, indent), line.no);
            } else if (const std::optional<QString> text = scalar(value, line.no)) {
                add(keyPath, *text, line.no);
            }
        }
        skipBlank(idx);
        if (idx < int(m_lines.size()) && m_lines[idx].indent > indent)
            fail(m_lines[idx].no, QStringLiteral("unexpected indentation"));
    }
};

void readJson(const QByteArray &bytes, const QString &rel, YamlReader::Leaves &out) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        const QString head = QString::fromUtf8(bytes.left(error.offset));
        raise(QStringLiteral("The wildcard file %1, %2: %3.").arg(rel, lineColumn(head, int(head.size())), error.errorString()));
    }
    const QString base = rel.left(rel.lastIndexOf(QLatin1Char('.')));
    const auto addAll = [&](const QString &path, const QJsonArray &array) {
        for (const QJsonValue &item : array) {
            if (item.isString())
                out[path] << makeEntry(item.toString(), QStringLiteral("%1, %2").arg(rel, path));
            else if (item.isArray() || item.isObject())
                raise(QStringLiteral("The wildcard file %1: %2 holds a list or mapping inside a list; only text is read.").arg(rel, path));
        }
    };
    struct Walk {
        static void run(const QJsonValue &value, const QString &path, const std::function<void(const QString &, const QJsonArray &)> &addList,
                        YamlReader::Leaves &leaves, const QString &rel) {
            if (value.isObject()) {
                const QJsonObject object = value.toObject();
                for (auto it = object.begin(); it != object.end(); ++it)
                    run(it.value(), path.isEmpty() ? it.key() : path + QLatin1Char('/') + it.key(), addList, leaves, rel);
            } else if (value.isArray()) {
                addList(path, value.toArray());
            } else if (value.isString()) {
                leaves[path] << makeEntry(value.toString(), QStringLiteral("%1, %2").arg(rel, path));
            }
        }
    };
    if (document.isObject())
        Walk::run(document.object(), QString(), addAll, out, rel);
    else
        Walk::run(document.array(), base, addAll, out, rel);
}

QString globPattern(const QString &name) {
    QString re = QStringLiteral("^");
    for (qsizetype k = 0; k < name.size(); ++k) {
        if (name.at(k) == QLatin1Char('*')) {
            if (k + 1 < name.size() && name.at(k + 1) == QLatin1Char('*')) {
                re += QStringLiteral(".*");
                ++k;
            } else {
                re += QStringLiteral("[^/]*");
            }
        } else {
            re += QRegularExpression::escape(name.at(k));
        }
    }
    return re + QLatin1Char('$');
}

// The lines of a wildcard with their weights, summed once so a pick of a big file is a search.
struct Collection {
    QList<Entry> entries;
    std::vector<double> weights;
    std::vector<double> cumulative;
};
using CollectionP = std::shared_ptr<const Collection>;

// The wildcard files under `root` (txt, yaml, yml, json) as paths relative to it, sorted: the one
// listing every lookup and the cache signature use.
QStringList listWildcardFiles(const QString &root) {
    QStringList out;
    QDirIterator it(root, {QStringLiteral("*.txt"), QStringLiteral("*.yaml"), QStringLiteral("*.yml"), QStringLiteral("*.json")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next().mid(root.size() + 1);
    out.sort();
    return out;
}

class Wildcards {
public:
    Wildcards(const Options &options) : m_options(options) {
        for (const QString &folder : options.folders) {
            const QString canonical = QFileInfo(folder).canonicalFilePath();
            if (!canonical.isEmpty() && QFileInfo(canonical).isDir() && !m_roots.contains(canonical))
                m_roots << canonical;
        }
    }

    // The first folder that has the name in any format wins (its .txt, else its lists in .yaml and
    // .json; with a glob, all of those in that folder).
    CollectionP lookup(const QString &name) {
        const auto cached = m_cache.constFind(name);
        if (cached != m_cache.constEnd())
            return *cached;
        if (const QString why = badName(name); !why.isEmpty())
            raise(QStringLiteral("The wildcard name \"%1\" is not allowed: %2.").arg(name, why));
        const bool glob = name.contains(QLatin1Char('*'));
        QList<Entry> out;
        bool emptyFile = false;
        for (const QString &root : std::as_const(m_roots)) {
            out = lookupIn(root, name, glob, emptyFile);
            if (!out.isEmpty() || emptyFile)
                break;
        }
        if (out.isEmpty() && emptyFile)
            raise(QStringLiteral("The wildcard \"%1\" has no values: %1.txt only has comments or blank lines.").arg(name));
        if (out.isEmpty())
            raise(notFound(name), QStringLiteral("Put %1.txt (one value per line) in %2.")
                                      .arg(glob ? QStringLiteral("a file") : name,
                                           m_options.folders.isEmpty() ? QStringLiteral("a wildcards folder") : m_options.folders.first()));
        auto collection = std::make_shared<Collection>();
        double sum = 0;
        for (const Entry &e : out) {
            collection->weights.push_back(e.weight);
            sum += e.weight;
            collection->cumulative.push_back(sum);
        }
        collection->entries = std::move(out);
        m_cache.insert(name, collection);
        return collection;
    }

private:
    const Options &m_options;
    QStringList m_roots;
    QHash<QString, CollectionP> m_cache;
    QHash<QString, QStringList> m_listing;
    QHash<QString, YamlReader::Leaves> m_index;
    QStringList m_badFiles;
    int m_listed = 0;

    // The files of a root, listed once per run; the limit counts these distinct files only.
    const QStringList &files(const QString &root) {
        const auto found = m_listing.constFind(root);
        if (found != m_listing.constEnd())
            return *found;
        QStringList listed = listWildcardFiles(root);
        m_listed += int(listed.size());
        if (m_listed > m_options.limits.maxFiles)
            raise(QStringLiteral("There are more than %1 wildcard files.").arg(m_options.limits.maxFiles));
        return *m_listing.insert(root, std::move(listed));
    }

    QList<Entry> lookupIn(const QString &root, const QString &name, bool glob, bool &emptyFile) {
        QList<Entry> out;
        QSet<QString> seen;
        if (!glob) {
            const QString path = root + QLatin1Char('/') + name + QStringLiteral(".txt");
            if (QFileInfo::exists(path)) {
                seen.insert(name);
                out += readText(root, path, name); // a name that is asked for exactly must be readable
            }
        } else {
            const QRegularExpression pattern(globPattern(name));
            for (const QString &file : files(root)) {
                if (!file.endsWith(QLatin1String(".txt")))
                    continue;
                const QString rel = file.chopped(4);
                if (!pattern.match(rel).hasMatch())
                    continue;
                seen.insert(rel);
                try {
                    out += readText(root, root + QLatin1Char('/') + file, rel);
                } catch (const Problem &problem) {
                    m_badFiles << problem.message; // one unreadable file among a glob's matches is skipped
                }
            }
        }
        if (out.isEmpty() || glob) {
            const YamlReader::Leaves &index = this->index(root);
            const QRegularExpression pattern(glob ? globPattern(name) : QString());
            for (auto it = index.begin(); it != index.end(); ++it) {
                const bool hit = glob ? pattern.match(it.key()).hasMatch() : (it.key() == name || it.key().startsWith(name + QLatin1Char('/')));
                if (hit && !seen.contains(it.key()))
                    out += it.value();
            }
        }
        emptyFile = out.isEmpty() && seen.contains(name);
        return out;
    }

    QString notFound(const QString &name) const {
        QString message = QStringLiteral("The wildcard \"%1\" was not found. Looked for %1.txt, and for a %1 list in .yaml and .json files, in: %2.")
                              .arg(name, m_options.folders.isEmpty() ? QStringLiteral("no wildcard folder") : m_options.folders.join(QStringLiteral(", ")));
        if (!m_badFiles.isEmpty())
            message += QStringLiteral(" These files could not be read: %1").arg(m_badFiles.join(QStringLiteral(" ")));
        return message;
    }

    // (A file swapped for a link between this check and the read needs write access to the wildcard
    // folder, which already lets its owner change the wildcards; not defended.)
    QByteArray readChecked(const QString &root, const QString &path, const QString &what) {
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !info.isFile())
            raise(QStringLiteral("The wildcard file %1 cannot be read.").arg(what));
        if (!canonical.startsWith(root + QLatin1Char('/')))
            raise(QStringLiteral("The wildcard file %1 is a link to outside the wildcard folders and is not read.").arg(what),
                  QStringLiteral("Copy the file into %1 instead of linking it.").arg(root));
        if (info.size() > m_options.limits.maxFileBytes)
            raise(QStringLiteral("The wildcard file %1 is %2 bytes, over the %3 byte limit.").arg(what).arg(info.size()).arg(m_options.limits.maxFileBytes));
        QFile file(canonical);
        if (!file.open(QIODevice::ReadOnly))
            raise(QStringLiteral("The wildcard file %1 cannot be read.").arg(what));
        return file.read(m_options.limits.maxFileBytes + 1);
    }

    QList<Entry> readText(const QString &root, const QString &path, const QString &name) {
        const QString what = name + QStringLiteral(".txt");
        const QString text = QString::fromUtf8(readChecked(root, path, what));
        QList<Entry> out;
        int no = 0;
        for (const QString &raw : text.split(QLatin1Char('\n'))) {
            ++no;
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            out << makeEntry(line, QStringLiteral("%1, line %2").arg(what).arg(no));
        }
        return out;
    }

    // The lists of one root's .yaml, .yml and .json files, by key path.
    const YamlReader::Leaves &index(const QString &root) {
        const auto found = m_index.constFind(root);
        if (found != m_index.constEnd())
            return *found;
        YamlReader::Leaves all;
        for (const QString &rel : files(root)) {
            if (rel.endsWith(QLatin1String(".txt")))
                continue;
            try {
                const QByteArray bytes = readChecked(root, root + QLatin1Char('/') + rel, rel);
                YamlReader::Leaves leaves;
                if (rel.endsWith(QLatin1String(".json")))
                    readJson(bytes, rel, leaves);
                else
                    YamlReader(QString::fromUtf8(bytes), rel, leaves, m_options.limits.maxDepth).read();
                for (auto leaf = leaves.begin(); leaf != leaves.end(); ++leaf)
                    all[leaf.key()] += leaf.value();
            } catch (const Problem &problem) {
                m_badFiles << problem.message;
            }
        }
        return *m_index.insert(root, std::move(all));
    }
};

// ---------------------------------------------------------------------------------------------
// Choosing

class Chooser {
public:
    virtual ~Chooser() = default;
    // An index below `arity`, by weights when given.
    virtual int pick(int arity, const std::vector<double> *weights, Sampler sampler) = 0;
    // An index by cumulative weights (a wildcard's lines, summed once), `cum.size()` long.
    virtual int pickCumulative(const std::vector<double> &cum, Sampler sampler) = 0;
    // `k` different indexes below `n`, in the order they are used.
    virtual std::vector<int> many(int n, int k, const std::vector<double> &weights) = 0;
    // A number from lo to hi.
    virtual int range(int lo, int hi) = 0;
};

class Random {
public:
    explicit Random(quint64 seed) : m_gen(seed) {}
    void reseed(quint64 seed) { m_gen.seed(seed); }
    quint64 next() { return m_gen(); }
    double unit() { return double(m_gen() >> 11) * (1.0 / 9007199254740992.0); }
    int below(int n) { return int(m_gen() % quint64(n)); }

    int weighted(int arity, const std::vector<double> *weights) {
        if (!weights)
            return below(arity);
        double total = 0;
        for (double w : *weights)
            total += w;
        if (!(total > 0))
            raise(QStringLiteral("Every option has weight 0, so none can be picked."));
        double u = unit() * total;
        int last = 0;
        for (int k = 0; k < arity; ++k) {
            const double w = (*weights)[size_t(k)];
            if (w <= 0)
                continue;
            last = k;
            if (u < w)
                return k;
            u -= w;
        }
        return last;
    }

    int cumulative(const std::vector<double> &cum) {
        const double total = cum.empty() ? 0 : cum.back();
        if (!(total > 0))
            raise(QStringLiteral("Every option has weight 0, so none can be picked."));
        const double u = unit() * total;
        const auto at = std::upper_bound(cum.begin(), cum.end(), u);
        return at == cum.end() ? int(cum.size()) - 1 : int(at - cum.begin());
    }

    // `k` picks without replacement; the total is kept as the picks go (each pick is one pass over
    // the weights, which the caller has charged to the step limit).
    std::vector<int> without(int n, int k, std::vector<double> weights) {
        std::vector<int> out;
        double total = 0;
        for (double w : weights)
            total += w;
        for (int j = 0; j < k && total > 0; ++j) {
            double u = unit() * total;
            int chosen = -1;
            for (int i = 0; i < n; ++i) {
                const double w = weights[size_t(i)];
                if (w <= 0)
                    continue;
                chosen = i;
                if (u < w)
                    break;
                u -= w;
            }
            if (chosen < 0)
                break;
            out.push_back(chosen);
            total -= weights[size_t(chosen)];
            weights[size_t(chosen)] = 0;
        }
        return out;
    }

private:
    std::mt19937_64 m_gen;
};

class RandomChooser : public Chooser {
public:
    RandomChooser(quint64 seed) : m_random(seed), m_cycle(seed) {}
    int pick(int arity, const std::vector<double> *weights, Sampler sampler) override {
        if (sampler == Sampler::Cyclic)
            return int(m_cycle % quint64(arity));
        return m_random.weighted(arity, weights);
    }
    int pickCumulative(const std::vector<double> &cum, Sampler sampler) override {
        if (sampler == Sampler::Cyclic)
            return int(m_cycle % quint64(cum.size()));
        return m_random.cumulative(cum);
    }
    std::vector<int> many(int n, int k, const std::vector<double> &weights) override { return m_random.without(n, k, weights); }
    int range(int lo, int hi) override { return lo + m_random.below(hi - lo + 1); }

private:
    Random m_random;
    quint64 m_cycle;
};

// Walks every combination: each choice is recorded with its size, and the next walk repeats the
// record with its last choice that has room moved one on.
class EnumChooser : public Chooser {
public:
    EnumChooser(quint64 seed) : m_seed(seed), m_random(seed) {}

    void begin(quint64 ordinal) {
        m_pos = 0;
        m_ordinal = m_seed + ordinal;
        m_random.reseed(m_seed ^ (ordinal * 0x9E3779B97F4A7C15ULL));
    }
    bool advance() {
        while (!m_tape.empty()) {
            Step &last = m_tape.back();
            if (last.chosen + 1 < last.arity) {
                ++last.chosen;
                return true;
            }
            m_tape.pop_back();
        }
        return false;
    }

    // A ~ or @ part is not enumerated, but it is part of the path: its value is recorded the first
    // time the path is walked and repeated after, so a replay always meets the same parts.
    int pick(int arity, const std::vector<double> *weights, Sampler sampler) override {
        if (sampler == Sampler::Cyclic)
            return fixedChoice([&] { return int(m_ordinal % quint64(arity)); });
        if (sampler == Sampler::Random)
            return fixedChoice([&] { return m_random.weighted(arity, weights); });
        return take(arity);
    }
    int pickCumulative(const std::vector<double> &cum, Sampler sampler) override {
        if (sampler == Sampler::Cyclic)
            return fixedChoice([&] { return int(m_ordinal % quint64(cum.size())); });
        if (sampler == Sampler::Random)
            return fixedChoice([&] { return m_random.cumulative(cum); });
        return take(int(cum.size()));
    }
    std::vector<int> many(int n, int k, const std::vector<double> &) override {
        std::vector<int> out;
        int last = -1;
        for (int j = 0; j < k; ++j) {
            const int remaining = k - j;
            const int c = take(n - remaining - last);
            last = last + 1 + c;
            out.push_back(last);
        }
        return out;
    }
    int range(int lo, int hi) override { return lo + take(hi - lo + 1); }

private:
    struct Step {
        int chosen = 0;
        int arity = 1;
        bool fixed = false;
    };
    int take(int arity) {
        if (m_pos < m_tape.size()) {
            if (m_tape[m_pos].arity != arity || m_tape[m_pos].fixed)
                raise(QStringLiteral("The template does not expand the same way twice."));
            return m_tape[m_pos++].chosen;
        }
        m_tape.push_back({0, arity, false});
        ++m_pos;
        return 0;
    }
    template <typename F>
    int fixedChoice(F make) {
        if (m_pos < m_tape.size()) {
            if (!m_tape[m_pos].fixed)
                raise(QStringLiteral("The template does not expand the same way twice."));
            return m_tape[m_pos++].chosen;
        }
        const int value = make();
        m_tape.push_back({value, 1, true});
        ++m_pos;
        return value;
    }
    quint64 m_seed;
    quint64 m_ordinal = 0;
    Random m_random;
    std::vector<Step> m_tape;
    size_t m_pos = 0;
};

// ---------------------------------------------------------------------------------------------
// Evaluation

struct Var {
    bool immediate = false;
    QString text; // immediate
    SeqP seq;     // else
};

class Evaluator {
public:
    Evaluator(const Options &options, Wildcards &wildcards, Chooser &chooser, QHash<QString, SeqP> &parsed)
        : m_options(options), m_wildcards(wildcards), m_chooser(chooser), m_parsed(parsed) {}

    QString run(const Seq &top) {
        m_vars.clear();
        m_chain.clear();
        m_steps = 0;
        return evalSeq(top, 0).trimmed();
    }

private:
    const Options &m_options;
    Wildcards &m_wildcards;
    Chooser &m_chooser;
    QHash<QString, SeqP> &m_parsed;
    QHash<QString, Var> m_vars;
    QStringList m_chain;
    qint64 m_steps = 0;

    [[noreturn]] void fail(const Seq &seq, int pos, const QString &message, const QString &hint = {}) const {
        raise(QStringLiteral("%1 (%2).").arg(message, seq.src->where(pos)), hint);
    }

    void enter(const Seq &seq, int pos, int depth, const QString &label) {
        if (depth < m_options.limits.maxDepth)
            return;
        QStringList shown = m_chain;
        shown << label;
        const bool cut = shown.size() > 6;
        if (cut)
            shown = shown.mid(shown.size() - 6);
        fail(seq, pos, QStringLiteral("Wildcards and variables are nested more than %1 deep, which is a loop: %2%3")
                           .arg(m_options.limits.maxDepth).arg(cut ? QStringLiteral("... -> ") : QString(), shown.join(QStringLiteral(" -> "))),
             QStringLiteral("Make one of them stop leading back to the others."));
    }

    QString evalSeq(const Seq &seq, int depth) {
        QString out;
        for (const Node &node : seq.nodes) {
            if (++m_steps > m_options.limits.maxSteps)
                fail(seq, node.pos, QStringLiteral("The template needs more than %1 steps to expand").arg(m_options.limits.maxSteps),
                     QStringLiteral("A variable or wildcard that expands to itself many times over is the usual cause."));
            switch (node.kind) {
            case Node::Kind::Literal:
                out += node.text;
                break;
            case Node::Kind::Variant:
                out += evalVariant(seq, node, depth);
                break;
            case Node::Kind::Wildcard:
                out += evalWildcardPick(seq, node, depth);
                break;
            case Node::Kind::VarSet: {
                if (node.keep && m_vars.contains(node.text))
                    break;
                Var var;
                if (node.immediate) {
                    var.immediate = true;
                    enter(seq, node.pos, depth, QStringLiteral("${%1}").arg(node.text));
                    m_chain << QStringLiteral("${%1}").arg(node.text);
                    var.text = evalSeq(*node.a, depth + 1).trimmed();
                    m_chain.removeLast();
                } else {
                    var.seq = node.a;
                }
                m_vars.insert(node.text, std::move(var));
                break;
            }
            case Node::Kind::VarUse: {
                const auto found = m_vars.constFind(node.text);
                if (found != m_vars.constEnd()) {
                    if (found->immediate) {
                        out += found->text;
                    } else {
                        enter(seq, node.pos, depth, QStringLiteral("${%1}").arg(node.text));
                        m_chain << QStringLiteral("${%1}").arg(node.text);
                        const SeqP held = found->seq;
                        out += evalSeq(*held, depth + 1).trimmed();
                        m_chain.removeLast();
                    }
                } else if (node.a) {
                    out += evalSeq(*node.a, depth).trimmed();
                } else {
                    fail(seq, node.pos, QStringLiteral("The variable ${%1} is not set").arg(node.text),
                         QStringLiteral("Set it with ${%1=value}, give a default with ${%1:default}, or pass it to a wildcard as __name(%1=value)__.").arg(node.text));
                }
                break;
            }
            case Node::Kind::Wrap: {
                const QString wrapper = evalSeq(*node.a, depth);
                const QString inner = evalSeq(*node.b, depth).trimmed();
                static const QRegularExpression ellipsis(QStringLiteral("\\.\\.\\.|…|⋯"));
                const QRegularExpressionMatch at = ellipsis.match(wrapper);
                if (!at.hasMatch())
                    fail(seq, node.pos, QStringLiteral("The wrapper of %{...} has no ... to put the prompt in"));
                out += QString(wrapper).replace(at.capturedStart(0), at.capturedLength(0), inner).trimmed();
                break;
            }
            }
            if (out.size() > m_options.limits.maxOutput)
                fail(seq, node.pos, QStringLiteral("The prompt is longer than %1 characters").arg(m_options.limits.maxOutput));
        }
        return out;
    }

    SeqP parsedEntry(const Entry &entry) {
        const QString key = entry.origin + QLatin1Char('\n') + entry.text;
        const auto found = m_parsed.constFind(key);
        if (found != m_parsed.constEnd())
            return *found;
        SeqP seq = parseText(entry.text, entry.origin, m_options.limits);
        m_parsed.insert(key, seq);
        return seq;
    }

    QString evalEntry(const Seq &from, const Node &wildcard, const Entry &entry, int depth) {
        enter(from, wildcard.pos, depth, wildcard.text);
        QHash<QString, std::optional<Var>> saved;
        for (const auto &param : wildcard.params) {
            saved.insert(param.first, m_vars.contains(param.first) ? std::optional<Var>(m_vars.value(param.first)) : std::nullopt);
            Var var;
            var.immediate = true;
            var.text = param.second;
            m_vars.insert(param.first, var);
        }
        m_chain << wildcard.text;
        const SeqP seq = parsedEntry(entry);
        const QString text = evalSeq(*seq, depth + 1).trimmed();
        m_chain.removeLast();
        for (auto it = saved.begin(); it != saved.end(); ++it) {
            if (it.value())
                m_vars.insert(it.key(), *it.value());
            else
                m_vars.remove(it.key());
        }
        return text;
    }

    CollectionP collectionOf(const Seq &seq, const Node &wildcard) {
        try {
            return m_wildcards.lookup(wildcard.text);
        } catch (const Problem &problem) {
            raise(QStringLiteral("%1 (%2)").arg(problem.message.endsWith(QLatin1Char('.')) ? problem.message.chopped(1) : problem.message,
                                                 seq.src->where(wildcard.pos)) + QLatin1Char('.'),
                  problem.hint);
        }
    }

    void charge(const Seq &seq, int pos, qint64 steps) {
        m_steps += steps;
        if (m_steps > m_options.limits.maxSteps)
            fail(seq, pos, QStringLiteral("The template needs more than %1 steps to expand").arg(m_options.limits.maxSteps),
                 QStringLiteral("A pick of very many values, or a variable or wildcard that expands to itself many times over, is the usual cause."));
    }

    QString evalWildcardPick(const Seq &seq, const Node &node, int depth) {
        const CollectionP collection = collectionOf(seq, node);
        const int at = m_chooser.pickCumulative(collection->cumulative, node.sampler);
        return evalEntry(seq, node, collection->entries.at(at), depth);
    }

    QString evalVariant(const Seq &seq, const Node &node, int depth) {
        // {2$$__colours__}: the values of the wildcard are the options.
        const Node *only = nullptr;
        if (node.options.size() == 1 && node.options[0].seq->nodes.size() == 1 && node.options[0].seq->nodes[0].kind == Node::Kind::Wildcard)
            only = &node.options[0].seq->nodes[0];
        CollectionP collection;
        std::vector<double> own;
        if (only)
            collection = collectionOf(*node.options[0].seq, *only);
        else
            for (const Option &option : node.options)
                own.push_back(option.weight);
        const std::vector<double> &weights = only ? collection->weights : own;
        const int n = int(weights.size());
        if (n == 0)
            return {};
        const auto text = [&](int k) {
            if (only)
                return evalEntry(*node.options[0].seq, *only, collection->entries.at(k), depth);
            return evalSeq(*node.options[size_t(k)].seq, depth).trimmed();
        };
        const int lo = std::min(node.lo, n);
        const int hi = node.hiOpen ? n : std::min(node.hi, n);
        const int count = lo == hi ? lo : m_chooser.range(lo, hi);
        if (count == 0)
            return {};
        if (count == 1)
            return text(only ? m_chooser.pickCumulative(collection->cumulative, node.sampler) : m_chooser.pick(n, &weights, node.sampler));
        charge(seq, node.pos, qint64(count) * n); // each pick without repeats is a pass over the options
        QStringList parts;
        for (const int k : m_chooser.many(n, count, weights))
            parts << text(k);
        return parts.join(node.sep);
    }
};

// ---------------------------------------------------------------------------------------------
// Walks of the parsed template

void walk(const Seq &seq, const std::function<void(const Node &)> &visit) {
    for (const Node &node : seq.nodes) {
        visit(node);
        for (const Option &option : node.options)
            walk(*option.seq, visit);
        if (node.a)
            walk(*node.a, visit);
        if (node.b)
            walk(*node.b, visit);
    }
}

} // namespace

QStringList wildcardFolders() {
    QStringList folders{dataDir() + QStringLiteral("/wildcards")};
    if (sessionInherits().contains(QStringLiteral("prompts"))) {
        const QString inherited = baseDataDir() + QStringLiteral("/wildcards");
        if (QDir::cleanPath(inherited) != QDir::cleanPath(folders.first()))
            folders << inherited;
    }
    for (const QStringList &roots : {packSearchPaths(), inheritedPackPaths()})
        for (const QString &root : roots)
            for (const QString &pack : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                const QString folder = root + QLatin1Char('/') + pack + QStringLiteral("/wildcards");
                if (QFileInfo(folder).isDir() && !folders.contains(folder))
                    folders << folder;
            }
    return folders;
}

QString wildcardSignature(const QStringList &folders) {
    QStringList lines;
    for (const QString &folder : folders)
        for (const QString &rel : listWildcardFiles(folder)) { // every file a lookup can reach, sorted
            const QFileInfo info(folder + QLatin1Char('/') + rel);
            lines << QStringLiteral("%1|%2|%3|%4").arg(folder, rel).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
        }
    return QString::fromLatin1(QCryptographicHash::hash(lines.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex());
}

Result<QString> generate(const QString &templ, quint64 seed, const Options &options) {
    try {
        const SeqP top = parseText(templ, QString(), options.limits);
        Wildcards wildcards(options);
        RandomChooser chooser(seed);
        QHash<QString, SeqP> parsed;
        Evaluator evaluator(options, wildcards, chooser, parsed);
        return evaluator.run(*top);
    } catch (const Problem &problem) {
        return Result<QString>(toError(problem));
    }
}

Result<Expansion> expandAll(const QString &templ, const Options &options, int cap, quint64 seed) {
    try {
        const SeqP top = parseText(templ, QString(), options.limits);
        Wildcards wildcards(options);
        EnumChooser chooser(seed);
        QHash<QString, SeqP> parsed;
        Evaluator evaluator(options, wildcards, chooser, parsed);
        Expansion out;
        cap = std::max(cap, 1);
        for (quint64 ordinal = 0;; ++ordinal) {
            chooser.begin(ordinal);
            out.prompts << evaluator.run(*top);
            if (!chooser.advance())
                break;
            if (out.prompts.size() >= cap) {
                out.capped = true;
                break;
            }
        }
        return out;
    } catch (const Problem &problem) {
        return Result<Expansion>(toError(problem));
    }
}

Errors check(const QString &templ, const Options &options) {
    try {
        const SeqP top = parseText(templ, QString(), options.limits);
        Errors errors;
        QSet<QString> assigned;
        bool wildcardsUsed = false; // a wildcard line may set a variable: not told without running
        walk(*top, [&](const Node &node) {
            if (node.kind == Node::Kind::VarSet)
                assigned.insert(node.text);
            if (node.kind == Node::Kind::Wildcard)
                wildcardsUsed = true;
        });
        Wildcards wildcards(options);
        QSet<QString> checked;
        walk(*top, [&](const Node &node) {
            if (node.kind == Node::Kind::VarUse && !node.a && !wildcardsUsed && !assigned.contains(node.text))
                errors << toError({QStringLiteral("The variable ${%1} is never set in the template (%2).").arg(node.text, top->src->where(node.pos)),
                                   QStringLiteral("Set it with ${%1=value} or give a default with ${%1:default}.").arg(node.text)});
            if (node.kind == Node::Kind::Wildcard && !checked.contains(node.text)) {
                checked.insert(node.text);
                try {
                    wildcards.lookup(node.text);
                } catch (const Problem &problem) {
                    errors << toError({QStringLiteral("%1 (%2)").arg(problem.message, top->src->where(node.pos)), problem.hint});
                }
            }
        });
        return errors;
    } catch (const Problem &problem) {
        return {toError(problem)};
    }
}

} // namespace fugue::dynprompt
