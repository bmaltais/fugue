#include "command_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMetaObject>
#include <QRegularExpression>
#include <algorithm>

#include "engine/paths.h"
#include "menu/menus.h"

namespace fugue {

namespace {

// Least to most specific (12.4: the more specific context owns the key).
const QStringList kContexts = {QStringLiteral("app"), QStringLiteral("canvas"), QStringLiteral("group-selected"),
                               QStringLiteral("compare"), QStringLiteral("group"), QStringLiteral("run"),
                               // #230: the pointer is over a Canvas view (or it has the focus): its tools' keys
                               QStringLiteral("canvas-view"),
                               // #226: the pointer is over a Canvas view's layers panel (or it has the focus): its keys win there
                               QStringLiteral("canvas-layers")};

// A keymap file is a few lines; this keeps a wrong file (a log, a binary) from being read whole.
constexpr qint64 kMaxKeymapBytes = 1024 * 1024;

bool validId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)+$"));
    return pattern.match(id).hasMatch();
}

} // namespace

CommandRegistry::CommandRegistry(QObject *parent) : QObject(parent) { m_activeContexts.insert(kContexts.first()); }

QStringList CommandRegistry::contexts() { return kContexts; }

QString CommandRegistry::canonicalKey(const QString &key, QString *problem) {
    const QKeySequence sequence(key.trimmed(), QKeySequence::PortableText);
    if (key.trimmed().isEmpty() || sequence.isEmpty() || sequence.count() != 1 || sequence[0].toCombined() == 0) {
        if (problem)
            *problem = tr("\"%1\" is not a key (one chord such as Ctrl+S)").arg(key);
        return {};
    }
    return sequence.toString(QKeySequence::PortableText);
}

QString CommandRegistry::add(const CommandSpec &spec, Handler handler) {
    if (!validId(spec.id))
        return QStringLiteral("\"%1\" is not a command id (lowercase `group.name`)").arg(spec.id);
    if (m_index.contains(spec.id))
        return QStringLiteral("%1 is registered twice").arg(spec.id);
    if (spec.description.trimmed().isEmpty())
        return QStringLiteral("%1 has no description").arg(spec.id);
    if (!kContexts.contains(spec.context))
        return QStringLiteral("%1: unknown context \"%2\"").arg(spec.id, spec.context);
    Entry entry{spec, std::move(handler), {}, {}, {}, {}, {}, {}};
    entry.spec.keys.clear();
    for (const QString &key : spec.keys) {
        QString problem;
        const QString canonical = canonicalKey(key, &problem);
        if (canonical.isEmpty())
            return spec.id + ": " + problem;
        if (!entry.spec.keys.contains(canonical))
            entry.spec.keys << canonical;
    }
    m_index.insert(spec.id, int(m_entries.size()));
    m_entries.append(std::move(entry));
    rebuild();
    return {};
}

bool CommandRegistry::setHandler(const QString &id, Handler handler) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].handler = std::move(handler);
    emit changed();
    return true;
}

bool CommandRegistry::setLabel(const QString &id, const QString &label) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].spec.label = label;
    emit changed();
    return true;
}

bool CommandRegistry::setLabelWhen(const QString &id, std::function<QString()> label) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].labelWhen = std::move(label);
    return true;
}

bool CommandRegistry::setEnabledWhen(const QString &id, Check check) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].enabledWhen = std::move(check);
    return true;
}

bool CommandRegistry::setAlsoWhen(const QString &id, Check check) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].alsoWhen = std::move(check);
    return true;
}

bool CommandRegistry::setCheckedWhen(const QString &id, Check check) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    m_entries[*it].checkedWhen = std::move(check);
    return true;
}

QStringList CommandRegistry::ids() const {
    QStringList ids;
    for (const Entry &entry : m_entries)
        ids << entry.spec.id;
    ids.sort();
    return ids;
}

const CommandSpec *CommandRegistry::spec(const QString &id) const {
    const auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : &m_entries[*it].spec;
}

bool CommandRegistry::hasHandler(const QString &id) const {
    const auto it = m_index.constFind(id);
    return it != m_index.constEnd() && bool(m_entries[*it].handler);
}

