#pragma once

// Provider modules (spec 8.8): a provider is a folder with a provider.json. This file reads
// the manifest and lists the modules; making a provider from one is createProvider (providers.h).
// The two providers compiled into Fugue (omarchy, and the helper's own launch) have a manifest
// too, with transport "builtin", so the registry lists shipped and third-party modules alike.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "engine/result.h"

namespace fugue {

struct ProviderModule {
    QString id;        // reverse-DNS like or a short word: letters, digits, . - _
    QString version;
    QString name;      // for people; defaults to the id
    // "http" or "stdio" (the protocol of 8.8 over that transport), or "builtin": an adapter that
    // is part of Fugue and speaks its server's own API (omarchy).
    QString transport;
    QString folder;    // where the manifest is; empty for a builtin one
    // start: how Fugue starts it. Empty command means the user runs it (http only).
    QStringList command;      // argument 0 is made absolute against `folder` when it has a slash
    QString when = QStringLiteral("on_demand"); // on_demand or never
    int idleTimeoutS = 600;   // 0 never stops it for idling
    int startTimeoutS = 30;   // how long an http module has to answer after it is started
    QString address;          // default address for http; {port} is a free loopback port
    QStringList capabilities; // what it may offer; the live list is describe's
    QJsonArray settings;      // [{name, type, secret?}] an instance can set
    QJsonObject needs;        // as in 6.3

    bool isBuiltin() const { return transport == QLatin1String("builtin"); }
    // The command's program with `folder` applied, and its arguments (with {port} not yet put in).
    QString program() const;
    QStringList arguments() const;
};

// The folder installed modules are looked for in: ~/.local/share/fugue/providers.
QString providerModulesDir();

// A manifest read from `folder` (its folder name is the id, 8.8). Errors are invalid_document
// and name the file.
Result<ProviderModule> parseProviderModule(const QJsonObject &manifest, const QString &folder);

// The modules that come with Fugue: omarchy and fugue-helper.
QList<ProviderModule> builtinProviderModules();

// Every folder of `root` with a provider.json that reads, in folder-name order. A folder that
// does not read is a warning in the result (so one bad module hides no other), and so is a
// module that reuses a builtin id.
Result<QList<ProviderModule>> loadProviderModules(const QString &root);

// builtinProviderModules() then those of `root`. Problems come along as warnings.
Result<QList<ProviderModule>> providerModules(const QString &root);
Result<QList<ProviderModule>> providerModules();

} // namespace fugue
