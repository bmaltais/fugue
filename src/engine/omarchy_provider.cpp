#include "omarchy_provider.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace fugue {

namespace {

// Fantasi gives each request 15 seconds (generator.cpp setTransferTimeout).
constexpr int kTransferTimeoutMs = 15000;
constexpr int kDefaultPollMs = 500;

const QString kEngine = QStringLiteral("sdcpp");
const QString kModelResource = QStringLiteral("fugue/image-model");

QJsonObject readObject(const QString &path, bool *exists) {
    QFile file(path);
    *exists = file.open(QIODevice::ReadOnly);
    return *exists ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}

QByteArray readKey(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
}

// The native API's root: the endpoint without its trailing /v1. Not the URL's origin, because a
// paired host's endpoint has a path (/local-ai/<recipe>/v1).
QString baseOf(const QString &endpoint) {
    QString base = endpoint;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.endsWith(QLatin1String("/v1")))
        base.chop(3);
    return base;
}

QJsonObject jsonBody(QNetworkReply *reply) { return QJsonDocument::fromJson(reply->readAll()).object(); }

int httpStatus(QNetworkReply *reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

// Defaults (8.1 names) from sd-server's capabilities: defaults_by_mode.img_gen, or the
// deprecated `defaults` mirror (api.md "GET /sdcpp/v1/capabilities").
QJsonObject defaultsOf(const QJsonObject &capabilities) {
    QJsonObject mode = capabilities.value("defaults_by_mode").toObject().value("img_gen").toObject();
    if (mode.isEmpty())
        mode = capabilities.value("defaults").toObject();
    const QJsonObject sample = mode.value("sample_params").toObject();
    QJsonObject defaults;
    for (const char *key : {"width", "height"})
        if (mode.contains(QLatin1String(key)))
            defaults.insert(QLatin1String(key), mode.value(QLatin1String(key)));
    if (sample.contains("sample_steps"))
        defaults.insert("steps", sample.value("sample_steps"));
    const QJsonObject guidance = sample.value("guidance").toObject();
    if (guidance.contains("txt_cfg"))
        defaults.insert("guidance", guidance.value("txt_cfg"));
    return defaults;
}

QJsonObject limitsOf(const QJsonObject &capabilities) {
    const QJsonObject all = capabilities.value("limits").toObject();
    QJsonObject limits;
    for (const char *key : {"min_width", "max_width", "min_height", "max_height"})
        if (all.contains(QLatin1String(key)))
            limits.insert(QLatin1String(key), all.value(QLatin1String(key)));
    return limits;
}

} // namespace

struct OmarchyProvider::Job {};

OmarchyPaths OmarchyPaths::standard() {
    return OmarchyPaths{QDir::home().filePath(QStringLiteral(".local/state/omarchy/agent-local")),
                        QDir::home().filePath(QStringLiteral(".config/omarchy/agent-local"))};
}

OmarchyProvider::OmarchyProvider(ProviderInstance instance, OmarchyPaths paths, QObject *parent)
    : Provider(parent), m_instance(std::move(instance)), m_paths(std::move(paths)) {
    m_network.setTransferTimeout(kTransferTimeoutMs);
    m_poll.setInterval(m_instance.settings.value("poll_ms").toInt(kDefaultPollMs));
    connect(&m_poll, &QTimer::timeout, this, &OmarchyProvider::poll);
}

OmarchyProvider::~OmarchyProvider() = default;

