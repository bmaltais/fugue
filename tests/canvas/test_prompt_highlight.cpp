// Prompt syntax highlighting (#715 slice 4, spec 6.1 `syntax`, 6.2): the spans the scan finds in a Dynamic Prompt template and a Text
// Template, the first problem it names, the colours' contrast on the well in every theme, and the highlighter on a document: the
// formats drawn, the blocks drawn again when a far brace changes them, and how fast a 64 KB prompt types.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

#include "canvas/prompt_highlight.h"
#include "look.h"
#include "theme.h"

using namespace fugue;

namespace {

// "A{" for an accent span, "L" label, "R" role, "C" comment; a trailing "!" marks a problem. One entry per span, with its text.
QStringList show(PromptSyntax syntax, const QString &text) {
    QStringList out;
    for (const PromptSpan &span : promptSpans(syntax, text)) {
        static const char *kinds = "ALRC";
        out << QStringLiteral("%1:%2%3").arg(QLatin1Char(kinds[int(span.kind)]), text.mid(span.start, span.length), span.problem ? "!" : "");
    }
    return out;
}
QStringList dyn(const QString &text) { return show(PromptSyntax::DynamicPrompt, text); }
QStringList tmpl(const QString &text) { return show(PromptSyntax::Template, text); }

QHash<QString, QColor> palette(const QString &accent, const QString &background, const QString &foreground, const QString &red,
                               const QString &blue) {
    return {{"accent", QColor(accent)}, {"background", QColor(background)}, {"foreground", QColor(foreground)}, {"red", QColor(red)},
            {"green", QColor("#9ece6a")}, {"yellow", QColor("#e0af68")}, {"orange", QColor("#ff9e64")}, {"blue", QColor(blue)},
            {"magenta", QColor("#bb9af7")}, {"cyan", QColor("#7dcfff")}};
}

QColor foregroundAt(const QTextDocument &document, int position) {
    QTextCursor cursor(const_cast<QTextDocument *>(&document));
    cursor.setPosition(position);
    const QTextBlock block = cursor.block();
    const int at = position - block.position();
    for (const QTextLayout::FormatRange &range : block.layout()->formats())
        if (at >= range.start && at < range.start + range.length)
            return range.format.foreground().color();
    return {};
}

} // namespace

class TestPromptHighlight : public QObject {
    Q_OBJECT

private slots:
    void aSyntaxNameIsOneOfTwo() {
        QCOMPARE(promptSyntaxOf("dynamic-prompt"), PromptSyntax::DynamicPrompt);
        QCOMPARE(promptSyntaxOf("template"), PromptSyntax::Template);
        QCOMPARE(promptSyntaxOf(""), PromptSyntax::None);
        QCOMPARE(promptSyntaxOf("markdown"), PromptSyntax::None);
        QVERIFY(promptSpans(PromptSyntax::None, "{a|b}").isEmpty());
    }

    void variantsGetAccentBracesAndBars() {
        QCOMPARE(dyn("a {red|green} cat"), (QStringList{"A:{", "A:|", "A:}"}));
        QCOMPARE(dyn("{a|{b|c}}"), (QStringList{"A:{", "A:|", "A:{", "A:|", "A:}", "A:}"}));
    }

    void weightsAndPickCountsAreLabels() {
        QCOMPARE(dyn("{0.5::a|2::b}"), (QStringList{"A:{", "L:0.5::", "A:|", "L:2::", "A:}"}));
        QCOMPARE(dyn("{2$$a|b|c}"), (QStringList{"A:{", "L:2", "A:$$", "A:|", "A:|", "A:}"}));
        QCOMPARE(dyn("{1-3$$ and $$a|b}"), (QStringList{"A:{", "L:1-3", "A:$$", "A:$$", "A:|", "A:}"}));
        QCOMPARE(dyn("{~a|b}"), (QStringList{"A:{", "A:~", "A:|", "A:}"}));
        // $$ with nothing before it is not a count, and outside braces it is text.
        QCOMPARE(dyn("cost $$ 5"), QStringList());
    }

    void wildcardsAndVariablesAreRoleColoured() {
        QCOMPARE(dyn("a __colours__ cat"), (QStringList{"R:__colours__"}));
        QCOMPARE(dyn("__people/**__ and __name(v=x)__"), (QStringList{"R:__people/**__", "R:__name(v=x)__"}));
        QCOMPARE(dyn("${v=red} a ${v} ${w:blue}"), (QStringList{"R:${v", "R:}", "R:${v", "R:}", "R:${w", "R:}"}));
        // Underscores that close nothing are text.
        QCOMPARE(dyn("cute ^__^ face a__b"), QStringList());
    }

    void commentsAreLabelItalicAndEscapesStayPlain() {
        QCOMPARE(dyn("# a note\nred {a|b} # tail"), (QStringList{"C:# a note", "A:{", "A:|", "A:}", "C:# tail"}));
        // A # inside a word is text, and so is an escaped one.
        QCOMPARE(dyn("a#b \\# \\{ \\} \\|"), QStringList());
        QCOMPARE(dyn("\\{a|b\\}"), QStringList());
    }

