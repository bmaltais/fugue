#pragma once

// Structured errors and the JSON envelope (spec 13.2, 13.3). Every engine part
// that can fail reports these, so the CLI prints them unchanged.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace fugue {

struct Error {
    enum class Severity { Error, Warning };

    QString code;            // a stable code from 13.3, e.g. "type_mismatch"
    Severity severity = Severity::Error;
    QString node;            // path id, when about a node
    QString input;           // port, when about an input
    QString output;          // port, when about an output
    QString message;         // one sentence a person can read
    QJsonArray valid;        // the valid options, in full, never truncated
    QString hint;            // how to fix it

    // Only the fields that are set; `valid` only when it is non-empty.
    QJsonObject toJson() const;
    static Error fromJson(const QJsonObject &object);
};

using Errors = QList<Error>;

// True when any entry has severity error (warnings alone are not a failure).
bool hasErrors(const Errors &errors);

// The list as it goes into an envelope, run.json or a run.done event.
QJsonArray toJson(const Errors &errors);

// {"ok": true, "data": data}
QJsonObject okEnvelope(const QJsonValue &data);
// {"ok": false, "errors": [...]}
QJsonObject errorEnvelope(const Errors &errors);

} // namespace fugue
