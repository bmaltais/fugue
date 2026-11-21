// Upscale Video with Model (spec 14.1, #138): a video through an upscale model on the helper, a chunk
// of frames at a time, so a long video is never whole in memory or on disk.
//
// One job runs per chunk (ProviderKernel::next) and the helper keeps its upscaler loaded between
// them, so the model is read once. Around each job, in this order:
//
//   ffmpeg decodes chunk k of the source to PNGs  ->  the helper upscales the folder  ->  ffmpeg
//   encodes the result to a segment (x264 CRF 12, as Upscale Video)  ->  the chunk's PNGs go
//
// and at the end one pass joins the segments (copied, not encoded again) with the source's audio.
// What stays on disk at any time is one chunk's frames in and out, plus the segments so far (the
// size of the result). Between the jobs the state is a file in the node's work folder, as a kernel
// keeps none of its own; a cancel or an error removes it all (abandon).
//
// The frame rate is the source's (as the exact fraction ffprobe reports), and each chunk is cut by
// frame number at that rate, so the frame count, the rate, the length and the sound are the
// source's. A video of a variable rate comes out at its average rate.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "nodes/kernels.h"
#include "nodes/upscale_model.h"

namespace fugue::nodes {

namespace {

constexpr int kMostFrames = 64;       // frames in a chunk, at most
constexpr int kLeastFrames = 4;       // and at least, however big the frames
constexpr double kBudget = 28e6;      // pixels of a chunk's input frames: about 30 frames of 1080p

struct State {
    QString source;      // the video, as the nodes before it left it (no pending edits)
    QString rate;        // its frame rate as ffprobe's fraction, "30000/1001"
    QString matrix;      // its colour matrix as ffmpeg names it, empty when it has none
    double fps = 0;
    int chunk = 0;       // the chunk the job in flight is on
    int per = kMostFrames;
    qint64 done = 0;     // frames encoded in earlier chunks
    qint64 total = 1;    // the frames the video has, from its length (an estimate)
    int count = 0;       // the frames in this chunk
};

QString workPath(const NodeContext &context, const QString &name) { return QDir(context.workDir).filePath(name); }
QString statePath(const NodeContext &context) { return workPath(context, QStringLiteral("upscale-state.json")); }
QString framesIn(const NodeContext &context) { return workPath(context, QStringLiteral("frames")); }
QString framesOut(const NodeContext &context) { return workPath(context, QStringLiteral("frames-out")); } // the helper's name
QString segment(const NodeContext &context, int chunk) {
    return workPath(context, QStringLiteral("segment-%1.mkv").arg(chunk, 6, 10, QLatin1Char('0')));
}

void clearFolder(const QString &path) { QDir(path).removeRecursively(); }

void save(const NodeContext &context, const State &state) {
    const QJsonObject json{{"source", state.source}, {"rate", state.rate}, {"matrix", state.matrix}, {"fps", state.fps},
                           {"chunk", state.chunk},   {"per", state.per},   {"done", double(state.done)},
                           {"total", double(state.total)}, {"count", state.count}};
    QFile file(statePath(context));
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(json).toJson(QJsonDocument::Compact));
}

State load(const NodeContext &context) {
    State state;
    QFile file(statePath(context));
    if (!file.open(QIODevice::ReadOnly))
        return state;
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    state.source = json.value("source").toString();
    state.rate = json.value("rate").toString();
    state.matrix = json.value("matrix").toString();
    state.fps = json.value("fps").toDouble();
    state.chunk = json.value("chunk").toInt();
    state.per = json.value("per").toInt(kMostFrames);
    state.done = qint64(json.value("done").toDouble());
    state.total = std::max<qint64>(1, qint64(json.value("total").toDouble(1)));
    state.count = json.value("count").toInt();
    return state;
}

// Frames in a chunk: fewer as the frames grow, so what a chunk holds on disk, in and out, stays
// about as big whatever the size.
int framesPerChunk(int width, int height) {
    const double pixels = std::max(1.0, double(width) * height);
    return std::clamp(int(kBudget / pixels), kLeastFrames, kMostFrames);
}

// ffmpeg's name for the matrix that turns the source's colours to RGB and back, when the source
// says it: untagged video goes through ffmpeg's default both ways, and so comes back the same.
QString matrixName(const QString &colorSpace) {
    if (colorSpace == QLatin1String("bt709") || colorSpace == QLatin1String("smpte170m") || colorSpace == QLatin1String("bt470bg"))
        return colorSpace;
    if (colorSpace == QLatin1String("bt2020nc"))
        return QStringLiteral("bt2020");
    return {};
}

Result<State> inspect(const MediaHandle &source) {
    State state;
    state.source = source.path();
    const Result<QByteArray> probed = ffmpeg::run(
        QStringLiteral("ffprobe"),
        {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=avg_frame_rate,color_space", "-of", "json", source.path()},
        QStringLiteral("node_failed"));
    if (!probed)
        return probed.errors();
    const QJsonArray streams = QJsonDocument::fromJson(probed.value()).object().value("streams").toArray();
    const QJsonObject stream = streams.isEmpty() ? QJsonObject() : streams.first().toObject();
    const QString rate = stream.value("avg_frame_rate").toString();
    const QStringList parts = rate.split(QLatin1Char('/'));
    const double top = parts.value(0).toDouble(), bottom = parts.size() > 1 ? parts.value(1).toDouble() : 1.0;
    if (top <= 0 || bottom <= 0)
        return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("video"), QStringLiteral("The video has no frame rate."));
    state.rate = rate.contains(QLatin1Char('/')) ? rate : rate + QStringLiteral("/1");
    state.fps = top / bottom;
    state.matrix = matrixName(stream.value("color_space").toString());
    state.per = framesPerChunk(source.width(), source.height());
    state.total = std::max<qint64>(1, std::llround(source.duration() * state.fps));
    return state;
}

// The frames of chunk `state.chunk` as 000001.png.. in a fresh folder: seeked by time to half a
// frame before the first (the seek is exact, ffmpeg decodes up to it), then `per` frames. Returns
// how many there are, and none at the end of the video.
Result<int> decode(const NodeContext &context, const State &state) {
    const QString dir = framesIn(context);
    clearFolder(dir);
    QDir().mkpath(dir);
    const qint64 first = qint64(state.chunk) * state.per;
    QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y"};
    if (first > 0)
        args += {"-ss", QString::number((double(first) - 0.5) / state.fps, 'f', 6)};
    args += {"-i", state.source, "-map", "0:v:0", "-frames:v", QString::number(state.per), "-fps_mode", "passthrough",
             "-start_number", "1", QDir(dir).filePath(QStringLiteral("%06d.png"))};
    const Result<QByteArray> done = ffmpeg::runWatched(args, QStringLiteral("node_failed"), {}, context.cancelled);
    if (!done)
        return done.errors();
    return int(QDir(dir).entryList({QStringLiteral("*.png")}, QDir::Files).size());
}

Result<QByteArray> encode(const NodeContext &context, const State &state, const QString &out) {
    QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-framerate", state.rate, "-start_number", "1",
                     "-i", QDir(framesOut(context)).filePath(QStringLiteral("%06d.png"))};
    QString filter = QStringLiteral("scale=trunc(iw/2)*2:trunc(ih/2)*2"); // 4:2:0 needs even sides
    if (!state.matrix.isEmpty())
        filter += QStringLiteral(":out_color_matrix=") + state.matrix;
    args += {"-vf", filter, "-c:v", "libx264", "-preset", "medium", "-crf", "12", "-pix_fmt", "yuv420p"};
    if (!state.matrix.isEmpty())
        args += {"-colorspace", state.matrix == QLatin1String("bt2020") ? QStringLiteral("bt2020nc") : state.matrix};
    args += {"-f", "matroska", out};
    return ffmpeg::runWatched(args, QStringLiteral("node_failed"), {}, context.cancelled);
}

