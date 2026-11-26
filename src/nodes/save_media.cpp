// Save Video and Save Audio (spec 14.2, #116): the file a video or a sound is, written to the output
// folder under a name that never overwrites, with the workflow embedded in the container's tags the
// way Save Image embeds it in a PNG. Naming, folder and embedding are Save Image's (save_files.h).
// A file already in the chosen format is copied as it is, with no new encoding; anything else goes
// through ffmpeg, which the engine already runs for probing (engine/ffmpeg.h).

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "engine/atomic_file.h"
#include "engine/ffmpeg.h"
#include "engine/file_hashes.h"
#include "engine/media.h"
#include "nodes/kernels.h"
#include "nodes/save_files.h"

namespace fugue::nodes {

namespace {

// How a container is made: its file extension and ffmpeg muxer, the codecs a file that is not in it
// already is encoded with, and whether a file in the same container is copied as it is.
struct Container {
    QString extension;
    QString muxer;
    QStringList encode;
    bool copyWhenSame;
};

// Video: H.264 and AAC in MP4 (plays everywhere), VP9 and Opus in WebM (what Generate Video makes).
// An even size is what H.264 and VP9 in 4:2:0 need.
const QList<Container> &videoContainers() {
    static const QList<Container> list{
        {QStringLiteral("mp4"), QStringLiteral("mp4"),
         {"-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt",
          "yuv420p", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart+use_metadata_tags"},
         true},
        {QStringLiteral("webm"), QStringLiteral("webm"),
         {"-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "0", "-row-mt", "1",
          "-pix_fmt", "yuv420p", "-c:a", "libopus", "-b:a", "128k"},
         true}};
    return list;
}

// Audio: FLAC (lossless, the format of ComfyUI's Save Audio), MP3 (VBR about 190 kb/s) and Opus.
// A FLAC or MP3 file is copied as it is; an Ogg file may hold Vorbis, so Opus is always encoded.
const QList<Container> &audioContainers() {
    static const QList<Container> list{
        {QStringLiteral("flac"), QStringLiteral("flac"), {"-c:a", "flac"}, true},
        {QStringLiteral("mp3"), QStringLiteral("mp3"), {"-c:a", "libmp3lame", "-q:a", "2", "-id3v2_version", "3"}, true},
        {QStringLiteral("opus"), QStringLiteral("opus"), {"-c:a", "libopus", "-b:a", "128k"}, false}};
    return list;
}

// A value in an ffmetadata file: `=`, `;`, `#`, `\` and a new line are written after a backslash.
QByteArray escaped(const QByteArray &value) {
    QByteArray out;
    for (const char c : value) {
        if (c == '=' || c == ';' || c == '#' || c == '\\' || c == '\n')
            out += '\\';
        out += c;
    }
    return out;
}

class SaveMediaKernel : public NodeKernel {
public:
    explicit SaveMediaKernel(MediaType type) : m_type(type) {}

    // Under validate: the name and folder written are ones a save can use.
    Errors check(const NodeContext &context) const override {
        return context.inputs.contains(QStringLiteral("format")) ? saveLocationProblems(context, context.input("format").text()) : Errors{};
    }

    Result<Values> run(const NodeContext &context) override {
        const QString kind = mediaTypeName(m_type);
        const QString format = context.input("format").text();
        const QList<Container> &containers = m_type == MediaType::Video ? videoContainers() : audioContainers();
        const Container *container = nullptr;
        for (const Container &candidate : containers)
            if (candidate.extension == format)
                container = &candidate;
        if (!container)
            return makeInputError(QStringLiteral("node_failed"), QStringLiteral("format"),
                                  QStringLiteral("\"%1\" is not a format this node writes.").arg(format));

        const Result<SaveLocation> where = saveLocation(context, container->extension);
        if (!where)
            return where.errors();

        // The file to write from: pending trim and crop of a video are applied first (7.2).
        const MediaHandle input = context.input(kind).media();
        const Result<MediaHandle> source = input.plainFile(context.workDir, QStringLiteral("source"), *context.hashes);
        if (!source)
            return source.errors();
        if (!QFileInfo(source->path()).isFile())
            return makeInputError(QStringLiteral("file_not_found"), kind, QStringLiteral("The %1 file is gone: %2.").arg(kind, source->path()),
                                  QStringLiteral("Run the node that makes it again."));

        const Result<QByteArray> embedded = workflowToEmbed(context, context.input("embed").text(), kind);
        const QByteArray workflow = embedded.value();

        PartialFile::removeStale(*where->dir);
        PartialFile partial(*where->dir, numberedName(where->name, 1, container->extension));
        if (const Errors made = partial.create(); hasErrors(made))
            return made;
        const bool copy = container->copyWhenSame && source->format() == container->extension;
        // The tags go in through a file: a workflow is longer than one argument may be.
        QString tags;
        if (!workflow.isEmpty()) {
            tags = QDir(context.workDir).filePath(QStringLiteral("tags.ffmetadata"));
            if (const Errors written = replaceFile(tags, QByteArray(";FFMETADATA1\nfugue=") + escaped(workflow) + '\n', tags); hasErrors(written))
                return written;
        }
        QStringList args{"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", source->path()};
        if (!tags.isEmpty())
            args += {"-f", "ffmetadata", "-i", tags};
        if (m_type == MediaType::Video)
            args += {"-map", "0:v:0", "-map", "0:a:0?"};
        else
            args += {"-map", "0:a:0", "-vn"};
        args += {"-map_metadata", tags.isEmpty() ? "0" : "1"};
        if (copy)
            args += {"-c", "copy", "-movflags", "+faststart+use_metadata_tags"};
        else
            args += container->encode;
        args += {"-f", container->muxer, partial.path()};
        const Result<QByteArray> made = ffmpeg::run(QStringLiteral("ffmpeg"), args, QStringLiteral("node_failed"));
        if (!made)
            return made.errors();
        if (!tags.isEmpty())
            QFile::remove(tags);

        const Result<QString> target = commitAsNew(partial, where->folder, where->name, container->extension);
        if (!target)
            return target.errors();
        const Result<MediaHandle> file = MediaHandle::probe(m_type, *target, *context.hashes);
        if (!file)
            return file.errors();
        return Result<Values>(Values{{"file", Value(file.value())}}, embedded.warnings());
    }

private:
    const MediaType m_type;
};

} // namespace

void addSaveMediaKernels(Kernels &kernels) {
    kernels.add("fugue/save-video", std::make_shared<SaveMediaKernel>(MediaType::Video));
    kernels.add("fugue/save-audio", std::make_shared<SaveMediaKernel>(MediaType::Audio));
}

} // namespace fugue::nodes
