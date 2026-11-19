// A test-only `fugue`: the real command line, with fake node types and kernels registered
// through cli::Hooks. The run path, queue, lock and worker (which re-runs this same
// binary) are the production code; only what the nodes are and do is fake. It is built
// for tests and never installed.
//
// Node types: every node.json under the folder $FUGUE_NODE_MANIFESTS names, which the
// tests write. A missing folder is an empty registry.
//
// Kernels (the matching manifests are written by the tests):
//   fugue/note   -> text "hello"
//   fugue/echo   -> its `text` input as `text`
//   fugue/hold   -> waits until the file named by $FUGUE_TEST_GATE exists (forever when it
//                   is unset), or the run is cancelled; then text "released"
//   fugue/boom   -> fails with node_failed
//   fugue/gen    -> a provider node (capability image.generate), never reached by a run

#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cstdio>

#include "cli/args.h"
#include "cli/cli.h"

using namespace fugue;

namespace {

class Fn : public NodeKernel {
public:
    explicit Fn(std::function<Result<Values>(const NodeContext &)> function) : m_function(std::move(function)) {}
    Result<Values> run(const NodeContext &context) override { return m_function(context); }

private:
    std::function<Result<Values>(const NodeContext &)> m_function;
};

class Gen : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &) const override {
        Requirement requirement;
        requirement.capability = QStringLiteral("image.generate");
        return requirement;
    }
    Result<JobRequest> request(const NodeContext &, const Selection &) override {
        return makeError(QStringLiteral("internal"), QStringLiteral("not used"));
    }
    Result<Values> outputs(const NodeContext &, const Selection &, const JobResult &) override {
        return makeError(QStringLiteral("internal"), QStringLiteral("not used"));
    }
};

void addManifestFolder(Registry &registry, const QString &folder) {
    if (folder.isEmpty())
        return;
    QStringList files;
    QDirIterator it(folder, {"node.json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    files.sort();
    for (const QString &path : files) {
        QFile file(path);
        const QJsonDocument document = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
        Errors errors;
        if (document.isObject())
            errors = registry.addManifest(document.object());
        else
            errors << makeError("invalid_json", QStringLiteral("%1 is not a JSON object.").arg(path));
        for (const Error &error : errors)
            std::fprintf(stderr, "%s: %s: %s\n", qPrintable(path), qPrintable(error.code), qPrintable(error.message));
    }
}

} // namespace

int main(int argc, char **argv) {
    cli::Hooks hooks;
    hooks.addNodeTypes = [](Registry &registry) {
        addManifestFolder(registry, qEnvironmentVariable("FUGUE_NODE_MANIFESTS"));
    };
    hooks.addKernels = [](Kernels &kernels) {
        kernels.add("fugue/note", std::make_shared<Fn>([](const NodeContext &) -> Result<Values> {
                        return Values{{"text", "hello"}};
                    }));
        kernels.add("fugue/echo", std::make_shared<Fn>([](const NodeContext &context) -> Result<Values> {
                        return Values{{"text", context.input("text").text()}};
                    }));
        kernels.add("fugue/hold", std::make_shared<Fn>([](const NodeContext &context) -> Result<Values> {
                        const QString gate = qEnvironmentVariable("FUGUE_TEST_GATE");
                        for (int i = 0; i < 1500; ++i) { // 30 s at most, so a failed test leaves nothing behind
                            if (context.cancelled())
                                return makeError(QStringLiteral("cancelled"), QStringLiteral("Cancelled."));
                            if (!gate.isEmpty() && QFile::exists(gate))
                                return Values{{"text", "released"}};
                            QThread::msleep(20);
                        }
                        return makeError(QStringLiteral("node_timeout"), QStringLiteral("The gate never opened."));
                    }));
        kernels.add("fugue/boom", std::make_shared<Fn>([](const NodeContext &) -> Result<Values> {
                        return makeError(QStringLiteral("node_failed"), QStringLiteral("It always fails."));
                    }));
        kernels.add("fugue/gen", std::make_shared<Gen>());
    };
    if (!cli::wantsCli(argc, argv))
        return 2;
    return cli::run(argc, argv, hooks);
}
