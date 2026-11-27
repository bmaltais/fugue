// The guard for the single root (epic #204): a headless `fugue run` with FUGUE_HOME set writes
// nothing outside the root. HOME is an EMPTY folder and the XDG and temp variables are unset, so a
// path that ignores the root lands in the empty HOME (or /tmp) and the test sees it. A new write
// path added to Fugue that bypasses engine/paths.h fails here.
//
// Seam: the real `fugue` binary (its built-in nodes, real queue and worker) as a subprocess, with
// no display and no GPU helper. Two workflows: images only, and one through ffmpeg (skipped when
// ffmpeg is missing). A third variant runs the first under `strace -f` and checks every file the
// process and its children opened for writing, which also catches /tmp and absolute paths that a
// before-and-after listing of one folder cannot (skipped, saying why, without strace).

#include "../test_paths.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
};

QByteArray node(const char *id, const char *type, const char *version, const QByteArray &inputs) {
    return QByteArray("\"") + id + "\": {\"type\": \"fugue/" + type + "\", \"version\": \"" + version + "\", \"inputs\": " + inputs + "}";
}

QByteArray flow(const QList<QByteArray> &nodes) {
    return "{\"fugue\": 1, \"graph\": {\"nodes\": {" + nodes.join(", ") + "}}}";
}

// Every file and folder under `path`, relative, with its size: what a change shows up as.
QStringList tree(const QString &path) {
    QStringList lines;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        lines << QStringLiteral("%1 %2").arg(QDir(path).relativeFilePath(it.filePath())).arg(it.fileInfo().isDir() ? -1 : it.fileInfo().size());
    }
    lines.sort();
    return lines;
}

} // namespace

