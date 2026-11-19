#pragma once

// The settings panel's model (spec 1.4, #51): settings.json and providers.json as the panel
// shows them, and every change it makes.
//
// QML only asks: each change goes through set(), the list and provider calls, which check the
// value here (never trusting the panel) and write the file at once; a refused change returns
// why and writes nothing. What applies live is pushed out from here: the language to the
// Translator, what new nodes start with to the CanvasModel; the canvas binds `navigation`.
//
// Provider keys and other secret fields never reach QML: an instance shows only whether
// one is set. The panel sends a new one once, to setProviderField(), and it goes only to
// providers.json.

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <functional>

#include "engine/providers.h"
#include "engine/settings.h"

class CanvasModel;
class Translator;

namespace fugue {

class CommandRegistry;

class SettingsModel : public QObject {
    Q_OBJECT
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
    Q_PROPERTY(QString newSeed READ newSeed NOTIFY changed)
    Q_PROPERTY(QString embedWorkflow READ embedWorkflow NOTIFY changed)
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

public:
    // `settingsFile` and `providersFile` are settings.json and providers.json (the standard
    // paths when empty).
    explicit SettingsModel(const QString &settingsFile = {}, const QString &providersFile = {},
                           QObject *parent = nullptr);

    // Where the settings that apply live go. Any may be null. Each gets the current values
    // at once.
    void attach(Translator *translator, CanvasModel *canvas);
    // app.settings opens the panel; the keymap page opens files with `openUrl`.
    void registerCommands(CommandRegistry &registry, std::function<void(const QUrl &)> openUrl);

    QString path() const { return m_path; }
    QString navigation() const { return m_file.settings.navigation; }
    QString language() const { return m_file.settings.language; }
    QVariantList languages() const;
    QString outputFolder() const;
    bool outputFolderIsDefault() const { return m_file.settings.outputFolder.isEmpty(); }
    QString inputsFolder() const;
    bool inputsFolderIsDefault() const { return m_file.settings.inputsFolder.isEmpty(); }
    QStringList modelPaths() const { return m_file.settings.modelPaths; }
    QStringList packPaths() const { return m_file.settings.packPaths; }
    QString newSeed() const { return m_file.settings.newSeed; }
    QString embedWorkflow() const { return m_file.settings.embedWorkflow; }
    QStringList problems() const { return m_file.problems; }
    bool writable() const { return m_file.writable; }

    QString providersPath() const { return m_providersPath; }
    QVariantList providers() const;
    QString providersProblem() const { return m_providersProblem; }
    QStringList providerTypes() const;

    // Sets one setting (a settings.json key) and writes the file. For a folder, empty text
    // goes back to the default; a folder may be text or the folder picker's URL. Empty when done, else why not (nothing is written).
    Q_INVOKABLE QString set(const QString &key, const QVariant &value);
    // Adds a folder to, or removes one from, model_paths or pack_paths.
    Q_INVOKABLE QString addPath(const QString &key, const QVariant &path);
    Q_INVOKABLE QString removePath(const QString &key, int index);
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

    QString m_path;
    QString m_providersPath;
    SettingsFile m_file;
    QList<ProviderInstance> m_instances;
    bool m_providersSaved = false;
    QString m_providersProblem;
    Translator *m_translator = nullptr;
    CanvasModel *m_canvas = nullptr;
    CommandRegistry *m_registry = nullptr;
    std::function<void(const QUrl &)> m_openUrl;
};

} // namespace fugue
