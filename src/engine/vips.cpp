#include "vips.h"

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

Error vipsUnavailable() {
    return makeError(QStringLiteral("internal"), QStringLiteral("libvips could not be started."));
}

Error vipsError(const vips::VError &error, const QString &code) {
    return makeError(code, QString::fromUtf8(error.what()).trimmed());
}

} // namespace fugue
