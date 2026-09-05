#include "windows_menu_bar.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gameboy/display_palette.hpp"

#include <atomic>
#include <memory>

struct DesktopMenuBar::Impl {
    ~Impl() { detach(); }

    void attach(SDL_Window* window) {
        detach();
        hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd_ == nullptr) return;
        root_ = CreateMenu();
        file_ = CreatePopupMenu();
        emulation_ = CreatePopupMenu();
        view_ = CreatePopupMenu();
        palette_ = CreatePopupMenu();
        video_ = CreatePopupMenu();
        tools_ = CreatePopupMenu();
        help_ = CreatePopupMenu();
        if (root_ == nullptr || file_ == nullptr || emulation_ == nullptr ||
            view_ == nullptr || palette_ == nullptr || video_ == nullptr ||
            tools_ == nullptr || help_ == nullptr) {
            detach();
            return;
        }
        append(file_, DesktopMenuCommand::open_rom, L"&Open ROM...\tCtrl+O");
        append(file_, DesktopMenuCommand::library, L"Game &Library\tCtrl+L");
        AppendMenuW(file_, MF_SEPARATOR, 0, nullptr);
        append(file_, DesktopMenuCommand::save_state, L"&Save State");
        append(file_, DesktopMenuCommand::load_state, L"&Load State");
        AppendMenuW(file_, MF_SEPARATOR, 0, nullptr);
        append(file_, DesktopMenuCommand::exit_app, L"E&xit");

        append(emulation_, DesktopMenuCommand::pause, L"&Pause / Resume\tSpace");
        append(emulation_, DesktopMenuCommand::reset, L"&Reset\tCtrl+R");
        append(emulation_, DesktopMenuCommand::link_session,
               L"Local &Link Session\tCtrl+Shift+L");
        append(emulation_, DesktopMenuCommand::link_retry,
               L"Retry Link Handshake\tCtrl+Shift+R");
        append(emulation_, DesktopMenuCommand::remote_host,
               L"Host TCP Link\tCtrl+Shift+H");
        append(emulation_, DesktopMenuCommand::remote_join,
               L"Join TCP Link\tCtrl+Shift+J");
        append(emulation_, DesktopMenuCommand::remote_discover,
               L"Discover LAN Link Hosts\tCtrl+Shift+D");
        append(emulation_, DesktopMenuCommand::remote_stop,
               L"Stop TCP Link");

