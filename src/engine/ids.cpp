#include "ids.h"

#include <QRegularExpression>

namespace fugue {

namespace {

bool matches(const QRegularExpression &form, const QString &text) {
    return form.match(text).hasMatch();
}

} // namespace

bool isNodeId(const QString &text) {
    static const QRegularExpression form(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    return matches(form, text);
}

bool isPathId(const QString &text) {
    for (const QString &segment : text.split(QLatin1Char('/')))
        if (!isNodeId(segment))
            return false;
    return true;
}

bool isTypeId(const QString &text) {
    static const QRegularExpression form(QStringLiteral(
        "^(fugue|local|[a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+)"
        "/[a-z][a-z0-9-]*$"));
    return matches(form, text);
}

QString typeNamespace(const QString &typeId) {
    return typeId.section(u'/', 0, 0);
}

QString packOfType(const QString &typeId) {
    const QString ns = typeNamespace(typeId);
    return ns == QLatin1String("fugue") || ns == QLatin1String("local") || ns == typeId ? QString() : ns;
}

bool isPortName(const QString &text) {
    static const QRegularExpression form(QStringLiteral("^[a-z][a-z0-9_]*$"));
    return matches(form, text);
}

} // namespace fugue
