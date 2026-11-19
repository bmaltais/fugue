// The share copy (spec 11.5, 11.7): local absolute paths replaced by bare file names,
// prompts and layout optionally removed. Seam: shareCopy() on a document's JSON with a
// registry of small manifests.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "engine/share_copy.h"

using namespace fugue;

namespace {

QJsonObject json(const QByteArray &text) {
    QByteArray bytes(text);
    bytes.replace('\'', '"');
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (!document.isObject())
        qFatal("bad test JSON: %s in %s", qPrintable(error.errorString()), text.constData());
    return document.object();
}

} // namespace

class TestShareCopy : public QObject {
    Q_OBJECT

    Registry m_registry;

    QJsonValue input(const QJsonObject &document, const QString &node, const QString &name) const {
        return document.value("graph").toObject().value("nodes").toObject().value(node).toObject().value("inputs").toObject().value(name);
    }

private slots:
    void initTestCase() {
        const char *manifests[] = {
            "{'manifest': 1, 'id': 'fugue/loader', 'version': '1.0.0', 'name': 'L', 'runtime': {'kind': 'builtin'},"
            " 'inputs': [{'name': 'file', 'type': 'text', 'widget': 'file'},"
            "            {'name': 'dir', 'type': 'text', 'widget': 'folder', 'optional': true},"
            "            {'name': 'note', 'type': 'text', 'default': ''}],"
            " 'outputs': [{'name': 'text', 'type': 'text'}]}",
            "{'manifest': 1, 'id': 'fugue/prompter', 'version': '1.0.0', 'name': 'P', 'runtime': {'kind': 'builtin'},"
            " 'inputs': [{'name': 'prompt', 'type': 'text', 'widget': 'multiline', 'default': ''},"
            "            {'name': 'title', 'type': 'text', 'default': ''}],"
            " 'outputs': [{'name': 'text', 'type': 'text'}]}",
        };
        for (const char *manifest : manifests) {
            const Errors errors = m_registry.addManifest(json(manifest));
            QVERIFY2(!hasErrors(errors), qPrintable(errors.value(0).message));
        }
    }