QNetworkReply *OmarchyProvider::send(const Server &server, const QByteArray &method,
                                     const QString &path, const QByteArray &body) {
    QNetworkRequest request(QUrl(server.base + path));
    request.setRawHeader("Authorization", "Bearer " + server.key);
    if (!body.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    return m_network.sendCustomRequest(request, method, body);
}

void OmarchyProvider::discover(std::function<void(const Outcome<Discovery> &)> done) {
    const QDir state(m_paths.stateDir);
    const QDir config(m_paths.configDir);
    const bool remote = !m_instance.address.isEmpty();
    const QString dns = QUrl(m_instance.address).host();
    const QString host = dns.section(QLatin1Char('.'), 0, 0);
    const auto fail = [&](const QString &message, const QString &hint) {
        done(Outcome<Discovery>{{}, {providerError(QStringLiteral("provider_unreachable"), message, hint)}});
    };

    // The rows to serve from, and the key for them.
    QList<QJsonObject> rows;
    QByteArray key = m_instance.key.toUtf8();
    if (!remote) {
        bool exists = false;
        const QJsonObject status = readObject(state.filePath("status.json"), &exists);
        if (!exists)
            return fail(QStringLiteral("Omarchy's local models are not set up on this machine (no %1).")
                            .arg(state.filePath("status.json")),
                        QStringLiteral("Install omarchy-local-ai, then set up an image model: "
                                       "omarchy-agent-local use <recipe>."));
        const QJsonObject models = status.value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it)
            rows.append(it.value().toObject());
        if (key.isEmpty())
            key = readKey(config.filePath("key"));
        if (key.isEmpty())
            return fail(QStringLiteral("No key to talk to Omarchy's model servers (no %1).").arg(config.filePath("key")),
                        QStringLiteral("Set up a model with omarchy-agent-local, which writes the key, "
                                       "or set this provider's key in ~/.config/fugue/providers.json."));
    } else {
        bool exists = false;
        const QJsonObject network = readObject(state.filePath("network.json"), &exists);
        const QJsonObject models = network.value("models").toObject();
        for (auto it = models.begin(); it != models.end(); ++it) {
            const QJsonObject row = it.value().toObject();
            if (row.value("dns").toString() == dns && row.value("paired").toBool())
                rows.append(row);
        }
        const QString pairHint = QStringLiteral("Pair with it: omarchy-agent-local pair %1").arg(host);
        if (rows.isEmpty())
            return fail(QStringLiteral("This machine is not paired with %1.").arg(dns), pairHint);
        if (key.isEmpty())
            key = readKey(config.filePath(QStringLiteral("remotes/%1.key").arg(host)));
        if (key.isEmpty())
            return fail(QStringLiteral("No key for %1 (no %2).").arg(dns, config.filePath("remotes/" + host + ".key")),
                        pairHint);
    }

    // One model per image row; running servers are asked for their capabilities.
    struct Pending {
        Discovery discovery;
        QList<OfferedModel> models;
        QList<bool> edits;
        int waiting = 0;
        std::function<void(const Outcome<Discovery> &)> done;
    };
    auto pending = std::make_shared<Pending>();
    pending->done = std::move(done);
    const auto assemble = [pending] {
        Description &description = pending->discovery.description;
        description.engine = kEngine;
        CapabilityOffer generate{QStringLiteral("image.generate"), pending->models};
        CapabilityOffer edit{QStringLiteral("image.edit"), {}};
        for (qsizetype i = 0; i < pending->models.size(); ++i)
            if (pending->edits.at(i))
                edit.models.append(pending->models.at(i));
        if (!generate.models.isEmpty())
            description.capabilities.append(generate);
        if (!edit.models.isEmpty())
            description.capabilities.append(edit);
        pending->done(Outcome<Discovery>{pending->discovery, {}});
    };

    const QString where = remote ? QStringLiteral(" on %1").arg(host) : QString();
    for (const QJsonObject &row : rows) {
        if (row.value("kind").toString() != QLatin1String("image"))
            continue;
        const QString recipe = row.value("recipe").toString();
        const QString unit = row.value("unit").toString();
        const Server server{recipe, baseOf(row.value("endpoint").toString()), key};
        pending->discovery.servers.insert(recipe, server);

        OfferedModel model;
        model.handle = QJsonObject{{"resource", kModelResource}, {"name", recipe}};
        model.properties = QJsonObject{{"edit", row.value("edit").toBool()},
                                       {"input", row.value("input").isArray() ? row.value("input")
                                                                               : QJsonArray{"text"}}};
        const QString command = QStringLiteral("omarchy-agent-local start ") + recipe;
        const QString offline = row.value("offline").toString();
        if (!offline.isEmpty()) {
            model.status = QStringLiteral("unavailable");
            model.reason = QStringLiteral("%1 is %2.").arg(host, offline);
            model.hint = QStringLiteral("Turn on %1, or check its tailnet connection.").arg(host);
        } else if (unit == QLatin1String("active")) {
            model.status = QStringLiteral("ready");
        } else if (unit == QLatin1String("activating")) {
            model.status = QStringLiteral("loading");
            model.reason = QStringLiteral("%1 is starting.").arg(recipe);
            model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
        } else if (unit == QLatin1String("installing")) {
            model.status = QStringLiteral("unavailable");
            model.reason = QStringLiteral("%1 is still being set up.").arg(recipe);
            model.hint = QStringLiteral("Wait for its setup to finish, then start it.");
        } else {
            model.status = QStringLiteral("unavailable");
            const QString error = row.value("error").toString();
            const QString fit = row.value("fits").toBool(true) ? QString() : row.value("fit_reason").toString();
            model.reason = !error.isEmpty() ? error
                           : !fit.isEmpty() ? fit
                                            : QStringLiteral("%1 is stopped%2.").arg(recipe, where);
            model.hint = (fit.isEmpty() ? QStringLiteral("Start it%1: %2") : QStringLiteral("Free memory, then start it%1: %2"))
                             .arg(where, command);
        }
        const qsizetype index = pending->models.size();
        pending->models.append(model);
        pending->edits.append(row.value("edit").toBool());
        if (model.status != QLatin1String("ready"))
            continue;

        ++pending->waiting;
        QNetworkReply *reply = send(server, "GET", QStringLiteral("/sdcpp/v1/capabilities"));
        connect(reply, &QNetworkReply::finished, this, [pending, reply, index, recipe, server, assemble, this] {
            reply->deleteLater();
            OfferedModel &model = pending->models[index];
            const int code = httpStatus(reply);
            const QJsonObject body = jsonBody(reply);
            if (code == 200) {
                model.defaults = defaultsOf(body);
                model.limits = limitsOf(body);
            } else if (code == 503 && body.value("code").toString() == QLatin1String("model_loading")) {
                model.status = QStringLiteral("loading");
                model.reason = QStringLiteral("%1 is loading its model.").arg(recipe);
                model.hint = QStringLiteral("Wait for %1 to finish loading.").arg(recipe);
            } else if (code == 401 || code == 403) {
                model.status = QStringLiteral("unavailable");
                model.reason = QStringLiteral("%1 refused the key.").arg(recipe);
                model.hint = m_instance.key.isEmpty()
                                 ? QStringLiteral("Check Omarchy's key file, or pair again.")
                                 : QStringLiteral("Check this provider's key in ~/.config/fugue/providers.json.");
            } else {
                model.status = QStringLiteral("unavailable");
                model.reason = code == 0 ? QStringLiteral("%1 is not answering at %2.").arg(recipe, server.base)
                                         : QStringLiteral("%1 answered HTTP %2.").arg(recipe).arg(code);
                model.hint = QStringLiteral("Restart it: omarchy-agent-local stop %1, then start %1.").arg(recipe);
            }
            if (--pending->waiting == 0)
                assemble();
        });
    }
    if (pending->waiting == 0)
        QTimer::singleShot(0, this, assemble);
}

void OmarchyProvider::describe(Reply<Description> reply) {
    discover([reply = std::move(reply)](const Outcome<Discovery> &found) {
        reply(Outcome<Description>{found.value.description, found.errors});
    });
}

void OmarchyProvider::submit(const JobRequest &, Reply<QString> reply) {
    later(std::move(reply), Outcome<QString>{{}, {providerError("internal", "Not built yet.")}});
}

void OmarchyProvider::status(const QString &, Reply<JobStatus> reply) {
    later(std::move(reply), Outcome<JobStatus>{{}, {providerError("internal", "Not built yet.")}});
}

void OmarchyProvider::cancel(const QString &, Reply<QString> reply) {
    later(std::move(reply), Outcome<QString>{{}, {providerError("internal", "Not built yet.")}});
}

void OmarchyProvider::result(const QString &, Reply<JobResult> reply) {
    later(std::move(reply), Outcome<JobResult>{{}, {providerError("internal", "Not built yet.")}});
}

void OmarchyProvider::poll() {}

void OmarchyProvider::pollJob(const QString &) {}

void OmarchyProvider::finishJob(const QString &, const QJsonObject &) {}

void OmarchyProvider::change(const QString &, const JobStatus &) {}

} // namespace fugue