CommandRegistry::Handler CommandRegistry::handlerOf(const QString &id) const {
    const auto it = m_index.constFind(id);
    return it != m_index.constEnd() ? m_entries[*it].handler : Handler();
}

QStringList CommandRegistry::keys(const QString &id) const {
    const auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? QStringList() : m_entries[*it].keys;
}

bool CommandRegistry::run(const QString &id) {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd() || !m_entries[*it].handler)
        return false;
    // Copied: the handler may register or replace handlers.
    const Handler handler = m_entries[*it].handler;
    handler();
    return true;
}

bool CommandRegistry::isEnabled(const QString &id) const {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    const Entry &entry = m_entries[*it];
    return entry.handler && entryOn(entry) && (!entry.enabledWhen || entry.enabledWhen());
}

bool CommandRegistry::trigger(const QString &id) { return triggerOn(id, {}); }

bool CommandRegistry::triggerOn(const QString &id, const QVariantMap &target) {
    if (!isEnabled(id, target))
        return false;
    const QVariantMap previous = std::exchange(m_target, target);
    run(id);
    m_target = previous;
    return true;
}

bool CommandRegistry::isEnabled(const QString &id, const QVariantMap &target) const {
    const QVariantMap previous = std::exchange(m_target, target);
    const bool enabled = isEnabled(id);
    m_target = previous;
    return enabled;
}

QString CommandRegistry::ownerOf(const QString &key) const {
    const QString canonical = canonicalKey(key);
    if (canonical.isEmpty())
        return {};
    const Entry *best = nullptr;
    int bestRank = -1;
    for (const Entry &entry : m_entries) {
        if (!entry.handler || !entry.keys.contains(canonical) || !entryOn(entry))
            continue;
        // Specificity first, then the user's own binding over a default, then registration order.
        const int rank = int(kContexts.indexOf(entry.spec.context)) * 2 + (entry.userKeys.contains(canonical) ? 1 : 0);
        if (rank > bestRank) {
            best = &entry;
            bestRank = rank;
        }
    }
    return best ? best->spec.id : QString();
}

bool CommandRegistry::triggerKey(const QString &key) {
    if (m_textFieldFocus && belongsToTextField(key))
        return false;
    const QString owner = ownerOf(key);
    return !owner.isEmpty() && run(owner);
}

QString CommandRegistry::ownerOfPress(int key, int modifiers) const {
    const Qt::KeyboardModifiers mods = Qt::KeyboardModifiers(modifiers) & ~Qt::KeypadModifier;
    const auto owner = [this, key](Qt::KeyboardModifiers m) {
        return ownerOf(QKeySequence(QKeyCombination(m, Qt::Key(key))).toString(QKeySequence::PortableText));
    };
    const QString exact = owner(mods);
    return exact.isEmpty() && (mods & Qt::ShiftModifier) ? owner(mods & ~Qt::ShiftModifier) : exact;
}

bool CommandRegistry::actsOnGraph(const QString &id) const {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return false;
    // The window's own contexts are not the Graph's: `app` (quit, undo, save: the document's undo is the region's too),
    // `run` and the layers panel's.
    const QString &context = m_entries[*it].spec.context;
    if (context == QLatin1String("app") || context == QLatin1String("run") || context == QLatin1String("canvas-layers") || context == QLatin1String("canvas-view"))
        return false;
    // The panels and bars share the `canvas` context and act on the window.
    static const QStringList graphViews = {QStringLiteral("view.fit"), QStringLiteral("view.zoom-in"), QStringLiteral("view.zoom-out"),
                                           QStringLiteral("view.toggle-links"),
                                           QStringLiteral("view.picture-smart"), QStringLiteral("view.picture-sinks"), QStringLiteral("view.picture-all")};
    if (id.startsWith(QLatin1String("panel.")) || (id.startsWith(QLatin1String("view.")) && !graphViews.contains(id)))
        return false;
    return true;
}

void CommandRegistry::setContextActive(const QString &context, bool active) {
    if (!kContexts.contains(context) || context == kContexts.first() || m_activeContexts.contains(context) == active)
        return;
    if (active)
        m_activeContexts.insert(context);
    else
        m_activeContexts.remove(context);
    emit changed();
}

