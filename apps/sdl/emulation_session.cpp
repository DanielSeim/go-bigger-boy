#include "emulation_session.hpp"

#include "input_lifecycle.hpp"
#include "input_mapping.hpp"
#include "link_trace_file.hpp"
#include "pokemon_link_diagnostics.hpp"

#include <iomanip>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace gbb::sdl {

bool configure_video_pipeline(SdlResources& sdl,
                              const gameboy::VideoMode mode) {
    const auto presentation = mode == gameboy::VideoMode::integer
                                  ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                  : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    const auto logical_width = static_cast<int>(
        sdl.core_video_width * (sdl.split_screen ? 2U : 1U));
    if (!SDL_SetRenderLogicalPresentation(sdl.renderer, logical_width,
                                          static_cast<int>(sdl.core_video_height),
                                          presentation)) {
        return false;
    }
    const auto filtering = mode == gameboy::VideoMode::bilinear
                               ? SDL_SCALEMODE_LINEAR
                               : SDL_SCALEMODE_NEAREST;
    if (!SDL_SetTextureScaleMode(sdl.texture, filtering) ||
        !SDL_SetTextureScaleMode(sdl.link_texture, filtering)) {
        return false;
    }
    sdl.video_mode = mode;
    return true;
}

// Overlay rendering temporarily disables logical coordinates so controls and
// the menu can use the full Android window. Restore only the presentation
// transform afterwards; resetting texture filtering every frame is needlessly
// expensive on mobile GPUs and made touch feedback feel delayed.
bool restore_video_presentation(const SdlResources& sdl) {
    const auto presentation = sdl.video_mode == gameboy::VideoMode::integer
                                  ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                  : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    return SDL_SetRenderLogicalPresentation(
        sdl.renderer,
        static_cast<int>(sdl.core_video_width *
                         (sdl.split_screen ? 2U : 1U)),
        static_cast<int>(sdl.core_video_height), presentation);
}
void load_rom(const std::string& path,
              std::unique_ptr<gbb::EmulatorCore>& core,
              const gameboy::DisplayPalette& palette, SdlResources& sdl,
              const std::filesystem::path& preference_path) {
#ifdef __ANDROID__
    std::size_t byte_count{};
    void* loaded = SDL_LoadFile(path.c_str(), &byte_count);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string{"Could not read ROM: "} +
                                 SDL_GetError());
    }
    const std::unique_ptr<void, decltype(&SDL_free)> owned(loaded, SDL_free);
    const auto* begin = static_cast<const std::uint8_t*>(loaded);
    std::vector<std::uint8_t> bytes(begin, begin + byte_count);
    // Probe once to derive the persistence filename without exposing
    // cartridge details to the frontend. The actual instance is created
    // through the core registry with that path applied.
    auto metadata = gbb::create_core(bytes);
    gbb::CoreLoadOptions options;
    if (metadata->descriptor().has_battery && !preference_path.empty()) {
        const auto save_directory = preference_path / "saves";
        std::filesystem::create_directories(save_directory);
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0')
             << metadata->rom_fingerprint() << ".gb";
        options.persistence_path = save_directory / name.str();
    }
    auto replacement = gbb::create_core(std::move(bytes), options);
#else
    static_cast<void>(preference_path);
    auto replacement = gbb::create_core_from_file(
        std::filesystem::u8path(path));
#endif
    if (gbb::has_capability(replacement->descriptor().capabilities,
                            gbb::CoreCapability::printer)) {
        replacement->set_printer_enabled(true);
    }
    if (gbb::has_capability(replacement->descriptor().capabilities,
                            gbb::CoreCapability::compatibility_palette)) {
        replacement->set_compatibility_colors(palette.cgb_compatibility);
    }
    sdl.core_video_width = replacement->descriptor().video_width;
    sdl.core_video_height = replacement->descriptor().video_height;
    if (sdl.texture != nullptr) SDL_DestroyTexture(sdl.texture);
    sdl.texture = SDL_CreateTexture(
        sdl.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(sdl.core_video_width),
        static_cast<int>(sdl.core_video_height));
    if (sdl.texture == nullptr) {
        throw std::runtime_error(std::string{"Could not create framebuffer texture: "} +
                                 SDL_GetError());
    }
    if (!configure_video_pipeline(sdl, sdl.video_mode)) {
        throw std::runtime_error(std::string{"Could not configure core video pipeline: "} +
                                 SDL_GetError());
    }
    stop_rumble(sdl);
    flush_battery_safely(core.get());
    core = std::move(replacement);
}

