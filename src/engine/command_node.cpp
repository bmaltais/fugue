#include "command_node.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <csignal>
#include <deque>
#include <map>
#include <optional>

#include <sys/types.h>
#include <unistd.h>

#include "engine/file_hashes.h"
#include "engine/registry.h"

namespace fugue {
namespace {

constexpr int kProtocol = 1;
constexpr int kTailLines = 20;

Error nodeError(const QString &code, const QString &message) { return makeError(code, message); }

struct Event {
    enum Kind { Line, Exited, StartFailed } kind = Line;
    QByteArray line;      // Line
    int exitCode = 0;     // Exited
    bool crashed = false; // Exited
    QString text;         // StartFailed
};

// One program and the thread that talks to it. QProcess needs an event loop and must stay on
// the thread that made it, but a node runs on whichever worker the scheduler picks, so the
// process lives on a thread of its own and the node's thread reads what it printed from a
// queue. Everything here may be called from any thread.
class Process {
public:
    Process(const QString &program, const QString &workDir, const QProcessEnvironment &environment,
            double idleSeconds, int graceMs)
        : m_grace(graceMs), m_idleMs(idleSeconds > 0 ? int(idleSeconds * 1000) : 0) {
        m_context = new QObject;
        m_context->moveToThread(&m_thread);
        m_thread.start();
        post([this, program, workDir, environment] { start(program, workDir, environment); });
    }

    ~Process() {
        QMetaObject::invokeMethod(
            m_context,
            [this] {
                signalGroup(SIGKILL);
                if (m_process) {
                    m_process->disconnect();
                    m_process->waitForFinished(3000);
                }
                delete m_process;
                m_process = nullptr;
                delete m_idle;
                m_idle = nullptr;
            },
            Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
        delete m_context;
    }

    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    void sendLine(const QJsonObject &message) {
        QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
        bytes.append('\n');
        post([this, bytes] {
            if (m_process && m_process->state() != QProcess::NotRunning)
                m_process->write(bytes);
        });
    }
    void closeInput() {
        post([this] {
            if (m_process)
                m_process->closeWriteChannel();
        });
    }
    void signal(int number) {
        post([this, number] { signalGroup(number); });
    }
    // A persistent process that is busy is never stopped for being idle; one that is free
    // is, after the idle timeout.
    void setBusy(bool busy) {
        post([this, busy] {
            if (!m_idle)
                return;
            if (busy || m_idleMs <= 0)
                m_idle->stop();
            else
                m_idle->start(m_idleMs);
        });
    }

    // The next thing the program did, waiting up to `ms`.
    std::optional<Event> next(int ms) {
        QMutexLocker lock(&m_mutex);
        if (m_events.empty())
            m_wake.wait(&m_mutex, QDeadlineTimer(ms));
        if (m_events.empty())
            return std::nullopt;
        Event event = std::move(m_events.front());
        m_events.pop_front();
        return event;
    }
    // True once the program has exited or never started.
    bool gone() const {
        QMutexLocker lock(&m_mutex);
        return m_gone;
    }
    // The last lines the program wrote to stderr.
    QStringList tail() const {
        QMutexLocker lock(&m_mutex);
        return m_tail;
    }

private:
    template <typename F> void post(F function) { QMetaObject::invokeMethod(m_context, function, Qt::QueuedConnection); }

    void push(Event event) {
        QMutexLocker lock(&m_mutex);
        if (event.kind != Event::Line)
            m_gone = true;
        m_events.push_back(std::move(event));
        m_wake.wakeAll();
    }

    // Runs on the process's thread.
    void start(const QString &program, const QString &workDir, const QProcessEnvironment &environment) {
        m_process = new QProcess(m_context);
        m_process->setWorkingDirectory(workDir);
        m_process->setProcessEnvironment(environment);
        // Its own process group: a cancel reaches what a shell script started too.
        m_process->setChildProcessModifier([] { ::setsid(); });
        QObject::connect(m_process, &QProcess::readyReadStandardOutput, m_context, [this] { readOutput(false); });
        QObject::connect(m_process, &QProcess::readyReadStandardError, m_context, [this] { readErrors(false); });
        QObject::connect(m_process, &QProcess::finished, m_context, [this](int code, QProcess::ExitStatus status) {
            readOutput(true);
            readErrors(true);
            Event event;
            event.kind = Event::Exited;
            event.exitCode = code;
            event.crashed = status == QProcess::CrashExit;
            push(std::move(event));
        });
        QObject::connect(m_process, &QProcess::errorOccurred, m_context, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            Event event;
            event.kind = Event::StartFailed;
            event.text = m_process->errorString();
            push(std::move(event));
        });
        if (m_idleMs > 0) {
            m_idle = new QTimer(m_context);
            m_idle->setSingleShot(true);
            QObject::connect(m_idle, &QTimer::timeout, m_context, [this] { stopWhenIdle(); });
        }
        m_process->start(program, {});
    }

