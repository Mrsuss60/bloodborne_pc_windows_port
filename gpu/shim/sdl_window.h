// bbport: the game window. Created by the VideoOut driver on first open; the
// event pump runs on the port's window thread (see window.cpp).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    SDL_Window* GetSDLWindow() const { return window; }
    void Hide();
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    bool IsOpen() const { return is_open.load(std::memory_order_relaxed); }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();
    /// Keyboard text entry for the system IME dialog; typed text shows in the title bar.
    void BeginTextInput(const std::string& initial, const std::string& prompt);
    /// 0 while typing, 1 confirmed (Enter), 2 cancelled (Escape); text is UTF-8.
    int PollTextInput(std::string& text);
    bool IsTextInputActive() const { return text_active; }
    std::string GetTextInputPrompt();
    std::string GetTextInputValue();
    void ConsumeMouseDelta(float* dx, float* dy);
    /// Whether a mouse motion, button or wheel event arrived within `ms` (any capture mode).
    bool MouseActiveWithin(uint64_t ms) const;
    void CaptureMouse(bool capture);
    bool IsMouseCaptured() const { return relative_mouse_active.load(std::memory_order_acquire); }

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    std::mutex mouse_delta_mutex;
    float accumulated_mouse_dx = 0.0f;
    float accumulated_mouse_dy = 0.0f;
    std::atomic<bool> relative_mouse_active{false};
    std::mutex text_mutex;
    bool text_requested{}, text_active{};
    int text_state{};
    std::string text, text_prompt, base_title;
    void UpdateTextTitle();
    SDL_Window* window{};
    WindowSystemInfo window_info{};
    std::atomic<uint64_t> last_mouse_motion_ms{0};
    std::atomic<bool> mouse_input_seen{false};
    bool mouse_cursor_hidden = false;
    bool wheel_cursor_hidden = false; ///< hidden while the wheel steps a game menu's cursor
    bool window_focused = true, mouse_inside = true;
    bool menu_released = false; ///< a game menu released the mouse; it is taken back when they close
    void UpdateClipCursor(bool clip);
};

} // namespace Frontend