    void unmatchedBracesAreMarked() {
        QCOMPARE(dyn("a {b|c"), (QStringList{"A:{!", "A:|"}));
        QCOMPARE(dyn("a b}"), (QStringList{"A:}!"}));
        QCOMPARE(dyn("{a} }"), (QStringList{"A:{", "A:}", "A:}!"}));
        QCOMPARE(dyn("${v and {a|b}"), (QStringList{"R:${v!", "A:{", "A:|", "A:}"}));
        QCOMPARE(dyn("%{wrap $$ {a}}"), (QStringList{"A:%{", "A:$$", "A:{", "A:}", "A:}"}));
    }

    void aTemplateHasPlaceholdersAndDoubledBraces() {
        QCOMPARE(tmpl("{a}\n{b} and {f}"), (QStringList{"A:{a}", "A:{b}", "A:{f}"}));
        QCOMPARE(tmpl("{{literal}} {a}"), (QStringList{"L:{{", "L:}}", "A:{a}"}));
        QCOMPARE(tmpl("{g} {ab} {} {a"), (QStringList{"A:{g}!", "A:{ab}!", "A:{}!", "A:{!"}));
        QCOMPARE(tmpl("one } two"), (QStringList{"A:}!"}));
    }

    void theFirstProblemIsInTheKernelsWordsWithItsLine() {
        QCOMPARE(promptProblem(PromptSyntax::DynamicPrompt, "a {b|c}\nfine"), QString());
        QCOMPARE(promptProblem(PromptSyntax::DynamicPrompt, "a {b|c}\nfine __no_such_file__"), QString()); // no wildcard lookup
        QCOMPARE(promptProblem(PromptSyntax::DynamicPrompt, "one\ntwo {b|c"),
                 QString("The { is never closed (line 2, column 5 of the template)."));
        QVERIFY(promptProblem(PromptSyntax::DynamicPrompt, "ok\nstray } here").contains("line 2"));
        QCOMPARE(promptProblem(PromptSyntax::Template, "{a}\n{b}"), QString());
        QVERIFY2(promptProblem(PromptSyntax::Template, "{a}\nx {zz}").contains("line 2, column 3"),
                 qPrintable(promptProblem(PromptSyntax::Template, "{a}\nx {zz}")));
        QVERIFY(promptProblem(PromptSyntax::Template, "a }").contains("line 1, column 3"));
        QCOMPARE(promptProblem(PromptSyntax::None, "{"), QString());
    }

    // The colours come from the theme: each token, lifted onto the well, reads at 4.5:1 (the underline at 3:1), in a dark theme, a light
    // one and every shipped Omarchy theme.
    void theColoursReadOnTheWell_data() {
        QTest::addColumn<QString>("accent");
        QTest::addColumn<QString>("background");
        QTest::addColumn<QString>("foreground");
        QTest::addColumn<QString>("red");
        QTest::addColumn<QString>("blue");
        QTest::addColumn<bool>("light");
        QTest::newRow("dark") << "#7d82d9" << "#060B1E" << "#ffcead" << "#ED5B5A" << "#7d82d9" << false;
        QTest::newRow("light") << "#205EA6" << "#FFFCF0" << "#100F0F" << "#D14D41" << "#205EA6" << true;
        QTest::newRow("yellow-accent-light") << "#FFD60A" << "#f5f5f5" << "#222222" << "#cc3333" << "#4a78c2" << true;
        QTest::newRow("yellow-accent-dark") << "#FFD60A" << "#0e0e10" << "#e6e6e9" << "#f7768e" << "#7aa2f7" << false;
        const QDir themes(QStringLiteral("/usr/share/omarchy/themes"));
        for (const QString &name : themes.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString file = themes.filePath(name + "/colors.toml");
            if (!QFile::exists(file))
                continue;
            const Theme::Palette read = Theme::readThemeColors(file);
            const auto c = [&](const char *key, const char *fallback) { return read.colors.value(key, QColor(fallback)).name(); };
            QTest::newRow(qPrintable(name)) << c("accent", "#FFD60A") << c("background", "#0e0e10") << c("foreground", "#e6e6e9")
                                            << c("red", "#f7768e") << c("blue", "#7aa2f7") << read.light;
        }
    }
    void theColoursReadOnTheWell() {
        QFETCH(QString, accent);
        QFETCH(QString, background);
        QFETCH(QString, foreground);
        QFETCH(QString, red);
        QFETCH(QString, blue);
        QFETCH(bool, light);
        const QHash<QString, QColor> colors = palette(accent, background, foreground, red, blue);
        const look::Tokens t = look::tokens(colors, light);
        const QColor role = look::roleTokens(t, colors.value("blue"), colors.value("foreground"), light).frameEdge;
        for (const QColor &base : {colors.value("accent"), t.label, role})
            QVERIFY2(look::contrast(PromptHighlight::ink(base, t.well), t.well) >= 4.5, qPrintable(base.name()));
        QVERIFY2(look::contrast(PromptHighlight::ink(colors.value("red"), t.well, 3.0), t.well) >= 3.0, qPrintable(red));
        // Tokens that already read are not moved.
        QCOMPARE(PromptHighlight::ink(t.label, t.well), t.label);
    }

