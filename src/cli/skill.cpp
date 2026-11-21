// `fugue skill install` and `fugue skill uninstall` (spec 13.5): the agent skill the binary carries
// (share/skills/fugue/SKILL.md, compiled in) put where agents look for skills.
//
// One copy lives in ~/.agents/skills/fugue (Pi and other agents read that folder) and Claude's
// skills folder (~/.claude/skills, or $CLAUDE_CONFIG_DIR/skills) gets a link to it, so an update
// is one write. Nothing the user changed is overwritten or removed: a copy whose SKILL.md is
// neither the bundled one nor the one a past install recorded (the sha256 in `.fugue-skill.json`)
// is a `skill_modified` error, unless --force, which sets it aside as `<name>.bak-<time>` first.
// The whole plan is made before anything is written, so a conflict leaves everything as it was.

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "cli/commands.h"

namespace fugue::cli {

namespace {

const QString kName = QStringLiteral("fugue");
const QString kManifest = QStringLiteral(".fugue-skill.json");
const QString kSkillFile = QStringLiteral("SKILL.md");

enum class Mode { Install, Uninstall };

QByteArray bundledSkill() {
    QFile file(QStringLiteral(":/fugue/skills/fugue/SKILL.md"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString sha(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeAll(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    // Whole or not at all: a half-written skill is worse than the old one.
    return file.write(bytes) == bytes.size() && file.flush();
}

bool isLink(const QString &path) { return QFileInfo(path).isSymLink(); }
bool exists(const QString &path) { return isLink(path) || QFileInfo::exists(path); }

// What is at a skill folder, judged by its SKILL.md.
enum class Found {
    Absent,    // nothing there
    Empty,     // a folder with no SKILL.md of ours to protect
    Bundled,   // SKILL.md is the bundled one
    Recorded,  // SKILL.md is what a past install wrote, unchanged since
    Modified,  // SKILL.md is something else
    NotAFolder // a file, or a link elsewhere
};

struct Slot {
    QString target; // agents | claude
    QString path;   // the skill folder
};

Found inspect(const Slot &slot, const QString &bundledHash) {
    if (!exists(slot.path))
        return Found::Absent;
    if (isLink(slot.path))
        return Found::NotAFolder;
    const QFileInfo info(slot.path);
    if (!info.isDir())
        return Found::NotAFolder;
    const QString skillPath = slot.path + "/" + kSkillFile;
    if (!QFileInfo(skillPath).isFile())
        return Found::Empty;
    const QString have = sha(readAll(skillPath));
    if (have == bundledHash)
        return Found::Bundled;
    const QJsonObject manifest = QJsonDocument::fromJson(readAll(slot.path + "/" + kManifest)).object();
    return manifest.value("sha256").toString() == have ? Found::Recorded : Found::Modified;
}

bool linkPointsTo(const QString &link, const QString &goal) {
    return isLink(link) && QDir::cleanPath(QFileInfo(link).symLinkTarget()) == QDir::cleanPath(goal);
}

// Where each target keeps skills. Home is `HOME`, so a test (or a person) can move it.
QString skillsFolder(const QString &target) {
    if (target == "claude") {
        const QString config = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
        return (config.isEmpty() ? QDir::homePath() + "/.claude" : QDir::cleanPath(config)) + "/skills/" + kName;
    }
    return QDir::homePath() + "/.agents/skills/" + kName;
}

struct Step {
    QString target;
    QString path;
    QString action; // create update unchanged link remove absent skipped
    QString detail;
    bool backup = false; // what is there is set aside first (--force)
    QString goal;        // for a link
};

QString backupName(const QString &path) {
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMddHHmmss");
    QString name = path + ".bak-" + stamp;
    for (int i = 2; exists(name); ++i)
        name = path + ".bak-" + stamp + "-" + QString::number(i);
    return name;
}

Error conflict(const Slot &slot, const QString &what, Mode mode) {
    return makeError("skill_modified",
                     QStringLiteral("%1 %2.").arg(slot.path, what),
                     mode == Mode::Install
                         ? QStringLiteral("Fugue leaves it alone. Run `fugue skill install --force` to replace it (the old one is kept as %1.bak-<time>).").arg(slot.path)
                         : QStringLiteral("Fugue leaves it alone. Run `fugue skill uninstall --force` to set it aside as %1.bak-<time>.").arg(slot.path));
}

// A copy slot (a real folder holding SKILL.md) for install.
void planCopy(const Slot &slot, const QString &bundledHash, const QString &agentsPath, bool force, QList<Step> *steps,
              Errors *errors) {
    if (isLink(slot.path)) {
        if (linkPointsTo(slot.path, agentsPath)) {
            steps->append({slot.target, slot.path, "unchanged", "a link to the copy in agents", false, {}});
            return;
        }
        if (!force) {
            errors->append(conflict(slot, "is a link to something else", Mode::Install));
            return;
        }
        steps->append({slot.target, slot.path, "create", "replaces a link to something else", true, {}});
        return;
    }
    switch (inspect(slot, bundledHash)) {
    case Found::Absent:
    case Found::Empty:
        steps->append({slot.target, slot.path, "create", {}, false, {}});
        break;
    case Found::Bundled: {
        const QJsonObject manifest = QJsonDocument::fromJson(readAll(slot.path + "/" + kManifest)).object();
        const bool recorded = manifest.value("sha256").toString() == bundledHash;
        steps->append({slot.target, slot.path, recorded ? "unchanged" : "update", recorded ? QString() : QStringLiteral("records its hash"), false, {}});
        break;
    }
    case Found::Recorded:
        steps->append({slot.target, slot.path, "update", "an older Fugue's skill, not edited", false, {}});
        break;
    case Found::Modified:
        if (!force)
            errors->append(conflict(slot, "has a SKILL.md you changed (or another tool wrote)", Mode::Install));
        else
            steps->append({slot.target, slot.path, "create", "replaces your changed SKILL.md", true, {}});
        break;
    case Found::NotAFolder:
        if (!force)
            errors->append(conflict(slot, "is not a skill folder", Mode::Install));
        else
            steps->append({slot.target, slot.path, "create", "replaces what is there", true, {}});
        break;
    }
}

// Claude's slot when the agents copy is the real one: a link to it.
void planLink(const Slot &slot, const Slot &agents, const QString &bundledHash, bool force, QList<Step> *steps,
              Errors *errors) {
    if (linkPointsTo(slot.path, agents.path)) {
        steps->append({slot.target, slot.path, "unchanged", "a link to " + agents.path, false, {}});
        return;
    }
    if (isLink(slot.path)) {
        if (!force) {
            errors->append(conflict(slot, "is a link to something else", Mode::Install));
            return;
        }
        steps->append({slot.target, slot.path, "link", "replaces a link to something else", true, agents.path});
        return;
    }
    switch (inspect(slot, bundledHash)) {
    case Found::Absent:
        steps->append({slot.target, slot.path, "link", {}, false, agents.path});
        break;
    case Found::Empty:
    case Found::Bundled:
    case Found::Recorded: {
        // Our own copy (or an empty folder): it becomes the link, unless something else is in it.
        const QStringList entries = QDir(slot.path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        QSet<QString> others(entries.begin(), entries.end());
        others.subtract({kSkillFile, kManifest});
        const bool ours = others.isEmpty();
        if (ours)
            steps->append({slot.target, slot.path, "link", "replaces its own copy", false, agents.path});
        else if (force)
            steps->append({slot.target, slot.path, "link", "replaces a folder with other files", true, agents.path});
        else
            errors->append(conflict(slot, "has files of yours beside SKILL.md", Mode::Install));
        break;
    }
    case Found::Modified:
    case Found::NotAFolder:
        if (!force)
            errors->append(conflict(slot, inspect(slot, bundledHash) == Found::Modified ? "has a SKILL.md you changed (or another tool wrote)" : "is not a skill folder", Mode::Install));
        else
            steps->append({slot.target, slot.path, "link", "replaces what is there", true, agents.path});
        break;
    }
}

void planRemove(const Slot &slot, const Slot &agents, const QString &bundledHash, bool force, QList<Step> *steps,
                Errors *errors) {
    if (!exists(slot.path)) {
        steps->append({slot.target, slot.path, "absent", {}, false, {}});
        return;
    }
    if (isLink(slot.path)) {
        if (linkPointsTo(slot.path, agents.path))
            steps->append({slot.target, slot.path, "remove", "a link to " + agents.path, false, {}});
        else
            steps->append({slot.target, slot.path, "skipped", "a link that is not Fugue's", false, {}});
        return;
    }
    switch (inspect(slot, bundledHash)) {
    case Found::Bundled:
    case Found::Recorded:
        steps->append({slot.target, slot.path, "remove", {}, false, {}});
        break;
    case Found::Empty:
        steps->append({slot.target, slot.path, "skipped", "a folder with no SKILL.md", false, {}});
        break;
    default:
        if (!force)
            errors->append(conflict(slot, "has a SKILL.md you changed, or is not a skill folder", Mode::Uninstall));
        else
            steps->append({slot.target, slot.path, "remove", "your changed copy is set aside", true, {}});
        break;
    }
}

bool setAside(const QString &path, QString *where) {
    *where = backupName(path);
    return QFile::rename(path, *where) || QDir().rename(path, *where);
}

// Removes what a past install made: its two files, then the folder if that leaves it empty.
void removeOwn(const QString &path) {
    QFile::remove(path + "/" + kSkillFile);
    QFile::remove(path + "/" + kManifest);
    QDir().rmdir(path);
}

Error failed(const QString &what, const QString &path) {
    return makeError("permission_denied", QStringLiteral("Could not %1 %2.").arg(what, path),
                     "Check that you may write there.");
}

Errors apply(const QList<Step> &steps, const QByteArray &skill, QStringList *notes) {
    const QString hash = sha(skill);
    const QByteArray manifest =
        QJsonDocument(QJsonObject{{"name", kName}, {"version", QCoreApplication::applicationVersion()}, {"sha256", hash}})
            .toJson(QJsonDocument::Indented);
    for (const Step &step : steps) {
        if (step.backup && exists(step.path)) {
            QString where;
            if (!setAside(step.path, &where))
                return {failed("set aside", step.path)};
            notes->append(QStringLiteral("Kept the old %1 as %2.").arg(step.path, where));
        }
        if (step.action == "create" || step.action == "update") {
            if (!QDir().mkpath(step.path) || !writeAll(step.path + "/" + kSkillFile, skill)
                || !writeAll(step.path + "/" + kManifest, manifest))
                return {failed("write", step.path)};
        } else if (step.action == "link") {
            if (exists(step.path) && !isLink(step.path))
                removeOwn(step.path); // the plan only lets this through for our own copy
            else if (isLink(step.path))
                QFile::remove(step.path);
            if (exists(step.path))
                return {failed("replace", step.path)};
            if (!QDir().mkpath(QFileInfo(step.path).absolutePath()) || !QFile::link(step.goal, step.path))
                return {failed("link", step.path)};
        } else if (step.action == "remove") {
            if (isLink(step.path))
                QFile::remove(step.path);
            else if (exists(step.path))
                removeOwn(step.path);
        }
    }
    return {};
}

Reply run(const Command &command, Mode mode) {
    const QString choice = command.has("target") ? command.value("target") : QStringLiteral("all");
    if (choice != "all" && choice != "agents" && choice != "claude") {
        Error error = makeError("usage", QStringLiteral("Unknown target \"%1\".").arg(choice),
                                "agents is ~/.agents/skills (Pi and others), claude is Claude Code's skills folder.");
        error.valid = QJsonArray{"all", "agents", "claude"};
        return Reply::failure(error);
    }
    const bool wantAgents = choice != "claude";
    const bool wantClaude = choice != "agents";
    const bool force = command.has("force");
    const QByteArray skill = bundledSkill();
    if (skill.isEmpty())
        return Reply::failure(makeError("internal", "This Fugue build carries no skill."));
    const QString hash = sha(skill);
    const Slot agents{"agents", skillsFolder("agents")};
    const Slot claude{"claude", skillsFolder("claude")};

    QList<Step> steps;
    Errors errors;
    if (mode == Mode::Install) {
        if (wantAgents)
            planCopy(agents, hash, agents.path, force, &steps, &errors);
        if (wantClaude) {
            // Claude links to the shared copy when that is part of this install, else holds its own.
            if (wantAgents)
                planLink(claude, agents, hash, force, &steps, &errors);
            else
                planCopy(claude, hash, agents.path, force, &steps, &errors);
        }
    } else {
        // The link first, so it never points at nothing.
        if (wantClaude)
            planRemove(claude, agents, hash, force, &steps, &errors);
        if (wantAgents)
            planRemove(agents, agents, hash, force, &steps, &errors);
    }
    if (!errors.isEmpty())
        return Reply::failure(errors);

    const bool dry = command.has("dry-run");
    QStringList notes;
    if (!dry) {
        const Errors written = apply(steps, skill, &notes);
        if (!written.isEmpty())
            return Reply::failure(written);
    }

    QJsonArray actions;
    QStringList lines;
    for (const Step &step : steps) {
        actions.append(QJsonObject{{"target", step.target}, {"path", step.path}, {"action", step.action}, {"detail", step.detail}});
        // "agents: create /home/me/.agents/skills/fugue (an older Fugue's skill)": the plan reads
        // the same with and without --dry-run, which only adds the heading.
        QString line = QStringLiteral("%1: %2 %3").arg(step.target, step.action, step.path);
        if (!step.detail.isEmpty())
            line += QStringLiteral(" (%1)").arg(step.detail);
        lines << line;
    }
    lines = notes + lines;
    if (dry)
        lines.prepend(QStringLiteral("Dry run, nothing written:"));
    return Reply::success(QJsonObject{{"dry_run", dry}, {"skill", kName}, {"actions", actions}}, lines.join('\n'));
}

} // namespace

Reply skillInstall(const Command &command, Context &) { return run(command, Mode::Install); }
Reply skillUninstall(const Command &command, Context &) { return run(command, Mode::Uninstall); }

} // namespace fugue::cli
