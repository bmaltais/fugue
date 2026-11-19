#include "value.h"

#include <QJsonArray>

#include "engine/canonical.h"

namespace fugue {

namespace {

// A resource handle without its display-only fields, all the way down.
QJsonValue withoutDisplayFields(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue &item : value.toArray())
            out.append(withoutDisplayFields(item));
        return out;
    }
    if (!value.isObject())
        return value;
    const QJsonObject object = value.toObject();
    const bool handle = object.contains("resource");
    QJsonObject out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (handle && it.key() == QLatin1String("needs"))
            continue;
        if (handle && it.key() == QLatin1String("name") && object.contains("hash"))
            continue;
        out.insert(it.key(), withoutDisplayFields(it.value()));
    }
    return out;
}

QJsonValue jsonIdentity(const QJsonValue &value) {
    if (!value.isObject())
        return value;
    QJsonObject object = value.toObject();
    if (object.contains("resource"))
        return QJsonObject{{"resource", resourceIdentity(object)}};
    if (object.contains("data"))
        object.remove("path");
    return object;
}

} // namespace

Value::Value(const QJsonValue &json) {
    if (json.isNull() || json.isUndefined())
        return;
    m_kind = Kind::Json;
    m_json = json;
}

Value::Value(const MediaHandle &media) : m_kind(Kind::Media), m_media(media) {}

Value Value::list(const QList<Value> &items) {
    Value value;
    value.m_kind = Kind::List;
    value.m_items = items;
    return value;
}

bool Value::isData() const {
    return m_kind == Kind::Json && m_json.isObject() && m_json.toObject().contains("data");
}

QJsonValue Value::toJson() const {
    switch (m_kind) {
    case Kind::Empty:
        return QJsonValue::Null;
    case Kind::Json:
        return m_json;
    case Kind::Media:
        return m_media.toJson();
    case Kind::List: {
        QJsonArray array;
        for (const Value &item : m_items)
            array.append(item.toJson());
        return array;
    }
    }
    return QJsonValue::Null;
}

QJsonValue Value::identity() const {
    switch (m_kind) {
    case Kind::Empty:
        return QJsonValue::Null;
    case Kind::Json:
        return jsonIdentity(m_json);
    case Kind::Media: {
        QJsonObject object = m_media.toJson();
        object.remove("path");
        return object;
    }
    case Kind::List: {
        QJsonArray array;
        for (const Value &item : m_items)
            array.append(item.identity());
        return array;
    }
    }
    return QJsonValue::Null;
}

QString resourceIdentity(const QJsonObject &handle) {
    return canonicalHash(withoutDisplayFields(handle));
}

} // namespace fugue
