// Dynamic prompts (engine/dynamic_prompt.h): the template language of adieyal's dynamicprompts, the
// examples of its SYNTAX.md, wildcard files of every kind, the errors, the folder limits and expandAll.
//
// Seam: generate, expandAll and check on wildcard files in a temporary folder. A random result is
// asserted as the set of what 300 seeds give (the cases here have at most a few dozen outcomes).

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/dynamic_prompt.h"

using namespace fugue;

namespace {

void write(const QString &path, const QString &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8()) < 0)
        qFatal("cannot write %s", qPrintable(path));
}

} // namespace

class TestDynamicPrompt : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    QTemporaryDir m_outside;
    dynprompt::Options m_options;

    QString one(const QString &templ, quint64 seed = 1) {
        const Result<QString> r = dynprompt::generate(templ, seed, m_options);
        return r.ok() ? r.value() : "ERROR: " + r.error().message;
    }
    QSet<QString> outcomes(const QString &templ, int seeds = 300) {
        QSet<QString> out;
        for (int s = 0; s < seeds; ++s)
            out.insert(one(templ, quint64(s)));
        return out;
    }
    static QSet<QString> set(const QStringList &list) { return QSet<QString>(list.begin(), list.end()); }
    QString failure(const QString &templ) {
        const Result<QString> r = dynprompt::generate(templ, 1, m_options);
        if (r.ok())
            return "NO ERROR: " + r.value();
        return r.error().code + ": " + r.error().message;
    }