#ifndef __ANDROID__
std::unique_ptr<gameboy::Emulator> load_link_player(
    const std::string& path, const gameboy::DisplayPalette& palette) {
    auto player = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge::from_file(std::filesystem::u8path(path)));
    player->bus().connect_printer();
    player->set_dmg_compatibility_colors(palette.cgb_compatibility);
    return player;
}

// A failed Pokémon Cable Club negotiation is otherwise indistinguishable
// from a game-side timeout. Keep a compact trace of the two serial ports so a
// user can reproduce one attempt and we can tell whether any bytes crossed
// the emulated cable. The file is deliberately outside the ROM/save data.
gbb::sdl::LinkTraceFile link_trace;

std::uint64_t link_trace_elapsed_ms() { return link_trace.elapsed_ms(); }

struct LinkTracePrevious {
    bool initialized{};
    std::uint64_t first_completed{};
    std::uint64_t second_completed{};
    bool first_active{};
    bool second_active{};
    struct PokemonState {
        bool initialized{};
        std::uint8_t link{};
        std::uint8_t link_alt{};
        std::uint8_t battle{};
        std::uint8_t battle_type{};
        std::uint8_t ui_state{};
    } first_pokemon, second_pokemon;
};

LinkTracePrevious link_trace_previous;

void append_trace_cpu(std::ostream& output,
                      const gameboy::Emulator& emulator) {
    const auto& registers = emulator.cpu().registers();
    output << " cpu_cycles=" << std::dec << emulator.cpu().total_cycles()
           << " pc=" << std::hex << registers.pc
           << " sp=" << registers.sp
           << " halted=" << emulator.cpu().halted()
           << " stopped=" << emulator.cpu().stopped();
}

void append_trace_pokemon(std::ostream& output,
                          const gameboy::Emulator& emulator) {
    if (!is_pokemon_gen1(emulator)) return;
    const auto& bus = emulator.bus();
    // These WRAM locations are Pokémon Red/Blue's serial exchange scratch
    // bytes and timeout counters. They are guest diagnostics only; reading
    // them does not affect the link handshake.
    const auto serial_wait_counter = static_cast<unsigned>(
        bus.read8(0xCC47) | (static_cast<unsigned>(bus.read8(0xCC48)) << 8));
    const auto serial_wait_counter2 = static_cast<unsigned>(
        bus.read8(0xD074) | (static_cast<unsigned>(bus.read8(0xD075)) << 8));
    const auto ui_state = pokemon_ui_state(bus);
    output << " game_link=" << std::hex << static_cast<unsigned>(bus.read8(0xD12B))
           << " game_link_alt=" << static_cast<unsigned>(bus.read8(0xD130))
           << " game_battle=" << static_cast<unsigned>(bus.read8(0xD057))
           << " game_battle_type=" << static_cast<unsigned>(bus.read8(0xD05A))
           << " game_serial_send=" << static_cast<unsigned>(bus.read8(0xCC42))
           << " game_serial_recv=" << static_cast<unsigned>(bus.read8(0xCC3E))
           << " game_serial_wait=" << serial_wait_counter
           << " game_serial_wait2=" << serial_wait_counter2
           << " game_ui=" << pokemon_ui_state_name(ui_state)
           << " party_count=" << static_cast<unsigned>(bus.read8(0xD163));
}

