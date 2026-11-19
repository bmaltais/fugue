#include "cli/process.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace fugue::cli {

namespace {

QString stampPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/fugue/gc.stamp");
}

} // namespace

Errors registerProviders(Registry &registry) {
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    if (!instances)
        return instances.errors();
    Errors problems;
    QList<ProviderEntry> entries;
    for (const ProviderInstance &instance : instances.value()) {
        ProviderEntry entry;
        entry.instance = instance;
        if (instance.enabled) {
            const Result<std::shared_ptr<Provider>> provider = createProvider(instance);
            if (provider) {
                entry.provider = provider.value();
            } else {
                for (Error problem : provider.errors()) {
                    problem.severity = Error::Severity::Warning;
                    problems << problem;
                }
            }
        }
        entries.append(entry);
    }
    const Errors refused = registry.setProviders(entries);
    return hasErrors(refused) ? refused : problems;
}

Process::Process(const Context &context)
    : cache(ResultCache::defaultRoot()), hashes(FileHashes::defaultPath()) {
    environment.registry = &context.registry;
    environment.kernels = &context.kernels;
    environment.cache = &cache;
    environment.hashes = &hashes;
}

void collectGarbageIfDue(const Context &context, Process &process) {
    const QString stamp = stampPath();
    const QFileInfo info(stamp);
    if (info.exists() && info.lastModified().secsTo(QDateTime::currentDateTime()) < 3600)
        return;
    process.cache.collectGarbage();
    removeEndedRunFolders(context.runsRoot, QDateTime::currentDateTimeUtc());
    QFile file(stamp);
    if (QDir().mkpath(info.path()) && file.open(QIODevice::WriteOnly))
        file.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
}

} // namespace fugue::cli
