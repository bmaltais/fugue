// `fugue canvas act` and `fugue canvas actions` (#231 slice 10, docs/canvas-actions-design.md). Seam: the `fugue` subprocess with an
// empty HOME (so the canvas store and the model folders are the test's own, and no model is here). The actions that run are
// stand-ins: a workflow with Canvas Region, Invert (no model) and Canvas Write, so the whole path of the bar's Generate runs for
// real on the CPU (the binding, Region and Write, Count, Expand's growth), as the bundled action files do with a model. The binding
// itself is tests/engine/test_canvas_action.cpp's and the bar's tests/canvas/test_canvas_actions.cpp.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/mask_paint.h"
#include "../test_shard.h"

namespace {

struct Outcome {
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
    QJsonObject json() const { return QJsonDocument::fromJson(out).object(); }
    QJsonObject data() const { return json().value("data").toObject(); }
    QJsonObject error() const { return json().value("errors").toArray().at(0).toObject(); }
    QString errorCode() const { return error().value("code").toString(); }
};

// A stand-in for Generative fill: Region of the selection, Invert, Write. The prompt and the seed go to nodes nothing reads.
const char *kFill = R"({"fugue": 1,
 "action": {"format": 1, "name": "Stand-in fill", "selection": "required", "region": "region",
            "inputs": [{"role": "prompt", "targets": ["said.text"], "optional": true},
                       {"role": "seed", "targets": ["roll.value"]},
                       {"name": "steps", "targets": ["count.value"], "label": "Steps", "section": "Advanced"}]},
 "graph": {"nodes": {
   "said": {"type": "fugue/text", "version": "1.0.0", "inputs": {"text": ""}},
   "roll": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 0}},
   "count": {"type": "fugue/number", "version": "1.0.0", "inputs": {"value": 4}},
   "region": {"type": "fugue/canvas-region", "version": "1.4.0", "inputs": {"canvas": "", "revision": "head", "x": 0, "y": 0, "width": 16, "height": 16, "padding": 8, "scale": 1, "multiple": 1}},
   "flip": {"type": "fugue/invert", "version": "1.0.0", "inputs": {"image": {"from": "region.image"}}},
   "write": {"type": "fugue/canvas-write", "version": "1.6.0", "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "flip.image"}, "stitch": {"from": "region.stitch"}, "blend": 0, "name": "Fill"}}}}})";

// The same on the canvas bounds, as Expand: the canvas grows first, then the strip is written.
const char *kGrow = R"({"fugue": 1,
 "action": {"format": 1, "name": "Stand-in expand", "selection": "bounds", "region": "region", "inputs": []},
 "graph": {"nodes": {
   "region": {"type": "fugue/canvas-region", "version": "1.4.0", "inputs": {"canvas": "", "revision": "head", "x": 0, "y": 0, "width": 16, "height": 16, "padding": 8, "scale": 1, "multiple": 1}},
   "flip": {"type": "fugue/invert", "version": "1.0.0", "inputs": {"image": {"from": "region.image"}}},
   "write": {"type": "fugue/canvas-write", "version": "1.6.0", "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "flip.image"}, "stitch": {"from": "region.stitch"}, "blend": 0, "name": "Grown"}}}}})";

// An action on a layer, as Remove background: the layer's mask comes from the layer's own picture.
const char *kLayer = R"({"fugue": 1,
 "action": {"format": 1, "name": "Stand-in mask", "selection": "optional", "region": "region",
            "inputs": [{"role": "layer", "targets": ["region.layer", "write.onto"]}]},
 "graph": {"nodes": {
   "region": {"type": "fugue/canvas-region", "version": "1.4.0", "inputs": {"canvas": "", "revision": "head", "layer": "", "x": 0, "y": 0, "width": 0, "height": 0, "padding": 0, "scale": 1, "multiple": 1}},
   "find": {"type": "fugue/image-to-mask", "version": "1.0.1", "inputs": {"image": {"from": "region.image"}}},
   "write": {"type": "fugue/canvas-write", "version": "1.6.0", "inputs": {"canvas": {"from": "region.canvas"}, "stitch": {"from": "region.stitch"}, "layer_mask": {"from": "find.mask"}, "onto": "", "blend": 0}}}}})";

// An action that redraws one layer's own pixels (Canvas Write 1.7.0 onto_pixels): the layer role targets the Region's layer and the Write's.
const char *kPixels = R"({"fugue": 1,
 "action": {"format": 1, "name": "Stand-in repaint", "selection": "optional", "region": "region",
            "inputs": [{"role": "layer", "targets": ["region.layer", "write.onto_pixels"]}]},
 "graph": {"nodes": {
   "region": {"type": "fugue/canvas-region", "version": "1.4.0", "inputs": {"canvas": "", "revision": "head", "layer": "", "x": 0, "y": 0, "width": 0, "height": 0, "padding": 0, "scale": 1, "multiple": 1}},
   "flip": {"type": "fugue/invert", "version": "1.0.0", "inputs": {"image": {"from": "region.image"}}},
   "write": {"type": "fugue/canvas-write", "version": "1.7.0", "inputs": {"canvas": {"from": "region.canvas"}, "image": {"from": "flip.image"}, "stitch": {"from": "region.stitch"}, "onto_pixels": "", "blend": 0}}}}})";

} // namespace

