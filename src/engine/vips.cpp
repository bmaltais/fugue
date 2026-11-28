#include "vips.h"

#include <QFile>

#include <mutex>

namespace fugue {

bool initVips() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = VIPS_INIT("fugue") == 0;
        if (ok) {
            vips_concurrency_set(1);
            vips_cache_set_max(0);
        }
    });
    return ok;
}

QString untrustedLoaderProblem(const QString &path) {
    if (!initVips())
        return QStringLiteral("libvips could not be started.");
    const char *loader = vips_foreign_find_load(QFile::encodeName(path).constData());
    if (!loader) {
        vips_error_clear();
        return QStringLiteral("no image loader recognises it.");
    }
    const GType type = vips_type_find("VipsOperation", loader);
    if (!type)
        return QStringLiteral("its loader (%1) is not known.").arg(QString::fromLatin1(loader));
    auto *klass = static_cast<VipsOperationClass *>(g_type_class_ref(type));
    const bool untrusted = klass && (klass->flags & VIPS_OPERATION_UNTRUSTED);
    if (klass)
        g_type_class_unref(klass);
    const char *nickname = vips_nickname_find(type);
    return untrusted ? QStringLiteral("its format (%1) is not read from a pack's program.").arg(QString::fromLatin1(nickname ? nickname : loader))
                     : QString();
}

Error vipsUnavailable() {
    return makeError(QStringLiteral("internal"), QStringLiteral("libvips could not be started."));
}

Error vipsError(const vips::VError &error, const QString &code) {
    return makeError(code, QString::fromUtf8(error.what()).trimmed());
}

} // namespace fugue
