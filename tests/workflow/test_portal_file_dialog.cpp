// The portal file picker's parts that need no bus (src/menu/portal_file_dialog.h): the Qt name
// filters as the portal's filters, the call's options, the Request path, the Response, the default
// suffix, and what PortalFileDialog does with no session bus (the tests have none).
// The call shape follows https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html;
// no real portal is asked.

#include <QDBusArgument>
#include <QSignalSpy>
#include <QtTest>

#include "menu/portal_file_dialog.h"

using namespace fugue;
using namespace fugue::portal;

namespace {
QList<QString> patterns(const Filter &filter) {
    QList<QString> out;
    for (const FilterRule &rule : filter.rules)
        out << rule.pattern;
    return out;
}
} // namespace

class TestPortalFileDialog : public QObject {
    Q_OBJECT

private slots:
    void aQtNameFilterBecomesAPortalFilter() {
        const auto filters = filtersFromNameFilters({"Fugue workflows (*.fugue)"});
        QCOMPARE(filters.size(), 1);
        QCOMPARE(filters[0].name, QString("Fugue workflows"));
        QCOMPARE(patterns(filters[0]), (QList<QString>{"*.fugue", "*.FUGUE"})); // globs are case-sensitive: the uppercase twin too
        QCOMPARE(filters[0].rules[0].kind, 0u);                                   // 0 is a glob
    }

    void severalGlobsAndAllFiles() {
        const auto filters = filtersFromNameFilters(
            {"Pictures and canvases (*.png *.jpg *.ora)", "All files (*)", "*.json", "Nothing in it ()", "no pattern"});
        QCOMPARE(filters.size(), 3);
        QCOMPARE(patterns(filters[0]), (QList<QString>{"*.png", "*.PNG", "*.jpg", "*.JPG", "*.ora", "*.ORA"}));
        QCOMPARE(filters[1].name, QString("All files"));
        QCOMPARE(patterns(filters[1]), (QList<QString>{"*"}));
        QCOMPARE(filters[2].name, QString("*.json")); // no name: the pattern is it
        QCOMPARE(patterns(filters[2]), (QList<QString>{"*.json", "*.JSON"}));
    }

    void aLongNameWithParenthesesKeepsItsName() {
        const auto filters =
            filtersFromNameFilters({"Fugue workflows, packages and images with one (*.fugue *.fuguepack *.png *.webp)"});
        QCOMPARE(filters.size(), 1);
        QCOMPARE(filters[0].name, QString("Fugue workflows, packages and images with one"));
        QCOMPARE(filters[0].rules.size(), 8);
    }

    void filtersMarshalAsTheSignatureTheServiceWants() {
        registerDBusTypes();
        QDBusArgument arg;
        arg << filtersFromNameFilters({"JSON (*.json)"});
        QCOMPARE(arg.currentSignature(), QString("a(sa(us))"));
        QDBusArgument one;
        one << filtersFromNameFilters({"JSON (*.json)"}).first();
        QCOMPARE(one.currentSignature(), QString("(sa(us))"));
    }

    void theFolderIsANulTerminatedByteArray() {
        const QByteArray bytes = folderBytes(QUrl::fromLocalFile("/home/a b/Documents"));
        QCOMPARE(bytes, QByteArray("/home/a b/Documents\0", 20));
        QCOMPARE(bytes.size(), 20);
    }

    void saveOptions() {
        Request request;
        request.mode = Mode::SaveFile;
        request.token = "fugue_1_1";
        request.filters = filtersFromNameFilters({"Fugue workflows (*.fugue)", "All files (*)"});
        request.folder = QUrl::fromLocalFile("/home/a/Documents/Fugue");
        request.name = "scene.fugue";
        const QVariantMap options = portal::options(request);
        QCOMPARE(options.value("handle_token").toString(), QString("fugue_1_1"));
        QCOMPARE(options.value("modal").toBool(), true);
        QCOMPARE(options.value("current_name").toString(), QString("scene.fugue"));
        QCOMPARE(options.value("current_folder").toByteArray(), QByteArray("/home/a/Documents/Fugue\0", 24));
        QCOMPARE(options.value("filters").value<QList<Filter>>().size(), 2);
        QCOMPARE(options.value("current_filter").value<Filter>().name, QString("Fugue workflows")); // the first one
        QVERIFY(!options.contains("multiple"));
        QVERIFY(!options.contains("directory"));
        QCOMPARE(methodFor(request.mode), QString("SaveFile"));
    }