class TestCliHomeGuard : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    // A session as a project would start it: FUGUE_HOME set, HOME an empty folder, nothing from the
    // test runner's XDG and temp variables, and no display.
    Outcome fugue(const QStringList &args, const QStringList &wrapper = {}) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR",
                                     "TMPDIR", "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HOME", path("root"));
        env.insert("FUGUE_HELPER", ""); // no GPU helper
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
        if (wrapper.isEmpty())
            process.start(fugueBinary(), args);
        else
            process.start(wrapper.first(), wrapper.mid(1) + QStringList{fugueBinary()} + args);
        Outcome outcome;
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            qWarning("fugue timed out: %s", qPrintable(args.join(' ')));
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    void writeFile(const QString &file, const QByteArray &bytes) const {
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(bytes);
    }

    // load-image -> invert -> save-image
    QString imageWorkflow() const {
        // A 1x1 PNG: the guard needs a real image through the real loader, not a big one.
        writeFile(path("work/inputs/in.png"),
                  QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg=="));
        const QString file = path("work/images.fugue");
        writeFile(file, flow({node("load", "load-image", "1.1.0", "{\"file\": \"" + path("work/inputs/in.png").toUtf8() + "\"}"),
                        node("invert", "invert", "1.0.0", "{\"image\": {\"from\": \"load.image\"}}"),
                        node("save", "save-image", "1.0.0", "{\"image\": {\"from\": \"invert.image\"}, \"name\": \"guard\"}")}));
        return file;
    }

    // load-video -> save-video, both through ffmpeg
    QString videoWorkflow(QString *error) const {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        QProcess make;
        make.start(ffmpeg, {"-nostdin", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=8:duration=1", "-pix_fmt", "yuv420p",
                            "-y", path("work/inputs/clip.mp4")});
        if (!make.waitForFinished(60000) || make.exitCode() != 0) {
            *error = QString::fromLocal8Bit(make.readAllStandardError());
            return {};
        }
        const QString file = path("work/video.fugue");
        writeFile(file, flow({node("load", "load-video", "1.0.0", "{\"file\": \"" + path("work/inputs/clip.mp4").toUtf8() + "\"}"),
                        node("save", "save-video", "1.0.0", "{\"video\": {\"from\": \"load.video\"}, \"name\": \"guard\"}")}));
        return file;
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        for (const char *folder : {"home", "work/inputs"})
            QVERIFY(QDir().mkpath(path(QString::fromLatin1(folder))));
    }
    void cleanup() {
        for (const char *folder : {"root", "work", "home"})
            QDir(path(QString::fromLatin1(folder))).removeRecursively();
    }

    void treeSeesAHiddenFileInHome() { // the diff is not blind: a dot file shows
        writeFile(path("home/.leak"), "x");
        QVERIFY(!tree(path("home")).isEmpty());
    }

    void anImageWorkflowStaysInTheRoot() {
        const QString file = imageWorkflow();
        const QStringList before = tree(path("work"));
        const Outcome o = fugue({"run", file, "--wait", "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(tree(path("home")), QStringList()); // an empty HOME is still empty
        QCOMPARE(tree(path("work")), before);          // nor did it write beside the workflow
        // And it wrote there: the saved picture is in the root's outputs, the cache and run folders are in the root.
        const QStringList saved = QDir(path("root/outputs")).entryList({"guard*.png"});
        QVERIFY2(!saved.isEmpty(), qPrintable(tree(path("root")).join('\n')));
        QVERIFY(QDir(path("root/cache/fugue/runs")).exists());
        QVERIFY(QDir(path("root/cache/fugue/results")).exists());
    }

    void aWorkflowThroughFfmpegStaysInTheRoot() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
            QSKIP("ffmpeg is not installed");
        QString error;
        const QString file = videoWorkflow(&error);
        QVERIFY2(!file.isEmpty(), qPrintable(error));
        const QStringList before = tree(path("work"));
        const Outcome o = fugue({"run", file, "--wait", "--json"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QCOMPARE(tree(path("home")), QStringList());
        QCOMPARE(tree(path("work")), before);
        QVERIFY2(!QDir(path("root/outputs")).entryList({"guard*.mp4"}).isEmpty(), qPrintable(tree(path("root")).join('\n')));
    }

    // Every file the run and its children (the worker, ffmpeg) opened for writing, created or
    // removed is under the root, or a device or a /proc entry.
    void straceSeesNoWriteOutsideTheRoot() {
        const QString strace = QStandardPaths::findExecutable("strace");
        if (strace.isEmpty())
            QSKIP("strace is not installed: only the folder-diff checks ran");
        QProcess probe;
        probe.start(strace, {"-f", "-o", "/dev/null", "true"});
        if (!probe.waitForFinished(20000) || probe.exitCode() != 0)
            QSKIP("strace cannot trace here (ptrace denied): only the folder-diff checks ran");
        const QString file = imageWorkflow();
        const QString log = path("strace.log");
        const Outcome o = fugue({"run", file, "--wait", "--json"},
                                {strace, "-f", "-y", "-qq", "-s", "512", "-o", log, "-e",
                                 "trace=open,openat,openat2,creat,mkdir,mkdirat,rename,renameat,renameat2,unlink,unlinkat,rmdir,"
                                 "symlink,symlinkat,link,linkat,truncate,mknod,mknodat,chmod,fchmodat"});
        QVERIFY2(o.exitCode == 0, o.out + o.err);
        QFile in(log);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QString root = QDir(path("root")).canonicalPath();
        const QString work = QDir(path("work")).canonicalPath();
        static const QRegularExpression writing(R"(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC|O_TMPFILE)");
        static const QRegularExpression quoted("\"((?:[^\"\\\\]|\\\\.)*)\"");
        static const QRegularExpression fdPath("(?:\\d+|AT_FDCWD)<([^>]*)>,\\s*\"");
        QStringList leaks;
        int checked = 0;
        for (const QByteArray &raw : in.readAll().split('\n')) {
            const QString line = QString::fromLocal8Bit(raw);
            if (line.isEmpty() || line.contains(QLatin1String("= -1 ")))
                continue; // a failed call changed nothing
            const bool open = line.contains(QRegularExpression(R"(^\d+\s+(open|openat|openat2|creat)\()"));
            if (open && !line.contains(writing) && !line.contains(QLatin1String("creat(")))
                continue; // read-only open
            const QRegularExpressionMatch match = quoted.match(line);
            if (!match.hasMatch())
                continue;
            QString target = match.captured(1);
            if (!target.startsWith('/')) {
                const QRegularExpressionMatch dir = fdPath.match(line); // openat(5</some/dir>, "name", ...)
                target = (dir.hasMatch() ? dir.captured(1) : work) + '/' + target;
            }
            target = QDir::cleanPath(target);
            ++checked;
            // The one deliberate exception: the cross-root GPU lock, a content-free file in the real runtime
            // folder, where every root meets (spec 1.4.2), and its `.wait` time stamp, which a run waiting for the GPU
            // leaves (and drops) so a helper holding it lets go (spec 8.10). Both are content-free and name no root.
            static const QRegularExpression gpuLock(R"(/fugue-gpu-\d+\.lock(\.wait)?$)");
            const bool allowed = target == root || target.startsWith(root + '/') || target.startsWith("/dev/") || target.startsWith("/proc/")
                                 || gpuLock.match(target).hasMatch();
            if (!allowed)
                leaks << line.left(240);
        }
        QVERIFY2(checked > 20, "the strace log has too few write calls: the parser may be blind");
        QVERIFY2(leaks.isEmpty(), qPrintable("writes outside the root:\n" + leaks.join('\n')));
        QCOMPARE(tree(path("home")), QStringList());
    }
};

QTEST_GUILESS_MAIN(TestCliHomeGuard)
#include "test_cli_home_guard.moc"
