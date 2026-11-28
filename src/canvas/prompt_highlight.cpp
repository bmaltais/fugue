#include "canvas/prompt_highlight.h"

#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>

#include <algorithm>

#include "engine/dynamic_prompt.h"
#include "look.h"
#include "nodes/kernels.h"

namespace fugue {

namespace {

bool isNameStart(QChar c) { return (c.unicode() < 128 && c.isLetter()) || c == QLatin1Char('_'); }
bool isNameChar(QChar c) { return (c.unicode() < 128 && c.isLetterOrNumber()) || c == QLatin1Char('_') || c == QLatin1Char('-'); }

// Dynamic Prompt (engine/dynamic_prompt.cpp's Parser, read leniently: it never stops at a problem, it marks it).
class DynamicScan {
public:
    explicit DynamicScan(const QString &text) : t(text) {}

    QList<PromptSpan> run() {
        qsizetype i = 0;
        while (i < t.size()) {
            const QChar c = t.at(i);
            const QChar next = at(i + 1);
            if (c == QLatin1Char('\\') && !next.isNull() && QStringLiteral("#~!@{}|\\$%_").contains(next)) {
                i += 2;
            } else if (c == QLatin1Char('#') && isCommentStart(i)) {
                qsizetype end = t.indexOf(QLatin1Char('\n'), i);
                if (end < 0)
                    end = t.size();
                add(i, end - i, PromptKind::Comment);
                i = end;
            } else if (c == QLatin1Char('$') && next == QLatin1Char('{')) {
                qsizetype end = i + 2;
                qsizetype k = end;
                while (k < t.size() && t.at(k).isSpace())
                    ++k;
                if (isNameStart(at(k))) {
                    while (k < t.size() && isNameChar(t.at(k)))
                        ++k;
                    end = k;
                }
                stack.append({add(i, end - i, PromptKind::Role), true});
                i = end;
            } else if (c == QLatin1Char('$') && next == QLatin1Char('$') && !stack.isEmpty()) {
                add(i, 2, PromptKind::Accent);
                i += 2;
            } else if (c == QLatin1Char('%') && next == QLatin1Char('{')) {
                stack.append({add(i, 2, PromptKind::Accent), false});
                i += 2;
            } else if (c == QLatin1Char('{')) {
                stack.append({add(i, 1, PromptKind::Accent), false});
                i = afterOpen(i + 1);
            } else if (c == QLatin1Char('|') && !stack.isEmpty() && !stack.last().variable) {
                add(i, 1, PromptKind::Accent);
                i = weight(i + 1);
            } else if (c == QLatin1Char('}')) {
                if (stack.isEmpty()) {
                    spans[add(i, 1, PromptKind::Accent)].problem = true;
                } else {
                    add(i, 1, stack.last().variable ? PromptKind::Role : PromptKind::Accent);
                    stack.removeLast();
                }
                ++i;
            } else if (c == QLatin1Char('_') && next == QLatin1Char('_')) {
                const qsizetype end = wildcardEnd(i);
                if (end > 0) {
                    add(i, end - i, PromptKind::Role);
                    i = end;
                } else {
                    while (at(i) == QLatin1Char('_'))
                        ++i;
                }
            } else {
                ++i;
            }
        }
        for (const Open &open : std::as_const(stack))
            spans[open.span].problem = true;
        return spans;
    }

private:
    struct Open {
        qsizetype span;
        bool variable; // ${...}: its close is a variable's
    };
    const QString &t;
    QList<PromptSpan> spans;
    QList<Open> stack;

