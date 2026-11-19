#include "vips.h"

#include <mutex>

#include <vips/vips8>

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

} // namespace fugue
