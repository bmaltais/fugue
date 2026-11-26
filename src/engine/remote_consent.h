#pragma once

// Which provider instances send a run's inputs off this machine, and which of them the person has allowed (spec 8.2, #404).
//
// Where an instance runs (placementOf), from its type and address:
//   - ThisMachine: no address, or a loopback one (localhost, 127.0.0.1, ::1): nothing leaves the machine.
//   - OwnHost: a paired host (an `omarchy` instance with an address, 8.7, the person's own omarchy hosts) or any other instance
//     (a `fugue-helper` included: it has no pairing step) whose address is on the person's own network: a private (RFC 1918,
//     unique local), link-local or CGNAT (Tailscale) address, or a `.local`, `.internal` or `.ts.net` name.
//     The address is the one the provider really uses: a module's default when the instance has none. It is the same test remote_request.h isLocalHost() applies to what
//     a service may make Fugue fetch, so there is one idea of "local". The person typed that address in, so nothing is asked.
//   - OffMachine: an instance of any other type whose address is none of those: a public name or address. An `omarchy`
//     instance is never off-machine (it is a paired host by definition). A stdio provider module is a program that runs here:
//     it counts as this machine even if the program itself calls a cloud API.
//
// A run that routes a node to an off-machine instance asks once per instance. The yes is kept in `remote-consent.json` beside
// trust.json in the state folder of the root, per instance name with the instance's fingerprint (its type and address):
//
//   {"instances": {"My endpoint": "http|https://api.example.com/v1"}}
//
// A changed address or type no longer matches, so it asks again, and an instance that is no longer configured is dropped from the
// file (prune), so removing an instance forgets the answer. A pack node with the `network` permission is not asked here: the
// pack's trust (6.7) already says so.

#include "engine/providers.h"
#include "engine/result.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace fugue {

// Ordered nearest to farthest: placementOf takes the max of a fal instance's two hosts, so keep this order.
enum class Placement { ThisMachine, OwnHost, OffMachine };

Placement placementOf(const ProviderInstance &instance);
// The host an instance is at, for people to read: its address's host, empty without one.
QString hostOf(const ProviderInstance &instance);
// The address the provider really uses: the instance's, else the default of its provider module (an http module that has one).
QString effectiveAddress(const ProviderInstance &instance);
// What a yes is for: the instance's type and address, so a changed address is another service.
QString consentFingerprint(const ProviderInstance &instance);

// What a node routed to an instance hands over, from its inputs: "pictures", "masks", "audio", "video", "prompts" (text
// inputs that are not a choice), in that order, each once. Declared by the node's type; an input counts when it is written
// (a value or a link) or a text input has a default.
struct NodeType;
struct CompiledNode;
QStringList sendsOf(const NodeType &type, const CompiledNode &node);

// A node routed to an instance, as a check or run reports it.
struct NodeRoute {
    QString instance;
    QStringList sends;
};

// One off-machine instance a run would use, and what would be sent there.
struct RemoteUse {
    QString instance;
    QString host;
    QString fingerprint; // consentFingerprint of the instance as it was when the use was found: what a yes is recorded for
    QStringList sends; // the kinds of every node that is routed there and would run, in order, each once
    QStringList nodes; // path ids
};

// The off-machine uses of a check: the stale nodes of `routes` whose instance (in `instances`) is off-machine, grouped per
// instance in the order of `instances`.
QList<RemoteUse> remoteUses(const QMap<QString, NodeRoute> &routes, const QSet<QString> &runs, const QList<ProviderInstance> &instances);
// Those not in `allowed` (instance names).
QList<RemoteUse> notAllowed(const QList<RemoteUse> &uses, const QSet<QString> &allowed);
// Whether any enabled instance is off-machine (a check is only worth making when one is).
bool anyOffMachine(const QList<ProviderInstance> &instances);

// "pictures and prompts", "audio": a list for a sentence.
QString sendsText(const QStringList &sends);
// The one refusal for `uses` (code consent_needed): names each service and what would leave, and how to allow it.
Error consentNeededError(const QList<RemoteUse> &uses);

class RemoteConsent {
public:
    // <state folder>/remote-consent.json
    static QString defaultPath();
    explicit RemoteConsent(QString path = defaultPath());

    bool isAllowed(const ProviderInstance &instance) const;
    // The names of the instances in `instances` that are allowed.
    QSet<QString> allowedNames(const QList<ProviderInstance> &instances) const;
    Errors allow(const ProviderInstance &instance);
    // A yes for what was shown: `fingerprint` is consentFingerprint() of the instance the person was asked about.
    Errors allow(const QString &name, const QString &fingerprint);
    // The names of `instances` whose consentFingerprint equals the one `kept` ({"<name>": "<fingerprint>"}) holds for them: the
    // answers of one run (`fugue run --allow-remote`) or the file's own.
    static QSet<QString> matching(const QJsonObject &kept, const QList<ProviderInstance> &instances);
    // Drops the answers of instances that are not in `instances` or whose address or type changed. Called when providers.json
    // is saved and before a run is checked, so removing an instance forgets the answer.
    Errors prune(const QList<ProviderInstance> &instances);

private:
    QString m_path;
};

} // namespace fugue
