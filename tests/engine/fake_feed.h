#pragma once

// A stand-in for the fugue-workflows feed (engine/template_feed.h) on a FakeHttpServer: a repo of files at one commit,
// served as `commits/main`, `git/trees/<sha>?recursive=1` and `raw/<sha>/<path>`, with the git blob shas a real tree has.
// A test changes the files and serves again for the next commit. Never point a test at GitHub.

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

#include "fake_http.h"

namespace fugue::test {

inline QByteArray gitBlobSha(const QByteArray &bytes) {
    return QCryptographicHash::hash("blob " + QByteArray::number(bytes.size()) + '\0' + bytes, QCryptographicHash::Sha1).toHex();
}

struct FakeRepo {
    QMap<QString, QByteArray> files;
    QMap<QString, QByteArray> served; // path -> bytes the raw server sends instead (a corrupted download)
    QJsonArray extra;                 // more tree entries, as JSON (a symlink, a submodule)
    bool truncated = false;
    QByteArray commit() const {
        QCryptographicHash hash(QCryptographicHash::Sha1);
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            hash.addData(it.key().toUtf8() + '\0' + gitBlobSha(it.value()));
        for (const QJsonValue &entry : extra)
            hash.addData(QJsonDocument(entry.toObject()).toJson());
        return hash.result().toHex();
    }
    void serve(FakeHttpServer &server) const {
        const QByteArray sha = commit();
        server.on("GET", "/commits/main", [sha](const HttpRequest &) { return HttpResponse{200, sha, "text/plain"}; });
        QJsonArray tree = extra;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            tree.append(QJsonObject{{"path", it.key()}, {"mode", "100644"}, {"type", "blob"},
                                    {"sha", QString::fromLatin1(gitBlobSha(it.value()))}, {"size", it.value().size()}});
            const QByteArray bytes = served.value(it.key(), it.value());
            server.on("GET", "/raw/" + sha + "/" + it.key().toUtf8(), [bytes](const HttpRequest &) {
                return HttpResponse{200, bytes, "application/octet-stream"};
            });
        }
        const QByteArray body = QJsonDocument(QJsonObject{{"sha", QString::fromLatin1(sha)}, {"tree", tree}, {"truncated", truncated}}).toJson();
        server.on("GET", "/git/trees/" + sha + "?recursive=1", [body](const HttpRequest &) { return HttpResponse{200, body}; });
    }
};

} // namespace fugue::test