class TestCliCanvasAct : public QObject {
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

    QString write(const QString &name, const QByteArray &bytes) const {
        QFile file(path("work/" + name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            qFatal("cannot write %s", qPrintable(name));
        return file.fileName();
    }

    // A white canvas of this size; its id.
    QString canvas(int size = 64) const {
        const Outcome made = fugue({"canvas", "new", "--size", QString("%1x%1").arg(size), "--background", "white", "--json"});
        return made.data().value("canvas").toString();
    }

    QJsonArray layers(const QString &id) const { return fugue({"canvas", "show", id, "--json"}).data().value("layers").toArray(); }

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

    // The actions of the task bar, with what each needs: a person with nothing downloaded sees what to fetch first.
    void theActionsAreListedWithWhatTheyNeed() {
        const Outcome list = fugue({"canvas", "actions", "--json"});
        QVERIFY2(list.exitCode == 0, list.out + list.err);
        QMap<QString, QJsonObject> by;
        for (const QJsonValue &row : list.data().value("actions").toArray())
            by.insert(row.toObject().value("id").toString(), row.toObject());
        for (const char *id : {"generative-fill", "refine", "remove", "expand", "harmonize", "remove-background-action", "upscale", "upscale-no-model"})
            QVERIFY2(by.contains(id), id);
        QCOMPARE(by["generative-fill"].value("selection").toString(), QString("required"));
        QVERIFY(by["generative-fill"].value("needs_prompt").toBool());
        QCOMPARE(by["expand"].value("selection").toString(), QString("bounds"));
        QCOMPARE(by["upscale"].value("selection").toString(), QString("none"));
        QVERIFY(by["generative-fill"].value("needs").toObject().value("models_missing").toInt() > 0);
        QCOMPARE(by["upscale-no-model"].value("needs").toObject().value("models").toInt(), 0);
        QVERIFY(by["remove-background-action"].value("has_layer").toBool());
        QVERIFY(!by["remove-background-action"].value("needs").toObject().value("pack").toString().isEmpty());
        QVERIFY(by["refine"].value("inputs").toArray().size() >= 3);
        // `act --list` is the same listing, and the plain text names each action.
        QCOMPARE(fugue({"canvas", "act", "--list", "--json"}).data(), list.data());
        QVERIFY(fugue({"canvas", "actions"}).out.contains("generative-fill  Generative fill"));
    }

    void anActionRunsAndMakesALayer() {
        const QString id = canvas();
        const QString fill = write("fill.fugue", kFill);
        const Outcome ran = fugue({"canvas", "act", id, fill, "--rect", "8,8,32,32", "--prompt", "a hat", "--seed", "7", "--set", "steps=9", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QCOMPARE(ran.data().value("status").toString(), QString("ok"));
        QCOMPARE(ran.data().value("layers").toArray().size(), 1);
        QVERIFY(ran.data().value("layers").toArray().at(0).toObject().value("name").toString().startsWith("Fill"));
        QCOMPARE(ran.data().value("runs").toArray().at(0).toObject().value("seed").toInt(), 7);
        QVERIFY(ran.data().value("head").toString() != ran.data().value("previous").toString());
        QCOMPARE(layers(id).size(), 2);
        QVERIFY2(ran.err.contains("write done"), ran.err); // progress is on stderr
        QVERIFY(fugue({"canvas", "act", id, fill, "--rect", "40,40,16,16", "--wait"}).out.contains("New layer(s)")); // the plain text says what came of it
    }

    // The same run without --wait queues and returns at once; the layer arrives when the worker is done.
    void withoutWaitItQueues() {
        const QString id = canvas();
        const Outcome queued = fugue({"canvas", "act", id, write("fill.fugue", kFill), "--rect", "0,0,32,32", "--json"});
        QVERIFY2(queued.exitCode == 0, queued.out + queued.err);
        QCOMPARE(queued.data().value("status").toString(), QString("queued"));
        QVERIFY(!queued.data().value("runs").toArray().at(0).toObject().value("run").toString().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(layers(id).size(), 2, 20000);
    }

    // A selection from a mask picture (white is selected), as the lasso's, kept by content in the store.
    void aSelectionComesFromAMaskPicture() {
        const QString id = canvas();
        const QString fill = write("fill.fugue", kFill);
        fugue::mask::MaskPaint mask(64, 64);
        mask.fillRect(10, 12, 20, 18);
        write("mask.png", mask.toPng().value());
        const Outcome ran = fugue({"canvas", "act", id, fill, "--selection", path("work/mask.png"), "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QCOMPARE(ran.data().value("layers").toArray().size(), 1);
        fugue::mask::MaskPaint blank(64, 64);
        write("blank.png", blank.toPng().value());
        QCOMPARE(fugue({"canvas", "act", id, fill, "--selection", path("work/blank.png"), "--json"}).errorCode(), QString("mask_empty"));
        fugue::mask::MaskPaint small(8, 8);
        small.fill();
        write("small.png", small.toPng().value());
        QCOMPARE(fugue({"canvas", "act", id, fill, "--selection", path("work/small.png"), "--json"}).errorCode(), QString("usage"));
    }

    // Count N: N layers, the first visible and the rest hidden, one seed each (s, s+1, ...).
    void countMakesHiddenVariations() {
        const QString id = canvas();
        const Outcome ran = fugue({"canvas", "act", id, write("fill.fugue", kFill), "--rect", "4,4,40,40", "--count", "3", "--seed", "20", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        const QJsonArray added = ran.data().value("layers").toArray();
        QCOMPARE(added.size(), 3);
        QVERIFY(added.at(0).toObject().value("name").toString().endsWith("(1 of 3)"));
        QVERIFY(added.at(0).toObject().value("visible").toBool());
        QVERIFY(!added.at(1).toObject().value("visible").toBool());
        QVERIFY(!added.at(2).toObject().value("visible").toBool());
        const QJsonArray runs = ran.data().value("runs").toArray();
        QCOMPARE(runs.at(2).toObject().value("seed").toInt(), 22);
        QCOMPARE(layers(id).size(), 4);
    }

    // Expand: the canvas grows by one revision first (the undo step), then the strip is written onto the grown canvas.
    void expandGrowsTheCanvasThenWrites() {
        const QString id = canvas();
        const QString grow = write("grow.fugue", kGrow);
        QCOMPARE(fugue({"canvas", "act", id, grow, "--json"}).errorCode(), QString("act_refused")); // no side
        const Outcome ran = fugue({"canvas", "act", id, grow, "--expand", "right", "--percent", "25", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QCOMPARE(ran.data().value("expanded").toObject().value("width").toInt(), 80);
        QCOMPARE(ran.data().value("expanded").toObject().value("height").toInt(), 64);
        const Outcome shown = fugue({"canvas", "show", id, "--json"});
        QCOMPARE(shown.data().value("width").toInt(), 80);
        const QJsonArray log = fugue({"canvas", "log", id, "--json"}).data().value("revisions").toArray();
        QVERIFY(log.size() >= 3);
        bool grew = false;
        for (const QJsonValue &revision : log)
            grew = grew || revision.toObject().value("producer").toString().startsWith("action:");
        QVERIFY(grew);
        QVERIFY(ran.data().value("layers").toArray().size() >= 1);
        QCOMPARE(fugue({"canvas", "act", id, grow, "--expand", "left", "--pixels", "32", "--wait", "--json"}).data().value("expanded").toObject().value("width").toInt(), 112);
    }

    // An Expand that fails after the canvas grew takes the strip back (#310): the head returns to the revision before the growth, and the
    // error says so. (A cancel is the same code; the bar's test covers it, a signal to the process is not worth a flaky test here.)
    void aFailedExpandTakesTheGrowthBack() {
        const QString id = canvas();
        const QString before = fugue({"canvas", "show", id, "--json"}).data().value("revision").toString();
        // A Write onto a layer that is not there fails after the growth.
        QByteArray broken = kGrow;
        broken.replace("\"blend\": 0,", "\"blend\": 0, \"onto\": \"Nowhere\",");
        const Outcome ran = fugue({"canvas", "act", id, write("broken.fugue", broken), "--expand", "right", "--percent", "25", "--wait", "--json"});
        QCOMPARE(ran.exitCode, 1);
        QVERIFY2(ran.error().value("message").toString().contains("The canvas is back to its size"), ran.out + ran.err);
        const Outcome shown = fugue({"canvas", "show", id, "--json"});
        QCOMPARE(shown.data().value("width").toInt(), 64);
        QCOMPARE(shown.data().value("revision").toString(), before);
        // The growth revision is still in the history: Redo has it.
        QCOMPARE(fugue({"canvas", "redo", id, "--json"}).exitCode, 0);
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("width").toInt(), 80);
    }

    // A layer-role action works on the layer named by --layer (a name or a position), and says so when none is given.
    void aLayerActionTakesALayer() {
        const QString id = canvas();
        const QString mask = write("mask.fugue", kLayer);
        const Outcome none = fugue({"canvas", "act", id, mask, "--json"});
        QCOMPARE(none.errorCode(), QString("act_refused"));
        QVERIFY(none.error().value("message").toString().contains("Select a layer"));
        QCOMPARE(fugue({"canvas", "act", id, mask, "--layer", "Nope", "--json"}).errorCode(), QString("usage"));
        const Outcome ran = fugue({"canvas", "act", id, mask, "--layer", "0", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QVERIFY(ran.data().value("head").toString() != ran.data().value("previous").toString());
        QVERIFY(fugue({"canvas", "layer", "list", id, "--json"}).out.contains("\"has_mask\":true") ||
                fugue({"canvas", "layer", "list", id}).out.contains("mask"));
    }

    // Write onto a layer's pixels: no layer is added, the head moves by one revision.
    void aRepaintWritesTheLayersOwnPixels() {
        const QString id = canvas();
        const int count = layers(id).size();
        const Outcome ran = fugue({"canvas", "act", id, write("pixels.fugue", kPixels), "--layer", "0", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        QVERIFY(ran.data().value("head").toString() != ran.data().value("previous").toString());
        QCOMPARE(layers(id).size(), count);
    }

    // Upscale makes a new canvas and says which; the one it ran on is as it was.
    void upscaleMakesANewCanvas() {
        const QString id = canvas(32);
        const QString before = fugue({"canvas", "show", id, "--json"}).data().value("revision").toString();
        const Outcome ran = fugue({"canvas", "act", id, "upscale-no-model", "--wait", "--json"});
        QVERIFY2(ran.exitCode == 0, ran.out + ran.err);
        const QJsonArray made = ran.data().value("canvases").toArray();
        QCOMPARE(made.size(), 1);
        QCOMPARE(fugue({"canvas", "show", made.at(0).toString(), "--json"}).data().value("width").toInt(), 64);
        QCOMPARE(layers(made.at(0).toString()).at(0).toObject().value("name").toString(), QString("Upscaled")); // not "Layer 1" (#311)
        QCOMPARE(fugue({"canvas", "show", id, "--json"}).data().value("revision").toString(), before);
        QCOMPARE(ran.data().value("layers").toArray().size(), 0);
        QVERIFY(QString::fromUtf8(ran.err).contains("new"));
    }

    // A model is picked by its file name, as the bar's picker does (#309): the file is found in the model folders and hashed
    // (remembered). Not found is a plain usage error that lists nothing it made up; a full resource handle still works as given.
    void aModelIsSetByItsFileName() {
        const QString id = canvas(32);
        QVERIFY(QDir().mkpath(path("home/.local/share/fugue/models/upscale_models/sub")));
        QFile file(path("home/.local/share/fugue/models/upscale_models/sub/stand-in.safetensors"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a real model");
        file.close();
        const Outcome absent = fugue({"canvas", "act", id, "upscale", "--set", "model=nonesuch.safetensors", "--json"});
        QCOMPARE(absent.errorCode(), QString("usage"));
        QVERIFY2(absent.error().value("message").toString().contains("nonesuch.safetensors"), absent.out);
        QVERIFY(absent.error().value("valid").toArray().contains("sub/stand-in.safetensors"));
        // The name (as the library lists it, or the bare file name) finds the file: the run goes on and fails on the fake file, not on the name.
        for (const char *name : {"sub/stand-in.safetensors", "stand-in.safetensors"}) {
            const Outcome ran = fugue({"canvas", "act", id, "upscale", "--set", QString("model=%1").arg(name), "--wait", "--json"});
            QVERIFY2(ran.errorCode() != "usage" && ran.errorCode() != "model_missing" && ran.errorCode() != "type_mismatch", ran.out + ran.err);
        }
        // A full handle is taken as it is: a name no file has is model_missing, not a usage or type error.
        const QString zeros(64, QLatin1Char('0'));
        const QString handle = QString(R"({"resource":"fugue/upscale-model","name":"gone.safetensors","hash":"sha256:%1"})").arg(zeros);
        QCOMPARE(fugue({"canvas", "act", id, "upscale", "--set", "model=" + handle, "--json"}).errorCode(), QString("model_missing"));
    }

    // What cannot run says why, in words, with a coded error and a failing exit status.
    void whatCannotRunSaysWhy() {
        const QString id = canvas();
        const Outcome models = fugue({"canvas", "act", id, "generative-fill", "--rect", "0,0,32,32", "--prompt", "x", "--json"});
        QCOMPARE(models.exitCode, 1);
        QCOMPARE(models.errorCode(), QString("model_missing"));
        QVERIFY(models.error().value("message").toString().contains(".gguf"));
        QVERIFY(models.error().value("hint").toString().contains("fugue model get generative-fill"));
        const Outcome pack = fugue({"canvas", "act", id, "remove-background-action", "--layer", "0", "--json"});
        QCOMPARE(pack.exitCode, 1);
        QCOMPARE(pack.errorCode(), QString("pack_missing"));
        QVERIFY(pack.error().value("message").toString().contains("Remove Background pack"));
        const Outcome plain = fugue({"canvas", "act", id, "generative-fill", "--rect", "0,0,32,32", "--prompt", "x"});
        QVERIFY(plain.err.contains("not on this machine"));
        QCOMPARE(fugue({"canvas", "layer", "list", id, "--json"}).data().value("layers").toArray().size(), 1); // nothing changed
        QCOMPARE(fugue({"canvas", "log", id, "--json"}).data().value("revisions").toArray().size(), 1);
        // The words of the bar for what the person left out.
        const QString fill = write("fill.fugue", kFill);
        const Outcome nothing = fugue({"canvas", "act", id, fill, "--json"});
        QCOMPARE(nothing.errorCode(), QString("act_refused"));
        QVERIFY(nothing.error().value("message").toString().contains("Select an area first"));
        QCOMPARE(fugue({"canvas", "act", "canvas-nope", fill, "--rect", "0,0,8,8", "--json"}).errorCode(), QString("canvas_not_found"));
    }

    void anUnknownActionListsTheKnownOnes() {
        const Outcome out = fugue({"canvas", "act", canvas(), "nonesuch", "--json"});
        QCOMPARE(out.exitCode, 1);
        QCOMPARE(out.errorCode(), QString("file_not_found"));
        const QJsonArray valid = out.error().value("valid").toArray();
        QVERIFY2(valid.contains("generative-fill") && valid.contains("upscale"), out.out);
        QVERIFY(fugue({"canvas", "act", canvas(), "nonesuch"}).err.contains("generative-fill"));
    }

    void badArgumentsAreUsageErrors() {
        const QString id = canvas();
        const QString fill = write("fill.fugue", kFill);
        const QString grow = write("grow.fugue", kGrow);
        for (const QStringList &args : QList<QStringList>{{"--rect", "1,2,3"}, {"--rect", "1,2,0,5"}, {"--count", "9"}, {"--seed", "-1"}, {"--set", "steps"},
                                                           {"--set", "nonesuch=1"}, {"--set", "seed=1"}, {"--expand", "left"}, {"--rect", "0,0,8,8", "--selection", "x.png"}}) {
            const Outcome out = fugue(QStringList{"canvas", "act", id, fill} + args + QStringList{"--json"});
            QVERIFY2(out.errorCode() == "usage", qPrintable(args.join(' ') + ": " + out.out));
        }
        QCOMPARE(fugue({"canvas", "act", id, grow, "--expand", "right", "--percent", "40", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "act", id, grow, "--expand", "right", "--percent", "25", "--pixels", "16", "--json"}).errorCode(), QString("usage"));
        QCOMPARE(fugue({"canvas", "act", id, "--json"}).errorCode(), QString("usage")); // an action is missing
        QCOMPARE(fugue({"canvas", "act", id, grow, "--expand", "up", "--json"}).errorCode(), QString("act_refused"));
        QCOMPARE(fugue({"canvas", "act", id, write("nosd.fugue", kGrow), "--count", "2", "--expand", "left", "--json"}).errorCode(), QString("usage")); // no seed
    }
};

FUGUE_TEST_MAIN(TestCliCanvasAct, QCoreApplication)
#include "test_cli_canvas_act.moc"
