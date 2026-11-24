#include "cli/process.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>

#include "engine/cache_manager.h"
#include "engine/paths.h"

namespace fugue::cli {

namespace {

QString stampPath() {
    return stateDir() + QStringLiteral("/gc.stamp");
}

} // namespace

namespace {

bool sameInstance(const ProviderInstance &a, const ProviderInstance &b) {
    return a.name == b.name && a.type == b.type && a.address == b.address && a.key == b.key && a.enabled == b.enabled &&
           a.settings == b.settings;
}

} // namespace

Errors registerProviders(Registry &registry) {
    const Result<QList<ProviderInstance>> instances = loadProviderInstances(providersPath());
    if (!instances)
        return instances.errors();
    // An instance that has not changed keeps the provider it has: the window registers again at
    // each run's turn, and a new provider would end the helper the old one started, with the
    // models it holds (#382: Klein's DiT read again, 4 s, on every run).
    const QList<ProviderEntry> previous = registry.providers();
    Errors problems;
    QList<ProviderEntry> entries;
    for (const ProviderInstance &instance : instances.value()) {
        ProviderEntry entry;
        entry.instance = instance;
        const auto kept = std::find_if(previous.begin(), previous.end(), [&](const ProviderEntry &old) {
            return old.provider && sameInstance(old.instance, instance);
        });
        if (instance.enabled && kept != previous.end()) {
            entry.provider = kept->provider;
        } else if (instance.enabled) {
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

Process::Process(const Context &context) : Process(context, ResultCache::defaultRoot(), FileHashes::defaultPath()) {}

Process::Process(const Context &context, const QString &cacheRoot, const QString &hashesPath)
    : cache(cacheRoot), hashes(hashesPath) {
    cache.setLimit(CachePolicy::current().resultsBytes);
    environment.registry = &context.registry;
    environment.kernels = &context.kernels;
    environment.cache = &cache;
    environment.hashes = &hashes;
    // 8.10. Read only when a GPU node needs it, so a command that runs none sets up no Vulkan.
    environment.freeGpuMemoryMb = [this] {
        return gpu.freeMb().value_or(std::numeric_limits<qint64>::max()); // not measured: room enough
    };
    environment.gpuProcesses = [] { return fugue::gpuProcesses(); };
}

void collectGarbageIfDue(const Context &context, Process &process) {
    const QString stamp = stampPath();
    const QFileInfo info(stamp);
    if (info.exists() && info.lastModified().secsTo(QDateTime::currentDateTime()) < 3600)
        return;
    // The engine lock is held here (the run's own turn, or `fugue run`'s worker), so this is the
    // cleaning of 10.6 without taking it: the result store's limit and garbage, and the ended runs.
    CacheFolders folders;
    folders.results = process.cache.root();
    folders.runs = context.runsRoot;
    folders.queue = context.queueDir;
    CacheManager(folders).cleanupLocked(CachePolicy::current());
    QFile file(stamp);
    if (QDir().mkpath(info.path()) && file.open(QIODevice::WriteOnly))
        file.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
}

} // namespace fugue::cli