void append_trace_transfer_events(std::ostream& output,
                                  const std::uint64_t frame,
                                  const std::uint64_t elapsed_ms,
                                  const std::uint64_t first_completed,
                                  const std::uint64_t second_completed,
                                  const bool first_active,
                                  const bool second_active,
                                  const gameboy::SerialPort& first_serial,
                                  const gameboy::SerialPort& second_serial) {
    if (link_trace_previous.initialized) {
        if (first_completed > link_trace_previous.first_completed) {
            gbb::write_trace_event_prefix(output, "serial_complete",
                                          link_trace.session(), frame,
                                          elapsed_ms, link_trace.transport(),
                                          link_trace.role());
            output << " player=1 count=" << std::dec << first_completed
                   << " tx=" << std::hex
                   << static_cast<unsigned>(first_serial.last_transmitted())
                   << " rx=" << static_cast<unsigned>(first_serial.last_received())
                   << '\n';
        }
        if (second_completed > link_trace_previous.second_completed) {
            gbb::write_trace_event_prefix(output, "serial_complete",
                                          link_trace.session(), frame,
                                          elapsed_ms, link_trace.transport(),
                                          link_trace.role());
            output << " player=2 count=" << std::dec << second_completed
                   << " tx=" << std::hex
                   << static_cast<unsigned>(second_serial.last_transmitted())
                   << " rx=" << static_cast<unsigned>(second_serial.last_received())
                   << '\n';
        }
        if (first_active != link_trace_previous.first_active) {
            gbb::write_trace_event_prefix(output, "serial_active",
                                          link_trace.session(), frame,
                                          elapsed_ms, link_trace.transport(),
                                          link_trace.role());
            output << " player=1 value=" << first_active
                   << " internal=" << first_serial.internal_clock()
                   << " bits=" << std::dec
                   << static_cast<unsigned>(first_serial.bits_shifted()) << '\n';
        }
        if (second_active != link_trace_previous.second_active) {
            gbb::write_trace_event_prefix(output, "serial_active",
                                          link_trace.session(), frame,
                                          elapsed_ms, link_trace.transport(),
                                          link_trace.role());
            output << " player=2 value=" << second_active
                   << " internal=" << second_serial.internal_clock()
                   << " bits=" << std::dec
                   << static_cast<unsigned>(second_serial.bits_shifted()) << '\n';
        }
    }
    link_trace_previous.initialized = true;
    link_trace_previous.first_completed = first_completed;
    link_trace_previous.second_completed = second_completed;
    link_trace_previous.first_active = first_active;
    link_trace_previous.second_active = second_active;
}

void append_trace_pokemon_transition(
    std::ostream& output, const gameboy::Emulator& emulator,
    const unsigned player, const std::uint64_t frame,
    const std::uint64_t elapsed_ms, const std::uint64_t transfers_completed,
    LinkTracePrevious::PokemonState& previous) {
    if (!is_pokemon_gen1(emulator)) return;
    const auto& bus = emulator.bus();
    const auto ui_state = pokemon_ui_state(bus);
    const LinkTracePrevious::PokemonState current{
        true, bus.read8(0xD12B), bus.read8(0xD130), bus.read8(0xD057),
        bus.read8(0xD05A), static_cast<std::uint8_t>(ui_state)};
    if (!previous.initialized || current.link != previous.link ||
        current.link_alt != previous.link_alt ||
        current.battle != previous.battle ||
        current.battle_type != previous.battle_type) {
        gbb::write_trace_event_prefix(output, "pokemon_state", link_trace.session(),
                                      frame, elapsed_ms, link_trace.transport(),
                                      link_trace.role());
        output << " player=" << player
               << " link=" << std::hex << static_cast<unsigned>(current.link)
               << " link_alt=" << static_cast<unsigned>(current.link_alt)
               << " battle=" << static_cast<unsigned>(current.battle)
               << " battle_type="
               << static_cast<unsigned>(current.battle_type)
               << " ui="
               << pokemon_ui_state_name(static_cast<PokemonUiState>(current.ui_state))
               << " transfers=" << std::dec << transfers_completed << '\n';
    }
    previous = current;
}

void start_link_trace(const std::filesystem::path& preference_path,
                      const char* role_suffix) {
    link_trace.start(preference_path, role_suffix);
    link_trace_previous = {};
}

void stop_link_trace() noexcept {
    link_trace.stop();
    link_trace_previous = {};
}

