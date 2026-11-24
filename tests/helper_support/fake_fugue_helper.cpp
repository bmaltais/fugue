// A stand-in for `fugue-helper --serve` (helper/src/main.cpp) in the engine's tests: the same
// stdio protocol (spec 8.8), one job at a time, but no inference. What a job does comes from
// its params' "fake" object:
//
//   steps (2)       progress lines before it ends, 20 ms apart
//   fail "<code>"   ends with that error instead of a result
//   crash true      exits (code 3) at its first step, as a helper that crashed
//   hold true       runs until it is cancelled
//   ignore_cancel   a cancel changes nothing
//   frames true     a sample job asked to preview (the submit's "preview") writes output_dir/preview.png
//                   and says so ("preview": PATH) at each step, as the helper does
//
// A finished job writes one file into output_dir, named by what the capability makes
// (conditioning.bin, latent.bin, image.png). The .bin files hold the job's params without
// "fake" as compact JSON, so a file differs whenever what made it does; the image is a 1x1 PNG.
// A decode with a "video_frames" param (a video model) writes video.y4m instead, as the real
// helper does: two 16x16 gray frames at 8 a second.
// With FAKE_HELPER_LOG set, it appends "start <pid>", "job <capability>" and "shutdown" lines
// to that file; a submit's "preview" is logged as "preview <mode>".

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSocketNotifier>
#include <QTimer>

#include <cstdio>
#include <unistd.h>

