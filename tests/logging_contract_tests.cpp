#include "gbb/log.hpp"
#include "gbb/frontend_logging.hpp"
#include "gameboy/ppu.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_logging_contract() {
    auto& logger = gbb::Logger::instance();
    const auto directory = std::filesystem::temp_directory_path() /
                           "gbb-logger-contract-test";
    const auto path = directory / "nested" / "trace.log";
    std::filesystem::remove_all(directory);
    logger.set_level(gbb::LogLevel::trace);
    logger.set_memory_capacity(2);
    check(logger.set_file(path), "logger accepts a writable file sink");
    logger.write(gbb::LogLevel::debug, gbb::LogCategory::link,
                 "serial edge queued", {7, 12, 4096, 0xABCD});
    logger.flush();
    logger.close_file();

    std::ifstream input(path, std::ios::binary);
    const std::string record((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    check(record.find("[debug][link] session=7 frame=12 cycles=4096 rom=0xabcd ") !=
              std::string::npos &&
              record.find("serial edge queued") != std::string::npos,
          "logger writes structured level, category, and context fields");
    const auto recent = logger.recent_records();
    check(recent.size() == 1 &&
              recent.front().find("serial edge queued") != std::string::npos,
          "logger exposes recent records through its bounded memory sink");

    gbb::log_frontend_warning("frontend warning", {8, 13, 4097});
    const auto frontend_recent = logger.recent_records();
    check(frontend_recent.size() == 2 &&
              frontend_recent.back().find("[warning][frontend] session=8 frame=13 cycles=4097") !=
                  std::string::npos &&
              frontend_recent.back().find("frontend warning") != std::string::npos,
          "frontend adapter emits structured records with shared context");

    {
        gbb::LogContextScope session_scope({9, 20, 8192, 0x1234});
        logger.write(gbb::LogLevel::info, gbb::LogCategory::frontend,
                     "inherited context");
        {
            gbb::LogContextScope frame_scope({0, 21, 0});
            logger.write(gbb::LogLevel::info, gbb::LogCategory::frontend,
                         "nested context");
        }
    }
    const auto scoped_recent = logger.recent_records();
    check(scoped_recent.size() == 2 &&
              scoped_recent[0].find("session=9 frame=20 cycles=8192") !=
                  std::string::npos &&
              scoped_recent[0].find("rom=0x1234") != std::string::npos &&
              scoped_recent[1].find("session=9 frame=21 cycles=8192") !=
                  std::string::npos &&
              scoped_recent[1].find("rom=0x1234") != std::string::npos,
          "scoped logger context is inherited and supports partial overrides");

    // Asynchronous frontend work captures the initiating context and restores
    // it when the callback runs. Verify that a callback's captured metadata
    // wins over any unrelated context already active on its worker thread.
    gbb::LogContext captured_context{};
    {
        gbb::LogContextScope initiating_scope({11, 44, 16384, 0xCAFE});
        captured_context = gbb::current_log_context();
    }
    {
        gbb::LogContextScope unrelated_scope({99, 88, 777, 0xBEEF});
        auto callback_scope = gbb::LogContextScope::exact(captured_context);
        logger.write(gbb::LogLevel::info, gbb::LogCategory::frontend,
                     "asynchronous callback context");
    }
    const auto callback_recent = logger.recent_records();
    check(callback_recent.size() == 2 &&
              callback_recent.back().find(
                  "session=11 frame=44 cycles=16384 rom=0xcafe") !=
                  std::string::npos,
          "asynchronous callback scopes preserve the initiating diagnostic context");

    {
        gbb::LogContextScope unrelated_scope({99, 88, 777, 0xBEEF});
        auto empty_callback_scope = gbb::LogContextScope::exact({});
        const auto empty_context = gbb::current_log_context();
        check(empty_context.session == 0 && empty_context.frame == 0 &&
                  empty_context.cycles == 0 && empty_context.rom == 0,
              "an empty asynchronous snapshot clears stale worker context");
    }

    logger.set_memory_capacity(0);
    logger.set_memory_capacity(4);
    logger.set_level(gbb::LogLevel::trace);
    gameboy::Ppu ppu;
    const auto ppu_records = logger.recent_records();
    check(!ppu_records.empty() &&
              ppu_records.back().find("[trace][ppu]") != std::string::npos &&
              ppu_records.back().find("window event=construct") !=
                  std::string::npos,
          "PPU window tracing follows runtime logger level changes");
    logger.set_level(gbb::LogLevel::warning);
    logger.set_memory_capacity(0);
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    test_logging_contract();
    return failures == 0 ? 0 : 1;
}
