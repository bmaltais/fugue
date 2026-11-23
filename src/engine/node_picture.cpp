#include "engine/node_picture.h"

#include "engine/node_type.h"

namespace fugue {

QString pictureModeName(PictureMode mode) {
    switch (mode) {
    case PictureMode::Smart:
        break;
    case PictureMode::Sinks:
        return QStringLiteral("sinks");
    case PictureMode::All:
        return QStringLiteral("all");
    }
    return QStringLiteral("smart");
}

PictureMode pictureModeFromName(const QString &name) {
    if (name == QLatin1String("sinks"))
        return PictureMode::Sinks;
    if (name == QLatin1String("all"))
        return PictureMode::All;
    return PictureMode::Smart;
}

QString pictureOverrideName(PictureOverride value) {
    switch (value) {
    case PictureOverride::Show:
        return QStringLiteral("show");
    case PictureOverride::Hide:
        return QStringLiteral("hide");
    case PictureOverride::Default:
        break;
    }
    return {};
}

PictureOverride pictureOverrideFromName(const QString &name) {
    if (name == QLatin1String("show"))
        return PictureOverride::Show;
    if (name == QLatin1String("hide"))
        return PictureOverride::Hide;
    return PictureOverride::Default;
}

bool pictureRoleFromName(const QString &name, PictureRole *role) {
    if (name == QLatin1String("always"))
        *role = PictureRole::Always;
    else if (name == QLatin1String("result"))
        *role = PictureRole::Result;
    else if (name == QLatin1String("never"))
        *role = PictureRole::Never;
    else
        return false;
    return true;
}

PictureRole pictureRole(const NodeType &type) {
    PictureRole role;
    if (pictureRoleFromName(type.picture, &role))
        return role;
    if (type.sink)
        return PictureRole::Always;
    // Pure transforms repeat the picture before them.
    static const char *const quiet[] = {"Image/Transform", "Image/Adjust", "Image/Mask", "Image/Canvas"};
    for (const char *category : quiet)
        if (type.category == QLatin1String(category) || type.category.startsWith(QLatin1String(category) + QLatin1Char('/')))
            return PictureRole::Never;
    return PictureRole::Result;
}

bool isInputNode(const NodeType &type) {
    bool file = false;
    for (const InputSpec &input : type.inputs) {
        if (input.widget() == QLatin1String("file"))
            file = true;
        else if (const QString &id = input.type.type.id; id == "image" || id == "video" || id == "audio")
            return false;
    }
    return file;
}

bool hasPictureSlot(const NodeType &type) {
    if (type.sink || isInputNode(type))
        return true;
    for (const OutputSpec &output : type.outputs) {
        const QString &id = output.type.type.id;
        if (id == "image" || id == "video" || id == "audio")
            return true;
    }
    return false;
}

bool showsPicture(PictureMode mode, PictureRole role, bool inputNode, PictureOverride override) {
    if (override == PictureOverride::Show)
        return true;
    if (override == PictureOverride::Hide)
        return false;
    if (mode == PictureMode::All)
        return true;
    if (role == PictureRole::Always)
        return true;
    if (role == PictureRole::Never)
        return false;
    return mode == PictureMode::Smart || inputNode;
}

} // namespace fugue