        append(view_, DesktopMenuCommand::fullscreen, L"&Fullscreen\tF11");
        AppendMenuW(view_, MF_SEPARATOR, 0, nullptr);
        for (std::size_t index = 0; index < gameboy::display_palettes.size();
             ++index) {
            AppendMenuA(palette_, MF_STRING,
                        command_id(DesktopMenuCommand::palette_first) + index,
                        gameboy::display_palettes[index].name);
        }
        for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
            AppendMenuA(video_, MF_STRING,
                        command_id(DesktopMenuCommand::video_first) + index,
                        gameboy::video_modes[index].name.data());
        }
        AppendMenuW(view_, MF_POPUP, reinterpret_cast<UINT_PTR>(palette_),
                    L"&Color Palette");
        AppendMenuW(view_, MF_POPUP, reinterpret_cast<UINT_PTR>(video_),
                    L"&Video Pipeline");

        append(tools_, DesktopMenuCommand::controls,
               L"&Controls and Settings...\tCtrl+K");
        AppendMenuW(tools_, MF_SEPARATOR, 0, nullptr);
        append(tools_, DesktopMenuCommand::gameshark,
               L"&GameShark Cheats...\tCtrl+G");
        append(tools_, DesktopMenuCommand::debugger, L"&Debugger...\tF12");
        AppendMenuW(tools_, MF_SEPARATOR, 0, nullptr);
        append(tools_, DesktopMenuCommand::record_input,
               L"Record &Input...\tF6");
        append(tools_, DesktopMenuCommand::replay_input,
               L"&Replay Last Input\tF7");
        append(tools_, DesktopMenuCommand::tas_editor,
               L"&TAS Frame Editor...\tF8");
        append(tools_, DesktopMenuCommand::sprite_editor,
               L"&Sprite Editor...\tF9");

        append(help_, DesktopMenuCommand::shortcuts, L"&Shortcuts\tF1");
        append(help_, DesktopMenuCommand::about, L"&About Go Bigger Boy");

        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(file_), L"&File");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation_),
                    L"&Emulation");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(view_), L"&View");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_), L"&Tools");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(help_), L"&Help");
        SetMenu(hwnd_, root_);
        DrawMenuBar(hwnd_);
        active_ = this;
        previous_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
    }

    void detach() noexcept {
        if (hwnd_ != nullptr && previous_ != nullptr) {
            SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(previous_));
        }
        if (active_ == this) active_ = nullptr;
        if (hwnd_ != nullptr && root_ != nullptr) {
            SetMenu(hwnd_, nullptr);
            DrawMenuBar(hwnd_);
        }
        if (root_ != nullptr) DestroyMenu(root_);
        hwnd_ = nullptr;
        previous_ = nullptr;
        root_ = file_ = emulation_ = view_ = palette_ = video_ = tools_ = help_ =
            nullptr;
        pending_.store(0);
        state_valid_ = false;
    }

    [[nodiscard]] DesktopMenuCommand take_command() noexcept {
        return static_cast<DesktopMenuCommand>(pending_.exchange(0));
    }

    void update(const bool has_rom, const gbb::CoreCapability capabilities,
                const bool paused, const bool fullscreen,
                const bool recording, const std::size_t palette,
                const gameboy::VideoMode video, const bool link_active,
                const bool remote_link_active) {
        if (root_ == nullptr) return;
        // Updating a native Win32 menu is not a cheap draw-only operation:
        // each enable/check/modify call can invalidate the window and wake
        // the desktop compositor. This method runs from the emulation loop,
        // so repeating the same 20+ operations every frame caused severe
        // Windows-only stutter. Cache the complete menu state and touch the
        // native menu only when something observable has changed.
        if (state_valid_ && has_rom == last_has_rom_ &&
            capabilities == last_capabilities_ && paused == last_paused_ &&
            fullscreen == last_fullscreen_ && recording == last_recording_ &&
            palette == last_palette_ && video == last_video_ &&
            link_active == last_link_active_ &&
            remote_link_active == last_remote_link_active_) {
            return;
        }
        state_valid_ = true;
        last_has_rom_ = has_rom;
        last_capabilities_ = capabilities;
        last_paused_ = paused;
        last_fullscreen_ = fullscreen;
        last_recording_ = recording;
        last_palette_ = palette;
        last_video_ = video;
        last_link_active_ = link_active;
        last_remote_link_active_ = remote_link_active;
        enable(DesktopMenuCommand::save_state, has_rom);
        enable(DesktopMenuCommand::load_state, has_rom);
        enable(DesktopMenuCommand::pause, has_rom);
        enable(DesktopMenuCommand::reset, has_rom);
        const auto has = [&](const gbb::CoreCapability capability) {
            return has_rom && gbb::has_capability(capabilities, capability);
        };
        enable(DesktopMenuCommand::link_session,
               has(gbb::CoreCapability::link_cable) && !remote_link_active);
        enable(DesktopMenuCommand::link_retry,
               link_active || remote_link_active);
        enable(DesktopMenuCommand::remote_host,
               has(gbb::CoreCapability::link_cable) && !link_active &&
                   !remote_link_active);
        enable(DesktopMenuCommand::remote_join,
               has(gbb::CoreCapability::link_cable) && !link_active &&
                   !remote_link_active);
        enable(DesktopMenuCommand::remote_discover,
               has(gbb::CoreCapability::link_cable) && !remote_link_active);
        enable(DesktopMenuCommand::remote_stop, remote_link_active);
        ModifyMenuW(emulation_, command_id(DesktopMenuCommand::link_session),
                    MF_BYCOMMAND | MF_STRING,
                    command_id(DesktopMenuCommand::link_session),
                    link_active ? L"Stop Local &Link Session\tCtrl+Shift+L"
                                : L"Start Local &Link Session\tCtrl+Shift+L");
        enable(DesktopMenuCommand::gameshark,
               has(gbb::CoreCapability::cheats));
        enable(DesktopMenuCommand::debugger,
               has(gbb::CoreCapability::debugger));
        enable(DesktopMenuCommand::record_input,
               has(gbb::CoreCapability::debugger));
        enable(DesktopMenuCommand::replay_input,
               has(gbb::CoreCapability::debugger));
        enable(DesktopMenuCommand::tas_editor,
               has(gbb::CoreCapability::debugger));
        enable(DesktopMenuCommand::sprite_editor,
               has(gbb::CoreCapability::sprite_editor));
        check(DesktopMenuCommand::pause, paused);
        check(DesktopMenuCommand::fullscreen, fullscreen);
        check(DesktopMenuCommand::record_input, recording);
        for (std::size_t index = 0; index < gameboy::display_palettes.size();
             ++index) {
            CheckMenuItem(root_, command_id(DesktopMenuCommand::palette_first) +
                                     static_cast<UINT>(index),
                          MF_BYCOMMAND | (index == palette ? MF_CHECKED
                                                           : MF_UNCHECKED));
        }
        for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
            CheckMenuItem(root_, command_id(DesktopMenuCommand::video_first) +
                                     static_cast<UINT>(index),
                          MF_BYCOMMAND |
                              (gameboy::video_modes[index].mode == video
                                   ? MF_CHECKED
                                   : MF_UNCHECKED));
        }
    }