// The job for the chunk just decoded: `state.count` frames, placed in the node's bar.
JobRequest jobFor(const NodeContext &context, const QJsonObject &params, const State &state) {
    JobRequest request;
    request.capability = QStringLiteral("image.upscale");
    request.params = params;
    request.inputs.insert(QStringLiteral("frames"), QJsonObject{{"path", framesIn(context)}});
    request.progressFrom = std::min(1.0, double(state.done) / state.total);
    request.progressSpan = std::min(1.0 - request.progressFrom, double(state.count) / state.total);
    return request;
}

class UpscaleVideoWithModelKernel : public ProviderKernel {
public:
    Requirement requirement(const NodeType &, const Values &) const override { return upscaleRequirement(); }

    Result<JobRequest> request(const NodeContext &context, const Selection &selection) override {
        if (!selection.sharesFilesystem) // the frames are folders on this machine's disk
            return makeError(QStringLiteral("capability_unavailable"),
                             QStringLiteral("Upscale Video with Model needs a helper on this machine: the frames are passed as folders."),
                             QStringLiteral("Use a helper on this computer, or upscale the frames as images."));
        const Result<QJsonObject> params = upscaleParams(context, selection);
        if (!params)
            return params.errors();
        const MediaHandle input = context.input(QStringLiteral("video")).media();
        const Result<MediaHandle> source = input.plainFile(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!source)
            return source.errors();
        if (!QFileInfo(source->path()).isFile())
            return makeInputError(QStringLiteral("file_not_found"), QStringLiteral("video"),
                                  QStringLiteral("The video file is gone: %1.").arg(source->path()),
                                  QStringLiteral("Run the node that makes it again."));
        clearFolder(framesIn(context)); // a retry after out_of_memory starts over
        clearFolder(framesOut(context));
        Result<State> found = inspect(source.value());
        if (!found)
            return found.errors();
        State state = found.value();
        const Result<int> frames = decode(context, state);
        if (!frames)
            return frames.errors();
        if (frames.value() == 0)
            return makeInputError(QStringLiteral("media_unreadable"), QStringLiteral("video"), QStringLiteral("The video has no frames."));
        state.count = frames.value();
        save(context, state);
        return jobFor(context, params.value(), state);
    }