bool CommandRegistry::contextActive(const QString &context) const {
    if (!m_activeContexts.contains(context))
        return false;
    const bool ofGraph = context == QLatin1String("group-selected") || context == QLatin1String("group") || context == QLatin1String("compare");
    return !ofGraph || m_activeContexts.contains(QStringLiteral("canvas"));
}

bool CommandRegistry::entryOn(const Entry &entry) const {
    return contextActive(entry.spec.context) || (entry.alsoWhen && entry.alsoWhen());
}

bool CommandRegistry::belongsToTextField(const QString &key) {
    const QKeySequence sequence(key, QKeySequence::PortableText);
    if (sequence.count() != 1)
        return false;
    const QKeyCombination combination = sequence[0];
    const Qt::Key k = combination.key();
    const Qt::KeyboardModifiers mods = combination.keyboardModifiers() & ~Qt::KeypadModifier;
    if (k == Qt::Key_Escape || (k >= Qt::Key_F1 && k <= Qt::Key_F35))
        return false;
    if (!(mods & ~Qt::ShiftModifier))
        return true; // bare and Shift-only
    if (mods == Qt::ControlModifier)
        return k == Qt::Key_A || k == Qt::Key_C || k == Qt::Key_V || k == Qt::Key_X || k == Qt::Key_Z || k == Qt::Key_Y;
    return mods == (Qt::ControlModifier | Qt::ShiftModifier) && k == Qt::Key_Z; // redo
}

bool CommandRegistry::isTextInput(QObject *object) {
    if (!object)
        return false;
    for (const QMetaObject *meta = object->metaObject(); meta; meta = meta->superClass()) {
        const QByteArray name = meta->className();
        if (name == "QQuickTextInput" || name == "QQuickTextEdit")
            return true;
    }
    return false;
}

QString CommandRegistry::keymapPath() { return configDir() + QStringLiteral("/keybindings.json"); }

QString CommandRegistry::keymapFile() const {
    const QString path = m_keymapFile.isEmpty() ? keymapPath() : m_keymapFile;
    const QString home = QDir::homePath();
    return path.startsWith(home + '/') ? QStringLiteral("~") + path.mid(home.size()) : path;
}

void CommandRegistry::loadKeymap(const QString &path) {
    m_keymapFile = path;
    m_adds.clear();
    m_unsets.clear();
    m_fileProblems.clear();
    const QFileInfo info(path);
    const QString name = info.fileName();
    if (!info.exists()) {
        rebuild();
        return;
    }
    if (!info.isFile() || info.size() > kMaxKeymapBytes) {
        m_fileProblems << tr("%1 is not a keymap file (not a file, or over 1 MB); using the default keys").arg(name);
        rebuild();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_fileProblems << tr("%1 cannot be read (%2); using the default keys").arg(name, file.errorString());
        rebuild();
        return;
    }
    applyKeymap(file.read(kMaxKeymapBytes + 1), name);
}

void CommandRegistry::applyKeymap(const QByteArray &json, const QString &source) {
    m_adds.clear();
    m_unsets.clear();
    m_fileProblems.clear();
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        m_fileProblems << (error.error != QJsonParseError::NoError
                               ? tr("%1 is not valid JSON (%2, at byte %3); using the default keys")
                                     .arg(source, error.errorString()).arg(error.offset)
                               : tr("%1 must hold a JSON object like {\"add\": [], \"unset\": []}; using the default keys")
                                     .arg(source));
        rebuild();
        return;
    }
    const QJsonObject root = document.object();
    for (const QString &name : root.keys())
        if (name != QLatin1String("add") && name != QLatin1String("unset"))
            m_fileProblems << tr("%1: unknown entry \"%2\" ignored").arg(source, name);
    auto readList = [&](const QString &listName, QList<Edit> *into) {
        const QJsonValue value = root.value(listName);
        if (value.isUndefined())
            return;
        if (!value.isArray()) {
            m_fileProblems << tr("%1: \"%2\" must be a list; ignored").arg(source, listName);
            return;
        }
        int number = 0;
        for (const QJsonValue &item : value.toArray()) {
            ++number;
            const QString where = QStringLiteral("%1: %2 #%3").arg(source, listName).arg(number);
            const QJsonObject object = item.toObject();
            if (!item.isObject() || !object.value("command").isString() || !object.value("key").isString()) {
                m_fileProblems << tr("%1 needs a \"command\" and a \"key\", both text; skipped").arg(where);
                continue;
            }
            QString problem;
            const QString key = canonicalKey(object.value("key").toString(), &problem);
            if (key.isEmpty()) {
                m_fileProblems << tr("%1: %2; skipped").arg(where, problem);
                continue;
            }
            into->append({object.value("command").toString(), key});
        }
    };
    readList(QStringLiteral("add"), &m_adds);
    readList(QStringLiteral("unset"), &m_unsets);
    rebuild();
}