    // Shutdown message, then SIGTERM, then SIGKILL, a grace period apart (7.3).
    void stopWhenIdle() {
        if (!m_process || m_process->state() != QProcess::Running)
            return;
        m_process->write("{\"op\":\"shutdown\"}\n");
        QTimer::singleShot(m_grace, m_context, [this] {
            if (!m_process || m_process->state() != QProcess::Running)
                return;
            signalGroup(SIGTERM);
            QTimer::singleShot(m_grace, m_context, [this] { signalGroup(SIGKILL); });
        });
    }

    void signalGroup(int number) {
        if (m_process && m_process->state() != QProcess::NotRunning && m_process->processId() > 0)
            ::kill(-pid_t(m_process->processId()), number);
    }

    void readOutput(bool last) {
        m_out += m_process->readAllStandardOutput();
        qsizetype end;
        while ((end = m_out.indexOf('\n')) >= 0) {
            Event event;
            event.line = m_out.left(end);
            m_out.remove(0, end + 1);
            push(std::move(event));
        }
        if (last && !m_out.trimmed().isEmpty()) {
            Event event;
            event.line = m_out;
            push(std::move(event));
        }
        if (last)
            m_out.clear();
    }

    void readErrors(bool last) {
        m_err += m_process->readAllStandardError();
        QStringList lines;
        qsizetype end;
        while ((end = m_err.indexOf('\n')) >= 0) {
            lines.append(QString::fromUtf8(m_err.left(end)));
            m_err.remove(0, end + 1);
        }
        if (last && !m_err.isEmpty()) {
            lines.append(QString::fromUtf8(m_err));
            m_err.clear();
        }
        QMutexLocker lock(&m_mutex);
        for (QString &line : lines) {
            if (line.trimmed().isEmpty())
                continue;
            if (line.size() > 500)
                line = line.left(500) + QStringLiteral("…");
            m_tail.append(line);
        }
        while (m_tail.size() > kTailLines)
            m_tail.removeFirst();
    }

