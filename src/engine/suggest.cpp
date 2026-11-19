#include "suggest.h"

#include <QList>

#include <algorithm>

namespace fugue {

namespace {

int distance(const QString &a, const QString &b) {
    QList<int> row(b.size() + 1);
    for (qsizetype j = 0; j <= b.size(); ++j)
        row[j] = int(j);
    for (qsizetype i = 1; i <= a.size(); ++i) {
        int diagonal = row[0];
        row[0] = int(i);
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const int above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                               diagonal + (a.at(i - 1) == b.at(j - 1) ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

} // namespace

QString nearest(const QString &value, const QStringList &options) {
    QString best;
    int bestDistance = 3; // more than two edits is not a near miss
    for (const QString &option : options) {
        const int d = option.compare(value, Qt::CaseInsensitive) == 0 ? 0 : distance(value, option);
        if (d < bestDistance && d < value.size()) {
            best = option;
            bestDistance = d;
        }
    }
    return best;
}

} // namespace fugue
