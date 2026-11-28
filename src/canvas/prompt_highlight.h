#pragma once

// Prompt syntax highlighting (#715 slice 4, spec 6.1 `syntax`, 6.2): the colours a text editor gives a Dynamic Prompt template or a Text
// Template while it is typed. The scan (`promptSpans`) is plain text in, spans out, so a test proves it with no window; PromptHighlight
// is the QSyntaxHighlighter that puts the spans on a TextEdit's document, and says the first problem (`problem`) for the editor's footer.

#include <QColor>
#include <QList>
#include <QPointer>
#include <QQuickTextDocument>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace fugue {

enum class PromptSyntax { None, DynamicPrompt, Template };
// The manifest's name ("dynamic-prompt", "template"); anything else is None.
PromptSyntax promptSyntaxOf(const QString &name);

// Accent: braces, bars and `$$` (a Template's {a} to {f}); Label: weights, pick counts and a Template's {{ and }}; Role: wildcards and
// variables; Comment: a # comment (label colour, italic). `problem`: an unmatched or unknown brace, underlined with the error colour.
enum class PromptKind { Accent, Label, Role, Comment };
struct PromptSpan {
    int start = 0; // in the document's characters (a line break is one)
    int length = 0;
    PromptKind kind = PromptKind::Accent;
    bool problem = false;
};

// The spans of `text`, in order and never overlapping. Text outside them (escapes included) stays plain.
QList<PromptSpan> promptSpans(PromptSyntax syntax, const QString &text);

// The first thing wrong with the text, in the kernel's own words with its line and column ("... (line 2, column 5 of the template)."), or
// empty. A Dynamic Prompt's wildcard files are not looked up.
QString promptProblem(PromptSyntax syntax, const QString &text);

class PromptHighlight : public QSyntaxHighlighter {
    Q_OBJECT
    QML_ELEMENT
    // The TextEdit's `textDocument`; set once.
    Q_PROPERTY(QQuickTextDocument *target READ target WRITE setTarget NOTIFY targetChanged)
    // The manifest's `syntax`; empty or unknown colours nothing.
    Q_PROPERTY(QString syntax READ syntax WRITE setSyntax NOTIFY syntaxChanged)
    // The theme's tokens; each is lifted until it reads on `well` (4.5:1, the underline 3:1).
    Q_PROPERTY(QColor accent MEMBER m_accent NOTIFY colorsChanged)
    Q_PROPERTY(QColor label MEMBER m_label NOTIFY colorsChanged)
    Q_PROPERTY(QColor role MEMBER m_role NOTIFY colorsChanged)
    Q_PROPERTY(QColor error MEMBER m_error NOTIFY colorsChanged)
    Q_PROPERTY(QColor well MEMBER m_well NOTIFY colorsChanged)
    // The first problem of the text as it stands (promptProblem), up to a moment after the last key; empty: none.
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)

public:
    explicit PromptHighlight(QObject *parent = nullptr);

    QQuickTextDocument *target() const { return m_target; }
    void setTarget(QQuickTextDocument *target);
    QString syntax() const { return m_syntax; }
    void setSyntax(const QString &syntax);
    QString problem() const { return m_problem; }

    // The colour a token is drawn in: `base` moved in lightness only until it reads on `well` at `target`:1.
    static QColor ink(const QColor &base, const QColor &well, double target = 4.5);

signals:
    void targetChanged();
    void syntaxChanged();
    void colorsChanged();
    void problemChanged();

protected:
    void highlightBlock(const QString &text) override;

private:
    void restyle();
    void scan();
    void fixOtherBlocks();
    void updateProblem();
    QTextCharFormat makeFormat(PromptKind kind, bool problem) const;

    QPointer<QQuickTextDocument> m_target;
    QString m_syntax;
    PromptSyntax m_kind = PromptSyntax::None;
    QColor m_accent, m_label, m_role, m_error, m_well;
    QTextCharFormat m_formats[4][2]; // by kind, then with the problem underline
    QList<PromptSpan> m_spans;
    int m_revision = -1;
    bool m_fixing = false;
    QString m_problem;
    QTimer m_problemTimer;
};

} // namespace fugue
