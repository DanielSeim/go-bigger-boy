#include "gbb/plugin_loader.hpp"

#include <cstdint>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

bool expect_create_failure(const std::string& path, const bool save_failure) {
    std::string error;
    const auto loader = gbb::PluginLoader::load(path, error);
    if (loader == nullptr) {
        std::cerr << "FAIL: runtime fixture did not load: " << error << '\n';
        return false;
    }
    try {
        auto core = loader->create(std::vector<std::uint8_t>{1, 2, 3, 4});
        if (!save_failure) {
            std::cerr << "FAIL: invalid runtime fixture was accepted\n";
            return false;
        }
        static_cast<void>(core->save_state());
        std::cerr << "FAIL: malformed blob was accepted\n";
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

bool expect_concurrent_rejection(const std::string& path) {
    std::string error;
    const auto loader = gbb::PluginLoader::load(path, error);
    if (loader == nullptr) {
        std::cerr << "FAIL: concurrent fixture did not load: " << error << '\n';
        return false;
    }
    auto core = loader->create(std::vector<std::uint8_t>{1, 2, 3, 4});
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> rejected{0};
    auto step = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        try {
            static_cast<void>(core->step_instruction());
        } catch (const std::exception&) {
            rejected.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first(step);
    std::thread second(step);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
    return rejected.load(std::memory_order_relaxed) == 1;
}

bool expect_exception_conversion(const std::string& path) {
    std::string error;
    const auto loader = gbb::PluginLoader::load(path, error);
    if (loader == nullptr) {
        std::cerr << "FAIL: exception fixture did not load: " << error << '\n';
        return false;
    }
    auto core = loader->create(std::vector<std::uint8_t>{1, 2, 3, 4});
    try {
        static_cast<void>(core->step_instruction());
    } catch (const std::exception&) {
        return true;
    }
    std::cerr << "FAIL: plugin result error was not converted to an exception\n";
    return false;
}

bool expect_allocator_alignment(const std::string& path) {
    std::string error;
    const auto loader = gbb::PluginLoader::load(path, error);
    if (loader == nullptr) {
        std::cerr << "FAIL: allocator fixture did not load: " << error << '\n';
        return false;
    }
    auto core = loader->create(std::vector<std::uint8_t>{1, 2, 3, 4});
    try {
        const auto state = core->save_state();
        return !state.empty();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: valid allocator request failed: " << exception.what()
                  << '\n';
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::cerr << "usage: gameboy_plugin_loader_runtime_tests BAD_FRAME "
                     "BAD_BLOB CREATE_FAILURE CONCURRENT EXCEPTION ALIGNMENT\n";
        return 2;
    }
    int failures = 0;
    if (!expect_create_failure(argv[1], false)) ++failures;
    if (!expect_create_failure(argv[2], true)) ++failures;
    if (!expect_create_failure(argv[3], false)) ++failures;
    if (!expect_concurrent_rejection(argv[4])) ++failures;
    if (!expect_exception_conversion(argv[5])) ++failures;
    if (!expect_allocator_alignment(argv[6])) ++failures;
    return failures == 0 ? 0 : 1;
}