private:
    static UINT command_id(const DesktopMenuCommand command) noexcept {
        return static_cast<UINT>(command);
    }

    static void append(HMENU menu, const DesktopMenuCommand command,
                       const wchar_t* label) {
        AppendMenuW(menu, MF_STRING, command_id(command), label);
    }

    void enable(const DesktopMenuCommand command, const bool enabled) {
        EnableMenuItem(root_, command_id(command),
                       MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }

    void check(const DesktopMenuCommand command, const bool checked) {
        CheckMenuItem(root_, command_id(command),
                      MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam) {
        if (message == WM_COMMAND && active_ != nullptr &&
            HIWORD(wparam) == 0) {
            active_->pending_.store(static_cast<int>(LOWORD(wparam)));
            return 0;
        }
        return active_ != nullptr && active_->previous_ != nullptr
                   ? CallWindowProcW(active_->previous_, hwnd, message, wparam,
                                     lparam)
                   : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    inline static Impl* active_{};
    std::atomic_int pending_{};
    HWND hwnd_{};
    WNDPROC previous_{};
    HMENU root_{};
    HMENU file_{};
    HMENU emulation_{};
    HMENU view_{};
    HMENU palette_{};
    HMENU video_{};
    HMENU tools_{};
    HMENU help_{};
    bool state_valid_{};
    bool last_has_rom_{};
    gbb::CoreCapability last_capabilities_{};
    bool last_paused_{};
    bool last_fullscreen_{};
    bool last_recording_{};
    std::size_t last_palette_{};
    gameboy::VideoMode last_video_{gameboy::default_video_mode};
    bool last_link_active_{};
    bool last_remote_link_active_{};
};

DesktopMenuBar::DesktopMenuBar() : impl_(std::make_unique<Impl>()) {}

DesktopMenuBar::~DesktopMenuBar() = default;

void DesktopMenuBar::attach(SDL_Window* window) { impl_->attach(window); }

void DesktopMenuBar::detach() noexcept { impl_->detach(); }

DesktopMenuCommand DesktopMenuBar::take_command() noexcept {
    return impl_->take_command();
}

void DesktopMenuBar::update(const bool has_rom,
                            const gbb::CoreCapability capabilities,
                            const bool paused,
                            const bool fullscreen, const bool recording,
                            const std::size_t palette,
                            const gameboy::VideoMode video,
                            const bool link_active,
                            const bool remote_link_active) {
    impl_->update(has_rom, capabilities, paused, fullscreen, recording,
                  palette, video, link_active, remote_link_active);
}

#endif // _WIN32
