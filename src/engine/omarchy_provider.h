#pragma once

// The omarchy provider (spec 8.7): an adapter compiled into Fugue that serves capabilities from
// the model servers omarchy-local-ai runs, on this machine or on a paired tailnet host. It
// talks to them over HTTP with QtNetwork; no inference code is linked in.
//
// Sources the shapes come from (read, not run):
// - omarchy-local-ai origin/master 703ae0c: agent_local.py (model_record, write_status,
//   key_path, remote_key_path, load_remotes, clean_listing, remote_record, cmd_pair) and
//   docs/engine.md ("State and panel API", "Tailnet sharing").
// - stable-diffusion.cpp 532d2c15 (the commit Omarchy's sd-server image is built from):
//   examples/server/api.md ("sdcpp API": capabilities, img_gen, jobs, cancel, Completion
//   Result) and examples/server/async_jobs.cpp (make_async_job_json).
// - Fantasi 6aa8dc27 src/models.cpp and src/generator.cpp, via the workbench note
//   fugue/map/research/03-model-server-apis.md (endpoint minus /v1 is the native base; poll
//   every 500 ms; 15 s per request).
//
// Discovery:
// - No address: ~/.local/state/omarchy/agent-local/status.json, key
//   ~/.config/omarchy/agent-local/key.
// - An address (https://<host>.<tailnet>.ts.net): the rows of that host in
//   ~/.local/state/omarchy/agent-local/network.json that are paired, and the key
//   ~/.config/omarchy/agent-local/remotes/<host>.key that `omarchy-agent-local pair <host>`
//   stored. Their endpoints are https://<dns>/local-ai/<recipe>/v1 through the share proxy.
// The instance's own key, when set, replaces the key file.
//
// Each row of kind image becomes a model of image.generate, and also of image.edit when its
// `edit` is true. Each row of kind text becomes a model of text.generate (a fugue/text-model),
// served over its OpenAI-compatible API (POST /v1/chat/completions, streamed). Each row of kind
// video becomes a model of video.generate (a fugue/video-model), served by sd-server's
// POST /sdcpp/v1/vid_gen through the same job API as images; each row of kind music a model of
// audio.generate (a fugue/music-model), served by ace-server in two jobs (POST /lm, then
// POST /synth; GET /job?id=N). ace-server: acestep.cpp 973517be, docs/ARCHITECTURE.md and
// tools/ace-server.cpp. A served model is named by
// its recipe, with no content hash; its row's
// `name` ("FLUX.2 Klein 4B") is its title, for display only. Without providers.json each
// paired host is an instance of its own (providers.h defaultProviderInstances).

#include <QHash>
#include <QJsonArray>

#include <functional>
#include <QNetworkAccessManager>
#include <QTimer>

#include "engine/providers.h"

class QNetworkReply;

namespace fugue {

struct OmarchyPaths {
    QString stateDir;  // holds status.json and network.json
    QString configDir; // holds key and remotes/<host>.key

    // ~/.local/state/omarchy/agent-local and ~/.config/omarchy/agent-local. These are
    // omarchy-local-ai's folders, and it builds them from $HOME with no XDG variable, so
    // neither does this.
    static OmarchyPaths standard();
};

class OmarchyProvider : public Provider {
    Q_OBJECT

public:
    // Instance settings: `poll_ms`, how often running jobs are polled (default 500).
    OmarchyProvider(ProviderInstance instance, OmarchyPaths paths, QObject *parent = nullptr);
    ~OmarchyProvider() override;

    void describe(Reply<Description> reply) override;
    void submit(const JobRequest &request, Reply<QString> reply) override;
    void status(const QString &job, Reply<JobStatus> reply) override;
    void cancel(const QString &job, Reply<QString> reply) override;
    void result(const QString &job, Reply<JobResult> reply) override;

    // The time in milliseconds that finished jobs are aged by; the system clock when
    // unset. For tests.
    void setClock(std::function<qint64()> msecs) { m_clock = std::move(msecs); }

private:
    // One model server: where it is and how to reach it.
    struct Server {
        QString recipe;
        QString base; // the endpoint without its /v1: the native API's root
        QByteArray key;
        QString runtime; // the row's runtime (gufo, tensorfold-x86): a text job's engine
        QJsonArray loras; // a video server's capabilities.loras: sent with every job (8.7)
    };
    struct Discovery {
        Description description;
        QHash<QString, Server> servers; // by recipe
    };
    struct Job;

    void discover(std::function<void(const Result<Discovery> &)> done);
    // `timeoutMs`: how long the transfer may go silent; 0 keeps the network's 15 seconds.
    QNetworkReply *send(const Server &server, const QByteArray &method, const QString &path,
                        const QByteArray &body = {}, int timeoutMs = 0);
    void submitChat(const Server &server, const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    void readChat(const QString &id, bool atEnd = false);
    void finishChat(const QString &id);
    void poll();
    void pollJob(const QString &id);
    void finishJob(const QString &id, const QJsonObject &job);
    void finishVideo(const QString &id, const QJsonObject &result);
    void submitSong(const Server &server, const JobRequest &request, const QJsonObject &model, Reply<QString> reply);
    void pollSong(const QString &id);
    void fetchSong(const QString &id);
    void change(const QString &id, const JobStatus &status);
    qint64 now() const;
    void forgetOldJobs();

    ProviderInstance m_instance;
    OmarchyPaths m_paths;
    QNetworkAccessManager m_network;
    QTimer m_poll;
    // Jobs from submit until their result is taken, or until 600 seconds after they
    // ended when nobody takes one (forgetOldJobs).
    QHash<QString, std::shared_ptr<Job>> m_jobs;
    std::function<qint64()> m_clock;
    int m_chats = 0; // chat jobs so far, for their ids
};

} // namespace fugue
