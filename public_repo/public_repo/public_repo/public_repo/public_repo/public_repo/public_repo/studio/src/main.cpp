#include <cstdio>
#include <filesystem>

#include "studio/app.h"

int main(int argc, char** argv) {
    dragon::studio::StudioApp app;
    if (!app.init(1024, 680)) {
        std::fprintf(stderr, "error: failed to initialize DragonRecomp Studio\n");
        return 1;
    }

    if (argc >= 2) {
        std::filesystem::path rom_arg(argv[1]);
        if (std::filesystem::exists(rom_arg)) {
            app.load_rom(rom_arg);
        }
    }

    app.run();
    return 0;
}
