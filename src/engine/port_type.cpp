#include "port_type.h"

#include "ids.h"

#include <QRegularExpression>

namespace fugue {

namespace {

const QString anyId = QStringLiteral("any");

bool isBuiltinId(const QString &id) {
    static const QStringList builtins{"text", "number", "boolean", "image", "video", "audio"};
    return builtins.contains(id);
}

} // namespace

std::optional<PortType> PortType::parse(const QString &text) {
    if (isBuiltinId(text) || text == anyId)
        return PortType{text, 1};
    static const QRegularExpression versioned(QStringLiteral("^([^@]+)(@([1-9][0-9]{0,8}))?$"));
    const QRegularExpressionMatch match = versioned.match(text);
    if (!match.hasMatch() || !isTypeId(match.captured(1)))
        return std::nullopt;
    const int version = match.hasCaptured(3) ? match.captured(3).toInt() : 1;
    return PortType{match.captured(1), version};
}

QString PortType::toString() const {
    if (version == 1)
        return id;
    return id + u'@' + QString::number(version);
}

bool PortType::isBuiltin() const {
    return isBuiltinId(id);
}

bool PortType::isAny() const {
    return id == anyId;
}

QString PortSpec::toString() const {
    return list ? QStringLiteral("list of ") + type.toString() : type.toString();
}

Compatibility compatibility(const PortSpec &output, const PortSpec &input) {
    // An "any" output (a Switch's value) takes its type from what feeds it; the compiler checks that type.
    if (!input.type.isAny() && !output.type.isAny() && output.type != input.type)
        return Compatibility::DifferentType;
    if (output.list && !input.list && !input.takesList)
        return Compatibility::ListIntoSingle;
    return Compatibility::Compatible;
}

QString explain(Compatibility result, const PortSpec &output, const PortSpec &input) {
    switch (result) {
    case Compatibility::Compatible:
        return QStringLiteral("An output of type %1 can feed an input of type %2.")
            .arg(output.toString(), input.toString());
    case Compatibility::DifferentType:
        return QStringLiteral("An output of type %1 cannot feed an input of type %2; "
                              "types must match and nothing converts implicitly.")
            .arg(output.toString(), input.toString());
    case Compatibility::ListIntoSingle:
        return QStringLiteral("A list output (%1) cannot feed a single-value input (%2).")
            .arg(output.toString(), input.toString());
    }
    return {};
}

} // namespace fugue
