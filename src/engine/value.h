#pragma once

// A value on a port while a graph runs (spec 2): what a node kernel takes as
// an input and gives back as an output, and what the result cache records.
//
//   - Json: text, number or boolean, a resource handle (2.5, an object with
//     "resource") or a data handle (2.6, an object with "data", "path" and
//     "hash").
//   - Media: a media handle (2.4).
//   - List: the value of a `list` port; its items are never lists.
//   - Empty: an optional output that came back empty (3.3), or an optional
//     input left unset.

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>

#include "engine/media.h"

namespace fugue {

class Value {
public:
    enum class Kind { Empty, Json, Media, List };

    Value() = default;
    Value(const QJsonValue &json);  // null and undefined give Empty
    Value(const QJsonObject &object) : Value(QJsonValue(object)) {}
    Value(const QString &text) : Value(QJsonValue(text)) {}
    Value(const char *text) : Value(QJsonValue(QString::fromUtf8(text))) {}
    Value(double number) : Value(QJsonValue(number)) {}
    Value(int number) : Value(QJsonValue(number)) {}
    Value(qint64 number) : Value(QJsonValue(number)) {}
    Value(bool flag) : Value(QJsonValue(flag)) {}
    Value(const MediaHandle &media);
    static Value list(const QList<Value> &items);

    Kind kind() const { return m_kind; }
    bool isEmpty() const { return m_kind == Kind::Empty; }
    bool isMedia() const { return m_kind == Kind::Media; }
    bool isList() const { return m_kind == Kind::List; }
    // A data handle (2.6): a Json object with "data".
    bool isData() const;

    QJsonValue json() const { return m_json; }        // Json
    MediaHandle media() const { return m_media; }      // Media
    QList<Value> items() const { return m_items; }     // List

    // Shorthands for Json values; the fallback when the value is another kind.
    QString text() const { return m_json.toString(); }
    double number() const { return m_json.toDouble(); }

    // The serialized form used in events and command-node messages: the JSON
    // value, the media handle's toJson(), an array for a list, null for Empty.
    QJsonValue toJson() const;

    // What the value contributes to a cache key (10.1): its content, never
    // where it lies. A media or data handle without its `path`; a resource
    // handle as resourceIdentity(); anything else as it is.
    QJsonValue identity() const;

private:
    Kind m_kind = Kind::Empty;
    QJsonValue m_json;
    MediaHandle m_media;
    QList<Value> m_items;
};

// A node's inputs or outputs by port name. An empty value may be left out.
using Values = QMap<QString, Value>;

// A resource handle's identity for caching (2.5): the hash of its canonical
// JSON without display-only fields. `needs` is always dropped, `name` when
// the handle has a `hash`; handles inside `derived` are treated the same way.
QString resourceIdentity(const QJsonObject &handle);

} // namespace fugue
