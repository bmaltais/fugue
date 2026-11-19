#include "error.h"

namespace fugue {

namespace {

void setIfAny(QJsonObject &object, const char *key, const QString &value) {
    if (!value.isEmpty())
        object.insert(QLatin1String(key), value);
}

} // namespace

QJsonObject Error::toJson() const {
    QJsonObject object{
        {"code", code},
        {"severity", severity == Severity::Warning ? "warning" : "error"},
        {"message", message},
    };
    setIfAny(object, "node", node);
    setIfAny(object, "input", input);
    setIfAny(object, "output", output);
    setIfAny(object, "hint", hint);
    if (!valid.isEmpty())
        object.insert("valid", valid);
    return object;
}

Error Error::fromJson(const QJsonObject &object) {
    Error error;
    error.code = object.value("code").toString();
    error.severity = object.value("severity").toString() == QLatin1String("warning")
                         ? Severity::Warning
                         : Severity::Error;
    error.node = object.value("node").toString();
    error.input = object.value("input").toString();
    error.output = object.value("output").toString();
    error.message = object.value("message").toString();
    error.valid = object.value("valid").toArray();
    error.hint = object.value("hint").toString();
    return error;
}

bool hasErrors(const Errors &errors) {
    for (const Error &error : errors)
        if (error.severity == Error::Severity::Error)
            return true;
    return false;
}

QJsonObject okEnvelope(const QJsonValue &data) {
    return QJsonObject{{"ok", true}, {"data", data}};
}

QJsonObject errorEnvelope(const Errors &errors) {
    QJsonArray list;
    for (const Error &error : errors)
        list.append(error.toJson());
    return QJsonObject{{"ok", false}, {"errors", list}};
}

} // namespace fugue
