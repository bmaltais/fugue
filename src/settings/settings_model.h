#pragma once

// The settings panel's model (spec 1.4, #51): settings.json and providers.json as the panel
// shows them, and every change it makes.
//
// QML only asks: each change goes through set(), the list and provider calls, which check the
// value here (never trusting the panel) and write the file at once; a refused change returns
// why and writes nothing. What applies live is pushed out from here: the language to the
// Translator, what new nodes start with to the CanvasModel, the corner radius to the Theme;
// the canvas binds `navigation`.
//
// Provider keys and other secret fields never reach QML: an instance shows only whether
// one is set. The panel sends a new one once, to setProviderField(), and it goes only to
// providers.json.

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

#include "engine/providers.h"
#include "settings/cache_model.h"
#include "settings/packs_model.h"
#include "engine/settings.h"

class CanvasModel;
class Theme;
class Translator;

namespace fugue {

class CommandRegistry;
class Registry;

class SettingsModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("made in C++, read through App")
    Q_PROPERTY(QString path READ path CONSTANT)
    Q_PROPERTY(QString navigation READ navigation NOTIFY changed)
    Q_PROPERTY(QString language READ language NOTIFY changed) // empty: follow Omarchy
    // {code, name} for each shipped catalog, the name in its own language.
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    // The folders in effect, with ~ for the home folder, and whether each is the default.
    Q_PROPERTY(QString outputFolder READ outputFolder NOTIFY changed)
    Q_PROPERTY(bool outputFolderIsDefault READ outputFolderIsDefault NOTIFY changed)
    Q_PROPERTY(QString inputsFolder READ inputsFolder NOTIFY changed)
    Q_PROPERTY(bool inputsFolderIsDefault READ inputsFolderIsDefault NOTIFY changed)
    Q_PROPERTY(QStringList modelPaths READ modelPaths NOTIFY changed)
    Q_PROPERTY(QStringList packPaths READ packPaths NOTIFY changed)
    // model_folder_kinds: what each extra model folder is set to hold, by its path (#86).
    Q_PROPERTY(QVariantMap modelFolderKindsSet READ modelFolderKindsSet NOTIFY changed)
    Q_PROPERTY(QString newSeed READ newSeed NOTIFY changed)
    Q_PROPERTY(QString embedWorkflow READ embedWorkflow NOTIFY changed)
    Q_PROPERTY(QString pictureMode READ pictureMode NOTIFY changed)
    Q_PROPERTY(QString stepPreview READ stepPreview NOTIFY changed)
    // The canvas corners (#63): px from 0 to maxCornerRadius, or -1 to follow Omarchy.
    Q_PROPERTY(int cornerRadius READ cornerRadius NOTIFY changed)
    Q_PROPERTY(int maxCornerRadius READ maxCornerRadius CONSTANT)
    // The hover magnifier (#74, #123): off unless turned on; the canvas binds it.
    Q_PROPERTY(bool hoverMagnifier READ hoverMagnifier NOTIFY changed)
    // Reopen the last session's saved tabs at start (#212): on unless turned off; the window follows it live.
    Q_PROPERTY(bool restoreTabs READ restoreTabs NOTIFY changed)
    // The cache limits (#141): GB of results and of previews (0: no limit), runs kept past a week.
    Q_PROPERTY(double cacheResultsGb READ cacheResultsGb NOTIFY changed)
    Q_PROPERTY(double cacheThumbnailsGb READ cacheThumbnailsGb NOTIFY changed)
    Q_PROPERTY(int runsKept READ runsKept NOTIFY changed)
    // Where each setting's value comes from (spec 1.4.2), by key: "inherited" (the Default project's, shown
    // with the tag "Inherited from Default"), "override" (this project's own value over one the Default has:
    // useDefault() takes it back), "own" or "built-in". Only a project that inherits the key's class has
    // "inherited" or "override".
    Q_PROPERTY(QVariantMap origins READ origins NOTIFY changed)
    // The folders of the Default project this project reads and never writes: its models folder and
    // model_paths (when `models` is inherited), its packs folder and pack_paths (when `packs` is); ~ for home.
    Q_PROPERTY(QStringList inheritedModelPaths READ inheritedModelPaths NOTIFY changed)
    Q_PROPERTY(QStringList inheritedPackPaths READ inheritedPackPaths NOTIFY changed)
    // What is wrong with settings.json, one line each, and whether the panel may write it.
    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    Q_PROPERTY(bool writable READ writable NOTIFY changed)

    Q_PROPERTY(QString providersPath READ providersPath CONSTANT)
    // One map per instance, in preference order: name, type, address, enabled, keySet,
    // saved (false for a default not yet in providers.json), fields (the module's own
    // settings: name, secret, value (empty for a secret), set).
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    // Why providers.json cannot be read; the providers page is read-only while it is set.
    Q_PROPERTY(QString providersProblem READ providersProblem NOTIFY providersChanged)
    // The provider types an instance can be added as.
    Q_PROPERTY(QStringList providerTypes READ providerTypes CONSTANT)
    // The Packs page: install, trust and remove (#25).
    Q_PROPERTY(fugue::PacksModel *packs READ packs CONSTANT)
    // The Cache page: sizes, Clear and the automatic cleaning (#141).
    Q_PROPERTY(fugue::CacheModel *cache READ cache CONSTANT)

public:
    // `settingsFile` and `providersFile` are settings.json and providers.json (the standard
    // paths when empty).
    explicit SettingsModel(const QString &settingsFile = {}, const QString &providersFile = {},
                           QObject *parent = nullptr);