void trace_link_frame(const gameboy::Emulator& first,
                      const gameboy::Emulator& second,
                      const int audio_queued_bytes) {
    if (!link_trace.is_open()) return;
    const auto& first_serial = first.bus().serial_port();
    const auto& second_serial = second.bus().serial_port();
    link_trace.advance_frame();
    auto& output = link_trace.stream();
    const auto frame = link_trace.frame();
    const auto elapsed_ms = link_trace_elapsed_ms();
    // One line per emulated frame is enough to identify the negotiation while
    // keeping the log small enough to attach from Windows.
    gbb::write_trace_event_prefix(output, "frame", link_trace.session(), frame,
                                  elapsed_ms, link_trace.transport(),
                                  link_trace.role());
    output << std::dec
               << " audio_queued_bytes=" << audio_queued_bytes << ' '
               << "p1(hr=" << std::hex << static_cast<unsigned>(first.bus().read8(0xFFAA))
               << ",sb=" << static_cast<unsigned>(first_serial.read_data())
               << ",sc=" << static_cast<unsigned>(first_serial.read_control())
               << ",active=" << first_serial.transfer_active()
               << ",int=" << first_serial.internal_clock()
               << ",bits=" << static_cast<unsigned>(first_serial.bits_shifted())
               << ",done=" << std::dec << first_serial.transfers_completed()
               << ",tx=" << std::hex << static_cast<unsigned>(first_serial.last_transmitted())
               << ",rx=" << static_cast<unsigned>(first_serial.last_received())
               << ") p2(hr=" << static_cast<unsigned>(second.bus().read8(0xFFAA))
               << ",sb=" << static_cast<unsigned>(second_serial.read_data())
               << ",sc=" << static_cast<unsigned>(second_serial.read_control())
               << ",active=" << second_serial.transfer_active()
               << ",int=" << second_serial.internal_clock()
               << ",bits=" << static_cast<unsigned>(second_serial.bits_shifted())
               << ",done=" << std::dec << second_serial.transfers_completed()
               << ",tx=" << std::hex << static_cast<unsigned>(second_serial.last_transmitted())
               << ",rx=" << static_cast<unsigned>(second_serial.last_received())
               << ") if=" << static_cast<unsigned>(first.bus().read8(0xFF0F))
               << '/' << static_cast<unsigned>(second.bus().read8(0xFF0F))
               << " ie=" << static_cast<unsigned>(first.bus().read8(0xFFFF))
               << '/' << static_cast<unsigned>(second.bus().read8(0xFFFF))
               << " phase=" << std::dec << first_serial.phase()
               << '/' << second_serial.phase();
    append_trace_cpu(output, first);
    append_trace_cpu(output, second);
    append_trace_pokemon(output, first);
    append_trace_pokemon(output, second);
    output << '\n';
    append_trace_pokemon_transition(
        output, first, 1, frame, elapsed_ms,
        first_serial.transfers_completed(), link_trace_previous.first_pokemon);
    append_trace_pokemon_transition(
        output, second, 2, frame, elapsed_ms,
        second_serial.transfers_completed(), link_trace_previous.second_pokemon);
    append_trace_transfer_events(output,
                                 frame, elapsed_ms,
                                 first_serial.transfers_completed(),
                                 second_serial.transfers_completed(),
                                 first_serial.transfer_active(),
                                 second_serial.transfer_active(),
                                 first_serial, second_serial);
    output.flush();
}

