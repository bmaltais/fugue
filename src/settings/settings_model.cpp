#include "settings/settings_model.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/atomic_file.h"
#include "engine/json.h"
#include "engine/paths.h"
#include "engine/providers_file.h"
#include "translator.h"

namespace fugue {

namespace {

// ~ for the home folder, as the panel shows a path.
QString tilde(const QString &path) {
    const QString home = QDir::homePath();
    if (path == home)
        return QStringLiteral("~");
    if (path.startsWith(home + QLatin1Char('/')))
        return QLatin1Char('~') + path.mid(home.size());
    return path;
}

// A path from the panel: typed text, or the folder picker's file:// URL.
QString pathText(const QVariant &value) {
    if (value.typeId() == QMetaType::QUrl)
        return value.toUrl().toLocalFile();
    const QString text = value.toString();
    return text.startsWith(QLatin1String("file:")) ? QUrl(text).toLocalFile() : text;
}

// The path to write: ~ kept as typed, else cleaned.
QString folderValue(const QString &typed) {
    const QString path = typed.trimmed();
    return tilde(QDir::cleanPath(expandHome(path)));
}

struct ModuleField {
    QString name;
    bool secret;
};

// The settings a provider module declares beyond name, type, address, key and enabled (8.8).
// Only omarchy is compiled in (8.7) and it declares none; a field the file holds that no
// module declares is shown as it is, secret when its name says so.
QList<ModuleField> moduleFields(const ProviderInstance &instance) {
    static const QRegularExpression secretName(QStringLiteral("key|token|secret|password"),
                                               QRegularExpression::CaseInsensitiveOption);
    QList<ModuleField> fields;
    for (auto it = instance.settings.begin(); it != instance.settings.end(); ++it)
        fields.append({it.key(), secretName.match(it.key()).hasMatch()});
    return fields;
}

QString checkAddress(const QString &address) {
    if (address.isEmpty())
        return {};
    const QUrl url(address, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
        return QObject::tr("An address is an http:// or https:// URL, such as https://studio.example.ts.net.");
    return {};
}

} // namespace

SettingsModel::SettingsModel(const QString &settingsFile, const QString &providersFile, QObject *parent)
    : QObject(parent), m_path(settingsFile.isEmpty() ? settingsPath() : settingsFile),
      m_providersPath(providersFile.isEmpty() ? fugue::providersPath() : providersFile) {
    // What the Packs page shows follows the files: the pack folders of the settings change it.
    connect(this, &SettingsModel::changed, &m_packs, &PacksModel::refresh);
    loadSettings();
    loadProviders();
}

void SettingsModel::attach(Translator *translator, CanvasModel *canvas, const Registry *registry) {
    m_packs.setRegistry(registry);
    m_translator = translator;
    m_canvas = canvas;
    pushLive();
}

void SettingsModel::registerCommands(CommandRegistry &registry, std::function<void(const QUrl &)> openUrl) {
    m_registry = &registry;
    m_openUrl = std::move(openUrl);
    registry.setHandler(QStringLiteral("app.settings"),
                        [&registry] { emit registry.windowRequested(QStringLiteral("app.settings")); });
}

void SettingsModel::loadSettings() { m_file = readSettings(m_path); }

// providers.json as it is now, or (without one) the defaults it stands for, which follow
// Omarchy's paired hosts.
QByteArray SettingsModel::providersSnapshot() const {
    QFile file(m_providersPath);
    if (file.open(QIODevice::ReadOnly))
        return QByteArrayLiteral("file:") + file.readAll();
    if (file.exists())
        return QByteArrayLiteral("unreadable");
    return QByteArrayLiteral("defaults:") + writeJson(QJsonObject{{"providers", providerInstancesJson(defaultProviderInstances())}});
}

void SettingsModel::loadProviders() {
    m_instances.clear();
    m_providersProblem.clear();
    m_providersSnapshot = providersSnapshot();
    m_providersSaved = QFileInfo::exists(m_providersPath);
    const Result<QList<ProviderInstance>> loaded = loadProviderInstances(m_providersPath);
    if (loaded) {
        m_instances = loaded.value();
        return;
    }
    QStringList lines;
    for (const Error &error : loaded.errors())
        lines << error.message;
    m_providersProblem = lines.join(QLatin1Char('\n'));
}

void SettingsModel::pushLive() {
    if (m_translator)
        m_translator->setLanguageOverride(m_file.settings.language);
    if (m_canvas)
        m_canvas->setNewNodeOptions(m_file.settings.newSeed, m_file.settings.embedWorkflow);
}

void SettingsModel::reload() {
    loadSettings();
    loadProviders();
    pushLive();
    emit changed();
    emit providersChanged();
}

QVariantList SettingsModel::languages() const {
    QVariantList list;
    for (const QString &code : Translator::catalogs()) {
        // A bare code names the language, not a country's variant ("English", not "American English").
        QString name = code == QLatin1String("en") ? QStringLiteral("English") : QLocale(code).nativeLanguageName();
        if (!name.isEmpty())
            name[0] = name[0].toUpper();
        list.append(QVariantMap{{"code", code}, {"name", name.isEmpty() ? code : name}});
    }
    return list;
}

// As paths.h's outputDir() and inputsDir() find them, from this model's file.
QString SettingsModel::outputFolder() const {
    const QString chosen = m_file.settings.outputFolder;
    return tilde(chosen.isEmpty() ? defaultOutputDir() : QDir::cleanPath(expandHome(chosen)));
}

QString SettingsModel::inputsFolder() const {
    const QString chosen = m_file.settings.inputsFolder;
    return chosen.isEmpty() ? outputFolder() + QStringLiteral("/inputs") : tilde(QDir::cleanPath(expandHome(chosen)));
}

QString SettingsModel::set(const QString &key, const QVariant &value) {
    if (!m_file.writable)
        return tr("Fix %1 first: it is not a JSON object.").arg(tilde(m_path));
    QJsonValue json = QJsonValue::fromVariant(value);
    if (key == QLatin1String("output_folder") || key == QLatin1String("inputs_folder")) {
        const QString typed = pathText(value).trimmed();
        json = typed.isEmpty() ? settingDefault(key) : QJsonValue(folderValue(typed));
    } else if (key == QLatin1String("language")) {
        const QString code = value.toString();
        if (code.isEmpty())
            json = QJsonValue(QJsonValue::Undefined);
        else if (!Translator::catalogs().contains(code))
            return tr("Fugue has no %1 translation.").arg(code);
    }
    // Undefined goes back to the default: the key leaves the file.
    if (const Errors errors = changeSetting(m_path, key, json); !errors.isEmpty())
        return errors.first().message;
    loadSettings();
    pushLive();
    emit changed();
    return {};
}

QString SettingsModel::addPath(const QString &key, const QVariant &value) {
    const QString path = pathText(value);
    if (key != QLatin1String("model_paths") && key != QLatin1String("pack_paths"))
        return tr("%1 is not a list of folders.").arg(key);
    QJsonArray list = QJsonArray::fromStringList(key == QLatin1String("model_paths") ? modelPaths() : packPaths());
    list.append(path.trimmed().isEmpty() ? QString() : folderValue(path));
    return set(key, list.toVariantList());
}

QString SettingsModel::removePath(const QString &key, int index) {
    if (key != QLatin1String("model_paths") && key != QLatin1String("pack_paths"))
        return tr("%1 is not a list of folders.").arg(key);
    QStringList list = key == QLatin1String("model_paths") ? modelPaths() : packPaths();
    if (index < 0 || index >= list.size())
        return tr("There is no folder %1 in the list.").arg(index + 1);
    list.removeAt(index);
    return set(key, QJsonArray::fromStringList(list).toVariantList());
}

// --- providers -----------------------------------------------------------------------

QVariantList SettingsModel::providers() const {
    QVariantList list;
    for (const ProviderInstance &instance : m_instances) {
        QVariantList fields;
        for (const ModuleField &field : moduleFields(instance)) {
            const QJsonValue value = instance.settings.value(field.name);
            const QString text = value.isString() ? value.toString() : value.isDouble() ? QString::number(value.toDouble())
                                                  : value.isBool() ? (value.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                                                   : QString();
            fields.append(QVariantMap{{"name", field.name},
                                      {"secret", field.secret},
                                      {"value", field.secret ? QString() : text},
                                      {"set", !text.isEmpty()}});
        }
        list.append(QVariantMap{{"name", instance.name},
                                {"type", instance.type},
                                {"address", instance.address},
                                {"enabled", instance.enabled},
                                {"keySet", !instance.key.isEmpty()},
                                {"saved", m_providersSaved},
                                {"fields", fields}});
    }
    return list;
}

QStringList SettingsModel::providerTypes() const { return {QStringLiteral("omarchy")}; }

QString SettingsModel::saveProviders(const QList<ProviderInstance> &instances) {
    if (!m_providersProblem.isEmpty())
        return tr("Fix %1 first: %2").arg(tilde(m_providersPath), m_providersProblem);
    // The first save writes the defaults, which must hold the paired hosts: once the file is
    // there, nothing is added to it.
    if (!QFileInfo::exists(m_providersPath)) {
        if (const QString problem = pairedHostsProblem(); !problem.isEmpty())
            return tr("Nothing was saved: your paired hosts cannot be read now (%1). Try again in a moment.")
                .arg(problem);
    }
    // A change is made to the list the panel shows; one edited outside since is not written
    // over: the panel shows it instead, and the change can be made again.
    if (providersSnapshot() != m_providersSnapshot) {
        loadProviders();
        emit providersChanged();
        return tr("Nothing was saved: %1 changed outside the panel. It now shows the file as it is; make the change again.")
            .arg(tilde(m_providersPath));
    }
    const Errors errors = saveProviderInstances(m_providersPath, instances);
    if (!errors.isEmpty())
        return errors.first().message;
    loadProviders();
    emit providersChanged();
    return {};
}

QString SettingsModel::setProviderField(int row, const QString &field, const QVariant &value) {
    if (!m_providersProblem.isEmpty())
        return saveProviders({});
    if (row < 0 || row >= m_instances.size())
        return tr("There is no provider %1.").arg(row + 1);
    QList<ProviderInstance> instances = m_instances;
    ProviderInstance &instance = instances[row];
    if (field == QLatin1String("name")) {
        const QString name = value.toString().trimmed();
        if (name.isEmpty())
            return tr("A provider needs a name.");
        for (int i = 0; i < instances.size(); ++i)
            if (i != row && instances[i].name == name)
                return tr("Another provider is named %1.").arg(name);
        instance.name = name;
    } else if (field == QLatin1String("address")) {
        const QString address = value.toString().trimmed();
        if (const QString reason = checkAddress(address); !reason.isEmpty())
            return reason;
        instance.address = address;
    } else if (field == QLatin1String("enabled")) {
        if (value.typeId() != QMetaType::Bool)
            return tr("enabled is on or off.");
        instance.enabled = value.toBool();
    } else if (field == QLatin1String("key")) {
        instance.key = value.toString().trimmed();
    } else if (field == QLatin1String("type")) {
        return tr("The type of a provider cannot change; add another.");
    } else {
        bool known = false;
        for (const ModuleField &declared : moduleFields(instance))
            known = known || declared.name == field;
        if (!known)
            return tr("%1 has no setting %2.").arg(instance.type, field);
        const QString text = value.toString().trimmed();
        if (text.isEmpty())
            instance.settings.remove(field);
        else
            instance.settings.insert(field, text);
    }
    return saveProviders(instances);
}

QString SettingsModel::addProvider(const QString &type) {
    if (!m_providersProblem.isEmpty())
        return saveProviders({});
    if (!providerTypes().contains(type))
        return tr("Fugue has no provider module %1.").arg(type);
    QSet<QString> names;
    for (const ProviderInstance &instance : m_instances)
        names.insert(instance.name);
    ProviderInstance added;
    added.type = type;
    added.name = tr("New provider");
    for (int n = 2; names.contains(added.name); ++n)
        added.name = tr("New provider %1").arg(n);
    QList<ProviderInstance> instances = m_instances;
    instances.append(added);
    return saveProviders(instances);
}

QString SettingsModel::removeProvider(int row) {
    if (!m_providersProblem.isEmpty())
        return saveProviders({});
    if (row < 0 || row >= m_instances.size())
        return tr("There is no provider %1.").arg(row + 1);
    QList<ProviderInstance> instances = m_instances;
    instances.removeAt(row);
    return saveProviders(instances);
}

QString SettingsModel::moveProvider(int row, int to) {
    if (!m_providersProblem.isEmpty())
        return saveProviders({});
    if (row < 0 || row >= m_instances.size() || to < 0 || to >= m_instances.size())
        return tr("There is no provider %1.").arg((row < 0 || row >= m_instances.size() ? row : to) + 1);
    if (row == to)
        return {};
    QList<ProviderInstance> instances = m_instances;
    instances.move(row, to);
    return saveProviders(instances);
}

// --- keymap --------------------------------------------------------------------------

void SettingsModel::openKeymap() {
    const QString path = CommandRegistry::keymapPath();
    if (!QFileInfo::exists(path))
        replaceFile(path, QByteArrayLiteral("{\"add\": [], \"unset\": []}\n"), path);
    if (m_openUrl)
        m_openUrl(QUrl::fromLocalFile(path));
}

void SettingsModel::reloadKeymap() {
    if (m_registry)
        m_registry->loadKeymap(CommandRegistry::keymapPath());
}

} // namespace fugue
