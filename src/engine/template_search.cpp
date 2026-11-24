#include "engine/template_search.h"

#include <QStringList>
#include <QVariantList>

namespace fugue {

namespace {

struct Word {
    QString key; // "", "tag", "source" or "family"
    QString text;
};

// Splits on spaces; a double-quoted run is one piece, after `key:` or alone.
QList<Word> words(const QString &query) {
    QList<Word> out;
    int i = 0;
    const int n = int(query.size());
    while (i < n) {
        if (query.at(i).isSpace()) {
            ++i;
            continue;
        }
        QString piece;
        bool quoted = false;
        while (i < n && (quoted || !query.at(i).isSpace())) {
            if (query.at(i) == QLatin1Char('"'))
                quoted = !quoted;
            else
                piece += query.at(i);
            ++i;
        }
        Word word{{}, piece.toLower()};
        const qsizetype colon = piece.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            const QString key = piece.left(colon).toLower();
            if (key == QLatin1String("tag") || key == QLatin1String("source") || key == QLatin1String("family"))
                word = {key, piece.mid(colon + 1).toLower()};
        }
        if (!word.text.isEmpty())
            out.append(word);
    }
    return out;
}

QStringList listOf(const QVariant &value) {
    QStringList out;
    for (const QVariant &item : value.toList())
        out << item.toString();
    if (out.isEmpty() && value.metaType().id() == QMetaType::QStringList)
        out = value.toStringList();
    return out;
}

} // namespace

bool matchesWordStart(const QString &text, const QString &needle) {
    if (needle.isEmpty())
        return true;
    qsizetype at = 0;
    while ((at = text.indexOf(needle, at, Qt::CaseInsensitive)) >= 0) {
        if (at == 0 || !text.at(at - 1).isLetterOrNumber())
            return true;
        ++at;
    }
    return false;
}

bool templateMatchesSearch(const QString &query, const QVariantMap &templ) {
    const QVariantMap source = templ.value(QStringLiteral("source")).toMap();
    const QStringList tags = listOf(templ.value(QStringLiteral("tags")));
    const QStringList families = listOf(templ.value(QStringLiteral("families")));
    const QString sourceText = source.value(QStringLiteral("name")).toString() + QLatin1Char(' ')
                               + source.value(QStringLiteral("author")).toString();
    const QStringList all{templ.value(QStringLiteral("title")).toString(),
                          templ.value(QStringLiteral("description")).toString(),
                          templ.value(QStringLiteral("categoryTitle")).toString(),
                          templ.value(QStringLiteral("id")).toString(),
                          tags.join(QLatin1Char(' ')),
                          sourceText,
                          families.join(QLatin1Char(' '))};
    const QString everything = all.join(QLatin1Char(' '));
    for (const Word &word : words(query)) {
        const bool found = word.key == QLatin1String("tag")      ? matchesWordStart(tags.join(QLatin1Char(' ')), word.text)
                           : word.key == QLatin1String("source") ? matchesWordStart(sourceText, word.text)
                           : word.key == QLatin1String("family") ? matchesWordStart(families.join(QLatin1Char(' ')), word.text)
                                                                 : matchesWordStart(everything, word.text);
        if (!found)
            return false;
    }
    return true;
}

} // namespace fugue