void trace_remote_frame(const gameboy::Emulator& emulator,
                        const RemoteLinkSession& remote,
                        const int audio_queued_bytes) {
    if (!link_trace.is_open()) return;
    const auto& serial = emulator.bus().serial_port();
    link_trace.advance_frame();
    auto& output = link_trace.stream();
    const auto frame = link_trace.frame();
    const auto elapsed_ms = link_trace_elapsed_ms();
    gbb::write_trace_event_prefix(output, "frame", link_trace.session(), frame,
                                  elapsed_ms, link_trace.transport(),
                                  link_trace.role());
    output << std::dec
               << " audio_queued_bytes=" << audio_queued_bytes << ' '
               << "hr=" << std::hex
               << static_cast<unsigned>(emulator.bus().read8(0xFFAA))
               << " ab=" << static_cast<unsigned>(emulator.bus().read8(0xFFAB))
               << " ac=" << static_cast<unsigned>(emulator.bus().read8(0xFFAC))
               << " ad=" << static_cast<unsigned>(emulator.bus().read8(0xFFAD))
               << " sb=" << static_cast<unsigned>(serial.read_data())
               << " sc=" << static_cast<unsigned>(serial.read_control())
               << " active=" << serial.transfer_active()
               << " int=" << serial.internal_clock()
               << " bits=" << static_cast<unsigned>(serial.bits_shifted())
               << " done=" << std::dec << serial.transfers_completed()
               << " tx=" << std::hex
               << static_cast<unsigned>(serial.last_transmitted())
               << " rx=" << static_cast<unsigned>(serial.last_received())
               << " if=" << static_cast<unsigned>(emulator.bus().read8(0xFF0F))
               << " ie=" << static_cast<unsigned>(emulator.bus().read8(0xFFFF))
               << " q=" << std::dec << remote.endpoint.requests_sent()
               << " r=" << remote.endpoint.responses_received()
               << " x=" << remote.endpoint.transfers_completed()
               << " i=" << remote.endpoint.requests_received()
               << " s=" << remote.endpoint.responses_sent()
               << " d=" << remote.endpoint.denials_received()
               << " D=" << remote.endpoint.denials_sent()
               << " u=" << remote.endpoint.responses_unmatched()
               << " w=" << remote.endpoint.waiting_for_peer()
               << " z=" << remote.endpoint.response_ready()
               << " hh=" << remote.endpoint.peer_hello_seen()
               << " pr=" << remote.endpoint.peer_request_seen()
               << " pb=" << remote.endpoint.peer_byte_released()
               << " pc=" << remote.endpoint.peer_clock_busy()
               << " bo=" << remote.endpoint.request_backoff()
               << " m=" << remote.channel.malformed_packets()
               << " phase=" << std::dec << serial.phase();
    append_trace_cpu(output, emulator);
    append_trace_pokemon(output, emulator);
    output << '\n';
    append_trace_pokemon_transition(
        output, emulator, 1, frame, elapsed_ms,
        serial.transfers_completed(), link_trace_previous.first_pokemon);
    append_trace_transfer_events(output,
                                 frame, elapsed_ms,
                                 serial.transfers_completed(),
                                 0,
                                 serial.transfer_active(),
                                 false,
                                 serial, serial);
    output.flush();
}

void reset_pokemon_link_handshake(gameboy::Emulator& emulator) {
    // The Gen I Cable Club keeps its negotiated role in HRAM in addition to
    // FF01/FF02. A linked player starts from a copy of the current game state,
    // so clear that transient protocol state before attaching the cable.
    constexpr std::uint16_t serial_status = 0xFFAA;
    emulator.bus().write8(serial_status, 0xFF);
    emulator.bus().write8(0xFFAB, 0x00); // hSerialReceivedNewData
    emulator.bus().write8(0xFFAC, 0x00); // hSerialSendData
    emulator.bus().write8(0xFFAD, 0x00); // hSerialReceiveData
    emulator.bus().write8(0xFF01, 0x02);
    emulator.bus().write8(0xFF02, 0x80);
    emulator.bus().write8(0xFFFF, static_cast<std::uint8_t>(
                                      emulator.bus().read8(0xFFFF) | 0x08U));
    emulator.bus().write8(0xFF0F, static_cast<std::uint8_t>(
                                      emulator.bus().read8(0xFF0F) & ~0x08U));
}