    void theHighlighterDrawsTheSpansOnADocument() {
        QTextDocument document;
        document.setPlainText("a {red|green}\n{b} __w__ ${v} # note\nz }");
        PromptHighlight highlight;
        highlight.setProperty("accent", QColor("#ff0000"));
        highlight.setProperty("label", QColor("#00ff00"));
        highlight.setProperty("role", QColor("#0000ff"));
        highlight.setProperty("error", QColor("#ff00ff"));
        highlight.setProperty("well", QColor("#000000"));
        highlight.setDocument(&document);
        highlight.setSyntax("dynamic-prompt");
        highlight.rehighlight();
        const QColor accent = PromptHighlight::ink("#ff0000", "#000000"), role = PromptHighlight::ink("#0000ff", "#000000");
        QCOMPARE(foregroundAt(document, 2), accent);                // the {
        QCOMPARE(foregroundAt(document, 3), QColor());              // red: plain
        QCOMPARE(foregroundAt(document, 14 + 4), role);             // __w__ is on line 2 after "{b} "
        QVERIFY(foregroundAt(document, document.characterCount() - 2).isValid()); // the stray }
        QTRY_VERIFY2(highlight.problem().contains("line 3"), qPrintable(highlight.problem()));
    }

    // Closing the { many lines up draws the line of the stray } again: the other block's spans changed with no edit in it.
    void aFarBraceChangesAnotherBlocksSpans() {
        QTextDocument document;
        document.setPlainText("{a|b\nsecond\nthird }");
        PromptHighlight highlight;
        highlight.setProperty("accent", QColor("#ff0000"));
        highlight.setProperty("label", QColor("#00ff00"));
        highlight.setProperty("role", QColor("#0000ff"));
        highlight.setProperty("error", QColor("#ff00ff"));
        highlight.setProperty("well", QColor("#000000"));
        highlight.setDocument(&document);
        highlight.setSyntax("dynamic-prompt");
        highlight.rehighlight();
        const auto problemUnderline = [&](int line) {
            const QTextBlock block = document.findBlockByNumber(line);
            for (const QTextLayout::FormatRange &range : block.layout()->formats())
                if (range.format.underlineStyle() != QTextCharFormat::NoUnderline)
                    return true;
            return false;
        };
        // The { is open and the } on the last line closes it: nothing underlined.
        QVERIFY(!problemUnderline(0));
        QVERIFY(!problemUnderline(2));
        // Delete the last line's }: the { on the first line is now unmatched, and only the last block was edited.
        QTextCursor cursor(&document);
        cursor.movePosition(QTextCursor::End);
        cursor.deletePreviousChar();
        QTRY_VERIFY(problemUnderline(0));
        // Put it back.
        cursor.insertText("}");
        QTRY_VERIFY(!problemUnderline(0));
    }

    // Typing in a 64 KB prompt: one key re-scans the text and draws the block it is in. The numbers are printed for the record; the bound is
    // generous so a slow machine does not fail it, and still catches a scan that grew with the number of blocks drawn.
    void aKeyInA64KbPromptIsFast() {
        QString text;
        while (text.size() < 64 * 1024)
            text += "a {red|green|blue|{2$$ big|small}} cat, __colours__ ${v=x} 0.5::weighted # note\n";
        QTextDocument document;
        document.setPlainText(text);
        PromptHighlight highlight;
        highlight.setProperty("accent", QColor("#ff0000"));
        highlight.setProperty("label", QColor("#00ff00"));
        highlight.setProperty("role", QColor("#0000ff"));
        highlight.setProperty("error", QColor("#ff00ff"));
        highlight.setProperty("well", QColor("#000000"));
        highlight.setDocument(&document);
        QElapsedTimer timer;
        timer.start();
        highlight.setSyntax("dynamic-prompt");
        highlight.rehighlight();
        const qint64 whole = timer.elapsed();
        QTextCursor cursor(&document);
        cursor.setPosition(document.characterCount() / 2);
        timer.restart();
        for (int i = 0; i < 20; ++i) {
            cursor.insertText("x");
            QCoreApplication::processEvents(); // the highlighter draws the changed block, and the other blocks are checked
        }
        const double perKey = timer.nsecsElapsed() / 20.0 / 1e6;
        timer.restart();
        const QString problem = promptProblem(PromptSyntax::DynamicPrompt, document.toPlainText());
        const qint64 parse = timer.elapsed();
        qInfo().nospace() << "64 KB prompt (" << document.blockCount() << " lines): all drawn " << whole << " ms, one key " << perKey
                          << " ms, the problem check " << parse << " ms";
        QVERIFY(problem.isEmpty());
        QVERIFY2(perKey < 30, qPrintable(QString::number(perKey)));
    }
};

QTEST_MAIN(TestPromptHighlight)
#include "test_prompt_highlight.moc"
