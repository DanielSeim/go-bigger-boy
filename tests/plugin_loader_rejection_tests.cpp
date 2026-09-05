#include "gbb/plugin_loader.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gameboy_plugin_loader_rejection_tests LIBRARY...\n";
        return 2;
    }
    int failures = 0;
    for (int index = 1; index < argc; ++index) {
        std::string error;
        const auto loader = gbb::PluginLoader::load(argv[index], error);
        if (loader != nullptr || error.empty()) {
            std::cerr << "FAIL: malformed plugin was accepted: " << argv[index]
                      << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
