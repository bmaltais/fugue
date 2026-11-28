#include "command_node.h"

#include <QDeadlineTimer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
#include <functional>
#include <mutex>
#include <map>
#include <optional>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <QCryptographicHash>

#include "engine/beneath_fs.h"
#include "engine/file_hashes.h"
#include "engine/model_library.h"
#include "engine/pack_secrets.h"
#include "engine/registry.h"
#include "engine/resident_models.h"
#include "engine/run_folder.h"
#include "engine/sandbox.h"
#include "engine/trust_store.h"

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
    // `descriptors`: what the command line names by number (bwrap's --ro-bind-fd), inherited by the program alone and closed
    // here once it has started.
    Process(const QString &program, const QStringList &arguments, const QString &workDir,
            const QProcessEnvironment &environment, double idleSeconds, int graceMs,
            std::shared_ptr<sandbox::Descriptors> descriptors = {})
        : m_grace(graceMs), m_idleMs(idleSeconds > 0 ? int(idleSeconds * 1000) : 0) {
        m_context = new QObject;
        m_context->moveToThread(&m_thread);
        m_thread.start();
        post([this, program, arguments, workDir, environment, descriptors = std::move(descriptors)]() mutable {
            start(program, arguments, workDir, environment, std::move(descriptors));
        });
    }

    ~Process() {
        QMetaObject::invokeMethod(
            m_context,
            [this] {
                if (m_pgid > 0)
                    ::kill(-m_pgid, SIGKILL);
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
        {
            QMutexLocker lock(&m_mutex);
            m_busy = busy;
        }
        post([this, busy] {
            if (!m_idle)
                return;
            if (busy || m_idleMs <= 0)
                m_idle->stop();
            else
                m_idle->start(m_idleMs);
        });
    }
    // Stops it now as the idle stop does, unless a run holds it (7.3: its memory is needed),
    // and returns once it is gone: shutdown, SIGTERM and SIGKILL take at most two grace periods.
    // At once when a run holds it.
    void stopUnlessBusyAndWait() {
        {
            QMutexLocker lock(&m_mutex);
            if (m_busy || m_gone)
                return;
        }
        post([this] { stopWhenIdle(); });
        QDeadlineTimer deadline(2 * m_grace + 1000);
        QMutexLocker lock(&m_mutex);
        while (!m_gone && !m_busy && !deadline.hasExpired())
            m_wake.wait(&m_mutex, deadline);
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
        return m_gone || m_retiring;
    }
    // Takes the process for a run: false when it is being stopped for idling (or is gone), so
    // the caller starts another. Once true, the idle stop leaves it alone until setBusy(false).
    bool acquire() {
        QMutexLocker lock(&m_mutex);
        if (m_gone || m_retiring)
            return false;
        m_busy = true;
        return true;
    }
    // Drops what the program printed while no run was waiting for it.
    void discardLines() {
        QMutexLocker lock(&m_mutex);
        std::deque<Event> kept;
        for (Event &event : m_events)
            if (event.kind != Event::Line)
                kept.push_back(std::move(event));
        m_events.swap(kept);
    }
    // A new run starts: the stderr tail of the one before is dropped (it may hold a value that has changed since), and
    // `secrets` (this run's values) join those masked in everything the program writes, before a line is cut short.
    void beginRun(const QStringList &secrets) {
        QMutexLocker lock(&m_mutex);
        m_tail.clear();
        for (const QString &secret : secrets)
            if (!m_secrets.contains(secret))
                m_secrets << secret;
    }
    QString mask(const QString &text) const {
        QMutexLocker lock(&m_mutex);
        return maskSecrets(text, m_secrets);
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
    void start(const QString &program, const QStringList &arguments, const QString &workDir, const QProcessEnvironment &environment,
               std::shared_ptr<sandbox::Descriptors> descriptors) {
        m_process = new QProcess(m_context);
        m_process->setWorkingDirectory(workDir);
        m_process->setProcessEnvironment(environment);
        // Its own process group: a cancel reaches what a shell script started too. The descriptors bwrap binds stay open across
        // exec in this child only (they are close-on-exec everywhere else).
        const QList<int> inherited = descriptors ? descriptors->numbers() : QList<int>();
        m_process->setChildProcessModifier([inherited] {
            ::setsid();
            beneath::inheritOnly(inherited);
        });
        QObject::connect(m_process, &QProcess::readyReadStandardOutput, m_context, [this] { readOutput(false); });
        QObject::connect(m_process, &QProcess::readyReadStandardError, m_context, [this] { readErrors(false); });
        QObject::connect(m_process, &QProcess::finished, m_context, [this](int code, QProcess::ExitStatus status) {
            readOutput(true);
            readErrors(true);
            // Whatever the program started and left running goes with it.
            if (m_pgid > 0)
                ::kill(-m_pgid, SIGKILL);
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
        m_process->start(program, arguments);
        m_pgid = pid_t(m_process->processId()); // setsid: the process id is the group id
        descriptors.reset(); // the child has its copies (or never started): ours close
    }

    // Shutdown message, then SIGTERM, then SIGKILL, a grace period apart (7.3).
    void stopWhenIdle() {
        if (!m_process || m_process->state() != QProcess::Running)
            return;
        {
            QMutexLocker lock(&m_mutex);
            if (m_busy)
                return;
            m_retiring = true;
        }
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
            for (QChar &c : line) // no escape sequence of the program's reaches a terminal or an error message
                if ((c.unicode() < 0x20 && c != QLatin1Char('\t')) || (c.unicode() >= 0x7f && c.unicode() < 0xa0))
                    c = QLatin1Char(' ');
            line = maskSecrets(line, m_secrets); // before it is cut short: a value cut in two is no longer found
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
    pid_t m_pgid = 0;
    QByteArray m_out;
    QByteArray m_err;

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    std::deque<Event> m_events;
    QStringList m_tail;
    QStringList m_secrets; // the values to mask, guarded by m_mutex
    bool m_gone = false;
    bool m_busy = true;     // a run owns it (or it is starting)
    bool m_retiring = false; // told to shut down for idling
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

// The file a line names with {"path": …}: one the program wrote inside `workDir` (canonical; a
// relative path resolves against it), after symlinks. Else an error with `code` whose message
// starts with `what` ("Output image", "A preview's image"). A work_dir that is gone (empty)
// holds nothing.
Result<QString> fileInWorkDir(const QJsonValue &value, const QString &workDir, const QString &code,
                              const QString &what) {
    if (workDir.isEmpty())
        return nodeError(code, QStringLiteral("%1 cannot be read: the node's work_dir is gone.").arg(what));
    const QString path = value.isObject() ? value.toObject().value("path").toString() : QString();
    if (path.isEmpty())
        return nodeError(code,
                         QStringLiteral("%1 must be {\"path\": …}, a file the node wrote in its work_dir.").arg(what));
    const QString file = QFileInfo(QDir(workDir).absoluteFilePath(path)).canonicalFilePath();
    if (file.isEmpty())
        return nodeError(code, QStringLiteral("%1 names a file that does not exist: %2.").arg(what, path));
    if (!file.startsWith(workDir + QLatin1Char('/')))
        return nodeError(code,
                         QStringLiteral("%1 names a file outside the node's work_dir: %2.").arg(what, path));
    return file;
}

// A `preview` line (7.2): its image is shown (NodeContext::preview) when it is a file inside
// work_dir. A preview is only a picture, so a bad one is left out and the run goes on; the first
// bad one of a run becomes a warning in `warnings`, so the node's author hears of it.
// `fetch` (a sandboxed program that lives on): the preview's file copied out of the exchange folder into the run's work folder,
// before anything looks at it; empty when it may not be.
using Fetch = std::function<std::optional<QString>(const QString &)>;

void showPreview(const QJsonObject &line, const NodeContext &context, Errors *warnings, const Fetch &fetch = {}) {
    const QString workDir = QFileInfo(context.workDir).canonicalFilePath();
    QJsonValue image = line.value("image");
    if (fetch && image.isObject()) {
        const std::optional<QString> copied = fetch(image.toObject().value("path").toString());
        QJsonObject fetched = image.toObject();
        fetched.insert("path", copied ? *copied : QString());
        image = fetched;
    }
    Result<QString> file = fileInWorkDir(image, workDir, QStringLiteral("node_bad_preview"), QStringLiteral("A preview's image"));
    if (file) {
        if (context.preview)
            context.preview(file.value());
        return;
    }
    if (warnings && warnings->isEmpty()) {
        Error warning = file.error();
        warning.severity = Error::Severity::Warning;
        warning.message += QStringLiteral(" It was not shown.");
        warnings->append(warning);
    }
}

// Reads the program's answers until the run ends: its result line (an empty object for
// Expect::Ready), or the error that ended it. Handles progress, previews (a bad one goes into
// `warnings`), cancel, the timeout and a program that dies or breaks the protocol.
Result<QJsonObject> await(Process &process, const NodeContext &context, const QString &id, bool persistent,
                          Expect expect, int timeoutMs, int graceMs, Errors *warnings = nullptr,
                          bool *heard = nullptr, const Fetch &fetch = {}) {
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
            if (heard)
                *heard = true;
            if (event->line.trimmed().isEmpty())
                continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(event->line, &parseError);
            if (!document.isObject()) {
                protocolMessage = QStringLiteral("The node's program wrote a line that is not a JSON object: %1")
                                      .arg(process.mask(QString::fromUtf8(event->line)).left(200));
                stop(Why::Protocol);
            } else if (why != Why::Protocol) {
                const QJsonObject line = document.object();
                QString op = line.value("op").toString();
                if (op.isEmpty() && line.contains("outputs"))
                    op = QStringLiteral("result");
                const bool mine = !persistent || line.value("id").toString() == id;
                if (op == QLatin1String("ready") && expect == Expect::Ready) {
                    return QJsonObject();
                } else if (op == QLatin1String("progress") && mine) {
                    if (context.progress)
                        context.progress(qBound(0.0, line.value("value").toDouble(), 1.0), QString(),
                                         line.value("message").toString());
                } else if (op == QLatin1String("preview") && mine && expect == Expect::Result) {
                    // Once a one-shot program has given its answer, what it shows no longer counts.
                    if (!result && !reported)
                        showPreview(line, context, warnings, fetch);
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
                // Anything else (an unknown op, a line for another run) is ignored.
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
    if (workDir.isEmpty())
        return nodeError(QStringLiteral("node_bad_output"), QStringLiteral("The node's work_dir is gone."));
    auto fileIn = [&](const QJsonValue &value, const QString &output) -> Result<QString> {
        Result<QString> file = fileInWorkDir(value, workDir, QStringLiteral("node_bad_output"),
                                             QStringLiteral("Output %1").arg(output));
        if (!file) {
            Error error = file.error();
            error.output = output;
            return error;
        }
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
                Error error = nodeError(QStringLiteral("node_bad_output"),
                                        QStringLiteral("Output %1 is not a readable %2: %3")
                                            .arg(spec.name, id, handle.error().message));
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

// The outputs with the run's warnings (a bad preview) beside their own; a failure stays one.
Result<Values> withWarnings(Result<Values> outputs, const Errors &warnings) {
    if (!outputs || warnings.isEmpty())
        return outputs;
    return Result<Values>(outputs.value(), warnings + outputs.warnings());
}

// An input as the program gets it: a media handle with nothing pending, so `path` is the whole
// value (7.2).
Result<QJsonValue> plainJson(const QString &name, const Value &value, const NodeContext &context) {
    if (value.isMedia()) {
        const Result<MediaHandle> plain = value.media().plainFile(context.workDir, name, *context.hashes);
        if (!plain)
            return plain.error();
        return QJsonValue(plain->toJson());
    }
    if (value.isList()) {
        QJsonArray items;
        for (const Value &item : value.items()) {
            const Result<QJsonValue> one = plainJson(name, item, context);
            if (!one)
                return one;
            items.append(one.value());
        }
        return QJsonValue(items);
    }
    return value.toJson();
}

// The values of the secrets the node's pack declares and has stored (6.4, #402), for the `secrets` of the
// request. Only the names pack.json declares are read, so a pack never gets another's or an undeclared one.
Result<QMap<QString, StoredSecret>> secretsFor(const NodeType &type) {
    if (type.packSecrets.isEmpty())
        return QMap<QString, StoredSecret>{};
    // Stored under the pack's id and the source it came from: a pack from another source gets none.
    Result<QMap<QString, StoredSecret>> stored =
        PackSecretStore().values(packSecretKey(TrustStore::packOf(type.id), type.packSource), type.packSecrets);
    if (!stored)
        return stored;
    // A value that needs an origin and was stored without one is not handed over (it could be sent anywhere).
    for (const QString &name : type.packSecretsOriginRequired)
        if (stored->contains(name) && stored->value(name).origin.isEmpty())
            stored->remove(name);
    return stored;
}

QJsonValue maskedJson(const QJsonValue &value, const QStringList &secrets) {
    if (value.isString())
        return maskSecrets(value.toString(), secrets);
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray())
            items.append(maskedJson(item, secrets));
        return items;
    }
    if (value.isObject()) {
        QJsonObject object;
        const QJsonObject source = value.toObject();
        for (auto it = source.begin(); it != source.end(); ++it)
            object.insert(maskSecrets(it.key(), secrets), maskedJson(it.value(), secrets));
        return object;
    }
    return value;
}

// A value with every text in it (a list's items, a JSON object's or array's strings, a data handle's meta) masked.
Value maskedValue(const Value &value, const QStringList &secrets) {
    if (value.kind() == Value::Kind::Json)
        return Value(maskedJson(value.json(), secrets));
    if (value.isList()) {
        QList<Value> items;
        for (const Value &item : value.items())
            items.append(maskedValue(item, secrets));
        return Value::list(items);
    }
    return value;
}

// An error with the secret values taken out of what it says: a program may echo a key it was sent (a
// server's answer, a log line), and an error goes to run.json, events and the screen.
Error maskedError(Error error, const QStringList &secrets) {
    error.message = maskSecrets(error.message, secrets);
    error.hint = maskSecrets(error.hint, secrets);
    return error;
}

Result<Values> maskedOutputs(Result<Values> result, const QStringList &secrets) {
    if (secrets.isEmpty())
        return result;
    if (!result) {
        Errors errors;
        for (const Error &error : result.errors())
            errors << maskedError(error, secrets);
        return errors;
    }
    Values values = result.value();
    for (auto it = values.begin(); it != values.end(); ++it)
        it.value() = maskedValue(it.value(), secrets);
    Errors warnings;
    for (const Error &warning : result.warnings())
        warnings << maskedError(warning, secrets);
    return Result<Values>(values, warnings);
}

Result<QJsonObject> requestFor(const NodeContext &context, const QString &id) {
    std::optional<ModelLibrary> library; // read from the settings for the first resource input
    QJsonObject inputs;
    for (auto it = context.inputs.begin(); it != context.inputs.end(); ++it) {
        if (it.value().isEmpty())
            continue;
        const InputSpec *spec = context.type->input(it.key());
        if (spec && context.registry && context.registry->portKind(spec->type.type) == PortKind::Resource) {
            // Each handle with its file's path (7.2); model_missing on the input when a file is not here.
            if (context.cancelled && context.cancelled())
                return nodeError(QStringLiteral("cancelled"), QStringLiteral("The node was cancelled."));
            if (!library)
                library.emplace(ModelLibrary::current(context.registry));
            const Result<QJsonValue> withPaths = library->withPaths(it.value().toJson(), *context.hashes);
            if (!withPaths) {
                Error error = withPaths.error();
                error.input = it.key();
                return error;
            }
            inputs.insert(it.key(), withPaths.value());
            continue;
        }
        const Result<QJsonValue> plain = plainJson(it.key(), it.value(), context);
        if (!plain)
            return plain.error();
        inputs.insert(it.key(), plain.value());
    }
    return QJsonObject{{"protocol", kProtocol},
            {"op", "run"},
            {"id", id},
            {"node", QJsonObject{{"type", context.type->id}, {"version", context.type->version.toString()}}},
            {"inputs", inputs},
            {"work_dir", context.workDir}};
}

// ---- The sandbox (6.7.1): what a pack's program is started with. ----

struct Launch {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
    QStringList mounted; // the model files its sandbox has bound: a program that lives on cannot see others
    std::shared_ptr<sandbox::Descriptors> descriptors; // what its command line binds by number, moved into the Process
};

bool inFolder(const QString &path, const QString &folder) {
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

bool covered(const QStringList &mounted, const QString &path) {
    for (const QString &folder : mounted)
        if (inFolder(path, folder))
            return true;
    return false;
}

// A node of an installed pack (the built-in packs are Fugue itself).
bool isPackNode(const NodeType &type) {
    return !type.packVersion.isEmpty() && !TrustStore::isBuiltIn(TrustStore::packOf(type.id));
}

// The pack's folder: the node's folder's parent when that holds pack.json, else the node's folder alone.
QString packFolderOf(const QString &nodeFolder) {
    const QString parent = QFileInfo(nodeFolder + QStringLiteral("/..")).canonicalFilePath();
    return !parent.isEmpty() && QFileInfo::exists(parent + QStringLiteral("/pack.json")) ? parent : nodeFolder;
}

// `value` with each top-level {"path": …} of a handle (or of each handle of a list) replaced by change(path).
QJsonValue mapHandlePaths(const QJsonValue &value, const std::function<QString(const QString &)> &change) {
    if (value.isArray()) {
        QJsonArray items;
        for (const QJsonValue &item : value.toArray())
            items.append(mapHandlePaths(item, change));
        return items;
    }
    if (!value.isObject() || !value.toObject().value("path").isString())
        return value;
    QJsonObject object = value.toObject();
    object.insert("path", change(object.value("path").toString()));
    return object;
}

void addResolvedModelPaths(const QJsonValue &value, QStringList &paths) {
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            addResolvedModelPaths(item, paths);
        return;
    }
    const QJsonObject handle = value.toObject();
    if (!handle.contains("derived")) {
        if (const QString path = handle.value("path").toString(); !path.isEmpty() && !paths.contains(path))
            paths << path;
        return;
    }
    const QJsonObject derived = handle.value("derived").toObject();
    addResolvedModelPaths(derived.value("base"), paths);
    for (auto it = derived.begin(); it != derived.end(); ++it)
        if (it.key() != QLatin1String("base") && it.key() != QLatin1String("apply") && it.value().isObject() &&
            it.value().toObject().contains("resource"))
            addResolvedModelPaths(it.value(), paths);
    for (const QJsonValue &step : derived.value("apply").toArray())
        addResolvedModelPaths(step.toObject().value("lora"), paths);
}

// A folder that a program kept between runs reads and writes in, outside the runs folder: each request gets a subfolder, and
// Fugue copies the inputs in and the outputs out, so the program never writes where another run's records are (#731). The
// program can change anything in it at any moment, so Fugue touches it through descriptors only (engine/beneath_fs.h).
struct Exchange {
    static std::shared_ptr<Exchange> make(QString *why) {
        auto made = std::make_shared<Exchange>();
        const QString base = cacheDir() + QStringLiteral("/exchange");
        QDir().mkpath(base); // Fugue's own cache folder: no program can reach it
        made->base = beneath::Fd(::open(QFile::encodeName(base).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!made->base) {
            *why = QStringLiteral("The exchange folder %1 cannot be opened.").arg(base);
            return {};
        }
        removeStale(made->base.get());
        made->name = beneath::mkdirBeneathRandom(made->base.get());
        if (!made->name.isEmpty())
            made->root = beneath::openBeneath(made->base.get(), made->name, O_RDONLY | O_DIRECTORY);
        made->path = beneath::realPathOf(made->root.get());
        if (!made->root || made->path.isEmpty()) {
            *why = QStringLiteral("An exchange folder cannot be made in %1 (%2).").arg(base, beneath::lastProblem());
            return {};
        }
        return made;
    }
    ~Exchange() {
        if (base && !name.isEmpty())
            beneath::removeTreeBeneath(base.get(), name);
    }
    beneath::Fd base; // <cache>/exchange
    beneath::Fd root; // this program's folder in it, bound into its sandbox at `path`
    QString name;
    QString path;

private:
    // What a crashed Fugue left: folders older than a day.
    static void removeStale(int base) {
        DIR *listing = ::fdopendir(beneath::dupCloexec(base));
        if (!listing)
            return;
        QStringList old;
        while (const dirent *entry = ::readdir(listing)) {
            struct stat info {};
            const QByteArray name(entry->d_name);
            if (name != "." && name != ".." && ::fstatat(base, name.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
                QDateTime::currentSecsSinceEpoch() - qint64(info.st_mtime) > 86400)
                old << QFile::decodeName(name);
        }
        ::closedir(listing);
        for (const QString &name : std::as_const(old))
            beneath::removeTreeBeneath(base, name);
    }
};

// The model files a node is handed: exactly the paths withPaths resolved for the handles of its resource inputs. Each must lie
// in a model folder (or be a link into one); sandbox::build checks that again on what it opens. Else an error.
Result<QStringList> modelFilesOf(const NodeContext &context, const QJsonObject &inputs) {
    QStringList roots;
    for (const QString &root : ModelLibrary::current(context.registry).roots())
        if (const QString real = QFileInfo(root).canonicalFilePath(); !real.isEmpty())
            roots << real;
    QStringList files;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const InputSpec *spec = context.type->input(it.key());
        if (!spec || !context.registry || context.registry->portKind(spec->type.type) != PortKind::Resource)
            continue;
        for (const QString &path : resolvedModelPaths(it.value())) {
            const QString real = QFileInfo(path).canonicalFilePath();
            if (real.isEmpty() || !covered(roots, real))
                return nodeError(QStringLiteral("node_failed"),
                                 QStringLiteral("The model file %1 of %2 is not in a model folder, so it is not handed to a sandboxed program.").arg(path, it.key()));
            if (!files.contains(path))
                files << path;
        }
    }
    return files;
}

// The other files a node is handed (pictures, videos, what an earlier node made), by the top-level path of each handle.
QStringList mediaFilesOf(const NodeContext &context, const QJsonObject &inputs) {
    QStringList files;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const InputSpec *spec = context.type->input(it.key());
        if (spec && context.registry && context.registry->portKind(spec->type.type) == PortKind::Resource)
            continue;
        mapHandlePaths(it.value(), [&](const QString &path) {
            if (!files.contains(path))
                files << path;
            return path;
        });
    }
    return files;
}

// One request of a program that lives on, in its exchange folder: `<exchange>/<128 random bits>/` with `in/` (the media
// inputs, copied in) and `work/` (its work_dir), each held open.
struct Staged {
    QString name;
    beneath::Fd folder;
    beneath::Fd work;
    QString workPath; // `work` as the program sees it (the exchange folder is bound at its own path)
};

// A file the program wrote in its request's work folder (`path`, relative to it or absolute inside it), copied into the run's
// own work folder through descriptors; the new path. Nothing when it is not a regular file beneath the work folder (a link, a
// folder, a FIFO, a path out of it): nothing of the exchange folder is then looked at by path.
std::optional<QString> copyOut(const QString &path, const Staged &staged, int workFd, const QString &workDir, int &counter) {
    QString relative = QDir::cleanPath(path);
    if (relative.startsWith(QLatin1Char('/'))) {
        if (!relative.startsWith(staged.workPath + QLatin1Char('/')))
            return std::nullopt;
        relative = relative.mid(staged.workPath.size() + 1);
    }
    QString name = QFileInfo(relative).fileName();
    if (path.isEmpty() || relative == QLatin1String(".") || relative.startsWith(QLatin1String("../")) || name.isEmpty() || name == QLatin1String(".."))
        return std::nullopt;
    struct stat info {};
    if (::fstatat(workFd, QFile::encodeName(name).constData(), &info, AT_SYMLINK_NOFOLLOW) == 0)
        name = QStringLiteral("%1-%2").arg(++counter).arg(name);
    if (!beneath::copyOutBeneath(staged.work.get(), relative, workFd, name))
        return std::nullopt;
    return workDir + QLatin1Char('/') + name;
}

// A request for a sandboxed program that lives on: a new request folder in its exchange folder, the media files copied in (a
// model file is bound where it is), and the request's paths and work_dir changed to them. Each media file is opened without
// links and must not be a file that is never handed over.
Result<Staged> stageRequest(const NodeContext &context, const Exchange &exchange, QJsonObject &request, const QString &home = {}) {
    Staged staged;
    staged.name = beneath::mkdirBeneathRandom(exchange.root.get());
    if (!staged.name.isEmpty())
        staged.folder = beneath::openBeneath(exchange.root.get(), staged.name, O_RDONLY | O_DIRECTORY);
    if (staged.folder && beneath::mkdirBeneath(staged.folder.get(), QStringLiteral("work")) && beneath::mkdirBeneath(staged.folder.get(), QStringLiteral("in")))
        staged.work = beneath::openBeneath(staged.folder.get(), QStringLiteral("work"), O_RDONLY | O_DIRECTORY);
    if (!staged.work)
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("The request's folder cannot be made in the exchange folder (%1).").arg(beneath::lastProblem()));
    const QString folder = exchange.path + QLatin1Char('/') + staged.name;
    staged.workPath = folder + QStringLiteral("/work");
    QJsonObject inputs = request.value("inputs").toObject();
    int copies = 0;
    QString problem;
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const InputSpec *spec = context.type->input(it.key());
        if (spec && context.registry && context.registry->portKind(spec->type.type) == PortKind::Resource)
            continue;
        it.value() = mapHandlePaths(it.value(), [&](const QString &path) {
            const QString real = QFileInfo(path).canonicalFilePath();
            const QString to = QStringLiteral("in/%1-%2").arg(++copies).arg(QFileInfo(QDir::cleanPath(path)).fileName());
            const beneath::Fd from = real.isEmpty() || sandbox::handedDenied(real, home) ? beneath::Fd() : beneath::openRegularFile(real);
            if (from && beneath::realPathOf(from.get()) == real && beneath::copyInBeneath(from.get(), staged.folder.get(), to))
                return folder + QLatin1Char('/') + to;
            if (problem.isEmpty())
                problem = path;
            return path;
        });
    }
    if (!problem.isEmpty())
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("The file %1 cannot be handed to the sandboxed program.").arg(problem));
    request.insert("inputs", inputs);
    request.insert("work_dir", staged.workPath);
    return staged;
}

// The program started inside bubblewrap: the node's entry, its pack, its work folder (a one-shot's own, or the exchange folder of a
// program that lives on), the model files in `models` and the media files in `media`.
Result<Launch> sandboxedLaunch(const NodeContext &context, const QString &folder, const QString &program, const QString &workDir,
                               const Exchange *exchange, const QStringList &models, const QStringList &media) {
    const NodeType &type = *context.type;
    const Result<sandbox::Needs> needs = sandbox::parseNeeds(type.needs);
    if (!needs)
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("%1: %2").arg(type.id, needs.error().message));
    sandbox::Profile profile;
    profile.packDir = packFolderOf(folder);
    if (exchange) {
        profile.exchangeFd = exchange->root.get();
        profile.exchangeDir = exchange->path;
    } else {
        profile.workDir = workDir;
    }
    profile.needs = *needs;
    profile.modelFiles = models;
    profile.modelRoots = ModelLibrary::current(context.registry).roots();
    profile.handedFiles = media;
    profile.program = program;
    profile.environment = {QStringLiteral("FUGUE_PROTOCOL=1"), QStringLiteral("FUGUE_NODE_DIR=") + folder, QStringLiteral("FUGUE_WORK_DIR=") + workDir};
    sandbox::Command command = sandbox::build(profile);
    if (!command.refused.isEmpty())
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("%1 was not run: %2").arg(type.id, command.refused));
    Launch launch;
    launch.program = command.program;
    launch.arguments = command.arguments;
    launch.environment = sandbox::bwrapEnvironment(); // none of Fugue's variables in bwrap's own process inside the sandbox
    launch.descriptors = std::move(command.descriptors);
    for (const QString &file : models)
        launch.mounted << QDir::cleanPath(file);
    return launch;
}

} // namespace

QStringList resolvedModelPaths(const QJsonValue &withPaths) {
    QStringList paths;
    addResolvedModelPaths(withPaths, paths);
    return paths;
}

QString nodeSeal(const QString &dir, const QString &entry, const QByteArray &packJson) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &name : {QStringLiteral("node.json"), entry}) {
        // A file that does not exist has no canonical path; QFile would warn about an empty name.
        const QString path = name.isEmpty() ? QString() : QFileInfo(QDir(dir).absoluteFilePath(name)).canonicalFilePath();
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
            return {};
        hash.addData(file.readAll());
        hash.addData(QByteArrayView("\0", 1));
    }
    // The pack's own pack.json (it declares the secrets the runner hands over, #497) and `.fugue-source.json` (where it
    // came from, which decides the key its secrets are stored under), beside the node's folder. A node outside a pack
    // folder has neither, and then only its own files count. The loader passes the bytes of pack.json it parsed, so what
    // is sealed is what was read, not what is on disk a moment later.
    const QString packFolder = QFileInfo(dir).canonicalFilePath() + QStringLiteral("/..");
    for (const QString &name : {QStringLiteral("pack.json"), QStringLiteral(".fugue-source.json")}) {
        QByteArray bytes;
        if (name == QLatin1String("pack.json") && !packJson.isEmpty()) {
            bytes = packJson;
        } else {
            QFile file(QFileInfo(packFolder + QLatin1Char('/') + name).canonicalFilePath());
            if (file.fileName().isEmpty() || !file.open(QIODevice::ReadOnly))
                continue;
            bytes = file.readAll();
        }
        hash.addData(name.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(bytes);
        hash.addData(QByteArrayView("\0", 1));
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString packSecretsKeyPart(const NodeType &type) {
    if (type.packSecrets.isEmpty())
        return {};
    const Result<QMap<QString, StoredSecret>> stored = secretsFor(type);
    if (!stored)
        return QString::fromLatin1(kSecretsUnreadable); // the run fails on it; the scheduler never caches under this key
    if (stored->isEmpty())
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (auto it = stored->cbegin(); it != stored->cend(); ++it) { // a QMap: sorted by name
        for (const QString &part : {it.key(), it.value().value, it.value().origin}) {
            hash.addData(part.toUtf8());
            hash.addData(QByteArrayView("\0", 1));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

struct CommandKernel::Impl {
    explicit Impl(int graceMs) : grace(graceMs) {}
    int grace;
    bool alwaysSandboxed = false;

    // A persistent program per node type, and the lock that makes it one run at a time.
    struct Slot {
        QMutex run;   // one run at a time
        QMutex guard; // `process`, for shutdown() to reach it during a run
        std::unique_ptr<Process> process;
        QString seal; // NodeType::seal of the files `process` was started from, and whether it runs in the sandbox
        QStringList models;  // the model files its sandbox has bound
        QStringList mounted; // what its sandbox can see (Launch::mounted)
        std::shared_ptr<Exchange> exchange; // the folder it works in (sandboxed only)
        // While the program is a resident model: the payload ResidentModels holds, whose drop
        // stops the program and waits for it, so the memory is back when the drop returns. Weak,
        // so that only ResidentModels keeps it.
        std::weak_ptr<void> resident;
    };
    QMutex mutex;
    bool closed = false;
    std::map<QString, std::shared_ptr<Slot>> packs;

    std::shared_ptr<Slot> slot(const QString &typeId) {
        QMutexLocker lock(&mutex);
        std::shared_ptr<Slot> &found = packs[typeId];
        if (!found)
            found = std::make_shared<Slot>();
        return found;
    }

    // 7.3, 8.10, 9.3: a persistent program that uses the GPU keeps its memory between runs, so
    // it is a resident model: the scheduler drops the least recently used one when memory is
    // needed, and dropping it stops the program. One that a run holds is not stopped; the next
    // run makes it resident again.
    static void keepResident(const std::shared_ptr<Slot> &slot, ResidentModels &models, const QString &typeId) {
        std::shared_ptr<void> payload;
        {
            QMutexLocker guard(&slot->guard);
            // expired(), not a null test: the payload points at nothing, only its drop matters.
            payload = slot->resident.lock();
            if (slot->resident.expired()) {
                const std::weak_ptr<Slot> weak = slot;
                payload = std::shared_ptr<void>(nullptr, [weak](void *) {
                    if (const std::shared_ptr<Slot> held = weak.lock()) {
                        QMutexLocker guard(&held->guard);
                        if (held->process)
                            held->process->stopUnlessBusyAndWait();
                    }
                });
                slot->resident = payload;
            }
        }
        models.put(residentProgramKey(typeId), payload);
    }
};

QString residentProgramKey(const NodeType &type) {
    const QJsonObject &runtime = type.runtime;
    if (runtime.value("kind").toString() != QLatin1String("command") ||
        runtime.value("mode").toString() != QLatin1String("persistent") || !type.needs.value("gpu").isObject())
        return {};
    return residentProgramKey(type.id);
}

QString residentProgramKey(const QString &typeId) { return QStringLiteral("program:") + typeId; }

CommandKernel::CommandKernel(int graceMs) : d(new Impl(graceMs)) {}

CommandKernel::~CommandKernel() { shutdown(); }

void CommandKernel::setAlwaysSandboxed(bool on) { d->alwaysSandboxed = on; }

void CommandKernel::shutdown() {
    std::map<QString, std::shared_ptr<Impl::Slot>> packs;
    {
        QMutexLocker lock(&d->mutex);
        d->closed = true;
        packs.swap(d->packs);
    }
    // A run in flight fails when its program is killed; then the lock is free.
    for (auto &entry : packs) {
        {
            QMutexLocker guard(&entry.second->guard);
            if (entry.second->process)
                entry.second->process->signal(SIGKILL);
        }
        QMutexLocker run(&entry.second->run);
        QMutexLocker guard(&entry.second->guard);
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

    if (!type.seal.isEmpty() && nodeSeal(folder, entry) != type.seal)
        return nodeError(QStringLiteral("node_failed"),
                         QStringLiteral("%1 changed on disk after it was loaded, so it is not run. Fugue loads the pack "
                                        "again in a moment; run again then (a new version asks for trust first).")
                             .arg(type.id));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("FUGUE_PROTOCOL"), QStringLiteral("1"));
    environment.insert(QStringLiteral("FUGUE_NODE_DIR"), folder);
    environment.insert(QStringLiteral("FUGUE_WORK_DIR"), context.workDir);

    // A pack's program runs in the sandbox (6.7.1), unless the person said, for this version, to run it without one.
    // `fugue node test` (alwaysSandboxed) runs a folder under test: a yes for an installed pack of the same id and version is
    // not its, and no id makes it a built-in.
    bool sandboxed = false;
    if (d->alwaysSandboxed ? !type.packVersion.isEmpty() : isPackNode(type)) {
        const bool optedOut = !d->alwaysSandboxed && !type.packCommunity && TrustStore().isUnsandboxed(TrustStore::packOf(type.id), type.packVersion);
        if (!optedOut) {
            if (!sandbox::available())
                return makeError(QStringLiteral("sandbox_unavailable"),
                                 QStringLiteral("%1 was not run: its program must run in a sandbox, and the sandbox cannot start here.").arg(type.id),
                                 sandbox::unavailableHint());
            sandboxed = true;
        }
    }

    const bool persistent = runtime.value("mode").toString() == QLatin1String("persistent");
    const double timeout = runtime.value("timeout_s").toDouble(0);
    const int timeoutMs = timeout > 0 ? int(timeout * 1000) : 0;
    const QString id = QStringLiteral("%1:%2").arg(context.run, context.node);
    if (!context.hashes)
        return nodeError(QStringLiteral("node_failed"), QStringLiteral("No file hashes to prepare the inputs with."));
    const Result<QJsonObject> made = requestFor(context, id);
    if (!made)
        return made.error();
    QJsonObject request = made.value();
    // The pack's own secrets, at run time and in this request only (never an input, 6.4): the node's
    // program reads them from `secrets`, and what it says back is masked before it is reported.
    const Result<QMap<QString, StoredSecret>> stored = secretsFor(type);
    if (!stored)
        return maskedError(stored.error(), {});
    QStringList secretValues;
    if (!stored->isEmpty()) {
        QJsonObject secrets;
        for (auto it = stored->begin(); it != stored->end(); ++it) {
            secrets.insert(it.key(), QJsonObject{{"value", it.value().value}, {"origin", it.value().origin}});
            secretValues << it.value().value;
        }
        request.insert("secrets", secrets);
    }
    // A sandboxed program is handed model files from a model folder only; a one-shot gets its media files bound read-only, a
    // program that lives on gets copies in its exchange folder (staged per request below).
    QStringList models;
    QStringList media;
    const QString workDir = QFileInfo(context.workDir).canonicalFilePath();
    if (sandboxed) {
        const QJsonObject inputs = request.value("inputs").toObject();
        const Result<QStringList> found = modelFilesOf(context, inputs);
        if (!found)
            return found.error();
        models = *found;
        media = mediaFilesOf(context, inputs);
        if (workDir.isEmpty())
            return nodeError(QStringLiteral("node_failed"), QStringLiteral("The node's work folder is gone."));
        request.insert("work_dir", workDir); // the folder that is bound is the one the program is told
    }
    const auto launchFor = [&](const QString &where, const Exchange *exchange, const QStringList &bound, const QStringList &handed) -> Result<Launch> {
        if (sandboxed)
            return sandboxedLaunch(context, folder, program, where, exchange, bound, handed);
        return Launch{program, {}, environment, {}, {}};
    };
    // What the program says as it goes (progress lines) goes through the same mask.
    NodeContext reported = context;
    if (!secretValues.isEmpty() && context.progress)
        reported.progress = [&context, &secretValues](double value, const QString &stage, const QString &message) {
            context.progress(value, maskSecrets(stage, secretValues), maskSecrets(message, secretValues));
        };
    const auto finish = [&secretValues](Result<Values> result) { return maskedOutputs(std::move(result), secretValues); };

    if (!persistent) {
        Result<Launch> launched = launchFor(workDir, nullptr, models, media);
        if (!launched)
            return finish(launched.error());
        Launch &launch = *launched;
        Process process(launch.program, launch.arguments, context.workDir, launch.environment, 0, d->grace, std::move(launch.descriptors));
        process.beginRun(secretValues);
        process.sendLine(request);
        process.closeInput();
        Errors warnings;
        const Result<QJsonObject> result =
            await(process, reported, id, false, Expect::Result, timeoutMs, d->grace, &warnings);
        if (!result)
            return finish(result.error());
        return finish(withWarnings(outputsFrom(result.value(), reported), warnings));
    }

    {
        QMutexLocker lock(&d->mutex);
        if (d->closed)
            return nodeError(QStringLiteral("node_failed"), QStringLiteral("Fugue is shutting down."));
    }
    const std::shared_ptr<Impl::Slot> slot = d->slot(type.id);
    // Another run of this node type may hold the program: wait, but a cancel is heard.
    while (!slot->run.tryLock(50)) {
        if (context.cancelled && context.cancelled())
            return nodeError(QStringLiteral("cancelled"), QStringLiteral("The node was cancelled."));
    }
    std::unique_lock<QMutex> run(slot->run, std::adopt_lock);
    // The pack was loaded again with other files (an update): the program of the old ones goes.
    const QString sealKey = type.seal + (sandboxed ? QString() : QStringLiteral("|unsandboxed"));
    if (slot->seal != sealKey) {
        QMutexLocker guard(&slot->guard);
        slot->process.reset();
        slot->seal = sealKey;
        slot->models.clear();
    }
    // The sandbox of a program that lives on shows it only what it was started with: a model file or a work folder it
    // cannot see means starting it again, with the model files it had before and this run's.
    if (sandboxed && slot->process) {
        bool seen = true;
        for (const QString &model : models)
            seen = seen && covered(slot->mounted, QDir::cleanPath(model));
        if (!seen) {
            QMutexLocker guard(&slot->guard);
            slot->process.reset();
        }
    }
    for (const QString &model : models)
        if (!slot->models.contains(model))
            slot->models << model;
    while (slot->models.size() > 16)
        slot->models.removeFirst();
    // A program that was reused may have just exited (its idle stop, or a crash between runs):
    // nothing it said is lost then, so the run starts again once with a new program.
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool reused = true;
        if (!slot->process || !slot->process->acquire()) {
            reused = false;
            const double idle = runtime.value("idle_timeout_s").toDouble(300);
            if (sandboxed) {
                QString why;
                slot->exchange = Exchange::make(&why);
                if (!slot->exchange)
                    return finish(nodeError(QStringLiteral("node_failed"), why));
            }
            Result<Launch> launched = launchFor(sandboxed ? slot->exchange->path : context.workDir, slot->exchange.get(), slot->models, {});
            if (!launched)
                return finish(launched.error());
            Launch &launch = *launched;
            auto fresh = std::make_unique<Process>(launch.program, launch.arguments, context.workDir, launch.environment, idle, d->grace,
                                                   std::move(launch.descriptors));
            {
                QMutexLocker guard(&slot->guard);
                slot->process = std::move(fresh);
                slot->mounted = launch.mounted;
            }
            const Result<QJsonObject> ready = await(*slot->process, reported, id, true, Expect::Ready, 0, d->grace);
            if (!ready) {
                QMutexLocker guard(&slot->guard);
                slot->process.reset();
                return finish(ready.error());
            }
        }
        Process &process = *slot->process;
        process.setBusy(true);
        process.discardLines();
        process.beginRun(secretValues); // its stderr tail starts empty, and the values of this run are masked from here
        // A sandboxed program works in its exchange folder: this request's files are copied in, what it makes is copied out.
        QJsonObject sent = request;
        std::optional<Staged> staged;
        beneath::Fd workFd;
        Fetch fetch;
        QStringList previews;
        int copies = 0;
        if (sandboxed) {
            Result<Staged> made = stageRequest(context, *slot->exchange, sent);
            if (made)
                workFd = beneath::openNoLinks(workDir, O_RDONLY | O_DIRECTORY);
            if (!made || !workFd) {
                process.setBusy(false);
                if (made)
                    beneath::removeTreeBeneath(slot->exchange->root.get(), made->name);
                return finish(made ? nodeError(QStringLiteral("node_failed"), QStringLiteral("The node's work folder cannot be opened.")) : made.error());
            }
            staged.emplace(std::move(*made));
            // Each preview is a copy in the run's work folder; only the last two are kept (the window may still be reading the one
            // before), so a program that sends previews without end fills nothing.
            fetch = [&staged, &workFd, workDir, &copies, &previews](const QString &file) {
                const std::optional<QString> copied = copyOut(file, *staged, workFd.get(), workDir, copies);
                if (copied) {
                    previews << *copied;
                    while (previews.size() > 2)
                        ::unlinkat(workFd.get(), QFile::encodeName(QFileInfo(previews.takeFirst()).fileName()).constData(), 0);
                }
                return copied;
            };
        }
        process.sendLine(sent);
        bool heard = false;
        Errors warnings;
        Result<QJsonObject> result =
            await(process, reported, id, true, Expect::Result, timeoutMs, d->grace, &warnings, &heard, fetch);
        if (staged && result) {
            QJsonObject line = result.value();
            QString refused;
            QJsonObject outputs = line.value("outputs").toObject();
            for (auto it = outputs.begin(); it != outputs.end(); ++it)
                it.value() = mapHandlePaths(it.value(), [&](const QString &path) {
                    const std::optional<QString> copied = copyOut(path, *staged, workFd.get(), workDir, copies);
                    if (!copied && refused.isEmpty())
                        refused = QStringLiteral("Output %1 names %2, which is not a regular file inside the node's work_dir (a link, a folder or a path out of it).")
                                      .arg(it.key(), path);
                    return copied ? *copied : path;
                });
            line.insert("outputs", outputs);
            result = refused.isEmpty() ? Result<QJsonObject>(line) : Result<QJsonObject>(nodeError(QStringLiteral("node_bad_output"), refused));
        }
        if (staged)
            beneath::removeTreeBeneath(slot->exchange->root.get(), staged->name);
        if (process.gone()) {
            QMutexLocker guard(&slot->guard);
            slot->process.reset();
        } else {
            process.setBusy(false);
        }
        if (!result && reused && !heard && attempt == 0 && result.error().code == QLatin1String("node_failed"))
            continue;
        if (!result)
            return finish(result.error());
        if (context.models && !residentProgramKey(type).isEmpty())
            Impl::keepResident(slot, *context.models, type.id);
        return finish(withWarnings(outputsFrom(result.value(), reported), warnings));
    }
    return nodeError(QStringLiteral("node_failed"), QStringLiteral("The node's program could not be run."));
}

} // namespace fugue