void start_local_link_session(
    const std::string& path, gameboy::Emulator& first,
    std::unique_ptr<gameboy::Emulator>& second,
    std::unique_ptr<gameboy::LinkSession>& session,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& first_endpoint,
    std::unique_ptr<gameboy::GameBoyLinkEndpoint>& second_endpoint,
    SdlResources& sdl, const gameboy::DisplayPalette& palette,
    const std::filesystem::path& preference_path,
    const bool link_diagnostics) {
    if (path.empty()) throw std::runtime_error("No ROM is currently loaded.");
    auto replacement = load_link_player(path, palette);
    // A second console must have its own battery image. Sharing the primary
    // .sav path gives both Pokémon instances the same trainer identity and
    // causes link trading/battles to reject the peer (and lets the last flush
    // silently overwrite the other console). Keep a persistent player-two
    // save beside the normal data, seeding it from player one's save only on
    // the first session. Copy the running game state below so both screens
    // begin at the same Cable Club prompt, but preserve player two's own RAM
    // image. The transient CPU/interrupt/timer/serial state is sanitized for
    // Pokémon immediately before the cable is attached.
    std::vector<std::uint8_t> preserved_player_two_save;
    auto player_two_save_exists = false;
    if (replacement->has_battery() && !preference_path.empty()) {
        const auto directory = preference_path / "link-saves";
        std::filesystem::create_directories(directory);
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0')
             << first.rom_fingerprint() << "-player2.gb";
        const auto player_two_base = directory / name.str();
        const auto player_two_save = [&] {
            auto save = player_two_base;
            save.replace_extension(".sav");
            return save;
        }();
        const auto already_exists = std::filesystem::exists(player_two_save);
        player_two_save_exists = already_exists;
        const auto seed = first.export_battery_save();
        replacement->bus().cartridge().set_persistence_path(player_two_base);
        if (already_exists) {
            preserved_player_two_save = replacement->export_battery_save();
        }
        if (!already_exists && !seed.empty()) {
            replacement->import_battery_save(seed);
        }
    }
    replacement->load_state(first.save_state());
    if (player_two_save_exists && !preserved_player_two_save.empty()) {
        replacement->import_battery_save(preserved_player_two_save);
    }
    if (is_pokemon_gen1(first)) {
        reset_pokemon_link_handshake(first);
        reset_pokemon_link_handshake(*replacement);
    }
    // Keep the trace counters scoped to this local session. This does not
    // touch guest-visible serial registers or an in-progress transfer.
    first.bus().serial_port().reset_diagnostics();
    replacement->bus().serial_port().reset_diagnostics();
    // Do not carry a held confirmation button into one side of the Cable Club
    // prompt when the session is attached.
    release_all_buttons(first);
    release_all_buttons(*replacement);
    // The normal single-console setup attaches the Game Boy Printer to the
    // serial port. During a link session that peripheral must be removed:
    // otherwise its response overwrites the byte received from the peer
    // before Pokémon's serial interrupt handler can read it.
    first.bus().connect_printer(false);
    replacement->bus().connect_printer(false);
    auto replacement_session = std::make_unique<gameboy::LinkSession>();
    auto replacement_first_endpoint =
        std::make_unique<gameboy::GameBoyLinkEndpoint>(first);
    auto replacement_second_endpoint =
        std::make_unique<gameboy::GameBoyLinkEndpoint>(*replacement);
    replacement_session->start(*replacement_first_endpoint,
                               *replacement_second_endpoint);
    second = std::move(replacement);
    session = std::move(replacement_session);
    first_endpoint = std::move(replacement_first_endpoint);
    second_endpoint = std::move(replacement_second_endpoint);
    if (link_diagnostics) start_link_trace(preference_path);
    if (link_trace.is_open()) {
        const auto message = "Link trace is being written to:\n" +
                             link_trace.path().string();
        static_cast<void>(SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, "GBB link diagnostics",
            message.c_str(), sdl.window));
    }
    sdl.split_screen = true;
    if (!configure_video_pipeline(sdl, sdl.video_mode)) {
        stop_link_trace();
        first.bus().connect_printer(true);
        sdl.split_screen = false;
        session.reset();
        second.reset();
        first_endpoint.reset();
        second_endpoint.reset();
        throw std::runtime_error("Could not configure split-screen presentation.");
    }
}