// Computes every command's keys from the defaults and the diff, and the problems that need the
// registry to be known (a command id that does not exist, a key that is not bound).
void CommandRegistry::rebuild() {
    m_keymapProblems.clear();
    for (Entry &entry : m_entries) {
        entry.keys = entry.spec.keys;
        entry.userKeys.clear();
    }
    for (const Edit &edit : std::as_const(m_unsets)) {
        const auto it = m_index.constFind(edit.command);
        if (it == m_index.constEnd()) {
            m_keymapProblems << tr("unset: no command \"%1\"").arg(edit.command);
        } else if (!m_entries[*it].keys.removeOne(edit.key)) {
            m_keymapProblems << tr("unset: %1 is not bound to %2").arg(edit.key, edit.command);
        }
    }
    for (const Edit &edit : std::as_const(m_adds)) {
        const auto it = m_index.constFind(edit.command);
        if (it == m_index.constEnd()) {
            m_keymapProblems << tr("add: no command \"%1\"").arg(edit.command);
            continue;
        }
        Entry &entry = m_entries[*it];
        if (!entry.keys.contains(edit.key))
            entry.keys << edit.key;
        if (!entry.userKeys.contains(edit.key))
            entry.userKeys << edit.key;
    }
    // Two commands on one key in one context: the user's binding wins, else the first
    // registered; either way the user is told.
    QHash<QString, QStringList> owners;
    for (const Entry &entry : std::as_const(m_entries))
        for (const QString &key : entry.keys)
            owners[entry.spec.context + '\n' + key] << entry.spec.id;
    for (auto it = owners.constBegin(); it != owners.constEnd(); ++it)
        if (it->size() > 1) {
            const auto [context, key] = std::pair(it.key().section('\n', 0, 0), it.key().section('\n', 1));
            m_keymapProblems << tr("%1 is bound to %2 in the same place (%3)")
                                    .arg(key, it->join(tr(" and ")), context);
        }
    m_keymapProblems.sort();
    emit changed();
}

QStringList CommandRegistry::problems() const { return m_fileProblems + m_keymapProblems; }

QStringList CommandRegistry::activeKeys() const {
    QSet<QString> keys;
    for (const Entry &entry : m_entries)
        if (entry.handler && entryOn(entry))
            for (const QString &key : entry.keys)
                keys.insert(key);
    QStringList sorted(keys.begin(), keys.end());
    sorted.sort();
    return sorted;
}

void CommandRegistry::setTextFieldFocus(bool focus) {
    if (std::exchange(m_textFieldFocus, focus) != focus)
        emit changed();
}

void CommandRegistry::setUnsavedWork(bool unsaved) {
    if (std::exchange(m_unsavedWork, unsaved) != unsaved)
        emit changed();
}

void CommandRegistry::setWorkflowOpen(bool open) {
    if (std::exchange(m_workflowOpen, open) != open)
        emit changed();
}

void CommandRegistry::menuShown(bool shown) {
    const bool wasOpen = menuOpen();
    m_openMenus = std::max(0, m_openMenus + (shown ? 1 : -1));
    if (menuOpen() != wasOpen)
        emit menuOpenChanged();
}

void CommandRegistry::quitNow() {
    m_quitting = true;
    QCoreApplication::quit();
}