    Result<std::optional<JobRequest>> next(const NodeContext &context, const Selection &selection, const JobResult &) override {
        State state = load(context);
        // The chunk's results: the same names as went in, all of them.
        const int made = int(QDir(framesOut(context)).entryList({QStringLiteral("*.png")}, QDir::Files).size());
        if (made != state.count)
            return makeError(QStringLiteral("helper_failed"),
                             QStringLiteral("The helper upscaled %1 of %2 frames of the video.").arg(made).arg(state.count));
        const Result<QByteArray> encoded = encode(context, state, segment(context, state.chunk));
        if (!encoded)
            return encoded.errors();
        clearFolder(framesIn(context));
        clearFolder(framesOut(context));
        state.done += state.count;
        if (context.progress)
            context.progress(std::min(1.0, double(state.done) / state.total), QStringLiteral("upscale"), QString());
        if (state.count < state.per) { // a short chunk was the end of the video
            save(context, state);
            return std::optional<JobRequest>();
        }
        ++state.chunk;
        const Result<int> frames = decode(context, state);
        if (!frames)
            return frames.errors();
        if (frames.value() == 0) {
            clearFolder(framesIn(context));
            state.count = 0;
            save(context, state);
            return std::optional<JobRequest>();
        }
        state.count = frames.value();
        save(context, state);
        const Result<QJsonObject> params = upscaleParams(context, selection);
        if (!params)
            return params.errors();
        return std::optional<JobRequest>(jobFor(context, params.value(), state));
    }

    // The segments joined as they are, with the source's first audio track copied: one pass, and
    // no frame is encoded twice.
    Result<Values> outputs(const NodeContext &context, const Selection &, const JobResult &) override {
        const State state = load(context);
        const QString list = workPath(context, QStringLiteral("segments.txt"));
        QFile file(list);
        if (!file.open(QIODevice::WriteOnly))
            return makeError(QStringLiteral("node_failed"), QStringLiteral("Cannot write %1.").arg(list));
        for (int chunk = 0; chunk <= state.chunk; ++chunk)
            if (QFileInfo::exists(segment(context, chunk)))
                file.write("file '" + segment(context, chunk).toUtf8() + "'\n");
        file.close();
        const QString out = workPath(context, QStringLiteral("video.mkv"));
        const Result<QByteArray> joined = ffmpeg::runWatched(
            {"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", list, "-i", state.source,
             "-map", "0:v:0", "-map", "1:a:0?", "-c", "copy", "-f", "matroska", out},
            QStringLiteral("node_failed"), {}, context.cancelled);
        abandon(context);
        if (!joined)
            return joined.errors();
        const Result<MediaHandle> video = MediaHandle::probe(MediaType::Video, out, *context.hashes);
        if (!video)
            return video.errors();
        return Values{{QStringLiteral("video"), Value(video.value())}};
    }

    void abandon(const NodeContext &context) override {
        clearFolder(framesIn(context));
        clearFolder(framesOut(context));
        for (const QString &name : QDir(context.workDir).entryList({QStringLiteral("segment-*.mkv")}, QDir::Files))
            QFile::remove(workPath(context, name));
        QFile::remove(workPath(context, QStringLiteral("segments.txt")));
        QFile::remove(statePath(context));
    }
};

} // namespace

void addUpscaleVideoModelKernels(Kernels &kernels) {
    kernels.add("fugue/upscale-video-with-model", std::make_shared<UpscaleVideoWithModelKernel>());
}

} // namespace fugue::nodes