namespace {

// A 1x1 PNG.
const QByteArray kPng = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==");

void log(const QByteArray &text) {
    const QByteArray path = qgetenv("FAKE_HELPER_LOG");
    if (path.isEmpty())
        return;
    QFile file(QString::fromLocal8Bit(path));
    if (file.open(QIODevice::Append))
        file.write(text + '\n');
}

void say(const QJsonObject &message) {
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

struct Job {
    QString id;
    QJsonObject request;
    QString state = QStringLiteral("queued");
    QJsonObject outputs;
    int step = 0;
    bool cancel = false;
};

class Helper : public QObject {
public:
    Helper() {
        log("start " + QByteArray::number(qint64(::getpid())));
        m_input = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
        connect(m_input, &QSocketNotifier::activated, this, [this] { read(); });
        m_tick.setInterval(20);
        connect(&m_tick, &QTimer::timeout, this, [this] { tick(); });
    }

private:
    void read() {
        char buffer[4096];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (got <= 0) { // Fugue went away
            QCoreApplication::exit(0);
            return;
        }
        m_in.append(buffer, got);
        qsizetype end;
        while ((end = m_in.indexOf('\n')) >= 0) {
            const QJsonObject message = QJsonDocument::fromJson(m_in.left(end)).object();
            m_in.remove(0, end + 1);
            handle(message);
        }
    }

    Job *find(const QString &id) {
        for (Job &job : m_jobs)
            if (job.id == id)
                return &job;
        return nullptr;
    }

    void handle(const QJsonObject &message) {
        const QString op = message.value("op").toString();
        QJsonObject reply{{"id", message.value("id")}};
        if (op == QLatin1String("submit")) {
            Job job;
            job.id = QString::number(++m_count);
            job.request = message;
            if (message.contains("preview"))
                log("preview " + message.value("preview").toString().toUtf8());
            m_jobs.append(job);
            reply.insert("job", job.id);
            say(reply);
            if (!m_tick.isActive())
                m_tick.start();
            return;
        }
        if (op == QLatin1String("shutdown")) {
            log("shutdown");
            say(reply);
            QCoreApplication::exit(0);
            return;
        }
        Job *job = find(message.value("job").toString());
        if (!job) {
            reply.insert("error", QJsonObject{{"code", "input_invalid"}, {"message", "no such job"}});
        } else if (op == QLatin1String("status")) {
            reply.insert("state", job->state);
        } else if (op == QLatin1String("cancel")) {
            const QJsonObject fake = job->request.value("params").toObject().value("fake").toObject();
            if (job->state == QLatin1String("queued")) {
                job->state = QStringLiteral("cancelled");
                say({{"op", "status"}, {"job", job->id}, {"state", job->state}});
            } else if (!fake.value("ignore_cancel").toBool()) {
                job->cancel = true;
            }
            reply.insert("state", job->state);
        } else if (op == QLatin1String("result")) {
            reply.insert("outputs", job->outputs);
            reply.insert("used", QJsonObject{{"seed", job->request.value("params").toObject().value("seed")}});
        } else {
            reply.insert("error", QJsonObject{{"code", "input_invalid"}, {"message", "unknown op " + op}});
        }
        say(reply);
    }

    // Moves the first job that is not over by one step.
    void tick() {
        Job *job = nullptr;
        for (Job &each : m_jobs)
            if (each.state == QLatin1String("queued") || each.state == QLatin1String("running")) {
                job = &each;
                break;
            }
        if (!job) {
            m_tick.stop();
            return;
        }
        const QJsonObject params = job->request.value("params").toObject();
        const QJsonObject fake = params.value("fake").toObject();
        if (job->state == QLatin1String("queued")) {
            job->state = QStringLiteral("running");
            log("job " + job->request.value("capability").toString().toUtf8());
        }
        if (fake.value("crash_oom").toBool()) { // what stable-diffusion.cpp does when a device allocation fails
            std::fputs("ggml: ggml_backend_cuda_buffer_type_alloc_buffer: allocating 2003.00 MiB on device 0: "
                       "cudaMalloc failed: out of memory\n"
                       "conditioner.hpp:1600: GGML_ASSERT(!chunk_hidden_states.empty()) failed\n",
                       stderr);
            std::fflush(stderr);
            ::_exit(134);
        }
        if (fake.value("crash").toBool()) {
            std::fputs("fake helper: crashing as asked\n", stderr);
            std::fflush(stderr);
            ::_exit(3);
        }
        const auto over = [&](const QString &state, const QJsonObject &extra = {}) {
            job->state = state;
            QJsonObject status{{"op", "status"}, {"job", job->id}, {"state", state}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                status.insert(it.key(), it.value());
            say(status);
        };
        if (job->cancel)
            return over(QStringLiteral("cancelled"));
        const int steps = fake.value("steps").toInt(2);
        if (fake.value("hold").toBool() || job->step < steps) {
            ++job->step;
            QJsonObject status{{"op", "status"},
                               {"job", job->id},
                               {"state", "running"},
                               {"stage", "sample"},
                               {"progress", steps > 0 ? double(std::min(job->step, steps)) / steps : 0.0}};
            if (fake.value("frames").toBool() && job->request.contains("preview")) {
                const QString file = QDir(job->request.value("output_dir").toString()).filePath("preview.png");
                QFile frame(file + ".tmp");
                if (frame.open(QIODevice::WriteOnly)) {
                    frame.write(kPng);
                    frame.close();
                    QFile::remove(file);
                    QFile::rename(file + ".tmp", file);
                }
                status.insert("preview", file);
            }
            say(status);
            return;
        }
        if (fake.contains("fail"))
            return over(QStringLiteral("error"),
                        {{"error", QJsonObject{{"code", fake.value("fail")}, {"message", "the fake failed as asked"}}}});
        const QString capability = job->request.value("capability").toString();
        const bool video = capability == QLatin1String("latent.decode") && params.contains("video_frames");
        const QString name = capability == QLatin1String("text.encode") ? QStringLiteral("conditioning")
                             : video                                      ? QStringLiteral("video")
                             : capability == QLatin1String("latent.decode") || capability == QLatin1String("image.upscale") || capability == QLatin1String("image.restore") ? QStringLiteral("image")
                                                                            : QStringLiteral("latent");
        const QString file = QDir(job->request.value("output_dir").toString())
                                 .filePath(name + (video ? ".y4m" : name == QLatin1String("image") ? ".png" : ".bin"));
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly))
            return over(QStringLiteral("error"),
                        {{"error", QJsonObject{{"code", "helper_failed"}, {"message", "cannot write " + file}}}});
        if (video) {
            out.write("YUV4MPEG2 W16 H16 F8:1 Ip A1:1 C420jpeg\n");
            for (int frame = 0; frame < 2; ++frame) {
                out.write("FRAME\n");
                out.write(QByteArray(16 * 16 + 2 * 8 * 8, char(128)));
            }
        } else if (name == QLatin1String("image")) {
            out.write(kPng);
        } else {
            QJsonObject made = params;
            made.remove(QStringLiteral("fake"));
            out.write(QJsonDocument(made).toJson(QJsonDocument::Compact));
        }
        out.close();
        job->outputs = QJsonObject{{name, QJsonObject{{"path", file}}}};
        over(QStringLiteral("done"));
    }

    QSocketNotifier *m_input = nullptr;
    QByteArray m_in;
    QList<Job> m_jobs;
    int m_count = 0;
    QTimer m_tick;
};

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) != QLatin1String("--serve")) {
        std::fputs("usage: fake_fugue_helper --serve\n", stderr);
        return 2;
    }
    Helper helper;
    return app.exec();
}