bool CommandRegistry::mayCloseWindow() {
    if (m_quitting || !m_unsavedWork)
        return true;
    emit quitConfirmationRequested();
    return false;
}

void CommandRegistry::requestQuit() {
    if (m_unsavedWork)
        emit quitConfirmationRequested();
    else
        quitNow();
}

void CommandRegistry::requestClose() {
    if (m_workflowOpen)
        emit closeWorkflowRequested();
    else
        requestQuit(); // nothing to close but the window itself
}

QVariantList CommandRegistry::list() const {
    QVariantList list;
    for (const Entry &entry : m_entries) {
        list.append(QVariantMap{
            {"id", entry.spec.id},
            {"description", QCoreApplication::translate("Command", entry.spec.description.toUtf8().constData())},
            {"keys", entry.keys},
            {"gestures", entry.spec.gestures},
            {"context", entry.spec.context},
            {"versus", entry.spec.versus},
            {"deviates", entry.spec.deviates()},
            {"available", bool(entry.handler)},
        });
    }
    return list;
}

QVariantMap CommandRegistry::entry(const QString &id, const QVariantMap &target) const {
    const auto it = m_index.constFind(id);
    if (it == m_index.constEnd())
        return {};
    const Entry &entry = m_entries[*it];
    const QVariantMap previous = std::exchange(m_target, target);
    const QString description = QCoreApplication::translate("Command", entry.spec.description.toUtf8().constData());
    const QString dynamic = entry.labelWhen ? entry.labelWhen() : QString();
    const QString source = !dynamic.isEmpty() ? dynamic : entry.spec.label;
    QVariantMap map{
        {"command", entry.spec.id},
        {"label", source.isEmpty() ? description : QCoreApplication::translate("Command", source.toUtf8().constData())},
        {"description", description},
        {"keys", entry.keys},
        {"available", bool(entry.handler)},
        {"enabled", isEnabled(id)},
        {"checkable", bool(entry.checkedWhen)},
        {"checked", entry.checkedWhen && entry.checkedWhen()},
    };
    m_target = previous;
    return map;
}

QVariantList CommandRegistry::resolve(const QVariantList &items, const QVariantMap &target, bool context) const {
    QVariantList resolved;
    for (const QVariant &value : items) {
        QVariantMap item = value.toMap();
        if (item.contains("items")) {
            item.insert("title", QCoreApplication::translate("Menu", item.value("title").toString().toUtf8().constData()));
            item.insert("items", resolve(item.value("items").toList(), target, context));
            if (context && item.value("items").toList().isEmpty())
                continue; // everything in it was left out
        } else if (item.contains("command")) {
            const QVariantMap state = entry(item.value("command").toString(), target);
            if (state.isEmpty())
                continue; // not registered (a ticket that is not in this build)
            if (context && (!state.value("available").toBool()
                            || (item.value("hideWhenOff").toBool() && !state.value("enabled").toBool())))
                continue;
            item.remove("hideWhenOff");
            item.insert(state);
        } else if (item.contains("placeholder")) {
            // Planned, not built: shown greyed with its issue, never a command (the shortcut map does not know it).
            item.insert("label", QCoreApplication::translate("Menu", item.value("placeholder").toString().toUtf8().constData()));
            item.insert("command", QString());
            item.insert("keys", QStringList());
            item.insert("available", false);
            item.insert("enabled", false);
            item.insert("checkable", false);
            item.insert("checked", false);
        } else if (item.value("separator").toBool()) {
            // Never first, never two in a row; a last one goes below.
            if (resolved.isEmpty() || resolved.last().toMap().value("separator").toBool())
                continue;
        }
        resolved.append(item);
    }
    while (!resolved.isEmpty() && resolved.last().toMap().value("separator").toBool())
        resolved.removeLast();
    return resolved;
}

QVariantList CommandRegistry::menu(const QString &name, const QVariantMap &target) const {
    // The bars (the window's, the Graph's and the Canvas view's, which are in headers) show every row, built or not; a
    // context menu leaves out what is not built.
    return resolve(menus::tree(name), target, name != QLatin1String("bar") && name != QLatin1String("graph") && name != QLatin1String("canvas-view"));
}

} // namespace fugue