private slots:
    void initTestCase() {
        const QString w = m_dir.path() + "/wildcards";
        m_options.folders = {w, m_dir.path() + "/missing-folder"};
        write(w + "/season.txt", "# seasons.txt\nsummer\nautumn\nwinter\nspring\n");
        write(w + "/weather.txt", "{autumn|fall}\n");
        write(w + "/colours-cold.txt", "# cold\nblue\ngreen\n");
        write(w + "/colours-warm.txt", "red\nyellow\n");
        write(w + "/flavours.txt", "chocolate\nvanilla\nstrawberry\n");
        write(w + "/people_of_the_world.txt", "__africa__\n__asia__\n");
        write(w + "/africa.txt", "Zimbabwean\nNamibian\n");
        write(w + "/asia.txt", "Thai\n");
        write(w + "/people/europe/french.txt", "French\n");
        write(w + "/people/europe/danish.txt", "Danish\n");
        write(w + "/people/asia.txt", "Korean\n");
        write(w + "/weird.txt", "{p|q}\nr\n");
        write(w + "/weighted.txt", "3::heavy\n0::never\n");
        write(w + "/season_clothes.txt", "In ${season}, I wear ${season} shirts and ${season} trousers\n");
        write(w + "/clothes_default.txt", "In ${season:summer}, I wear ${season:summer} shirts\n");
        write(w + "/loop-a.txt", "__loop-b__\n");
        write(w + "/loop-b.txt", "x __loop-a__\n");
        write(w + "/empty.txt", "# nothing\n\n");
        write(w + "/example.yaml",
              "# example.yaml\n"
              "clothing:\n  - T-shirt\n  - Pants\n  - Shoes\n"
              "artists:\n"
              "  finnish:\n    - Akseli Gallen-Kallela\n    - \"Eero J\u00e4rnefelt\"\n"
              "  dutch:\n    - Piet Mondrian  # a comment\n    - 'Rembrandt van Rijn'\n"
              "  italian: Leonardo da Vinci # one item\n"
              "  1234: 5678\n  flurp: 12345\n"
              "hues:\n  - 4::red\n  - 0::blue\n");
        write(w + "/publicprompts.yaml",
              "public-prompts:\n  cute-stuffed-animals:\n    - |\n      # Usage: __public-prompts/cuddly-toys(figure=elephant)__\n"
              "      cute kawaii Squishy ${figure} plush toy,\n      realistic texture,\n      square image\n");
        write(w + "/examples.yaml",
              "examples:\n  prompt:\n    - \"${subject?=!{man|woman}} ${weather?=!{sun|rain}} ${drink?=!{__examples/drink__}} a ${subject} standing in the ${weather} drinking ${drink}\"\n"
              "  drink:\n    - coffee\n    - tea\n"
              "  winter:\n    - \"${weather=snow} ${drink=hot chocolate} __examples/prompt__\"\n");
        write(w + "/data.json", "{\"clothing\": [\"hat\", \"coat\"], \"artists\": {\"dutch\": [\"Vermeer\"]}, \"count\": 3}");
        write(w + "/bad.yaml", "good:\n  - x\nbad: [a, b]\n");
        write(w + "/lex-ok.txt", "ok\n");
        write(w + "/setter.txt", "${v=red}\n");
        QString deep;
        for (int k = 0; k < 40; ++k)
            deep += QString(k * 2, QChar(' ')) + "k" + QString::number(k) + ":\n";
        write(w + "/deep.yaml", deep + QString(80, QChar(' ')) + "- x\n");
        write(m_outside.path() + "/secret.txt", "top secret\n");
        write(m_outside.path() + "/dir/secret2.txt", "top secret too\n");
        QVERIFY(QFile::link(m_outside.path() + "/secret.txt", w + "/leak.txt"));
        QVERIFY(QFile::link(m_outside.path() + "/dir", w + "/leakdir"));
        QVERIFY(QFile::link(m_outside.path() + "/secret.txt", w + "/lex-bad.txt"));
    }

    // ---- SYNTAX.md: variants
    void variantsPickOneOption() {
        QCOMPARE(outcomes("{summer|autumn|winter|spring} is coming"),
                 set({"summer is coming", "autumn is coming", "winter is coming", "spring is coming"}));
        QCOMPARE(outcomes("{a}"), set({"a"}));
        QCOMPARE(outcomes("x{a|}y"), set({"xay", "xy"}));
    }
    void sameSeedSameTextAndOthersDiffer() {
        const QString templ = "{a|b|c|d|e|f|g|h} {1|2|3|4|5|6|7|8} __flavours__";
        QCOMPARE(one(templ, 7), one(templ, 7));
        QVERIFY(outcomes(templ, 50).size() > 10);
    }
    void weightsAreRelative() {
        QCOMPARE(outcomes("{1::a|0::b}"), set({"a"}));
        QCOMPARE(outcomes("{0.5::summer|0.1::autumn|0.3::winter|0.1::spring}").size(), 4);
        int a = 0;
        for (int s = 0; s < 400; ++s)
            a += one("{9::a|1::b}", quint64(s)) == "a";
        QVERIFY2(a > 320 && a < 400, qPrintable(QString::number(a)));
        QVERIFY(failure("{0::a|0::b}").contains("weight 0"));
    }
    void severalPicksAreWithoutReplacement() {
        for (const QString &r : outcomes("My favourite ice-cream flavours are {2$$chocolate|vanilla|strawberry}")) {
            const QStringList two = r.mid(QString("My favourite ice-cream flavours are ").size()).split(", ");
            QCOMPARE(two.size(), 2);
            QVERIFY(two[0] != two[1]);
        }
        QVERIFY(outcomes("{2$$a|b|c}").contains("b, a")); // any order
        const QSet<QString> and_ = outcomes("{2$$ and $$chocolate|vanilla|strawberry}");
        QVERIFY(and_.contains("chocolate and vanilla"));
        QVERIFY(and_.contains("strawberry and chocolate"));
        QCOMPARE(and_.size(), 6);
    }
    void rangesAndOmittedBounds() {
        const QSet<QString> r = outcomes("{1-2$$ and $$chocolate|vanilla|strawberry}");
        QVERIFY(r.contains("chocolate") && r.contains("vanilla and chocolate"));
        QCOMPARE(r.size(), 3 + 6);
        QCOMPARE(outcomes("{-2$$chocolate|vanilla|strawberry}"), outcomes("{1-2$$chocolate|vanilla|strawberry}"));
        QCOMPARE(outcomes("{1-$$a|b|c}"), outcomes("{1-3$$a|b|c}"));
        QVERIFY(outcomes("{1-$$a|b|c}").contains("a, b, c") || outcomes("{1-$$a|b|c}").contains("c, b, a"));
        QCOMPARE(outcomes("p{4$$a|b|c}").size(), 6); // only as many as there are
        for (const QString &r2 : outcomes("p{4$$a|b|c}"))
            QCOMPARE(r2.count(", "), 2);
        QVERIFY(failure("{3-2$$a|b}").contains("backwards"));
    }

    // ---- SYNTAX.md: wildcards
    void wildcardTakesALineOfItsFile() {
        QCOMPARE(outcomes("__season__ is coming"), set({"summer is coming", "autumn is coming", "winter is coming", "spring is coming"}));
        QCOMPARE(outcomes("{2$$__flavours__}").size(), 6);        // no duplicates, from the wildcard's own lines
        QCOMPARE(outcomes("{2$$__flavours__|__flavours__}").size() >= 6, true);
        QCOMPARE(outcomes("{3-$$__flavours__}").size(), 6);
    }
    void variantsAndWildcardsNest() {
        QCOMPARE(outcomes("__weather__"), set({"autumn", "fall"}));
        QCOMPARE(outcomes("__people_of_the_world__"), set({"Zimbabwean", "Namibian", "Thai"}));
        QCOMPARE(outcomes("{__africa__|__asia__}"), set({"Zimbabwean", "Namibian", "Thai"}));
    }
    void globsStayInsideTheFolders() {
        QCOMPARE(outcomes("The colour of my shirt is __colours*__"),
                 set({"The colour of my shirt is blue", "The colour of my shirt is green", "The colour of my shirt is red",
                      "The colour of my shirt is yellow"}));
        QCOMPARE(outcomes("__people/**__"), set({"French", "Danish", "Korean"}));
        QCOMPARE(outcomes("__people/*__"), set({"Korean"}));
        QCOMPARE(outcomes("__people/europe/*__"), set({"French", "Danish"}));
    }
    void yamlAndJsonCollections() {
        QCOMPARE(outcomes("__clothing__"), set({"T-shirt", "Pants", "Shoes", "hat", "coat"}));
        QCOMPARE(outcomes("__artists/finnish__"), set({"Akseli Gallen-Kallela", "Eero J\u00e4rnefelt"}));
        QCOMPARE(outcomes("__artists/dutch__"), set({"Piet Mondrian", "Rembrandt van Rijn", "Vermeer"}));
        QCOMPARE(outcomes("__artists/italian__"), set({"Leonardo da Vinci"}));
        QCOMPARE(outcomes("__artists/**__").size(), 2 + 3 + 1); // numbers are ignored
        QCOMPARE(outcomes("__artists__").size(), 2 + 3 + 1);    // a name above the lists takes all of them
        QCOMPARE(outcomes("__hues__"), set({"red"}));         // YAML weights: 4::red, 0::blue
        QCOMPARE(outcomes("__weighted__"), set({"heavy"}));       // and a txt line has them too
    }
    void parametersFillVariables() {
        QCOMPARE(one("__season_clothes(season=winter)__"), QString("In winter, I wear winter shirts and winter trousers"));
        QCOMPARE(one("${season={summer|autumn|winter|spring}} __season_clothes__").count("In "), 1);
        QCOMPARE(one("__clothes_default__"), QString("In summer, I wear summer shirts"));
        QCOMPARE(one("__clothes_default(season=fall)__"), QString("In fall, I wear fall shirts"));
        // a parameter does not outlive the wildcard it was given to
        QVERIFY(failure("__season_clothes(season=winter)__ ${season}").contains("not set"));
        QCOMPARE(one("__public-prompts/cute-stuffed-animals(figure=elephant)__"),
                 QString("cute kawaii Squishy elephant plush toy, realistic texture, square image"));
    }

    // ---- SYNTAX.md: variables
    void variablesAreImmediateOrNot() {
        for (const QString &r : outcomes("${season=!__season__}In ${season}, I wear ${season} shirts and ${season} trousers")) {
            const QString s = r.mid(3).split(',')[0];
            QCOMPARE(r, QString("In %1, I wear %1 shirts and %1 trousers").arg(s));
        }
        bool differs = false;
        for (const QString &r : outcomes("${season=__season__}In ${season}, I wear ${season} shirts and ${season} trousers"))
            differs = differs || r.split(' ')[1] != r.split(' ')[4];
        QVERIFY(differs);
        QCOMPARE(outcomes("${s=!{a|b}}${s}${s}"), set({"aa", "bb"}));
        QCOMPARE(one("${d={red|blue}, {tall|short}} A ${d} man and a ${d} woman").count(" and a "), 1);
        QCOMPARE(one("${x:fallback} ${x=set}${x}"), QString("fallback set"));
    }
    void keepExistingAndParametersAcrossWildcards() {
        for (const QString &r : outcomes("${subject=boy} __examples/prompt__"))
            QVERIFY2(r.startsWith("a boy standing in the "), qPrintable(r));
        for (const QString &r : outcomes("${subject=cowboy} ${weather=sun} ${drink=sasparilla} __examples/prompt__"))
            QCOMPARE(r, QString("a cowboy standing in the sun drinking sasparilla"));
        QCOMPARE(outcomes("__examples/winter__"),
                 set({"a man standing in the snow drinking hot chocolate", "a woman standing in the snow drinking hot chocolate"}));
        QCOMPARE(one("${subject=boy} ${weather=rain} ${drink=iced tea} __examples/winter__"),
                 QString("a boy standing in the snow drinking hot chocolate"));
    }

    // ---- SYNTAX.md: whitespace, comments, wrap, samplers
    void whitespaceAndCommentsAreIgnored() {
        QCOMPARE(one("    # Set the season variable\n    ${season={\n        summer\n\t|autumn\n\t# | fall    # commented out\n\t|winter\n"
                     "    |spring\n    }}\n   In ${season:summer}, I wear ${season:summer} shirts")
                     .startsWith("In "),
                 true);
        QCOMPARE(outcomes("{a\n  |b # nope\n  # |c\n}"), set({"a", "b"}));
        QCOMPARE(one("tags #python and #ff0000 and \\# not"), QString("tags #python and #ff0000 and # not")); // hashtags and colours are text
        QCOMPARE(one("red # a comment\nblue"), QString("red blue"));
        QCOMPARE(one("a  b\n   c"), QString("a  b c"));
    }
    void wrapPutsAPromptInATemplate() {
        QCOMPARE(one("%{something wrapping ..., very cool, neat$$this internal prompt}"),
                 QString("something wrapping this internal prompt, very cool, neat"));
        QCOMPARE(outcomes("%{a ...$$__asia__}"), set({"a Thai"}));
        QVERIFY(failure("%{no placeholder$$x}").contains("no ..."));
    }
    void samplerMarks() {
        QCOMPARE(outcomes("{~red|green}"), set({"red", "green"}));
        QCOMPARE(outcomes("{!red|green}"), set({"red", "green"}));
        QCOMPARE(outcomes("{\\@aster crowley|@uo denim|@igiroitsu}"), set({"@aster crowley", "@uo denim", "@igiroitsu"}));
        // cyclical parts keep in step: the seed picks the place in both
        for (quint64 s = 0; s < 6; ++s) {
            const QString r = one("{@e1|e2} and {@e3|e4}", s);
            QVERIFY2(r == "e1 and e3" || r == "e2 and e4", qPrintable(r));
        }
        QVERIFY(one("{@e1|e2}", 0) != one("{@e1|e2}", 1));
        QCOMPARE(one("\\~ \\! \\@ \\{ \\} \\| \\\\"), QString("~ ! @ { } | \\"));
    }

    // ---- errors
    void syntaxErrorsNameThePlace() {
        QVERIFY2(failure("a {b|c").contains("template_invalid: The { is never closed (line 1, column 3 of the template)"), qPrintable(failure("a {b|c")));
        QVERIFY(failure("line one\n  ${v=").contains("line 2, column 3"));
        QVERIFY(failure("x } y").contains("A } closes nothing (line 1, column 3"));
        QCOMPARE(one("cute ^__^ face"), QString("cute ^__^ face")); // underscores that close nothing are text
        QCOMPARE(one("a__b c and __season"), QString("a__b c and __season"));
        QVERIFY(failure("__x(a=1,a=2)__").contains("parameter a of __x(...)__ is given twice"));
        QVERIFY(failure("${a b}").contains("expected }"));
        QVERIFY(failure("${}").contains("needs a name"));
        QVERIFY(failure("%{a$$b").contains("never closed"));
        QVERIFY(failure("__x(a)__").contains("name=value"));
        QCOMPARE(one("a __ b ____ c"), QString("a __ b ____ c")); // lone underscores are text
    }
    void missingThingsAreErrorsNotHoles() {
        const QString missing = failure("a __nothere__");
        QVERIFY2(missing.contains("\"nothere\" was not found") && missing.contains(m_options.folders.first()) && missing.contains("line 1, column 3"),
                 qPrintable(missing));
        QVERIFY(failure("${nope}").contains("The variable ${nope} is not set (line 1, column 1"));
        QVERIFY2(failure("__empty__").contains("has no values"), qPrintable(failure("__empty__")));
        const QString bad = failure("__nothere__");
        QVERIFY2(bad.contains("bad.yaml, line 3") && bad.contains("flow list"), qPrintable(bad)); // a YAML outside the subset is named
    }
    void loopsReportTheirChain() {
        const QString loop = failure("__loop-a__");
        QVERIFY2(loop.contains("a loop") && loop.contains("loop-a -> loop-b -> loop-a"), qPrintable(loop));
        QVERIFY(failure("${a=${b}}${b=${a}}${a}").contains("${a} -> ${b} -> ${a}"));
    }
    void namesCannotLeaveTheFolders() {
        for (const QString name : {"../secret", "a/../../secret", "/etc/passwd", "..\\x", "c:/x", "./x", "a//b", "**/../x"}) {
            const QString r = failure("__" + name + "__");
            QVERIFY2(r.contains("is not allowed"), qPrintable(name + " -> " + r));
        }
        const QString link = failure("__leak__");
        QVERIFY2(link.contains("link to outside the wildcard folders") && !link.contains("top secret"), qPrintable(link));
        const QString dir = failure("__leakdir/secret2__");
        QVERIFY2(!dir.contains("top secret") && dir.startsWith("NO ERROR") == false, qPrintable(dir));
        QVERIFY(!outcomes("__**__").contains("top secret"));
        QVERIFY(!outcomes("__leakdir/*__").contains("top secret too"));
    }
    void limitsEndInErrors() {
        dynprompt::Options small = m_options;
        small.limits.maxOutput = 100;
        QVERIFY(dynprompt::generate("${a=xxxxxxxxxx}${a}${a}${a}${a}${a}${a}${a}${a}${a}${a}${a}", 1, small).error().message.contains("longer than 100"));
        small = m_options;
        small.limits.maxFileBytes = 10;
        QVERIFY(dynprompt::generate("__flavours__", 1, small).error().message.contains("over the 10 byte limit"));
        small = m_options;
        small.limits.maxDepth = 3;
        QVERIFY(dynprompt::generate("{{{{a}}}}", 1, small).error().message.contains("nested more than 3"));
        QVERIFY(failure("${a=${a}}${a}").contains("loop"));
        small = m_options;
        small.limits.maxSteps = 50;
        QVERIFY(dynprompt::generate("${a=${b}${b}}${b=${c}${c}}${c=${d}${d}}${d=${e}${e}}${e=}${a}${a}${a}${a}", 1, small).error().message.contains("steps"));
        small = m_options;
        small.limits.maxFiles = 2;
        QVERIFY(dynprompt::generate("__colours*__", 1, small).error().message.contains("wildcard files"));
        QCOMPARE(set(one("{99999999999999999999$$a|b}").split(", ")), set({"a", "b"})); // clamped to the options
    }
    void checkFindsWhatNeedsNoRun() {
        QVERIFY(dynprompt::check("{a|b} __season__ ${x=1}${x}", m_options).isEmpty());
        QVERIFY(dynprompt::check("{a|b", m_options).first().message.contains("never closed"));
        QCOMPARE(dynprompt::check("{a|b", m_options).first().input, QString("template"));
        QVERIFY(dynprompt::check("__nothere__", m_options).first().message.contains("was not found"));
        QVERIFY(dynprompt::check("${y}", m_options).first().message.contains("never set"));
        QVERIFY(dynprompt::check("${y:d}", m_options).isEmpty());
        QVERIFY(dynprompt::check("__../x__", m_options).first().message.contains("not allowed"));
    }

    // ---- every combination
    void expandAllListsEveryCombination() {
        const Result<dynprompt::Expansion> r = dynprompt::expandAll("A {red|green|blue} {square|circle}", m_options, 100);
        QVERIFY(r.ok());
        QCOMPARE(r->prompts, QStringList({"A red square", "A red circle", "A green square", "A green circle", "A blue square", "A blue circle"}));
        QVERIFY(!r->capped);
        QCOMPARE(dynprompt::expandAll("{2$$a|b|c}", m_options, 100)->prompts, QStringList({"a, b", "a, c", "b, c"}));
        QCOMPARE(dynprompt::expandAll("{1-2$$ and $$a|b}", m_options, 100)->prompts, QStringList({"a", "b", "a and b"}));
        QCOMPARE(dynprompt::expandAll("{a|b} __colours*__", m_options, 100)->prompts.size(), 2 * 4);
        QCOMPARE(dynprompt::expandAll("${x=!{1|2}}${x}${x}", m_options, 100)->prompts, QStringList({"11", "22"}));
        QCOMPARE(dynprompt::expandAll("no choices", m_options, 100)->prompts, QStringList({"no choices"}));
        // a ~ part stays random and an @ part cycles
        const QStringList mixed = dynprompt::expandAll("{red|green|blue} {~square|circle}", m_options, 100)->prompts;
        QCOMPARE(mixed.size(), 3);
        QCOMPARE(dynprompt::expandAll("{a|b|c} {@x|y}", m_options, 100)->prompts, QStringList({"a x", "b y", "c x"}));
    }
    void expandAllIsCappedAndSaysSo() {
        const Result<dynprompt::Expansion> r = dynprompt::expandAll("{a|b|c|d} {e|f|g|h} {i|j|k|l}", m_options, 10);
        QVERIFY(r.ok());
        QCOMPARE(r->prompts.size(), 10);
        QVERIFY(r->capped);
        QCOMPARE(r->prompts.first(), QString("a e i"));
        const Result<dynprompt::Expansion> exact = dynprompt::expandAll("{a|b}", m_options, 2);
        QCOMPARE(exact->prompts.size(), 2);
        QVERIFY(!exact->capped); // exactly the cap is not capped
        QVERIFY(!dynprompt::expandAll("{a", m_options, 5).ok());
    }

    // ---- limits and lookups found by review
    void aHugePickCountHitsTheStepLimitInsteadOfHanging() {
        QString templ = "{100000$$"; // 100000 picks over 100000 options: 1e10 steps, over the 200000 limit
        for (int k = 0; k < 100000; ++k)
            templ += "a|";
        templ += "}";
        QVERIFY(failure(templ).contains("steps"));
    }
    void expandAllWithRandomAndCyclicPartsAroundChoicesStaysCorrect() {
        for (const QString templ : {"{@{a|b}|c} {x|y|z}", "{@{a|b}|c} {x|y}", "{~{a|b}|c} {x|y}", "__~weird__ {x|y}", "{@{a|b}|c}{@{p|q}|r} {x|y}"}) {
            const Result<dynprompt::Expansion> r = dynprompt::expandAll(templ, m_options, 100);
            QVERIFY2(r.ok(), qPrintable(templ + ": " + r.error().message));
            QVERIFY2(!r->prompts.isEmpty(), qPrintable(templ));
            QCOMPARE(set(r->prompts).size(), int(r->prompts.size())); // no duplicates
            QVERIFY(r->prompts.size() % 2 == 0 || r->prompts.size() % 3 == 0);
        }
        QCOMPARE(dynprompt::expandAll("{@{a|b}|c} {x|y}", m_options, 100)->prompts, QStringList({"a x", "a y", "b x", "b y"}));
    }
    void aGlobOverManyFilesIsNotCountedPerGlob() {
        QTemporaryDir dir;
        for (int k = 0; k < 2000; ++k)
            write(dir.path() + QString("/%1%2.txt").arg(QChar('a' + k % 3)).arg(k), "v\n");
        dynprompt::Options options;
        options.folders = {dir.path()};
        QCOMPARE(dynprompt::generate("__a*__ __b*__ __c*__", 1, options).value(), QString("v v v"));
        const QString before = dynprompt::wildcardSignature(options.folders);
        QCOMPARE(dynprompt::wildcardSignature(options.folders), before);
        write(dir.path() + "/zz-last.txt", "w\n"); // the last file in order is part of the key too
        QVERIFY(dynprompt::wildcardSignature(options.folders) != before);
        options.limits.maxFiles = 100;
        QVERIFY(dynprompt::generate("__a*__", 1, options).error().message.contains("more than 100 wildcard files"));
    }
    void aLinkOutsideSkipsOneFileOfAGlobButNotAnExactName() {
        QCOMPARE(outcomes("__lex-*__"), set({"ok"}));
        QVERIFY(failure("__lex-bad__").contains("link to outside"));
    }
    void theFirstFolderWithTheNameWinsInAnyFormat() {
        QTemporaryDir user;
        QTemporaryDir pack;
        write(user.path() + "/colours.yaml", "colours:\n  - red\n");
        write(pack.path() + "/colours.txt", "blue\n");
        write(pack.path() + "/only-pack.txt", "p\n");
        dynprompt::Options options;
        options.folders = {user.path(), pack.path()};
        QSet<QString> got;
        for (quint64 s = 0; s < 40; ++s)
            got.insert(dynprompt::generate("__colours__ __only-pack__", s, options).value());
        QCOMPARE(got, set({"red p"}));
        options.folders = {pack.path(), user.path()};
        QCOMPARE(dynprompt::generate("__colours__", 1, options).value(), QString("blue"));
        write(pack.path() + "/colours.yaml", "colours:\n  - green\n");
        options.folders = {user.path(), pack.path()};
        QSet<QString> same;
        for (quint64 s = 0; s < 40; ++s)
            same.insert(dynprompt::generate("__colours__", s, options).value());
        QCOMPARE(same, set({"red"})); // lists of the same name in two folders are not merged
    }
    void deepYamlIsRefusedWithItsFile() {
        const QString r = failure("__deep__");
        QVERIFY2(r.contains("deep.yaml, line") && r.contains("nested more than 32"), qPrintable(r));
    }
    void checkLeavesVariablesOfWildcardLinesAlone() {
        QVERIFY(dynprompt::check("__setter__ ${v}", m_options).isEmpty());
        QCOMPARE(one("__setter__ ${v}"), QString("red")); // the line sets it when it is picked
    }

    // ---- second review
    void aLongChainOfBigVariantsPicksInLogTime() {
        QString big = "{";
        for (int k = 0; k < 20000; ++k)
            big += "x|";
        big += "y}";
        QString templ = "${a=" + big + "}${b=${a}${a}}";
        for (int k = 0; k < 14; ++k)
            templ += QString("${c%1=${%2}${%2}}").arg(k).arg(k == 0 ? QString("b") : QString("c%1").arg(k - 1));
        templ += "${c13}";
        const Result<QString> r = dynprompt::generate(templ, 1, m_options);
        QVERIFY2(r.ok(), qPrintable(r.error().message)); // 2^15 picks of a 20000-option variant, each a search
        QCOMPARE(r.value().size(), 1 << 15);
    }
    void fewPicksOfABigWildcardAreCheapAndLargeSharesWork() {
        QTemporaryDir dir;
        QString lines;
        for (int k = 0; k < 100000; ++k)
            lines += QString("tag%1\n").arg(k);
        write(dir.path() + "/tags.txt", lines);
        dynprompt::Options options;
        options.folders = {dir.path()};
        for (const QString templ : {"{2$$__tags__}", "{3$$__tags__}"}) {
            const Result<QString> r = dynprompt::generate(templ, 3, options);
            QVERIFY2(r.ok(), qPrintable(templ + r.error().message));
            const QStringList parts = r.value().split(", ");
            QCOMPARE(set(parts).size(), int(parts.size()));
        }
        QVERIFY(dynprompt::expandAll("{3$$__tags__}", options, 5)->capped);
        // a share of the options that is not small, with a zero weight that is never taken
        QCOMPARE(set(one("{3$$1::a|1::b|1::c|0::d}").split(", ")), set({"a", "b", "c"}));
        QVERIFY(failure("{40000$$" + QString("a|").repeated(40000) + "}").contains("steps"));
    }
    void builtinCollectionIsSearchedLastAndNeedsNoCanonicalPath() {
        QTemporaryDir user;
        const QString builtin = dynprompt::builtinWildcardFolder();
        QCOMPARE(dynprompt::wildcardFolders().last(), builtin);
        QVERIFY2(QFileInfo(builtin + "/places.txt").isFile(), "share/wildcards is not compiled in");
        dynprompt::Options options;
        options.folders = {user.path(), builtin}; // an empty user folder
        const Result<QString> place = dynprompt::generate("__places__", 3, options);
        QVERIFY2(place.ok(), qPrintable(place.error().message));
        QVERIFY(!place.value().isEmpty() && !place.value().startsWith('#'));
        // every starter list resolves and gives a line, never its header
        for (const QString &name : {"places", "eras", "weather", "time-of-day", "art-styles", "moods", "colour-palettes", "creatures"}) {
            const Result<QString> r = dynprompt::generate("__" + name + "__", 7, options);
            QVERIFY2(r.ok() && !r.value().isEmpty() && !r.value().startsWith('#'), qPrintable(name));
        }
        // a user's file of the same name wins, also over a glob
        write(user.path() + "/places.txt", "only here\n");
        QCOMPARE(dynprompt::generate("__places__", 3, options).value(), QString("only here"));
        // globs see the built-in lists; the file listing and the cache signature too
        const Result<QString> glob = dynprompt::generate("__colo*__", 3, options);
        QVERIFY2(glob.ok(), qPrintable(glob.error().message));
        QVERIFY(dynprompt::generate("__art-*__", 3, options).ok());
        write(user.path() + "/x.txt", "x\n");
        QVERIFY(dynprompt::wildcardSignature({builtin}) != dynprompt::wildcardSignature({user.path()}));
        QVERIFY(!dynprompt::wildcardSignature({builtin}).isEmpty());
        // another ":/" folder is not trusted: no canonical path, so it is not read
        dynprompt::Options other;
        other.folders = {QStringLiteral(":/fugue/looks")};
        QVERIFY(!dynprompt::generate("__anything__", 1, other).ok());
        QVERIFY(!dynprompt::generate("__../places__", 1, options).ok());
    }
    void cyclicAndRandomPartsBeforeChoicesAreDrawnAtEveryPrompt() {
        QCOMPARE(dynprompt::expandAll("{@x|y} {a|b|c}", m_options, 100)->prompts, QStringList({"x a", "y b", "x c"}));
        const QStringList shapes = dynprompt::expandAll("{~square|circle|star|ring|dot} {red|green|blue|pink|tan|grey|teal|lime}", m_options, 100)->prompts;
        QCOMPARE(shapes.size(), 8);
        QVERIFY(set(shapes).size() > 3); // not one shape repeated
        QCOMPARE(dynprompt::expandAll("{red|green|blue} {~square|circle}", m_options, 100)->prompts.size(), 3);
        QCOMPARE(dynprompt::expandAll("{a|b|c} {@x|y}", m_options, 100)->prompts, QStringList({"a x", "b y", "c x"}));
        // a pick that holds choices keeps its first pick, so those choices are enumerated
        QCOMPARE(dynprompt::expandAll("{@{a|b}|c} {x|y}", m_options, 100)->prompts, QStringList({"a x", "a y", "b x", "b y"}));
    }
};

QTEST_GUILESS_MAIN(TestDynamicPrompt)
#include "test_dynamic_prompt.moc"
