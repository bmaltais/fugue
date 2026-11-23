// `fugue canvas new|import|export|log|show|layer|undo|redo` (spec 13.1, 14.5, epic #206). Seam: the `fugue` subprocess
// with an empty HOME, so the store is the test's own ~/.local/share/fugue/canvases, and under
// `--home` the project's own. The store itself is tests/engine/test_canvas_store.cpp.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/canvas_store.h"
#include "engine/paint_document.h"
#include "engine/paint_file.h"

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QString errorCode() const { return json().value("errors").toArray().at(0).toObject().value("code").toString(); }
};

} // namespace

class TestCliCanvas : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    Outcome fugue(const QStringList &args) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "FUGUE_HOME",
                                     "FUGUE_PROJECT", "FUGUE_BASE_XDG_CONFIG_HOME", "FUGUE_BASE_XDG_DATA_HOME", "FUGUE_BASE_XDG_STATE_HOME",
                                     "FUGUE_BASE_XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
            env.remove(QString::fromLatin1(variable));
        env.insert("HOME", path("home"));
        env.insert("FUGUE_HELPER", "");
        QProcess process;
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(path("work"));
        process.start(FUGUE_BINARY, args);
        Outcome outcome;
        if (!process.waitForFinished(120000)) {
            process.kill();
            process.waitForFinished();
            return outcome;
        }
        outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        outcome.out = process.readAllStandardOutput();
        outcome.err = process.readAllStandardError();
        return outcome;
    }

    static QByteArray readFile(const QString &file) {
        QFile in(file);
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }

    // A small canvas as an .ora file in the work folder.
    QString oraFile(const QString &name) const {
        fugue::mask::PaintDocument doc(300, 200);
        doc.addLayer(fugue::mask::LayerKind::Paint, "Paint");
        doc.brush.color = fugue::mask::Rgba{200, 30, 30, 255};
        doc.fillRect(20, 20, 100, 100);
        doc.addLayer(fugue::mask::LayerKind::Mask, "Mask");
        doc.fillRect(0, 0, 50, 50);
        const QString file = path("work/" + name);
        if (fugue::hasErrors(fugue::mask::writePaintFile(doc, file)))
            return {};
        return file;
    }