    QChar at(qsizetype i) const { return i < t.size() ? t.at(i) : QChar(); }
    qsizetype add(qsizetype start, qsizetype length, PromptKind kind) {
        spans.append({int(start), int(length), kind, false});
        return spans.size() - 1;
    }
    qsizetype skipSpace(qsizetype i) const {
        while (i < t.size() && t.at(i).isSpace())
            ++i;
        return i;
    }
    // The parser's rule: a # that starts a line or follows only blanks, or is followed by a blank or the end.
    bool isCommentStart(qsizetype i) const {
        const QChar next = at(i + 1);
        if (i + 1 >= t.size() || next == QLatin1Char(' ') || next == QLatin1Char('\t') || next == QLatin1Char('\n') || next == QLatin1Char('\r'))
            return true;
        for (qsizetype k = i - 1; k >= 0; --k) {
            const QChar c = t.at(k);
            if (c == QLatin1Char('\n'))
                return true;
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t') && c != QLatin1Char('\r'))
                return false;
        }
        return true;
    }
    // A weight "0.5::" at the start of an option: the number and its colons.
    qsizetype weight(qsizetype i) {
        qsizetype k = skipSpace(i);
        const qsizetype digits = k;
        while (k < t.size() && (t.at(k).isDigit() || t.at(k) == QLatin1Char('.')))
            ++k;
        if (k > digits && at(k) == QLatin1Char(':') && at(k + 1) == QLatin1Char(':')) {
            add(digits, k + 2 - digits, PromptKind::Label);
            return k + 2;
        }
        return i;
    }
    // After a variant's {: a sampler mark, a count or range with its $$ (and a separator's $$), then the first option's weight.
    qsizetype afterOpen(qsizetype i) {
        qsizetype k = skipSpace(i);
        const QChar mark = at(k);
        if (mark == QLatin1Char('~') || mark == QLatin1Char('@') || mark == QLatin1Char('!')) {
            add(k, 1, PromptKind::Accent);
            k = skipSpace(k + 1);
        }
        qsizetype j = k;
        const auto digits = [&] {
            while (j < t.size() && t.at(j).isDigit())
                ++j;
        };
        digits();
        j = skipSpace(j);
        const bool dash = at(j) == QLatin1Char('-');
        if (dash)
            j = skipSpace(j + 1);
        digits();
        const qsizetype numberEnd = j;
        j = skipSpace(j);
        if (at(j) == QLatin1Char('$') && at(j + 1) == QLatin1Char('$') && numberEnd > k) {
            if (numberEnd > k)
                add(k, numberEnd - k, PromptKind::Label);
            add(j, 2, PromptKind::Accent);
            i = j + 2;
            // A separator is the text up to a second $$, when no | { } or __ comes first.
            const qsizetype close = t.indexOf(QStringLiteral("$$"), i);
            if (close >= 0) {
                const QStringView between = QStringView(t).mid(i, close - i);
                if (!between.contains(QLatin1Char('|')) && !between.contains(QLatin1Char('{')) && !between.contains(QLatin1Char('}'))
                    && !between.contains(QStringLiteral("__"))) {
                    add(close, 2, PromptKind::Accent);
                    i = close + 2;
                }
            }
        } else {
            i = k;
        }
        return weight(i);
    }
    // At "__": where a wildcard ends, or -1 (the parser's tryWildcard, without its errors).
    qsizetype wildcardEnd(qsizetype start) const {
        qsizetype j = start + 2;
        const QChar mark = at(j);
        if (mark == QLatin1Char('~') || mark == QLatin1Char('@') || mark == QLatin1Char('!'))
            ++j;
        const qsizetype nameStart = j;
        bool closed = false;
        while (j < t.size()) {
            const QChar ch = t.at(j);
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
        if (!closed || (j == nameStart && at(j) != QLatin1Char('(')))
            return -1;
        if (at(j) == QLatin1Char('(')) {
            const qsizetype close = t.indexOf(QLatin1Char(')'), j);
            if (close < 0)
                return -1;
            j = close + 1;
            if (at(j) != QLatin1Char('_') || at(j + 1) != QLatin1Char('_'))
                return -1;
        }
        return j + 2;
    }
};

// Text Template: {a} to {f}, {{ and }}; any other brace is a problem, as the kernel's fill() has it.
QList<PromptSpan> templateSpans(const QString &t) {
    QList<PromptSpan> spans;
    for (qsizetype i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        const QChar next = i + 1 < t.size() ? t.at(i + 1) : QChar();
        if ((c == QLatin1Char('{') || c == QLatin1Char('}')) && next == c) {
            spans.append({int(i), 2, PromptKind::Label, false});
            ++i;
        } else if (c == QLatin1Char('}')) {
            spans.append({int(i), 1, PromptKind::Accent, true});
        } else if (c == QLatin1Char('{')) {
            const qsizetype close = t.indexOf(QLatin1Char('}'), i + 1);
            if (close < 0) {
                spans.append({int(i), 1, PromptKind::Accent, true});
                continue;
            }
            const QStringView name = QStringView(t).mid(i + 1, close - i - 1);
            const bool known = name.size() == 1 && name.front() >= QLatin1Char('a') && name.front() <= QLatin1Char('f');
            spans.append({int(i), int(close + 1 - i), PromptKind::Accent, !known});
            i = close;
        }
    }
    return spans;
}

} // namespace

PromptSyntax promptSyntaxOf(const QString &name) {
    if (name == QLatin1String("dynamic-prompt"))
        return PromptSyntax::DynamicPrompt;
    if (name == QLatin1String("template"))
        return PromptSyntax::Template;
    return PromptSyntax::None;
}

QList<PromptSpan> promptSpans(PromptSyntax syntax, const QString &text) {
    switch (syntax) {
    case PromptSyntax::DynamicPrompt:
        return DynamicScan(text).run();
    case PromptSyntax::Template:
        return templateSpans(text);
    case PromptSyntax::None:
        break;
    }
    return {};
}

QString promptProblem(PromptSyntax syntax, const QString &text) {
    Errors errors;
    if (syntax == PromptSyntax::DynamicPrompt)
        errors = dynprompt::checkSyntax(text);
    else if (syntax == PromptSyntax::Template)
        errors = nodes::checkTextTemplate(text);
    return errors.isEmpty() ? QString() : errors.first().message;
}

PromptHighlight::PromptHighlight(QObject *parent) : QSyntaxHighlighter(parent) {
    // The problem is parsed a moment after the last key, so a long prompt types as fast as a plain one.
    m_problemTimer.setSingleShot(true);
    m_problemTimer.setInterval(150);
    connect(&m_problemTimer, &QTimer::timeout, this, &PromptHighlight::updateProblem);
    connect(this, &PromptHighlight::colorsChanged, this, &PromptHighlight::restyle);
}

QColor PromptHighlight::ink(const QColor &base, const QColor &well, double target) {
    return look::lift(base, well, target);
}

void PromptHighlight::setTarget(QQuickTextDocument *target) {
    if (m_target == target)
        return;
    m_target = target;
    setDocument(target ? target->textDocument() : nullptr);
    if (document())
        connect(document(), &QTextDocument::contentsChanged, &m_problemTimer, qOverload<>(&QTimer::start));
    m_revision = -1;
    updateProblem();
    emit targetChanged();
}

void PromptHighlight::setSyntax(const QString &syntax) {
    if (m_syntax == syntax)
        return;
    m_syntax = syntax;
    m_kind = promptSyntaxOf(syntax);
    emit syntaxChanged();
    restyle();
    updateProblem();
}

void PromptHighlight::restyle() {
    for (int kind = 0; kind < 4; ++kind)
        for (int problem = 0; problem < 2; ++problem)
            m_formats[kind][problem] = makeFormat(PromptKind(kind), problem);
    m_revision = -1;
    if (document())
        rehighlight();
}

QTextCharFormat PromptHighlight::makeFormat(PromptKind kind, bool problem) const {
    QTextCharFormat f;
    const QColor well = m_well.isValid() ? m_well : QColor(Qt::black);
    switch (kind) {
    case PromptKind::Accent:
        f.setForeground(ink(m_accent, well));
        break;
    case PromptKind::Label:
        f.setForeground(ink(m_label, well));
        break;
    case PromptKind::Role:
        f.setForeground(ink(m_role, well));
        break;
    case PromptKind::Comment:
        f.setForeground(ink(m_label, well));
        f.setFontItalic(true);
        break;
    }
    if (problem) {
        f.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        f.setUnderlineColor(ink(m_error, well, 3.0));
    }
    return f;
}

void PromptHighlight::scan() {
    if (!document() || m_revision == document()->revision())
        return;
    m_revision = document()->revision();
    m_spans = promptSpans(m_kind, document()->toPlainText());
    // A change far from the edited block can change another block's spans (the } that closes a { many lines up): those blocks are
    // highlighted again once this pass is done.
    if (!m_fixing)
        QMetaObject::invokeMethod(this, &PromptHighlight::fixOtherBlocks, Qt::QueuedConnection);
}

void PromptHighlight::highlightBlock(const QString &text) {
    if (m_kind == PromptSyntax::None) {
        setCurrentBlockState(0);
        return;
    }
    scan();
    const int blockStart = currentBlock().position();
    const int blockEnd = blockStart + int(text.size());
    // The first span that ends after the block starts.
    auto it = std::lower_bound(m_spans.cbegin(), m_spans.cend(), blockStart,
                               [](const PromptSpan &span, int at) { return span.start + span.length <= at; });
    size_t signature = 0;
    for (; it != m_spans.cend() && it->start < blockEnd; ++it) {
        const int from = std::max(it->start, blockStart), to = std::min(it->start + it->length, blockEnd);
        if (to <= from)
            continue;
        setFormat(from - blockStart, to - from, m_formats[int(it->kind)][it->problem]);
        signature = qHashMulti(signature, from - blockStart, to - from, int(it->kind), it->problem);
    }
    setCurrentBlockState(int(signature & 0x7fffffff));
}

void PromptHighlight::fixOtherBlocks() {
    if (!document() || m_kind == PromptSyntax::None)
        return;
    scan();
    m_fixing = true;
    // Each block's state is the signature of the spans it was drawn with; a block whose spans are now others is drawn again.
    auto it = m_spans.cbegin();
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const int blockStart = block.position(), blockEnd = blockStart + int(block.length()) - 1;
        while (it != m_spans.cend() && it->start + it->length <= blockStart)
            ++it;
        size_t signature = 0;
        for (auto at = it; at != m_spans.cend() && at->start < blockEnd; ++at) {
            const int from = std::max(at->start, blockStart), to = std::min(at->start + at->length, blockEnd);
            if (to > from)
                signature = qHashMulti(signature, from - blockStart, to - from, int(at->kind), at->problem);
        }
        if (block.userState() != int(signature & 0x7fffffff))
            rehighlightBlock(block);
    }
    m_fixing = false;
}

void PromptHighlight::updateProblem() {
    const QString problem = document() && m_kind != PromptSyntax::None ? promptProblem(m_kind, document()->toPlainText()) : QString();
    if (problem == m_problem)
        return;
    m_problem = problem;
    emit problemChanged();
}

} // namespace fugue
