#include "settings/settings_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

#include "canvas/canvas_model.h"
#include "commands/command_registry.h"
#include "engine/atomic_file.h"
#include "engine/node_picture.h"
#include "engine/json.h"
#include "engine/model_library.h"
#include "engine/pack_loader.h"
#include "engine/paths.h"
#include "engine/project.h"
#include "engine/providers_file.h"
#include "theme.h"
#include "translator.h"
#include "workflow/readiness_sheet.h"

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
// omarchy declares none; fugue-helper declares idle_timeout_s (engine/helper_provider.h). A
// field the file holds that no module declares is shown as it is, secret when its name says so.
QList<ModuleField> moduleFields(const ProviderInstance &instance) {
    static const QRegularExpression secretName(QStringLiteral("key|token|secret|password"),
                                               QRegularExpression::CaseInsensitiveOption);
    QList<ModuleField> fields;
    if (instance.type == QLatin1String("fugue-helper"))
        fields.append({QStringLiteral("idle_timeout_s"), false});
    for (auto it = instance.settings.begin(); it != instance.settings.end(); ++it)
        if (!std::any_of(fields.begin(), fields.end(), [&](const ModuleField &f) { return f.name == it.key(); }))
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

void SettingsModel::attach(Translator *translator, CanvasModel *canvas, const Registry *registry, Theme *theme) {
    m_packs.setRegistry(registry);
    m_types = registry;
    m_translator = translator;
    m_canvas = canvas;
    m_theme = theme;
    pushLive();
}

void SettingsModel::registerCommands(CommandRegistry &registry, std::function<void(const QUrl &)> openUrl) {
    m_registry = &registry;
    m_openUrl = std::move(openUrl);
    registry.setHandler(QStringLiteral("app.settings"),
                        [&registry] { emit registry.windowRequested(QStringLiteral("app.settings")); });
    // The picture mode (#263), a preference: the View menu writes the same key as the panel, as a choice with a check.
    for (const PictureMode mode : {PictureMode::Smart, PictureMode::Sinks, PictureMode::All}) {
        const QString name = pictureModeName(mode);
        const QString id = QStringLiteral("view.picture-") + name;
        registry.setHandler(id, [this, name] { set(QStringLiteral("picture_mode"), name); });
        registry.setCheckedWhen(id, [this, name] { return pictureMode() == name; });
    }
    registry.setHandler(QStringLiteral("cache.clear-previews"),
                        [&registry] { emit registry.windowRequested(QStringLiteral("cache.clear-previews")); });
}

void SettingsModel::loadSettings() {
    m_file = readSettings(m_path);
    const QStringList inherits = sessionInherits();
    const bool layered = inherits.contains(settingClassName(SettingClass::Models))
                         || inherits.contains(settingClassName(SettingClass::Preferences));
    m_base = layered ? readSettings(baseSettingsPath()) : SettingsFile();
    m_effective = layerSettings(m_file, m_base, inherits, baseDataDir() + QStringLiteral("/models"));
}

QVariantMap SettingsModel::origins() const {
    const QStringList inherits = sessionInherits();
    QVariantMap map;
    for (const QString &key : settingKeys()) {
        const SettingOrigin origin = m_effective.origins.value(key, SettingOrigin::BuiltIn);
        QString name = origin == SettingOrigin::Default ? QStringLiteral("inherited")
                       : origin == SettingOrigin::Own   ? QStringLiteral("own")
                                                        : QStringLiteral("built-in");
        // An own value of a key the Default also sets, in a class this project inherits: it can be given up.
        const QString kind = settingClassName(settingClass(key));
        if (origin == SettingOrigin::Own && !kind.isEmpty() && inherits.contains(kind) && m_base.document.contains(key)
            && checkSetting(key, m_base.document.value(key)).isEmpty() && key != QLatin1String("model_paths")
            && key != QLatin1String("model_folder_kinds"))
            name = QStringLiteral("override");
        map.insert(key, name);
    }
    return map;
}

QStringList SettingsModel::inheritedModelPaths() const {
    QStringList list;
    if (m_effective.settings.inheritsModels)
        list << tilde(baseDataDir() + QStringLiteral("/models"));
    for (const QString &path : m_effective.settings.inheritedModelPaths)
        list << path;
    return list;
}

QStringList SettingsModel::inheritedPackPaths() const {
    QStringList list;
    for (const QString &path : fugue::inheritedPackPaths())
        list << tilde(path);
    return list;
}

QString SettingsModel::removeInheritedPath(const QString &key, const QString &path) {
    if (key != QLatin1String("model_paths") && key != QLatin1String("pack_paths"))
        return tr("%1 is not a list of folders.").arg(key);
    return tr("%1 is a folder this project inherits from the Default project, and is read-only here. Change it in the Default project "
              "(start Fugue without a project), or turn off inheriting %2 in Project settings.")
        .arg(path, key == QLatin1String("model_paths") ? tr("models") : tr("packs"));
}

QString SettingsModel::useDefault(const QString &key) {
    if (origins().value(key).toString() != QLatin1String("override"))
        return tr("%1 has no value of its own to give up.").arg(key);
    return set(key, QJsonValue(QJsonValue::Undefined));
}

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
        m_translator->setLanguageOverride(m_effective.settings.language);
    if (m_canvas)
        m_canvas->setNewNodeOptions(m_effective.settings.newSeed, m_effective.settings.embedWorkflow);
    if (m_canvas)
        m_canvas->setPictureMode(pictureModeFromName(m_effective.settings.pictureMode));
    if (m_theme)
        m_theme->setCornerRadius(m_effective.settings.cornerRadius);
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
        // A bare code names the language, not a country's variant ("English", not "American
        // English").
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

namespace {
// The library of these settings, with the download folder they name.
ModelLibrary downloadLibrary(const Settings &settings, const Registry *types) {
    ModelLibrary library(ModelLibrary::roots(settings), types, ModelLibrary::rootKinds(settings));
    if (!settings.downloadFolder.isEmpty())
        library.setDownloadRoot(QDir::cleanPath(expandHome(settings.downloadFolder)));
    return library;
}
} // namespace

QString SettingsModel::downloadFolder() const { return downloadLibrary(m_effective.settings, m_types).downloadRoot(); }

QVariantList SettingsModel::downloadFolders() const {
    const ModelLibrary library = downloadLibrary(m_effective.settings, m_types);
    QStringList roots = m_types ? library.downloadRoots({}) : library.roots(); // those that take any model, as the sheet offers
    QVariantList list;
    for (const QString &root : roots)
        list.append(QVariantMap{{"path", root}, {"title", ReadinessSheet::rootTitle(root, library.roots().value(0))}});
    return list;
}

QString SettingsModel::set(const QString &key, const QVariant &value) {
    if (!m_file.writable)
        return tr("Fix %1 first: it is not a JSON object.").arg(tilde(m_path));
    QJsonValue json = QJsonValue::fromVariant(value);
    if (key == QLatin1String("output_folder") || key == QLatin1String("inputs_folder")) {
        const QString typed = pathText(value).trimmed();
        json = typed.isEmpty() ? settingDefault(key) : QJsonValue(folderValue(typed));
    } else if (key == QLatin1String("download_folder")) {
        // Fugue's own folder is the default: no key.
        const QString typed = QDir::cleanPath(expandHome(pathText(value).trimmed()));
        const QString own = ModelLibrary::roots(m_effective.settings).value(0);
        json = pathText(value).trimmed().isEmpty() || typed == own ? settingDefault(key) : QJsonValue(typed);
    } else if (key == QLatin1String("language") && !json.isUndefined()) { // undefined: give the key up
        const QString code = value.toString();
        if (code.isEmpty())
            json = settingUnsetValue(key); // follow Omarchy, written over a Default's language
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
    const QString removed = list.takeAt(index);
    if (const QString problem = set(key, QJsonArray::fromStringList(list).toVariantList()); !problem.isEmpty())
        return problem;
    if (key == QLatin1String("model_paths") && !modelFolderKind(removed).isEmpty())
        return setModelFolderKind(removed, QString());
    return {};
}

namespace {
// The kinds of model a folder can be set to hold, and how they read.
QList<std::pair<QString, QString>> folderKinds() {
    return {{QString(), QCoreApplication::translate("fugue::SettingsModel", "Found by its files")},
            {ModelLibrary::kLayout, QCoreApplication::translate("fugue::SettingsModel", "Model folders (ComfyUI layout)")},
            {QStringLiteral("fugue/lora"), QCoreApplication::translate("fugue::SettingsModel", "LoRAs")},
            {QStringLiteral("fugue/vae"), QCoreApplication::translate("fugue::SettingsModel", "VAEs")},
            {QStringLiteral("fugue/diffusion-model"), QCoreApplication::translate("fugue::SettingsModel", "Diffusion models")},
            {QStringLiteral("fugue/text-encoder"), QCoreApplication::translate("fugue::SettingsModel", "Text encoders")},
            {QStringLiteral("fugue/model"), QCoreApplication::translate("fugue::SettingsModel", "Checkpoints")}};
}
} // namespace

QString SettingsModel::modelFolderKind(const QString &path) const { return m_effective.settings.modelFolderKinds.value(path); }

QVariantMap SettingsModel::modelFolderKindsSet() const {
    QVariantMap map;
    for (auto it = m_effective.settings.modelFolderKinds.begin(); it != m_effective.settings.modelFolderKinds.end(); ++it)
        map.insert(it.key(), it.value());
    return map;
}

QString SettingsModel::setModelFolderKind(const QString &path, const QString &kind) {
    QJsonObject kinds = m_file.document.value(QStringLiteral("model_folder_kinds")).toObject();
    if (kind.isEmpty())
        kinds.remove(path);
    else
        kinds.insert(path, kind);
    return set(QStringLiteral("model_folder_kinds"), kinds.toVariantMap());
}

QString SettingsModel::modelFolderFound(const QString &path) const {
    const ModelLibrary library(ModelLibrary::roots(m_effective.settings), m_types);
    const QString kind = library.kindOf(QDir::cleanPath(expandHome(path)));
    if (kind.isEmpty())
        return tr("Nothing Fugue can tell: set what it holds");
    for (const auto &[value, text] : folderKinds())
        if (value == kind)
            return kind == ModelLibrary::kLayout ? tr("Found: ComfyUI's model folders") : tr("Found: %1").arg(text);
    return tr("Found: %1").arg(kind);
}

QVariantList SettingsModel::modelFolderKinds() const {
    QVariantList list;
    for (const auto &[value, text] : folderKinds())
        list.append(QVariantMap{{"value", value}, {"text", text}});
    return list;
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

QStringList SettingsModel::providerTypes() const { return {QStringLiteral("omarchy"), QStringLiteral("fugue-helper")}; }

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
        m_registry->loadKeymap(effectiveKeymapPath());
}

} // namespace fugue