private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(path("home")));
        QVERIFY(QDir().mkpath(path("work")));
    }
    void cleanup() {
        QDir(path("home")).removeRecursively();
        QDir(path("work")).removeRecursively();
    }

    // #246: with no window there is no canvas last used, so a Canvas Region with no canvas chosen reads the newest one,
    // named in compiled.json; a pinned one keeps its id; with no canvas at all the compile still works (the run refuses).
    void aRegionWithNoCanvasReadsTheNewestOneAtCompile() {
        QFile flow(path("work/flow.fugue"));
        QVERIFY(flow.open(QIODevice::WriteOnly));
        flow.write(R"({"fugue": 1, "graph": {"nodes": {"r": {"type": "fugue/canvas-region", "version": "1.4.0"},
            "p": {"type": "fugue/preview", "version": "1.0.0", "inputs": {"value": {"from": "r.image"}}}}}})");
        flow.close();
        const Outcome none = fugue({"compile", path("work/flow.fugue"), "--json"});
        QVERIFY2(none.exitCode == 0, none.out + none.err);
        const Outcome older = fugue({"canvas", "new", "--name", "Older", "--json"});
        QTest::qWait(1100); // creation time is to the second
        const Outcome newer = fugue({"canvas", "new", "--name", "Newer", "--json"});
        const QString olderId = older.data().value("canvas").toString();
        const QString newerId = newer.data().value("canvas").toString();
        QVERIFY(!olderId.isEmpty() && olderId != newerId);
        const Outcome compiled = fugue({"compile", path("work/flow.fugue"), "--json"});
        QVERIFY2(compiled.exitCode == 0, compiled.out + compiled.err);
        QVERIFY2(compiled.out.contains(newerId.toUtf8()), compiled.out);
        QVERIFY(!compiled.out.contains(olderId.toUtf8()));
        QVERIFY(compiled.out.contains(newer.data().value("head").toString().toUtf8())); // its head is named too
    }

    void newMakesACanvasWithOneRevision() {
        const Outcome made = fugue({"canvas", "new", "--name", "Sketch", "--size", "640x480", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        const QString id = made.data().value("canvas").toString();
        QVERIFY(id.startsWith("canvas-"));
        QCOMPARE(made.data().value("width").toInt(), 640);
        QVERIFY(QFileInfo(path("home/.local/share/fugue/canvases/canvases/" + id + "/head")).isFile());
        const Outcome all = fugue({"canvas", "list", "--json"});
        QCOMPARE(all.data().value("canvases").toArray().size(), 1);
        QCOMPARE(all.data().value("canvases").toArray().at(0).toObject().value("canvas").toString(), id);
        const Outcome shown = fugue({"canvas", "show", id, "--json"});
        QVERIFY2(shown.exitCode == 0, shown.out + shown.err);
        QCOMPARE(shown.data().value("name").toString(), QString("Sketch"));
        QVERIFY(shown.data().value("is_head").toBool());
        QCOMPARE(shown.data().value("layers").toArray().size(), 1);
        QCOMPARE(shown.data().value("revision").toString(), made.data().value("head").toString());
        const Outcome log = fugue({"canvas", "log", id, "--json"});
        QCOMPARE(log.data().value("revisions").toArray().size(), 1);
        QCOMPARE(log.data().value("revisions").toArray().at(0).toObject().value("producer").toString(), QString("new"));
    }

    // #232: a background fills the first layer, as the New canvas dialog's does.
    void newTakesABackground() {
        const Outcome white = fugue({"canvas", "new", "--size", "64x64", "--background", "white", "--json"});
        QVERIFY2(white.exitCode == 0, white.out + white.err);
        const Outcome shown = fugue({"canvas", "show", white.data().value("canvas").toString(), "--json"});
        const QJsonObject layer = shown.data().value("layers").toArray().at(0).toObject();
        QCOMPARE(layer.value("name").toString(), QString("Background"));
        QVERIFY(layer.value("tiles").toInt() > 0);
        const Outcome clear = fugue({"canvas", "new", "--size", "64x64", "--background", "transparent", "--json"});
        const QJsonObject empty = fugue({"canvas", "show", clear.data().value("canvas").toString(), "--json"}).data().value("layers").toArray().at(0).toObject();
        QCOMPARE(empty.value("name").toString(), QString("Layer 1"));
        QCOMPARE(empty.value("tiles").toInt(), 0);
        QCOMPARE(fugue({"canvas", "new", "--background", "#336699", "--json"}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "new", "--background", "teal-ish", "--json"}).errorCode(), QString("input_invalid"));
    }

    void importThenExportGivesTheSameFile() {
        const QString source = oraFile("in.ora");
        QVERIFY(!source.isEmpty());
        const Outcome imported = fugue({"canvas", "import", source, "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        QCOMPARE(imported.data().value("name").toString(), QString("in"));
        const QString out = path("work/out.ora");
        const Outcome exported = fugue({"canvas", "export", id, out, "--json"});
        QVERIFY2(exported.exitCode == 0, exported.out + exported.err);
        QCOMPARE(readFile(out), readFile(source));
        // By revision too, and an export never replaces a file.
        const QString head = imported.data().value("head").toString();
        QCOMPARE(fugue({"canvas", "export", id + "@" + head.mid(0, 15), path("work/rev.ora")}).exitCode, 0);
        QCOMPARE(readFile(path("work/rev.ora")), readFile(source));
        QVERIFY(fugue({"canvas", "export", id, out}).exitCode != 0);
    }

    // #227: a canvas out as a Photoshop file and back; the second file is the first, and what a .psd lacks is said.
    void aPhotoshopFileGoesOutAndComesBackWithItsLayers() {
        const QString source = oraFile("in.ora"); // a paint layer and a mask layer
        const Outcome imported = fugue({"canvas", "import", source, "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        const Outcome out = fugue({"canvas", "export", id, path("work/out.psd"), "--json"});
        QVERIFY2(out.exitCode == 0, out.out + out.err);
        QCOMPARE(out.data().value("format").toString(), QString("psd"));
        QVERIFY(out.data().value("notes").toArray().size() == 1); // the mask layer is a selection
        QVERIFY(readFile(path("work/out.psd")).startsWith("8BPS"));
        const Outcome back = fugue({"canvas", "import", path("work/out.psd"), "--json"});
        QVERIFY2(back.exitCode == 0, back.out + back.err);
        const QString again = back.data().value("canvas").toString();
        QCOMPARE(fugue({"canvas", "export", again, path("work/again.psd")}).exitCode, 0);
        QCOMPARE(readFile(path("work/again.psd")), readFile(path("work/out.psd")));
        QVERIFY(fugue({"canvas", "export", id, path("work/out.psd")}).exitCode != 0); // never replaced
        QVERIFY(fugue({"canvas", "export", id, path("work/q.psd"), "--quality", "50"}).exitCode != 0);
        // A cut-off Photoshop file: refused in words, nothing made.
        QFile bad(path("work/bad.psd"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("8BPS\x00\x02", 6);
        bad.close();
        QCOMPARE(fugue({"canvas", "import", path("work/bad.psd"), "--json"}).errorCode(), QString("media_unreadable"));
    }

    // #249 with #247 and #248: a .fuguepack target is the package path; the picture and layer flags clash with it, and --history clashes with anything else.
    void aPackageTargetAndThePictureFlagsDoNotMix() {
        const QString id = fugue({"canvas", "new", "--name", "Mix", "--size", "300x200", "--json"}).data().value("canvas").toString();
        for (const QStringList &flags : {QStringList{"--layers"}, QStringList{"--format", "png"}, QStringList{"--quality", "50"}, QStringList{"--scale", "50%"},
                                         QStringList{"--lossless"}, QStringList{"--estimate"}, QStringList{"--background", "white"}, QStringList{"--prefix", "x"}}) {
            QStringList args{"canvas", "export", id, path("work/mix.fuguepack")};
            args << flags << "--json";
            const Outcome out = fugue(args);
            QCOMPARE(out.errorCode(), QString("usage"));
            QVERIFY(!QFileInfo::exists(path("work/mix.fuguepack")));
        }
        QCOMPARE(fugue({"canvas", "export", id, path("work/mix.png"), "--history", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "export", id, path("work/mixdir"), "--layers", "--history", "--json"}).errorCode(), QString("usage"));
        // The plain forms still work side by side.
        QCOMPARE(fugue({"canvas", "export", id, path("work/mix.fuguepack"), "--json"}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "export", id, path("work/mix.webp"), "--quality", "80", "--json"}).exitCode, 0);
    }

    // #249: a canvas as a package, head only or with its history, and back in as a new canvas, never over one.
    void aCanvasPackageGoesOutAndComesBackAsANewCanvas() {
        const Outcome made = fugue({"canvas", "new", "--name", "Team", "--size", "300x200", "--background", "white", "--json"});
        const QString id = made.data().value("canvas").toString();
        QVERIFY(fugue({"canvas", "layer", "rename", id, "0", "Backdrop", "--json"}).exitCode == 0); // a second revision
        const Outcome head = fugue({"canvas", "export", id, path("work/head.fuguepack"), "--json"});
        QVERIFY2(head.exitCode == 0, head.out + head.err);
        QCOMPARE(head.data().value("part").toString(), QString("head"));
        const Outcome history = fugue({"canvas", "export", id, path("work/hist.fuguepack"), "--history", "--json"});
        QVERIFY2(history.exitCode == 0, history.out + history.err);
        // Never over a file, and never a revision other than the head.
        QVERIFY(fugue({"canvas", "export", id, path("work/head.fuguepack"), "--json"}).exitCode != 0);
        QVERIFY(fugue({"canvas", "export", id + "@sha256:abcdef0123", path("work/x.fuguepack"), "--json"}).exitCode != 0);

        // Into the store that has it: the id is taken, so it is a new canvas and the old one stays.
        const Outcome again = fugue({"canvas", "import", path("work/hist.fuguepack"), "--json"});
        QVERIFY2(again.exitCode == 0, again.out + again.err);
        QVERIFY(again.data().value("renamed").toBool());
        QCOMPARE(again.data().value("packagedId").toString(), id);
        QVERIFY(again.data().value("canvas").toString() != id);
        QCOMPARE(again.data().value("revisions").toInt(), 2);
        const Outcome onlyHead = fugue({"canvas", "import", path("work/head.fuguepack"), "--name", "Just the layers", "--json"});
        QCOMPARE(onlyHead.data().value("revisions").toInt(), 1);
        QCOMPARE(onlyHead.data().value("name").toString(), QString("Just the layers"));
        QCOMPARE(fugue({"canvas", "list", "--json"}).data().value("canvases").toArray().size(), 3);
        // A file that is not a package keeps the old behaviour.
        QCOMPARE(fugue({"canvas", "import", path("work/missing.png"), "--json"}).exitCode == 0, false);
    }

    void exportWritesAFlattenedPictureByTheExtension() {
        const QString source = oraFile("in.ora"); // 300 x 200: a red square on a transparent paint layer
        const Outcome imported = fugue({"canvas", "import", source, "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        const QString head = imported.data().value("head").toString();

        const Outcome png = fugue({"canvas", "export", id, path("work/flat.png"), "--json"});
        QVERIFY2(png.exitCode == 0, png.out + png.err);
        QCOMPARE(png.data().value("format").toString(), QString("png"));
        // Decoded by the engine's own reader: RGBA bytes.
        const auto decode = [&](const QString &name) {
            const auto picture = fugue::mask::MaskPaint::fromPicture(readFile(path(name)));
            return picture ? picture.value() : fugue::mask::MaskPaint(1, 1, 4);
        };
        const auto pixel = [](const fugue::mask::MaskPaint &picture, int x, int y) {
            const auto p = picture.rgbaAt(x, y);
            return QList<int>{p.r, p.g, p.b, p.a};
        };
        const auto flat = decode("work/flat.png");
        QCOMPARE(flat.width(), 300);
        QCOMPARE(flat.height(), 200);
        QCOMPARE(pixel(flat, 50, 50), (QList<int>{200, 30, 30, 255}));
        QCOMPARE(pixel(flat, 250, 150).at(3), 0);
        // The same pixels the history window shows (document->flattenToPng()).
        const auto document = fugue::canvas::CanvasStore(path("home/.local/share/fugue/canvases")).load(id, head);
        QVERIFY(document);
        QCOMPARE(readFile(path("work/flat.png")), document->flattenToPng().value());
        // By revision, and the extension is not case sensitive.
        QCOMPARE(fugue({"canvas", "export", id + "@head", path("work/again.PNG")}).exitCode, 0);

        // JPEG has no alpha: it is flattened onto white, and the answer says so.
        const Outcome jpg = fugue({"canvas", "export", id, path("work/flat.jpg"), "--json"});
        QVERIFY2(jpg.exitCode == 0, jpg.out + jpg.err);
        QCOMPARE(jpg.data().value("format").toString(), QString("jpg"));
        QCOMPARE(jpg.data().value("background").toString(), QString("white"));
        QVERIFY(!png.data().contains("background"));
        const Outcome plain = fugue({"canvas", "export", id, path("work/plain.jpg")});
        QVERIFY(plain.out.contains("white"));
        const auto jpeg = decode("work/flat.jpg");
        QCOMPARE(jpeg.width(), 300);
        QVERIFY(pixel(jpeg, 250, 150).at(0) > 240 && pixel(jpeg, 250, 150).at(2) > 240);
        QVERIFY(pixel(jpeg, 50, 50).at(0) > 150 && pixel(jpeg, 50, 50).at(1) < 90);
        QCOMPARE(fugue({"canvas", "export", id, path("work/flat.jpeg")}).exitCode, 0);

        const Outcome webp = fugue({"canvas", "export", id, path("work/flat.webp"), "--json"});
        QVERIFY2(webp.exitCode == 0, webp.out + webp.err);
        const auto web = decode("work/flat.webp");
        QCOMPARE(web.height(), 200);
        QCOMPARE(pixel(web, 50, 50), (QList<int>{200, 30, 30, 255}));
        QCOMPARE(pixel(web, 250, 150).at(3), 0); // lossless keeps the alpha

        // Never replaces a file, and an extension it does not write is said, not guessed.
        const QByteArray before = readFile(path("work/flat.png"));
        QVERIFY(fugue({"canvas", "export", id, path("work/flat.png")}).exitCode != 0);
        QCOMPARE(readFile(path("work/flat.png")), before);
        const Outcome odd = fugue({"canvas", "export", id, path("work/flat.gif"), "--json"});
        QCOMPARE(odd.errorCode(), QString("usage"));
        QVERIFY(!QFileInfo::exists(path("work/flat.gif")));
        QCOMPARE(fugue({"canvas", "export", id, path("work/noext"), "--json"}).errorCode(), QString("usage"));
    }

    void exportTakesAFormatAQualityASizeAndABackground() {
        const Outcome imported = fugue({"canvas", "import", oraFile("in.ora"), "--json"}); // 300 x 200, a Paint layer and a Mask layer
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        const auto bytesOf = [&](const QStringList &args) {
            const Outcome out = fugue(QStringList{"canvas", "export", id} + args + QStringList{"--estimate", "--json"});
            return out.exitCode == 0 ? out.data() : QJsonObject{{"error", QString(out.out + out.err)}};
        };
        // --estimate writes nothing and says the size the file would be; the file is the same size.
        const QJsonObject estimate = bytesOf({"--format", "webp", "--quality", "20", "--scale", "50%"});
        QCOMPARE(estimate.value("width").toInt(), 150);
        QCOMPARE(estimate.value("height").toInt(), 100);
        const Outcome written = fugue({"canvas", "export", id, path("work/half.webp"), "--quality", "20", "--scale", "0.5", "--json"});
        QVERIFY2(written.exitCode == 0, written.out + written.err);
        QCOMPARE(readFile(path("work/half.webp")).size(), estimate.value("bytes").toInt());
        QCOMPARE(written.data().value("width").toInt(), 150);
        // A quality moves the size of a jpg.
        QVERIFY(bytesOf({"--format", "jpg", "--quality", "10"}).value("bytes").toInt() < bytesOf({"--format", "jpg", "--quality", "100"}).value("bytes").toInt());
        // The ways to say a size.
        QCOMPARE(bytesOf({"--format", "png", "--width", "30"}).value("height").toInt(), 20);
        QCOMPARE(bytesOf({"--format", "png", "--height", "20"}).value("width").toInt(), 30);
        QCOMPARE(bytesOf({"--format", "png", "--size", "30x7"}).value("height").toInt(), 7);
        // A background under the transparency; jpg cannot be transparent.
        const Outcome blue = fugue({"canvas", "export", id, path("work/blue.png"), "--background", "#0000ff", "--json"});
        QVERIFY2(blue.exitCode == 0, blue.out + blue.err);
        QCOMPARE(blue.data().value("background").toString(), QString("#0000ff"));
        const auto picture = fugue::mask::MaskPaint::fromPicture(readFile(path("work/blue.png")));
        QVERIFY(picture);
        QCOMPARE(picture->rgbaAt(250, 150), (fugue::mask::Rgba{0, 0, 255, 255}));
        QCOMPARE(fugue({"canvas", "export", id, path("work/t.jpg"), "--background", "transparent", "--json"}).errorCode(), QString("input_invalid"));
        // Wrong or conflicting options are usage errors that write nothing.
        for (const QStringList &bad : {QStringList{path("work/a.png"), "--quality", "50"}, QStringList{path("work/a.jpg"), "--quality", "0"},
                                       QStringList{path("work/a.png"), "--format", "jpg"}, QStringList{path("work/a.png"), "--scale", "0"},
                                       QStringList{path("work/a.png"), "--scale", "50%", "--width", "10"}, QStringList{path("work/a.png"), "--size", "10"},
                                       QStringList{path("work/a.png"), "--lossless"}, QStringList{path("work/a.png"), "--prefix", "x"},
                                       QStringList{path("work/a.png"), "--width", "99999"}, QStringList{"--estimate"}}) {
            const Outcome out = fugue(QStringList{"canvas", "export", id} + bad + QStringList{"--json"});
            QVERIFY2(out.exitCode != 0, qPrintable(bad.join(' ')));
            QVERIFY2(out.errorCode() == "usage" || out.errorCode() == "input_invalid", qPrintable(bad.join(' ') + " " + out.out));
            QVERIFY(!QFileInfo::exists(path("work/a.png")) && !QFileInfo::exists(path("work/a.jpg")));
        }
        // A layered export at a size keeps the layers.
        QCOMPARE(fugue({"canvas", "export", id, path("work/small.ora"), "--scale", "50%"}).exitCode, 0);
        const auto small = fugue::mask::paintFromOra(readFile(path("work/small.ora")));
        QVERIFY(small);
        QCOMPARE(small->width(), 150);
        QCOMPARE(small->layerCount(), 2);
    }

    void exportLayersWritesOneFilePerLayerIntoAFolder() {
        const Outcome imported = fugue({"canvas", "import", oraFile("in.ora"), "--json"});
        const QString id = imported.data().value("canvas").toString();
        const Outcome out = fugue({"canvas", "export", id, path("work/layers"), "--layers", "--json"});
        QVERIFY2(out.exitCode == 0, out.out + out.err);
        // The Mask layer is a selection, not a picture: it is left out and said so.
        QCOMPARE(out.data().value("files").toArray().size(), 1);
        QCOMPARE(out.data().value("skipped").toArray().at(0).toString(), QString("Mask"));
        QVERIFY(QFileInfo::exists(path("work/layers/01-Paint.png")));
        const auto layer = fugue::mask::MaskPaint::fromPicture(readFile(path("work/layers/01-Paint.png")));
        QVERIFY(layer);
        QCOMPARE(layer->rgbaAt(50, 50), (fugue::mask::Rgba{200, 30, 30, 255}));
        // Never over a file: the second run is refused and writes nothing new.
        const Outcome again = fugue({"canvas", "export", id, path("work/layers"), "--layers", "--json"});
        QVERIFY(again.exitCode != 0);
        QCOMPARE(again.errorCode(), QString("permission_denied"));
        // Another prefix and format is another set of files.
        QCOMPARE(fugue({"canvas", "export", id, path("work/layers"), "--layers", "--prefix", "shot", "--format", "webp"}).exitCode, 0);
        QVERIFY(QFileInfo::exists(path("work/layers/shot-01-Paint.webp")));
        // Options of the other form are said, not ignored.
        QCOMPARE(fugue({"canvas", "export", id, path("work/l2"), "--layers", "--scale", "50%", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "export", id, path("work/l2"), "--layers", "--format", "jpg", "--json"}).errorCode(), QString("usage"));
        QVERIFY(!QFileInfo::exists(path("work/l2")));
    }

    void importTakesAPlainPictureAsAOneLayerCanvas() {
        fugue::mask::MaskPaint picture(64, 48, 4);
        picture.fill(fugue::mask::Rgba{5, 6, 7, 255});
        QFile png(path("work/photo.png"));
        QVERIFY(png.open(QIODevice::WriteOnly));
        png.write(picture.toPng().value());
        png.close();
        const Outcome imported = fugue({"canvas", "import", path("work/photo.png"), "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        QCOMPARE(imported.data().value("name").toString(), QString("photo"));
        QCOMPARE(imported.data().value("width").toInt(), 64);
        const QJsonArray layers = fugue({"canvas", "show", imported.data().value("canvas").toString(), "--json"}).data().value("layers").toArray();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers.at(0).toObject().value("name").toString(), QString("photo"));
        // Anything else says what is supported.
        QFile text(path("work/notes.txt"));
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.write("hello");
        text.close();
        const Outcome refused = fugue({"canvas", "import", path("work/notes.txt")});
        QCOMPARE(refused.exitCode, 1);
        QVERIFY(refused.err.contains("PNG, JPEG or WebP"));
    }

    void everyCommandPrintsItsOwnUsageForHelp() {
        for (const QStringList &args : {QStringList{"run", "--help"}, {"run", "-h"}, {"run", "x.fugue", "--help"}, {"canvas", "diff", "--help"}, {"canvas", "--help"},
                                        {"node", "list", "-h"}, {"cache", "clear", "--help"}, {"validate", "--help"}}) {
            const Outcome out = fugue(args);
            QVERIFY2(out.exitCode == 0, qPrintable(args.join(' ') + ": " + out.err));
            QVERIFY2(out.out.startsWith("usage: fugue " + args.first().toUtf8()), qPrintable(args.join(' ')));
        }
        const Outcome run = fugue({"run", "--help"});
        QVERIFY(run.out.contains("--wait"));
        QVERIFY(!run.out.contains("canvas"));
        const Outcome canvas = fugue({"canvas", "--help"});
        QVERIFY(canvas.out.contains("canvas diff") && canvas.out.contains("canvas new") && !canvas.out.contains("cache"));
        QVERIFY(fugue({"cache", "--help"}).out.contains("--older-than")); // the continuation line comes along
        // A value that looks like the flag is a value.
        QVERIFY(fugue({"canvas", "new", "--name", "--help", "--json"}).exitCode == 0);
    }

    void theSamePixelsHaveTheSameTreeInTwoCanvases() {
        const QString source = oraFile("in.ora");
        const QString a = fugue({"canvas", "import", source, "--json"}).data().value("canvas").toString();
        const QString b = fugue({"canvas", "import", source, "--json"}).data().value("canvas").toString();
        QVERIFY(a != b);
        QCOMPARE(fugue({"canvas", "show", a, "--json"}).data().value("tree").toString(), fugue({"canvas", "show", b, "--json"}).data().value("tree").toString());
    }

    void aProjectKeepsItsOwnCanvases() {
        QVERIFY(QDir().mkpath(path("proj")));
        const Outcome made = fugue({"--home", path("proj"), "canvas", "new", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        const QString id = made.data().value("canvas").toString();
        QVERIFY(QFileInfo(path("proj/data/fugue/canvases/canvases/" + id + "/canvas.json")).isFile());
        QVERIFY(fugue({"canvas", "show", id, "--json"}).errorCode() == "canvas_not_found"); // the Default session has not got it
        QCOMPARE(fugue({"--home", path("proj"), "canvas", "show", id}).exitCode, 0);
        QDir(path("proj")).removeRecursively();
    }

    // A canvas and two candidates made with the library (what Canvas Write makes), listed, compared,
    // accepted and dropped through the command line.
    void candidatesAreListedComparedAcceptedAndDropped() {
        const Outcome made = fugue({"canvas", "new", "--size", "512x512", "--json"});
        const QString id = made.data().value("canvas").toString();
        const QString head = made.data().value("head").toString();
        fugue::canvas::CanvasStore store(path("home/.local/share/fugue/canvases"));
        const auto candidate = [&](const QString &layer, int x) {
            fugue::mask::PaintDocument doc = store.load(id, head).value();
            doc.addLayer(fugue::mask::LayerKind::Paint, layer);
            doc.brush.color = fugue::mask::Rgba{10, 200, 10, 255};
            doc.fillRect(x, 40, 50, 50);
            return store.addRevision(id, doc, head, "canvas-write:fill").value().id;
        };
        const QString a = candidate("A", 10), b = candidate("B", 300);

        const Outcome listed = fugue({"canvas", "candidates", id, "--json"});
        QVERIFY2(listed.exitCode == 0, listed.out + listed.err);
        QCOMPARE(listed.data().value("candidates").toArray().size(), 2);
        QVERIFY(listed.data().value("candidates").toArray().at(0).toObject().value("on_head").toBool());
        QVERIFY(fugue({"canvas", "candidates", id}).out.contains(a.left(15).toUtf8()));

        const Outcome diff = fugue({"canvas", "diff", id + "@" + a, "--json"}); // against its parent
        QVERIFY2(diff.exitCode == 0, diff.out + diff.err);
        QCOMPARE(diff.data().value("from").toString(), head);
        const QJsonObject layer = diff.data().value("layers").toArray().at(0).toObject();
        QCOMPARE(layer.value("name").toString(), QString("A"));
        QCOMPARE(layer.value("change").toString(), QString("added"));
        QCOMPARE(layer.value("tiles").toInt(), 1);
        const Outcome both = fugue({"canvas", "diff", id + "@" + a, "@" + b, "--json"});
        QCOMPARE(both.data().value("layers").toArray().size(), 2); // A removed, B added
        QVERIFY(fugue({"canvas", "diff", id + "@" + head, "--json"}).errorCode() == "usage"); // the first has no parent

        // Two candidates: a bare id is ambiguous. Drop one, then the bare id means the other.
        QCOMPARE(fugue({"canvas", "accept", id, "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "drop", id + "@" + b, "--json"}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "candidates", id, "--json"}).data().value("candidates").toArray().size(), 1);
        QCOMPARE(fugue({"canvas", "candidates", id, "--all", "--json"}).data().value("candidates").toArray().size(), 2);
        const Outcome accepted = fugue({"canvas", "accept", id, "--json"});
        QVERIFY2(accepted.exitCode == 0, accepted.out + accepted.err);
        QCOMPARE(accepted.data().value("revision").toString(), a);
        QVERIFY(!accepted.data().value("rebased").toBool());
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("revision").toString(), a);
        QCOMPARE(fugue({"canvas", "accept", id + "@" + a, "--json"}).errorCode(), QString("canvas_not_candidate"));
        QCOMPARE(fugue({"canvas", "accept", id, "--json"}).errorCode(), QString("usage")); // none left but a dropped one
        // The dropped one was made from the old head: accepting it places its layer.
        const Outcome placed = fugue({"canvas", "accept", id + "@" + b, "--json"});
        QVERIFY2(placed.exitCode == 0, placed.out + placed.err);
        QVERIFY(placed.data().value("rebased").toBool());
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("layers").toArray().size(), 3);
        QVERIFY(fugue({"canvas", "candidates", id}).out.contains("no candidate"));
    }


    // New, merge, group, ungroup and flatten from the command line (#252): one revision each.
    void layersAreCreatedMergedGroupedAndFlattened() {
        const QString source = oraFile("in.ora"); // Paint (0), Mask (1)
        const QString id = fugue({"canvas", "import", source, "--json"}).data().value("canvas").toString();
        const auto layers = [&] { return fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray(); };

        const Outcome made = fugue({"canvas", "layer", "new", id, "--name", "Sky", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        QCOMPARE(layers().at(0).toObject().value("name").toString(), QString("Sky"));
        QCOMPARE(fugue({"canvas", "layer", "new", id, "0", "--json"}).exitCode, 0); // above the bottom one
        QCOMPARE(layers().size(), 4);

        const Outcome grouped = fugue({"canvas", "layer", "group", id, "0", "1", "--name", "Pair", "--json"});
        QVERIFY2(grouped.exitCode == 0, grouped.out + grouped.err);
        const QJsonObject top = layers().at(0).toObject();
        QCOMPARE(top.value("name").toString(), QString("Sky"));
        const QJsonObject pair = layers().at(2).toObject(); // top first: Sky, Mask, Pair
        QCOMPARE(pair.value("kind").toString(), QString("group"));
        QCOMPARE(pair.value("children").toArray().size(), 2);
        QCOMPARE(fugue({"canvas", "layer", "ungroup", id, "Pair", "--json"}).exitCode, 0);
        QCOMPARE(layers().size(), 4);
        QCOMPARE(fugue({"canvas", "layer", "ungroup", id, "0", "--json"}).errorCode(), QString("input_invalid"));

        // Merge down: the layer into the one under it (a mask layer does not merge).
        QCOMPARE(fugue({"canvas", "layer", "merge", id, "Layer 4", "--json"}).exitCode, 0);
        QCOMPARE(layers().size(), 3);
        QCOMPARE(fugue({"canvas", "layer", "merge", id, "Sky", "--json"}).errorCode(), QString("input_invalid")); // the Mask is under it
        QCOMPARE(fugue({"canvas", "layer", "merge", id, "0", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "layer", "merge", id, "--json"}).errorCode(), QString("usage"));

        const Outcome flat = fugue({"canvas", "layer", "flatten", id, "--json"});
        QVERIFY2(flat.exitCode == 0, flat.out + flat.err);
        const QJsonArray after = layers();
        QCOMPARE(after.at(after.size() - 1).toObject().value("name").toString(), QString("Background"));
        QCOMPARE(fugue({"canvas", "layer", "flatten", id, "0", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "layer", "rm", id, "0", "--name", "x", "--json"}).errorCode(), QString("usage"));
    }

    // Layer edits from the command line (#226): each is one revision of the head, named by position or name.
    void layersAreListedEditedAndUndone() {
        const QString source = oraFile("in.ora"); // Paint (0), Mask (1)
        const Outcome imported = fugue({"canvas", "import", source, "--json"});
        QVERIFY2(imported.exitCode == 0, imported.out + imported.err);
        const QString id = imported.data().value("canvas").toString();
        const QString first = imported.data().value("head").toString();

        const Outcome listed = fugue({"canvas", "layer", "list", id, "--json"});
        QVERIFY2(listed.exitCode == 0, listed.out + listed.err);
        const QJsonArray layers = listed.data().value("layers").toArray();
        QCOMPARE(layers.size(), 2);
        QCOMPARE(layers.at(0).toObject().value("name").toString(), QString("Mask")); // top first
        QCOMPARE(layers.at(0).toObject().value("position").toInt(), 1);
        QCOMPARE(layers.at(1).toObject().value("blend").toString(), QString("normal"));

        const Outcome set = fugue({"canvas", "layer", "set", id, "Paint", "--blend", "multiply", "--opacity", "0.5", "--json"});
        QVERIFY2(set.exitCode == 0, set.out + set.err);
        QCOMPARE(set.data().value("revisions").toArray().size(), 2); // two properties, two revisions
        const Outcome renamed = fugue({"canvas", "layer", "rename", id, "0", "Red", "--json"});
        QVERIFY2(renamed.exitCode == 0, renamed.out + renamed.err);
        const Outcome moved = fugue({"canvas", "layer", "move", id, "Red", "--to", "1", "--json"});
        QVERIFY2(moved.exitCode == 0, moved.out + moved.err);
        const Outcome dup = fugue({"canvas", "layer", "dup", id, "Red", "--json"});
        QVERIFY2(dup.exitCode == 0, dup.out + dup.err);
        const QJsonArray now = fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray();
        QCOMPARE(now.size(), 3);
        QCOMPARE(now.at(0).toObject().value("name").toString(), QString("Red copy"));
        QCOMPARE(now.at(0).toObject().value("blend").toString(), QString("multiply"));
        QCOMPARE(now.at(0).toObject().value("opacity").toDouble(), 0.5);
        QCOMPARE(fugue({"canvas", "layer", "rm", id, "2", "--json"}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "log", id, "--json"}).data().value("revisions").toArray().size(), 7);
        QCOMPARE(fugue({"canvas", "log", id, "--json"}).data().value("revisions").toArray().at(0).toObject().value("producer").toString(),
                 QString("canvas-cli:delete"));

        // Undo walks back to the first revision; redo forward again.
        for (int i = 0; i < 6; ++i)
            QCOMPARE(fugue({"canvas", "undo", id}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("revision").toString(), first);
        const Outcome nothing = fugue({"canvas", "undo", id, "--json"});
        QVERIFY(nothing.exitCode != 0);
        QCOMPARE(nothing.errorCode(), QString("canvas_no_step"));
        QVERIFY(fugue({"canvas", "candidates", id, "--json"}).data().value("candidates").toArray().isEmpty());
        QCOMPARE(fugue({"canvas", "redo", id}).exitCode, 0); // set applies its properties in a fixed order: opacity, then blend
        QCOMPARE(fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray().at(1).toObject().value("opacity").toDouble(), 0.5);
        QCOMPARE(fugue({"canvas", "redo", id}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray().at(1).toObject().value("blend").toString(), QString("multiply"));

        // Errors: a layer that is not there, an ambiguous action, a revision given to an edit.
        QCOMPARE(fugue({"canvas", "layer", "rm", id, "9", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "layer", "rm", id, "Nope", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "layer", "paint", id, "0", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "layer", "rm", id + "@" + first, "0", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "layer", "set", id, "0", "--blend", "dodge", "--json"}).errorCode(), QString("input_invalid"));
    }

    // Resize (#240): one revision, the canvas of a rectangle of the old one, with a pre-fill layer when it grows.
    void aCanvasIsGrownShiftedAndCutInOneRevision() {
        const Outcome made = fugue({"canvas", "new", "--size", "64x32", "--background", "#c83c3c", "--json"});
        QVERIFY2(made.exitCode == 0, made.out + made.err);
        const QString id = made.data().value("canvas").toString();
        const QString first = made.data().value("head").toString();

        const Outcome grown = fugue({"canvas", "resize", id, "--to", "-16,0,96,48", "--json"});
        QVERIFY2(grown.exitCode == 0, grown.out + grown.err);
        QCOMPARE(grown.data().value("width").toInt(), 96);
        QCOMPARE(grown.data().value("fill").toString(), QString("edge-blur"));
        const QJsonArray layers = fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray();
        QCOMPARE(layers.size(), 2);
        QCOMPARE(layers.at(1).toObject().value("name").toString(), QString("Expanded area")); // the bottom
        QCOMPARE(fugue({"canvas", "log", id, "--json"}).data().value("revisions").toArray().at(0).toObject().value("producer").toString(),
                 QString("canvas-cli:resize"));

        const Outcome clear = fugue({"canvas", "resize", id, "--to", "0,0,96,56", "--fill", "transparent", "--json"});
        QVERIFY2(clear.exitCode == 0, clear.out + clear.err);
        QCOMPARE(fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray().size(), 2); // no new layer
        const Outcome cut = fugue({"canvas", "resize", id, "--to", "8,8,32,32", "--json"});
        QVERIFY2(cut.exitCode == 0, cut.out + cut.err);
        QCOMPARE(fugue({"canvas", "undo", id}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "undo", id}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "undo", id}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("revision").toString(), first);

        QCOMPARE(fugue({"canvas", "resize", id, "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "resize", id, "--to", "1,2,3", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "resize", id, "--to", "0,0,8,8", "--fill", "paint", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "resize", id, "--to", "0,0,8,8", "--blur", "-1", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "resize", id, "--to", "100,0,8,8", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "resize", id, "--to", "0,0,99999,8", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "resize", "canvas-nope", "--to", "0,0,8,8", "--json"}).errorCode(), QString("canvas_not_found"));
    }

    void errorsAreCodedAndHelpful() {
        QCOMPARE(fugue({"canvas", "show", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QCOMPARE(fugue({"canvas", "new", "--size", "big", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "new", "--size", "99999x10", "--json"}).errorCode(), QString("input_invalid"));
        QCOMPARE(fugue({"canvas", "log", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QCOMPARE(fugue({"canvas", "candidates", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QCOMPARE(fugue({"canvas", "diff", "canvas-nope", "--json"}).errorCode(), QString("canvas_not_found"));
        QFile bad(path("work/bad.ora"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("nope");
        bad.close();
        QCOMPARE(fugue({"canvas", "import", path("work/bad.ora"), "--json"}).errorCode(), QString("media_unreadable"));
        QVERIFY(fugue({"--help"}).out.contains("canvas new"));
    }
};

QTEST_GUILESS_MAIN(TestCliCanvas)
#include "test_cli_canvas.moc"