    void absolutePathsBecomeBareFileNames() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': '/home/me/pics/fox.png', 'dir': '~/out', 'note': '/not/a/path'}}"
            "}}}");
        const QJsonObject copy = shareCopy(document, m_registry);
        QCOMPARE(input(copy, "a", "file").toString(), QString("fox.png"));
        QCOMPARE(input(copy, "a", "dir").toString(), QString("out"));
        // A text input that is not a path widget is the user's text, whatever it looks like.
        QCOMPARE(input(copy, "a", "note").toString(), QString("/not/a/path"));
        // The original is untouched.
        QCOMPARE(input(document, "a", "file").toString(), QString("/home/me/pics/fox.png"));
    }

    void aRelativePathStays() {
        const QJsonObject copy = shareCopy(
            json("{'fugue': 1, 'graph': {'nodes': {'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': 'pics/fox.png'}}}}}"),
            m_registry);
        QCOMPARE(input(copy, "a", "file").toString(), QString("pics/fox.png"));
    }

    void aLinkedInputIsLeftAlone() {
        const QJsonObject copy = shareCopy(
            json("{'fugue': 1, 'graph': {'nodes': {'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 'b.text'}}}}}}"),
            m_registry);
        QCOMPARE(input(copy, "a", "file").toObject().value("from").toString(), QString("b.text"));
    }

    void anUnknownTypeLosesAbsolutePathsOnly() {
        const QJsonObject copy = shareCopy(
            json("{'fugue': 1, 'graph': {'nodes': {'a': {'type': 'org.x/missing', 'version': '1.0.0',"
                 " 'inputs': {'where': '/data/x/in.wav', 'what': 'a/b', 'list': ['/a/b.png', 'c.png']}}}}}"),
            m_registry);
        QCOMPARE(input(copy, "a", "where").toString(), QString("in.wav"));
        QCOMPARE(input(copy, "a", "what").toString(), QString("a/b"));
        QCOMPARE(input(copy, "a", "list").toArray().at(0).toString(), QString("b.png"));
        QCOMPARE(input(copy, "a", "list").toArray().at(1).toString(), QString("c.png"));
    }

    void promptsStayUnlessAskedToGo() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {'a': {'type': 'fugue/prompter', 'version': '1.0.0',"
            " 'inputs': {'prompt': 'a red fox', 'title': 'Fox'}}}}}");
        QCOMPARE(input(shareCopy(document, m_registry), "a", "prompt").toString(), QString("a red fox"));
        ShareOptions options;
        options.withoutPrompts = true;
        const QJsonObject copy = shareCopy(document, m_registry, options);
        QCOMPARE(input(copy, "a", "prompt").toString(), QString());
        QCOMPARE(input(copy, "a", "title").toString(), QString("Fox"));
    }

    void layoutGoesOnlyWhenAskedAndOtherKeysStay() {
        const QJsonObject document = json("{'fugue': 1, 'graph': {'nodes': {}}, 'layout': {'view': {'zoom': 1}}, 'x-extra': 5}");
        QVERIFY(shareCopy(document, m_registry).contains("layout"));
        ShareOptions options;
        options.withoutLayout = true;
        const QJsonObject copy = shareCopy(document, m_registry, options);
        QVERIFY(!copy.contains("layout"));
        QCOMPARE(copy.value("x-extra").toInt(), 5);
    }

    void groupsAreCleanedInsideAndAtTheirFace() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'g': {'type': 'local/pair', 'version': '1.0.0', 'inputs': {'file': '/home/me/a.png', 'prompt': 'inst'}}}},"
            " 'groups': {'local/pair': {'version': '1.0.0', 'name': 'Pair',"
            "   'inputs': [{'name': 'file', 'targets': ['in.file'], 'default': '/home/me/d.png'},"
            "              {'name': 'prompt', 'targets': ['pr.prompt'], 'default': 'secret'}],"
            "   'outputs': [],"
            "   'nodes': {'in': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': '/home/me/b.png'}},"
            "             'pr': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'prompt': 'inner secret'}}}}}}");
        ShareOptions options;
        options.withoutPrompts = true;
        const QJsonObject copy = shareCopy(document, m_registry, options);
        QCOMPARE(input(copy, "g", "file").toString(), QString("a.png"));
        QCOMPARE(input(copy, "g", "prompt").toString(), QString()); // it drives a prompt input inside
        const QJsonObject group = copy.value("groups").toObject().value("local/pair").toObject();
        QCOMPARE(group.value("nodes").toObject().value("in").toObject().value("inputs").toObject().value("file").toString(),
                 QString("b.png"));
        QCOMPARE(group.value("nodes").toObject().value("pr").toObject().value("inputs").toObject().value("prompt").toString(),
                 QString());
        QCOMPARE(group.value("inputs").toArray().at(0).toObject().value("default").toString(), QString("d.png"));
        QCOMPARE(group.value("inputs").toArray().at(1).toObject().value("default").toString(), QString());
    }

    // A document from elsewhere may have an exposed input that drives nothing yet: it is plain.
    void anExposedInputWithNoTargetsIsLeftAsItIs() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'g': {'type': 'local/empty', 'version': '1.0.0', 'inputs': {'file': '/home/me/a.png', 'other': '/home/me/b.png'}}}},"
            " 'groups': {'local/empty': {'version': '1.0.0', 'name': 'Empty',"
            "   'inputs': [{'name': 'file', 'targets': [], 'default': '/home/me/d.png'}, {'name': 'other'}],"
            "   'outputs': [], 'nodes': {}}}}");
        const QJsonObject copy = shareCopy(document, m_registry);
        QCOMPARE(input(copy, "g", "file").toString(), QString("/home/me/a.png"));
        QCOMPARE(input(copy, "g", "other").toString(), QString("/home/me/b.png"));
        const QJsonObject group = copy.value("groups").toObject().value("local/empty").toObject();
        QCOMPARE(group.value("inputs").toArray().at(0).toObject().value("default").toString(), QString("/home/me/d.png"));
    }
};

QTEST_GUILESS_MAIN(TestShareCopy)
#include "test_share_copy.moc"
