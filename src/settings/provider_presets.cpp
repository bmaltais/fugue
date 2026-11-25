#include "settings/provider_presets.h"

#include <QCoreApplication>
#include <QVariantMap>

namespace fugue {

QList<ProviderPreset> knownProviderPresets() {
    QList<ProviderPreset> rows;
    const auto cloud = [&](const QString &id, const QString &name, const QString &type, const QString &address,
                           const QString &keyUrl, const QString &description) {
        ProviderPreset p;
        p.id = id;
        p.name = name;
        p.group = QStringLiteral("cloud");
        p.type = type;
        p.address = address;
        p.needsKey = true;
        p.keyUrl = keyUrl;
        p.description = description;
        p.privacy = QCoreApplication::translate("fugue::ProviderPresets", "Sends your pictures and prompts to %1").arg(name);
        rows.append(p);
    };
    // The addresses and key pages are the services' own documentation (checked 2026-11-25); fal's queue address is the
    // `fal` provider type's own default, so it stays empty here.
    cloud(QStringLiteral("fal"), QStringLiteral("fal.ai"), QStringLiteral("fal"), {},
          QStringLiteral("https://fal.ai/dashboard/keys"), QCoreApplication::translate("fugue::ProviderPresets", "Many image and video models, run in the cloud"));
    cloud(QStringLiteral("openai"), QStringLiteral("OpenAI"), QStringLiteral("http"), QStringLiteral("https://api.openai.com/v1"),
          QStringLiteral("https://platform.openai.com/settings/organization/api-keys"), QCoreApplication::translate("fugue::ProviderPresets", "GPT image and text models"));
    cloud(QStringLiteral("together"), QStringLiteral("Together AI"), QStringLiteral("http"), QStringLiteral("https://api.together.ai/v1"),
          QStringLiteral("https://api.together.ai/settings/api-keys"), QCoreApplication::translate("fugue::ProviderPresets", "Open image and text models, run in the cloud"));
    cloud(QStringLiteral("openrouter"), QStringLiteral("OpenRouter"), QStringLiteral("http"), QStringLiteral("https://openrouter.ai/api/v1"),
          QStringLiteral("https://openrouter.ai/settings/keys"), QCoreApplication::translate("fugue::ProviderPresets", "Text models from many makers through one key"));

    ProviderPreset local;
    local.id = QStringLiteral("local-openai");
    local.name = QCoreApplication::translate("fugue::ProviderPresets", "Local server");
    local.group = QStringLiteral("own");
    local.type = QStringLiteral("http");
    local.address = QStringLiteral("http://localhost:1234/v1");
    local.description = QCoreApplication::translate("fugue::ProviderPresets", "LM Studio, llama.cpp or vLLM on this machine");
    rows.append(local);

    ProviderPreset omarchy;
    omarchy.id = QStringLiteral("omarchy-host");
    omarchy.name = QCoreApplication::translate("fugue::ProviderPresets", "Omarchy host");
    omarchy.group = QStringLiteral("own");
    omarchy.type = QStringLiteral("omarchy");
    omarchy.needsAddress = true;
    omarchy.description = QCoreApplication::translate("fugue::ProviderPresets", "Another Omarchy machine's model servers");
    rows.append(omarchy);

    ProviderPreset helper;
    helper.id = QStringLiteral("fugue-helper-host");
    helper.name = QCoreApplication::translate("fugue::ProviderPresets", "Fugue helper");
    helper.group = QStringLiteral("own");
    helper.type = QStringLiteral("fugue-helper");
    helper.needsKey = true;
    helper.needsAddress = true;
    helper.description = QCoreApplication::translate("fugue::ProviderPresets", "Fugue's helper on another machine");
    rows.append(helper);
    return rows;
}

ProviderPreset providerPresetById(const QString &id) {
    for (const ProviderPreset &p : knownProviderPresets())
        if (p.id == id)
            return p;
    return {};
}

QVariantList providerPresetsVariant() {
    QVariantList list;
    for (const ProviderPreset &p : knownProviderPresets())
        list.append(QVariantMap{{"id", p.id},
                                {"name", p.name},
                                {"group", p.group},
                                {"groupTitle", p.group == QLatin1String("cloud") ? QCoreApplication::translate("fugue::ProviderPresets", "Cloud services") : QCoreApplication::translate("fugue::ProviderPresets", "Your own machines")},
                                {"type", p.type},
                                {"address", p.address},
                                {"needsKey", p.needsKey},
                                {"needsAddress", p.needsAddress},
                                {"keyUrl", p.keyUrl},
                                {"description", p.description},
                                {"privacy", p.privacy}});
    return list;
}

} // namespace fugue