    // Where the settings that apply live go. Any may be null. Each gets the current values
    // at once.
    void attach(Translator *translator, CanvasModel *canvas, const Registry *registry = nullptr,
                Theme *theme = nullptr);
    // app.settings opens the panel; cache.clear-previews opens it on the Cache page with the
    // question for the previews; the keymap page opens files with `openUrl`.
    void registerCommands(CommandRegistry &registry, std::function<void(const QUrl &)> openUrl);

    QString path() const { return m_path; }
    // The settings that can be inherited read as the session sees them (the project's, else the Default's).
    QString navigation() const { return m_effective.settings.navigation; }
    QString language() const { return m_effective.settings.language; }
    QVariantList languages() const;
    QString outputFolder() const;
    bool outputFolderIsDefault() const { return m_file.settings.outputFolder.isEmpty(); }
    QString inputsFolder() const;
    bool inputsFolderIsDefault() const { return m_file.settings.inputsFolder.isEmpty(); }
    QStringList modelPaths() const { return m_file.settings.modelPaths; }
    QStringList packPaths() const { return m_file.settings.packPaths; }
    QString newSeed() const { return m_effective.settings.newSeed; }
    QString embedWorkflow() const { return m_effective.settings.embedWorkflow; }
    QString pictureMode() const { return m_effective.settings.pictureMode; } // smart, sinks or all (#263)
    QString stepPreview() const { return m_effective.settings.stepPreview; } // off, fast, tae or vae (#297)
    int cornerRadius() const { return m_effective.settings.cornerRadius; }
    bool hoverMagnifier() const { return m_effective.settings.hoverMagnifier; }
    bool restoreTabs() const { return m_effective.settings.restoreTabs; }
    QVariantMap origins() const;
    QStringList inheritedModelPaths() const;
    QStringList inheritedPackPaths() const;
    double cacheResultsGb() const { return m_file.settings.cacheResultsGb; }
    double cacheThumbnailsGb() const { return m_file.settings.cacheThumbnailsGb; }
    int runsKept() const { return m_file.settings.runsKept; }
    static int maxCornerRadius() { return Settings::maxCornerRadius; }
    QStringList problems() const { return m_effective.problems; }
    bool writable() const { return m_file.writable; }

    QString providersPath() const { return m_providersPath; }
    QVariantList providers() const;
    QString providersProblem() const { return m_providersProblem; }
    QStringList providerTypes() const;
    PacksModel *packs() { return &m_packs; }
    CacheModel *cache() { return &m_cache; }

    // Sets one setting (a settings.json key) and writes the file. For a folder, empty text
    // goes back to the default; a folder may be text or the folder picker's URL. Empty when done,
    // else why not (nothing is written).
    Q_INVOKABLE QString set(const QString &key, const QVariant &value);
    // Adds a folder to, or removes one from, model_paths or pack_paths.
    Q_INVOKABLE QString addPath(const QString &key, const QVariant &path);
    Q_INVOKABLE QString removePath(const QString &key, int index);
    // What the panel says when asked to remove a folder this project inherits from the Default: the refusal,
    // naming the Default as the place to change it. Nothing is written.
    Q_INVOKABLE QString removeInheritedPath(const QString &key, const QString &path);
    // Takes this project's own value of a setting away, so the Default's shows through again.
    Q_INVOKABLE QString useDefault(const QString &key);
    // What an extra model folder holds (#86): "" (found by its files), "layout" or a model type
    // id, as model_folder_kinds says; setting "" takes the folder's entry out.
    Q_INVOKABLE QString modelFolderKind(const QString &path) const;
    QVariantMap modelFolderKindsSet() const;
    Q_INVOKABLE QString setModelFolderKind(const QString &path, const QString &kind);
    // What a folder's files say it holds, as a person reads it ("LoRAs").
    Q_INVOKABLE QString modelFolderFound(const QString &path) const;
    // The kinds a folder can be set to hold: {value, text}, "" first.
    Q_INVOKABLE QVariantList modelFolderKinds() const;
    // Reads both files again.
    Q_INVOKABLE void reload();

    // Provider instances: each change writes every effective instance to providers.json
    // (mode 0600), defaults included. `field` is name, address, enabled, key or one of the
    // module's settings; an empty key or secret clears it.
    Q_INVOKABLE QString setProviderField(int row, const QString &field, const QVariant &value);
    Q_INVOKABLE QString addProvider(const QString &type);
    Q_INVOKABLE QString removeProvider(int row);
    Q_INVOKABLE QString moveProvider(int row, int to);

    // The keymap page: opens keybindings.json in the user's editor, first writing an empty
    // diff when there is none, and reads it again into the registry.
    Q_INVOKABLE void openKeymap();
    Q_INVOKABLE void reloadKeymap();

signals:
    void changed();
    void providersChanged();

private:
    void loadSettings();
    void loadProviders();
    void pushLive();
    QString saveProviders(const QList<ProviderInstance> &instances);
    QByteArray providersSnapshot() const;

    PacksModel m_packs;
    CacheModel m_cache;
    QString m_path;
    QString m_providersPath;
    SettingsFile m_file;
    SettingsFile m_base;            // the Default project's file, read only when the project inherits a class
    EffectiveSettings m_effective;  // m_file layered over m_base (spec 1.4.2)
    QList<ProviderInstance> m_instances;
    bool m_providersSaved = false;
    // What m_instances was read from (providersSnapshot()), to tell an edit made outside
    // the panel since.
    QByteArray m_providersSnapshot;
    QString m_providersProblem;
    Translator *m_translator = nullptr;
    CanvasModel *m_canvas = nullptr;
    Theme *m_theme = nullptr;
    CommandRegistry *m_registry = nullptr;
    const Registry *m_types = nullptr; // the node types and port types, for what a folder holds
    std::function<void(const QUrl &)> m_openUrl;
};

} // namespace fugue
