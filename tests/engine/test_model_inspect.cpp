// What the engine says a model file is (#703, engine/model_inspect.h): the inspector with a fake helper script (one process for a
// batch, an answer kept by path, size and time, no helper, a failed file), and the marks that use it (the family fallback, the
// not-recognised mark). No real helper or model: the script answers by the file's name.

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "engine/model_family.h"
#include "engine/model_inspect.h"
#include "engine/model_recommend.h"

using namespace fugue;

class TestModelInspect : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    QString path(const QString &name) const { return m_dir.filePath(name); }

    void write(const QString &name, const QByteArray &bytes) {
        QFile file(path(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
    }

    // A helper that logs how many files each run was given and answers by name: "unknown" is not recognised, "bad" is an error.
    // `note` is in the script, so a different one is another helper (its size changes).
    void helper(const QString &note = QString()) {
        write("helper.sh", QByteArray("#!/bin/sh\n# " + note.toUtf8() + "\necho \"$#\" >> '" + path("runs.log").toUtf8() + "'\nshift\n" +
                                      R"(for f in "$@"; do
  case "$f" in
    *unknown*) printf '{"path":"%s","recognised":false}\n' "$f" ;;
    *bad*) printf '{"path":"%s","error":"unreadable"}\n' "$f" ;;
    *fillish*) printf '{"path":"%s","recognised":true,"version":"Flux Fill","id":"flux_fill","is_dit":true,"video":false,"inpaint":true,"latent_channels":16,"vae_kind":"flux"}\n' "$f" ;;
    *) printf '{"path":"%s","recognised":true,"version":"Fake DiT","id":"fake_dit","is_dit":true,"video":false,"inpaint":false,"latent_channels":16,"vae_kind":"flux"}\n' "$f" ;;
  esac
done
)"));
        QVERIFY(QFile::setPermissions(path("helper.sh"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }

    // The files each run was asked about, in order.
    QStringList runs() const {
        QFile log(path("runs.log"));
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList();
    }

private slots:
    void init() {
        QFile::remove(path("runs.log"));
        QFile::remove(path("cache.json"));
        helper();
    }

    void aBatchIsOneProcessAndItsAnswersAreKept() {
        for (const char *name : {"a.safetensors", "b.gguf", "unknown-c.gguf"})
            write(name, "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        const QStringList files{path("a.safetensors"), path("b.gguf"), path("unknown-c.gguf")};
        const QHash<QString, EngineModel> answers = inspector.inspect(files);
        QCOMPARE(runs(), QStringList{"4"}); // --inspect and three files, in one process
        QCOMPARE(answers.size(), 3);
        QCOMPARE(answers.value(files[0]).version, QStringLiteral("Fake DiT"));
        QVERIFY(answers.value(files[0]).isDit);
        QCOMPARE(answers.value(files[0]).latentChannels, 16);
        QVERIFY(!answers.value(files[2]).recognised);

        QCOMPARE(inspector.inspect(files).size(), 3); // from memory
        QCOMPARE(runs().size(), 1);

        ModelInspector later(path("helper.sh"), path("cache.json")); // from the cache file, as another process
        QVERIFY(later.cached(files[1]));
        QVERIFY(!later.cached(files[2])->recognised);
        QCOMPARE(later.inspect(files).size(), 3);
        QCOMPARE(runs().size(), 1);
    }

    void aChangedFileIsAskedAgain() {
        write("a.safetensors", "x");
        write("b.safetensors", "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        inspector.inspect({path("a.safetensors"), path("b.safetensors")});
        write("a.safetensors", "a longer file now");
        QVERIFY(!inspector.cached(path("a.safetensors")));
        QVERIFY(inspector.cached(path("b.safetensors")));
        inspector.inspect({path("a.safetensors"), path("b.safetensors")});
        QCOMPARE(runs(), (QStringList{"3", "2"})); // only the changed one the second time
    }

    void anotherHelperDropsWhatTheFirstSaid() {
        write("a.safetensors", "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        inspector.inspect({path("a.safetensors")});
        helper("a newer pin that knows more");
        ModelInspector updated(path("helper.sh"), path("cache.json"));
        QVERIFY(!updated.cached(path("a.safetensors")));
        updated.inspect({path("a.safetensors")});
        QCOMPARE(runs().size(), 2);
    }

    void withoutAHelperNothingIsAskedAndNothingChanges() {
        write("a.safetensors", "x");
        ModelInspector inspector(QString(), path("cache.json"));
        QVERIFY(!inspector.available());
        QVERIFY(inspector.inspect({path("a.safetensors")}).isEmpty());
        QVERIFY(!inspector.cached(path("a.safetensors")));
        QVERIFY(runs().isEmpty());

        ModelInspector broken(path("missing-helper"), path("cache.json"));
        QVERIFY(broken.inspect({path("a.safetensors")}).isEmpty());
    }

    void aFileTheHelperCannotReadHasNoAnswerAndIsNotAskedAgain() {
        write("bad.safetensors", "x");
        write("a.safetensors", "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        const QHash<QString, EngineModel> answers = inspector.inspect({path("bad.safetensors"), path("a.safetensors")});
        QCOMPARE(answers.size(), 1);
        QVERIFY(inspector.inspect({path("bad.safetensors")}).isEmpty());
        QCOMPARE(runs().size(), 1);
    }

    // A helper that dies (exit 3) on a file named "crash", after answering the files before it.
    void crashingHelper() {
        write("crasher.sh", QByteArray("#!/bin/sh\necho \"$#\" >> '" + path("runs.log").toUtf8() + "'\nshift\n" +
                                       R"(for f in "$@"; do
  case "$f" in
    *crash*) exit 3 ;;
    *) printf '{"path":"%s","recognised":true,"version":"Fake DiT","id":"fake_dit","is_dit":true,"video":false,"inpaint":false,"latent_channels":16,"vae_kind":"flux"}\n' "$f" ;;
  esac
done
)"));
        QVERIFY(QFile::setPermissions(path("crasher.sh"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }

    void aCrashOnOneFileCostsTheGoodFilesNothing() {
        crashingHelper();
        for (const char *name : {"a.gguf", "crash.gguf", "b.gguf", "c.gguf"})
            write(name, "x");
        ModelInspector inspector(path("crasher.sh"), path("cache.json"));
        const QStringList files{path("a.gguf"), path("crash.gguf"), path("b.gguf"), path("c.gguf")};
        const QHash<QString, EngineModel> answers = inspector.inspect(files);
        QVERIFY(answers.contains(files[0]) && answers.contains(files[2]) && answers.contains(files[3]));
        QVERIFY(!answers.contains(files[1]));
        // The batch, then the three it did not answer, one process each; the crashing one alone is the one remembered.
        QCOMPARE(runs(), (QStringList{"5", "2", "2", "2"}));
        QCOMPARE(inspector.inspect(files).size(), 3);
        QCOMPARE(runs().size(), 4); // nothing is asked again: the good ones are kept, the bad one is known
        QVERIFY(inspector.cached(files[2]));
    }

    void aHelperThatCrashesOnEverythingIsGivenUpOn() {
        write("crasher.sh", QByteArray("#!/bin/sh\necho \"$#\" >> '" + path("runs.log").toUtf8() + "'\nexit 3\n"));
        QVERIFY(QFile::setPermissions(path("crasher.sh"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QStringList files;
        for (int i = 0; i < 20; ++i) {
            files << path(QStringLiteral("m%1.gguf").arg(i));
            write(QStringLiteral("m%1.gguf").arg(i).toUtf8().constData(), "x");
        }
        ModelInspector inspector(path("crasher.sh"), path("cache.json"));
        QVERIFY(inspector.inspect(files).isEmpty());
        QCOMPARE(runs().size(), 1 + 3); // the batch and three alone, then it stops
        // Nothing was blamed on a file: a helper that works asks them again.
        write("crasher.sh", QByteArray("#!/bin/sh\necho \"$#\" >> '" + path("runs.log").toUtf8() + "'\nfor f in \"$@\"; do case \"$f\" in --inspect) ;; *) printf '{\"path\":\"%s\",\"recognised\":false}\\n' \"$f\" ;; esac; done\n"));
        QVERIFY(QFile::setPermissions(path("crasher.sh"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        inspector.configure(path("crasher.sh"), path("cache.json"));
        QCOMPARE(inspector.inspect(files).size(), 20);
    }

    void theStopFlagIsCheckedBetweenBatches() {
        QStringList files;
        for (int i = 0; i < 90; ++i) {
            files << path(QStringLiteral("m%1.gguf").arg(i));
            write(QStringLiteral("m%1.gguf").arg(i).toUtf8().constData(), "x");
        }
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        const QHash<QString, EngineModel> answers = inspector.inspect(files, [this] { return !runs().isEmpty(); });
        QCOMPARE(runs(), QStringList{"41"}); // the first batch ran, the other two did not
        QCOMPARE(answers.size(), 40);
        QVERIFY(inspector.cached(files[39]));
        QVERIFY(!inspector.cached(files[40]));
    }

    void aProcessIsGivenAFewSecondsNotAMinute() {
        QVERIFY(ModelInspector::askTimeoutMs(1) <= 5000);
        QVERIFY(ModelInspector::askTimeoutMs(40) <= 20000);
        QVERIFY(ModelInspector::askTimeoutMs(40) > ModelInspector::askTimeoutMs(1));
    }

    void theFamilyFallsBackToTheEnginesNameAndTheTableWins() {
        write("mystery.safetensors", "x");
        write("flux-2-klein-9b.gguf", "x");
        write("unknown-thing.gguf", "x");
        write("clip.safetensors", "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        const QList<MarkedFile> files{{path("mystery.safetensors"), 1}, {path("flux-2-klein-9b.gguf"), 1}, {path("unknown-thing.gguf"), 1}};
        const QHash<QString, ModelMark> marks =
            markModels(QStringLiteral("fugue/diffusion-model"), files, RecommendContext{}, ModelShapes::shared(), &inspector);
        QCOMPARE(marks.value(files[0].path).family, QStringLiteral("Fake DiT"));
        QVERIFY(!marks.value(files[0].path).notRecognised);
        QVERIFY(!marks.value(files[0].path).recommended); // no shape rules for a family Fugue does not know
        QCOMPARE(marks.value(files[1].path).family, QStringLiteral("FLUX.2 Klein 9B"));
        QVERIFY(marks.value(files[2].path).family.isEmpty());
        QVERIFY(marks.value(files[2].path).notRecognised);

        // A text encoder is never asked about (a CLIP-L comes back as a diffusion version), and with no inspector nothing is.
        QFile::remove(path("runs.log"));
        QVERIFY(markModels(QStringLiteral("fugue/text-encoder"), {{path("clip.safetensors"), 1}}, RecommendContext{}, ModelShapes::shared(),
                           &inspector)
                    .isEmpty());
        QVERIFY(markModels(QStringLiteral("fugue/diffusion-model"), files, RecommendContext{}, ModelShapes::shared()).value(files[0].path).family.isEmpty());
        QVERIFY(runs().isEmpty());
        QVERIFY(engineDetectsKind(QStringLiteral("fugue/model")));
        QVERIFY(!engineDetectsKind(QStringLiteral("fugue/vae")));
    }

    // The engine's names in the table's style: a fallback lands in the table's group where both are one family.
    void theEnginesNamesAreInTheTablesStyle() {
        const auto family = [](const char *engine) { return engineFamilyName(QString::fromLatin1(engine)); };
        QCOMPARE(family("Flux"), modelFamily(QStringLiteral("fugue/diffusion-model"), QStringLiteral("flux1-dev.safetensors")));
        QCOMPARE(family("Flux Fill"), QStringLiteral("FLUX.1"));
        QCOMPARE(family("Flux.2 klein"), QStringLiteral("FLUX.2 Klein")); // the engine cannot tell 4B from 9B
        QCOMPARE(family("Qwen Image Layered"), modelFamily(QStringLiteral("fugue/diffusion-model"), QStringLiteral("qwen_image_edit.gguf")));
        QCOMPARE(family("Wan 2.2 I2V"), QStringLiteral("Wan 2.2"));
        QCOMPARE(family("Wan 2.x"), QStringLiteral("Wan 2.x"));
        QCOMPARE(family("SDXL Inpaint"), QStringLiteral("SDXL"));
        QCOMPARE(family("SD 1.x Inpaint"), QStringLiteral("SD 1.5"));
        QCOMPARE(family("sd3.x"), QStringLiteral("SD3")); // whole name, any case
        QCOMPARE(family("Chroma Radiance"), QStringLiteral("Chroma"));
        QCOMPARE(family("Z-Image"), QStringLiteral("Z-Image"));
        QCOMPARE(family("Some New Engine Model"), QStringLiteral("Some New Engine Model")); // no entry: as the engine wrote it

        // Through the marks: the engine's "Flux Fill" for a file the table cannot name is the "FLUX.1" group.
        write("fillish.safetensors", "x");
        ModelInspector inspector(path("helper.sh"), path("cache.json"));
        const QList<MarkedFile> files{{path("fillish.safetensors"), 1}};
        QCOMPARE(markModels(QStringLiteral("fugue/diffusion-model"), files, RecommendContext{}, ModelShapes::shared(), &inspector)
                     .value(files[0].path)
                     .family,
                 QStringLiteral("FLUX.1"));
        QCOMPARE(inspector.inspect({files[0].path}).value(files[0].path).id, QStringLiteral("flux_fill")); // the id is the engine's string
    }
};

QTEST_GUILESS_MAIN(TestModelInspect)
#include "test_model_inspect.moc"
