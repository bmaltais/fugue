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

    // Only a path that names something is a path: a lone separator (Text Concat's "/" feeding a
    // folder) or a bare "~" names no one's files and stays. A folder path ending in a separator
    // keeps its last name.
    void aSeparatorAloneIsNotAPath() {
        const QJsonObject copy = shareCopy(
            json("{'fugue': 1, 'graph': {'nodes': {"
                 " 's': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'title': '/'}},"
                 " 'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 's.text'}, 'dir': '~'}},"
                 " 'b': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': '\\\\\\\\', 'dir': '/home/me/out/'}}"
                 "}}}"),
            m_registry);
        QCOMPARE(input(copy, "s", "title").toString(), QString("/"));
        QCOMPARE(input(copy, "a", "dir").toString(), QString("~"));
        QCOMPARE(input(copy, "b", "file").toString(), QString("\\\\"));
        QCOMPARE(input(copy, "b", "dir").toString(), QString("out"));
        QVERIFY(isLocalAbsolutePath("/home"));
        QVERIFY(isLocalAbsolutePath("~/x"));
        QVERIFY(isLocalAbsolutePath("C:\\Users"));
        QVERIFY(!isLocalAbsolutePath("/"));
        QVERIFY(!isLocalAbsolutePath("~/"));
        QVERIFY(!isLocalAbsolutePath("C:\\"));
    }

    void aLinkedInputIsLeftAlone() {
        const QJsonObject copy = shareCopy(
            json("{'fugue': 1, 'graph': {'nodes': {'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 'b.text'}}}}}}"),
            m_registry);
        QCOMPARE(input(copy, "a", "file").toObject().value("from").toString(), QString("b.text"));
    }

    // A path typed into a text node that feeds a file or folder input is a path too, however
    // many nodes it goes through; text that feeds no path is the user's, whatever it looks like.
    void aPathThatFlowsIntoAFileInputThroughLinksIsStripped() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 't': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'title': '/home/me/pics/fox.png', 'prompt': '~/x/y.png'}},"
            " 'mid': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'title': {'from': 't.text'}, 'prompt': 'C:\\\\Users\\\\me\\\\z.png'}},"
            " 'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 'mid.text'}, 'dir': [{'from': 'd.text'}]}},"
            " 'd': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'title': '/home/me/out'}},"
            " 'other': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'title': '/r/pics style', 'prompt': {'from': 't.text'}}}"
            "}}}");
        const QJsonObject copy = shareCopy(document, m_registry);
        QCOMPARE(input(copy, "t", "title").toString(), QString("fox.png"));
        QCOMPARE(input(copy, "t", "prompt").toString(), QString("y.png"));
        QCOMPARE(input(copy, "mid", "prompt").toString(), QString("z.png"));
        QCOMPARE(input(copy, "mid", "title").toObject().value("from").toString(), QString("t.text"));
        QCOMPARE(input(copy, "d", "title").toString(), QString("out")); // through a list input
        QCOMPARE(input(copy, "other", "title").toString(), QString("/r/pics style")); // feeds no path
        QVERIFY(!QJsonDocument(copy).toJson().contains("/home/me"));
    }

    // A group instance upstream of a path input: what is typed inside its definition can
    // reach the path through the group's outputs, so its absolute paths go too.
    void aGroupThatFeedsAPathLosesItsAbsolutePaths() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'g': {'type': 'local/namer', 'version': '1.0.0', 'inputs': {'start': '/home/me/a'}},"
            " 'a': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 'g.text'}}}}},"
            " 'groups': {'local/namer': {'version': '1.0.0', 'name': 'Namer',"
            "   'inputs': [{'name': 'start', 'targets': ['p.title'], 'default': '/home/me/b'}],"
            "   'outputs': [{'name': 'text', 'from': 'p.text'}],"
            "   'nodes': {'p': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {'prompt': '/home/me/c.png'}}}}}}");
        const QJsonObject copy = shareCopy(document, m_registry);
        QVERIFY2(!QJsonDocument(copy).toJson().contains("/home/me"), QJsonDocument(copy).toJson().constData());
        QCOMPARE(input(copy, "g", "start").toString(), QString("a"));
    }

    // An instance's literal that goes, inside the group, into a node that feeds a path input.
    void aGroupInputThatFeedsAPathInsideIsStripped() {
        const QJsonObject document = json(
            "{'fugue': 1, 'graph': {'nodes': {"
            " 'h': {'type': 'local/opener', 'version': '1.0.0', 'inputs': {'start': '/home/me/q.png', 'note': '/r/pics style'}}}},"
            " 'groups': {'local/opener': {'version': '1.0.0', 'name': 'Opener',"
            "   'inputs': [{'name': 'start', 'targets': ['p.title'], 'default': '/home/me/d.png'},"
            "              {'name': 'note', 'targets': ['n.title']}],"
            "   'outputs': [],"
            "   'nodes': {'p': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {}},"
            "             'n': {'type': 'fugue/prompter', 'version': '1.0.0', 'inputs': {}},"
            "             'l': {'type': 'fugue/loader', 'version': '1.0.0', 'inputs': {'file': {'from': 'p.text'}}}}}}}");
        const QJsonObject copy = shareCopy(document, m_registry);
        QCOMPARE(input(copy, "h", "start").toString(), QString("q.png"));
        QCOMPARE(input(copy, "h", "note").toString(), QString("/r/pics style"));
        const QJsonObject group = copy.value("groups").toObject().value("local/opener").toObject();
        QCOMPARE(group.value("inputs").toArray().at(0).toObject().value("default").toString(), QString("d.png"));
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