    void openOptionsPerMode() {
        Request request;
        request.token = "t";
        QCOMPARE(methodFor(request.mode), QString("OpenFile"));
        QVERIFY(!portal::options(request).contains("multiple"));
        QVERIFY(!portal::options(request).contains("current_name"));
        QVERIFY(!portal::options(request).contains("filters")); // none given, none sent
        request.mode = Mode::OpenFiles;
        QCOMPARE(portal::options(request).value("multiple").toBool(), true);
        request.mode = Mode::Directory;
        QCOMPARE(methodFor(request.mode), QString("OpenFile"));
        QCOMPARE(portal::options(request).value("directory").toBool(), true);
        request.name = "ignored";
        QVERIFY(!portal::options(request).contains("current_name")); // only a save names a file
    }

    void theRequestPathFollowsTheUniqueName() {
        QCOMPARE(requestPath(":1.42", "fugue_7_3"), QString("/org/freedesktop/portal/desktop/request/1_42/fugue_7_3"));
    }

    void anAcceptedResponseHasFileUrls() {
        const Response response = parseResponse(0, {{"uris", QStringList{"file:///home/a/one.png", "file:///home/a/two%20x.png"}}});
        QCOMPARE(response.status, Response::Accepted);
        QCOMPARE(response.urls, (QList<QUrl>{QUrl::fromLocalFile("/home/a/one.png"), QUrl::fromLocalFile("/home/a/two x.png")}));
    }

    void cancelledAndErrorsAreNotAccepted() {
        QCOMPARE(parseResponse(1, {}).status, Response::Cancelled);
        QCOMPARE(parseResponse(2, {}).status, Response::Failed);
        QCOMPARE(parseResponse(0, {}).status, Response::Failed);                                       // accepted with nothing
        QCOMPARE(parseResponse(0, {{"uris", QStringList{"https://example.com/x"}}}).status, Response::Failed); // not a local file
        QCOMPARE(parseResponse(1, {{"uris", QStringList{"file:///x"}}}).urls.size(), 0);
    }

    void aNameWithoutAnExtensionGetsTheDefaultSuffix() {
        QCOMPARE(withDefaultSuffix(QUrl::fromLocalFile("/a/scene"), "fugue"), QUrl::fromLocalFile("/a/scene.fugue"));
        QCOMPARE(withDefaultSuffix(QUrl::fromLocalFile("/a/scene"), ".fugue"), QUrl::fromLocalFile("/a/scene.fugue"));
        QCOMPARE(withDefaultSuffix(QUrl::fromLocalFile("/a/scene.json"), "fugue"), QUrl::fromLocalFile("/a/scene.json"));
        QCOMPARE(withDefaultSuffix(QUrl::fromLocalFile("/a/scene"), ""), QUrl::fromLocalFile("/a/scene"));
    }

    // The tests have no session bus: the portal is not available and a call fails at once, which is
    // what makes FilePicker open Qt's dialog (tests/fugue_tests.cpp).
    void withNoBusThePortalIsUnavailableAndOpenFails() {
        PortalFileDialog dialog;
        QVERIFY(!dialog.available());
        QSignalSpy failed(&dialog, &PortalFileDialog::failed);
        QSignalSpy accepted(&dialog, &PortalFileDialog::accepted);
        dialog.open();
        QCOMPARE(failed.size(), 1);
        QCOMPARE(accepted.size(), 0);
        QVERIFY(!dialog.visible());
    }

    void selectedFileCanBeSetBeforeOpen() {
        PortalFileDialog dialog;
        QVERIFY(dialog.selectedFile().isEmpty());
        dialog.setSelectedFile(QUrl::fromLocalFile("/a/b.png"));
        QCOMPARE(dialog.selectedFile(), QUrl::fromLocalFile("/a/b.png"));
        QCOMPARE(dialog.selectedFiles(), (QList<QUrl>{QUrl::fromLocalFile("/a/b.png")}));
    }
};

QTEST_GUILESS_MAIN(TestPortalFileDialog)
#include "test_portal_file_dialog.moc"