    int m_grace;
    int m_idleMs;
    QThread m_thread;
    QObject *m_context = nullptr;
    // Used on the process's thread only.
    QProcess *m_process = nullptr;
    QTimer *m_idle = nullptr;
    QByteArray m_out;
    QByteArray m_err;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    std::deque<Event> m_events;
    QStringList m_tail;
    bool m_gone = false;
};

enum class Expect { Ready, Result };

QString withTail(const QString &message, const Process &process) {
    const QStringList tail = process.tail();
    return tail.isEmpty() ? message : message + QStringLiteral("\n") + tail.join(QLatin1Char('\n'));
}

// What the node reported with an `error` line. A node's own codes are its business; the engine
// reports them as node_failed, keeping the node's code in the message, except `cancelled`.
Error reportedError(const QJsonObject &line) {
    const QString code = line.value("code").toString();
    const QString message = line.value("message").toString();
    if (code == QLatin1String("cancelled"))
        return nodeError(QStringLiteral("cancelled"), message.isEmpty() ? QStringLiteral("The node was cancelled.") : message);
    QString text = message.isEmpty() ? QStringLiteral("The node reported an error.") : message;
    if (!code.isEmpty())
        text += QStringLiteral(" (%1)").arg(code);
    return nodeError(QStringLiteral("node_failed"), text);
}

// Reads the program's answers until the run ends: its result line (an empty object for
// Expect::Ready), or the error that ended it. Handles progress, cancel, the timeout and a
// program that dies or breaks the protocol.
Result<QJsonObject> await(Process &process, const NodeContext &context, const QString &id, bool persistent,
                          Expect expect, int timeoutMs, int graceMs) {
    enum class Why { None, Cancelled, Timeout, Protocol };
    Why why = Why::None;
    QString protocolMessage;
    QElapsedTimer clock;
    clock.start();
    qint64 stoppedAt = 0;
    bool killed = false;
    std::optional<QJsonObject> result;   // one-shot: held until the program exits
    std::optional<Error> reported;       // likewise

    auto stop = [&](Why reason) {
        if (why != Why::None)
            return;
        why = reason;
        stoppedAt = clock.elapsed();
        if (persistent && expect == Expect::Result && reason != Why::Protocol)
            process.sendLine({{"op", "cancel"}, {"id", id}});
        else
            process.signal(SIGTERM);
    };

    for (;;) {
        const std::optional<Event> event = process.next(25);
        if (event) {
            if (event->kind == Event::StartFailed)
                return nodeError(QStringLiteral("node_failed"),
                                 QStringLiteral("The node's program could not be started: %1.").arg(event->text));
            if (event->kind == Event::Exited) {
                if (why == Why::Cancelled)
                    return nodeError(QStringLiteral("cancelled"), QStringLiteral("The node was cancelled."));
                if (why == Why::Timeout)
                    return nodeError(QStringLiteral("node_timeout"),
                                     withTail(QStringLiteral("The node did not finish in time."), process));
                if (why == Why::Protocol)
                    return nodeError(QStringLiteral("node_failed"), withTail(protocolMessage, process));
                if (reported)
                    return *reported;
                if (result && event->exitCode == 0 && !event->crashed)
                    return *result;
                const QString how = event->crashed
                                        ? QStringLiteral("The node's program crashed.")
                                        : result ? QStringLiteral("The node's program exited with code %1 after its result.").arg(event->exitCode)
                                        : event->exitCode == 0
                                            ? QStringLiteral("The node's program exited without a result.")
                                            : QStringLiteral("The node's program exited with code %1.").arg(event->exitCode);
                return nodeError(QStringLiteral("node_failed"), withTail(how, process));
            }

            // A line of the program's stdout.
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(event->line, &parseError);
            if (!document.isObject()) {
                protocolMessage = QStringLiteral("The node's program wrote a line that is not a JSON object: %1")
                                      .arg(QString::fromUtf8(event->line.left(200)));
                stop(Why::Protocol);
            } else if (why != Why::Protocol) {
                const QJsonObject line = document.object();
                QString op = line.value("op").toString();
                if (op.isEmpty() && line.contains("outputs"))
                    op = QStringLiteral("result");
                const bool mine = !persistent || !line.contains("id") || line.value("id").toString() == id;
                if (op == QLatin1String("ready") && expect == Expect::Ready) {
                    return QJsonObject();
                } else if (op == QLatin1String("progress") && mine) {
                    if (context.progress)
                        context.progress(qBound(0.0, line.value("value").toDouble(), 1.0), QString(),
                                         line.value("message").toString());
                } else if (op == QLatin1String("result") && mine && expect == Expect::Result) {
                    if (persistent)
                        return line;
                    if (!result && !reported)
                        result = line;
                } else if (op == QLatin1String("error") && mine && expect == Expect::Result) {
                    if (persistent) {
                        Error error = reportedError(line);
                        // A run stopped for its timeout answers `cancelled` too.
                        if (why == Why::Timeout && error.code == QLatin1String("cancelled"))
                            return nodeError(QStringLiteral("node_timeout"), QStringLiteral("The node did not finish in time."));
                        return error;
                    }
                    if (!result && !reported)
                        reported = reportedError(line);
                }
                // Anything else (preview, an unknown op, a line for another run) is ignored.
            }
        }

        if (why == Why::None && context.cancelled && context.cancelled())
            stop(Why::Cancelled);
        if (why == Why::None && timeoutMs > 0 && clock.elapsed() > timeoutMs)
            stop(Why::Timeout);
        if (why != Why::None && !killed && clock.elapsed() - stoppedAt > graceMs) {
            process.signal(SIGKILL);
            killed = true;
        }
    }
}

// The node's outputs from the result line: a media or data output is a file the program wrote
// inside work_dir, named by {"path": …}; the scheduler checks the rest against the manifest.
Result<Values> outputsFrom(const QJsonObject &result, const NodeContext &context) {
    const NodeType &type = *context.type;
    const QJsonValue given = result.value("outputs");
    if (!given.isObject() && !given.isUndefined())
        return nodeError(QStringLiteral("node_bad_output"), QStringLiteral("The result's outputs must be an object."));
    const QJsonObject outputs = given.toObject();
    if (!context.hashes)
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("No file hashes to read the outputs with."));

