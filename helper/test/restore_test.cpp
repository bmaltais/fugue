// The restore op's program against the engine's own checker (sd_dgl_check): a typo in the program or a
// DGL change at a pin move fails here, with no model and no GPU. The program's text comes from restore.h.
// Run: cmake --build build-helper-<backend> --target fugue-helper-restore-test && ctest --test-dir build-helper-<backend>

#include <cstdio>
#include <string>

#include "restore.h"
#include "stable-diffusion.h"

int main() {
    int failures = 0;
    const auto expect = [&](bool ok, const char *what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };
    const std::string program = fugue_helper::restoreScript("\"/models/seedvr2.safetensors\"", "\"/models/vae.safetensors\"", 512, 0.125, 959948902156062);
    const sd_status_t checked = sd_dgl_check(program.c_str(), true);
    expect(checked == SD_STATUS_OK, "the restore program passes sd_dgl_check");
    if (checked != SD_STATUS_OK)
        std::printf("     %s\n", sd_dgl_error());
    // The checker does fail a broken program (so the first check means something).
    std::string broken = fugue_helper::restoreScript("\"/m\"", "\"/v\"", 512, 0.125, 1);
    broken.replace(broken.find("load_dit("), 9, "load_dit_typo(");
    expect(sd_dgl_check(broken.c_str(), true) != SD_STATUS_OK, "a program with an unknown function fails sd_dgl_check");
    return failures ? 1 : 0;
}
