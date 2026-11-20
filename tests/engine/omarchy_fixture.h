#pragma once

// omarchy-local-ai's state as a real machine paired with a tailnet host has it, for tests:
// tests/engine/data/omarchy/{status,network}.json were copied from one (2026-11-19) and
// anonymized. This machine sets up only text models; `studio` is paired and online with two
// image models (FLUX.2 Klein 4B, which edits, and Z-Image Turbo) and a text model; `lab` is
// paired and offline; `neighbour` shares a model but is not paired.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include "engine/omarchy_provider.h"
#include "fake_http.h"

#ifndef FUGUE_SOURCE_DIR
#error "omarchy_fixture.h needs FUGUE_SOURCE_DIR"
#endif

namespace fugue::test {

inline bool writeFixtureFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

inline QByteArray fixtureBytes(const QString &name) {
    QFile file(QStringLiteral(FUGUE_SOURCE_DIR "/tests/engine/data/omarchy/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

// Writes the state under `home` where OmarchyPaths::standard() looks when $HOME is `home`
// (.local/state/omarchy/agent-local and .config/omarchy/agent-local), with a key for this
// machine and one for each paired host. `origin` (http://127.0.0.1:<port>), when set,
// replaces https://studio.tail1234.ts.net in the endpoints, so studio's models are asked
// on a fake server; the hosts keep their dns names, which is what an instance's address
// matches.
inline OmarchyPaths writeOmarchyState(const QString &home, const QString &origin = {}) {
    const OmarchyPaths paths{home + QStringLiteral("/.local/state/omarchy/agent-local"),
                             home + QStringLiteral("/.config/omarchy/agent-local")};
    QByteArray network = fixtureBytes(QStringLiteral("network.json"));
    if (!origin.isEmpty())
        network.replace("https://studio.tail1234.ts.net/", origin.toUtf8() + '/');
    writeFixtureFile(paths.stateDir + QStringLiteral("/status.json"), fixtureBytes(QStringLiteral("status.json")));
    writeFixtureFile(paths.stateDir + QStringLiteral("/network.json"), network);
    writeFixtureFile(paths.configDir + QStringLiteral("/key"), "local-test-key");
    writeFixtureFile(paths.configDir + QStringLiteral("/remotes/studio.key"), "studio-test-key");
    writeFixtureFile(paths.configDir + QStringLiteral("/remotes/lab.key"), "lab-test-key");
    return paths;
}

// studio's sd-servers through the share proxy: each model's capabilities (api.md "GET
// /sdcpp/v1/capabilities"), for studio's key only.
inline void serveStudio(FakeHttpServer &server) {
    const QJsonObject capabilities{
        {"supported_modes", QJsonArray{"img_gen"}},
        {"defaults_by_mode",
         QJsonObject{{"img_gen", QJsonObject{{"width", 1024}, {"height", 1024},
                                             {"sample_params", QJsonObject{{"sample_steps", 4}}}}}}},
        {"limits", QJsonObject{{"min_width", 64}, {"max_width", 4096}, {"min_height", 64}, {"max_height", 4096}}}};
    const QByteArray body = QJsonDocument(capabilities).toJson(QJsonDocument::Compact);
    for (const char *recipe : {"flux2-klein-4b-sdcpp-cuda", "z-image-turbo-sdcpp-cuda"})
        server.on("GET", QByteArray("/local-ai/") + recipe + "/sdcpp/v1/capabilities", [body](const HttpRequest &request) {
            if (request.headers.value("authorization") != "Bearer studio-test-key")
                return HttpResponse{401, "{}"};
            return HttpResponse{200, body};
        });
}

// A machine paired with studio, whose image models answer on a fake server, and with no
// providers.json: $HOME and $XDG_CONFIG_HOME point into a temporary folder while it lives,
// so the standard paths (OmarchyPaths::standard(), providersPath()) find it.
struct PairedHome {
    QTemporaryDir dir;
    FakeHttpServer server;
    OmarchyPaths paths;
    QByteArray home = qgetenv("HOME");
    QByteArray config = qgetenv("XDG_CONFIG_HOME");

    PairedHome() {
        server.listen();
        serveStudio(server);
        paths = writeOmarchyState(dir.path(), QString::fromLatin1(server.origin()));
        qputenv("HOME", dir.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", dir.filePath("config").toUtf8());
    }
    ~PairedHome() {
        qputenv("HOME", home);
        if (config.isNull())
            qunsetenv("XDG_CONFIG_HOME");
        else
            qputenv("XDG_CONFIG_HOME", config);
    }
    PairedHome(const PairedHome &) = delete;
    PairedHome &operator=(const PairedHome &) = delete;
};

} // namespace fugue::test