    const QString workDir = QFileInfo(context.workDir).canonicalFilePath();
    auto fileIn = [&](const QJsonValue &value, const QString &output) -> Result<QString> {
        auto bad = [&](const QString &message) {
            Error error = nodeError(QStringLiteral("node_bad_output"), message);
            error.output = output;
            return error;
        };
        const QString path = value.isObject() ? value.toObject().value("path").toString() : QString();
        if (path.isEmpty())
            return bad(QStringLiteral("Output %1 must be {\"path\": …}, a file the node wrote in its work_dir.").arg(output));
        const QString file = QFileInfo(QDir(workDir).absoluteFilePath(path)).canonicalFilePath();
        if (file.isEmpty())
            return bad(QStringLiteral("Output %1 names a file that does not exist: %2.").arg(output, path));
        if (!file.startsWith(workDir + QLatin1Char('/')))
            return bad(QStringLiteral("Output %1 names a file outside the node's work_dir: %2.").arg(output, path));
        return file;
    };

    auto one = [&](const OutputSpec &spec, const QJsonValue &value) -> Result<Value> {
        const QString id = spec.type.type.id;
        std::optional<MediaType> media;
        if (id == QLatin1String("image"))
            media = MediaType::Image;
        else if (id == QLatin1String("video"))
            media = MediaType::Video;
        else if (id == QLatin1String("audio"))
            media = MediaType::Audio;
        const bool data = !media && context.registry && context.registry->portKind(spec.type.type) == PortKind::Data;
        if (!media && !data)
            return Value(value);
        const Result<QString> file = fileIn(value, spec.name);
        if (!file)
            return file.error();
        if (media) {
            const Result<MediaHandle> handle = MediaHandle::probe(*media, file.value(), *context.hashes);
            if (!handle) {
                Error error = handle.error();
                error.output = spec.name;
                return error;
            }
            return Value(handle.value());
        }
        QJsonObject handle{{"data", id}, {"path", file.value()}};
        const QJsonValue meta = value.toObject().value("meta");
        if (meta.isObject())
            handle.insert("meta", meta);
        return Value(handle);
    };

    Values values;
    for (const OutputSpec &spec : type.outputs) {
        const QJsonValue value = outputs.value(spec.name);
        if (value.isNull() || value.isUndefined())
            continue; // the scheduler reports a required output that is missing
        if (spec.type.list) {
            if (!value.isArray()) {
                Error error = nodeError(QStringLiteral("node_bad_output"),
                                        QStringLiteral("Output %1 is a list and must be an array.").arg(spec.name));
                error.output = spec.name;
                return error;
            }
            QList<Value> items;
            for (const QJsonValue &item : value.toArray()) {
                const Result<Value> made = one(spec, item);
                if (!made)
                    return made.error();
                items.append(made.value());
            }
            values.insert(spec.name, Value::list(items));
            continue;
        }
        const Result<Value> made = one(spec, value);
        if (!made)
            return made.error();
        values.insert(spec.name, made.value());
    }
    // An output the manifest does not declare is for the scheduler to refuse.
    for (auto it = outputs.begin(); it != outputs.end(); ++it)
        if (!type.output(it.key()) && !it.value().isNull())
            values.insert(it.key(), Value(it.value()));
    return values;
}

QJsonObject requestFor(const NodeContext &context, const QString &id) {
    QJsonObject inputs;
    for (auto it = context.inputs.begin(); it != context.inputs.end(); ++it)
        if (!it.value().isEmpty())
            inputs.insert(it.key(), it.value().toJson());
    return {{"protocol", kProtocol},
            {"op", "run"},
            {"id", id},
            {"node", QJsonObject{{"type", context.type->id}, {"version", context.type->version.toString()}}},
            {"inputs", inputs},
            {"work_dir", context.workDir}};
}

} // namespace