void stop_local_link_session(gameboy::Emulator& first,
                             std::unique_ptr<gameboy::Emulator>& second,
                             std::unique_ptr<gameboy::LinkSession>& session,
                             std::unique_ptr<gameboy::GameBoyLinkEndpoint>&
                                 first_endpoint,
                             std::unique_ptr<gameboy::GameBoyLinkEndpoint>&
                                 second_endpoint,
                             SdlResources& sdl) noexcept {
    stop_link_trace();
    session.reset();
    first_endpoint.reset();
    second_endpoint.reset();
    flush_battery_safely(second.get());
    second.reset();
    // Restore the primary console's normal serial peripheral after the
    // linked session has detached.
    first.bus().connect_printer(true);
    sdl.split_screen = false;
    static_cast<void>(configure_video_pipeline(sdl, sdl.video_mode));
}

void retry_local_link_session(gameboy::Emulator& first,
                               gameboy::Emulator& second,
                               gameboy::LinkSession& session) noexcept {
    session.retry();
    if (is_pokemon_gen1(first)) {
        reset_pokemon_link_handshake(first);
        reset_pokemon_link_handshake(second);
    } else {
        first.bus().serial_port().reset_link();
        second.bus().serial_port().reset_link();
    }
    first.bus().serial_port().reset_diagnostics();
    second.bus().serial_port().reset_diagnostics();
    release_all_buttons(first);
    release_all_buttons(second);
}

constexpr std::uint16_t remote_link_port = 8765;

void start_remote_link_session(gameboy::Emulator& emulator,
                               RemoteLinkSession& remote,
                               const bool hosting,
                               const std::filesystem::path& preference_path,
                               const bool link_diagnostics,
                               SDL_Window* window) {
    if (remote.enabled) return;
    emulator.bus().connect_printer(false);
    emulator.bus().serial_port().reset_link();
    emulator.bus().serial_port().reset_diagnostics();
    if (is_pokemon_gen1(emulator)) reset_pokemon_link_handshake(emulator);
    release_all_buttons(emulator);
    const auto ready = hosting
                           ? remote.channel.listen(remote_link_port)
                           : remote.channel.connect("127.0.0.1",
                                                    remote_link_port);
    if (!ready) {
        emulator.bus().connect_printer(true);
        throw std::runtime_error(
            hosting ? "Could not host TCP link on loopback port 8765."
                    : "Could not connect to TCP link host on 127.0.0.1:8765.");
    }
    remote.hosting = hosting;
    remote.diagnostics = link_diagnostics;
    remote.endpoint.set_arbitration_priority(hosting);
    remote.endpoint.attach(emulator.bus().serial_port(), remote.channel);
    remote.enabled = true;
    if (link_diagnostics) {
        start_link_trace(preference_path, hosting ? "host" : "join");
        if (link_trace.is_open()) {
            const auto message = "TCP link trace is being written to:\n" +
                                 link_trace.path().string();
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "GBB TCP link diagnostics",
                message.c_str(), window));
        } else {
            const auto message =
                "Diagnostics are enabled, but no trace file could be opened.\n"
                "Check that the executable folder is writable.";
            static_cast<void>(SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_WARNING, "GBB TCP link diagnostics",
                message, window));
        }
    }
}

void stop_remote_link_session(gameboy::Emulator& emulator,
                              RemoteLinkSession& remote) noexcept {
    stop_link_trace();
    remote.endpoint.detach();
    remote.channel.close();
    remote.enabled = false;
    remote.diagnostics = false;
    emulator.bus().connect_printer(true);
}

void retry_remote_link_session(gameboy::Emulator& emulator,
                               RemoteLinkSession& remote) {
    if (!remote.enabled) return;
    remote.endpoint.detach();
    remote.channel.close();
    emulator.bus().serial_port().reset_link();
    emulator.bus().serial_port().reset_diagnostics();
    if (is_pokemon_gen1(emulator)) reset_pokemon_link_handshake(emulator);
    release_all_buttons(emulator);
    const auto ready = remote.hosting
                           ? remote.channel.listen(remote_link_port)
                           : remote.channel.connect("127.0.0.1",
                                                    remote_link_port);
    if (!ready) {
        remote.endpoint.attach(emulator.bus().serial_port(), remote.channel);
        throw std::runtime_error("Could not retry the TCP link session.");
    }
    remote.endpoint.attach(emulator.bus().serial_port(), remote.channel);
}
#endif



} // namespace gbb::sdl
