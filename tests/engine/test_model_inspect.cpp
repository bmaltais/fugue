// What the engine says a model file is (#703, engine/model_inspect.h): the inspector with a fake helper script (one process for a
// batch, an answer kept by path, size and time, no helper, a failed file), and the marks that use it (the family fallback, the
// not-recognised mark). No real helper or model: the script answers by the file's name.

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

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
    *) printf '{"path":"%s","recognised":true,"version":"Fake DiT","id":3,"is_dit":true,"video":false,"inpaint":false,"latent_channels":16,"vae_kind":"flux"}\n' "$f" ;;
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
};

QTEST_GUILESS_MAIN(TestModelInspect)
#include "test_model_inspect.moc"