struct CommandKernel::Impl {
    explicit Impl(int graceMs) : grace(graceMs) {}
    int grace;

    // A persistent program per node type, and the lock that makes it one run at a time.
    struct Slot {
        QMutex run;
        std::unique_ptr<Process> process;
    };
    QMutex mutex;
    std::map<QString, std::shared_ptr<Slot>> packs;

    std::shared_ptr<Slot> slot(const QString &typeId) {
        QMutexLocker lock(&mutex);
        std::shared_ptr<Slot> &found = packs[typeId];
        if (!found)
            found = std::make_shared<Slot>();
        return found;
    }
};

CommandKernel::CommandKernel(int graceMs) : d(new Impl(graceMs)) {}

CommandKernel::~CommandKernel() { shutdown(); }

void CommandKernel::shutdown() {
    std::map<QString, std::shared_ptr<Impl::Slot>> packs;
    {
        QMutexLocker lock(&d->mutex);
        packs.swap(d->packs);
    }
    for (auto &entry : packs) {
        QMutexLocker run(&entry.second->run);
        entry.second->process.reset();
    }
}

Result<Values> CommandKernel::run(const NodeContext &context) {
    const NodeType &type = *context.type;
    const QJsonObject &runtime = type.runtime;

    // The program: `entry` inside the node's folder, after symlinks (6.5).
    const QString folder = QFileInfo(type.dir).canonicalFilePath();
    if (folder.isEmpty())
        return nodeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1 has no folder to run a program from.").arg(type.id));
    const QString entry = runtime.value("entry").toString();
    const QString program = QFileInfo(QDir(folder).absoluteFilePath(entry)).canonicalFilePath();
    if (entry.isEmpty() || program.isEmpty() || !program.startsWith(folder + QLatin1Char('/')))
        return nodeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1's entry must be a file inside the node's folder: %2.").arg(type.id, entry));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("FUGUE_PROTOCOL"), QStringLiteral("1"));
    environment.insert(QStringLiteral("FUGUE_NODE_DIR"), folder);
    environment.insert(QStringLiteral("FUGUE_WORK_DIR"), context.workDir);

    const bool persistent = runtime.value("mode").toString() == QLatin1String("persistent");
    const double timeout = runtime.value("timeout_s").toDouble(0);
    const int timeoutMs = timeout > 0 ? int(timeout * 1000) : 0;
    const QString id = QStringLiteral("%1:%2").arg(context.run, context.node);
    const QJsonObject request = requestFor(context, id);

    if (!persistent) {
        Process process(program, context.workDir, environment, 0, d->grace);
        process.sendLine(request);
        process.closeInput();
        const Result<QJsonObject> result = await(process, context, id, false, Expect::Result, timeoutMs, d->grace);
        if (!result)
            return result.error();
        return outputsFrom(result.value(), context);
    }

    const std::shared_ptr<Impl::Slot> slot = d->slot(type.id);
    QMutexLocker run(&slot->run);
    if (!slot->process || slot->process->gone()) {
        const double idle = runtime.value("idle_timeout_s").toDouble(300);
        slot->process = std::make_unique<Process>(program, context.workDir, environment, idle, d->grace);
        const Result<QJsonObject> ready = await(*slot->process, context, id, true, Expect::Ready, timeoutMs, d->grace);
        if (!ready) {
            slot->process.reset();
            return ready.error();
        }
    }
    Process &process = *slot->process;
    process.setBusy(true);
    process.sendLine(request);
    const Result<QJsonObject> result = await(process, context, id, true, Expect::Result, timeoutMs, d->grace);
    if (process.gone())
        slot->process.reset();
    else
        process.setBusy(false);
    if (!result)
        return result.error();
    return outputsFrom(result.value(), context);
}

} // namespace fugue
