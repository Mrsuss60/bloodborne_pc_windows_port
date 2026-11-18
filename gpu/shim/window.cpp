// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_menu_pointer.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }

    // If an explicit render/output resolution is specified, use it for window size
    if (const char* res_env = std::getenv("BB_RENDER_RES")) {
        int rw = 0, rh = 0;
        if (std::sscanf(res_env, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            width_ = rw;
            height_ = rh;
        }
    }

    const char* win_mode = std::getenv("BB_WINDOW_MODE");
    const char* fullscreen = std::getenv("BB_FULLSCREEN");

    bool is_fullscreen = false;
    bool is_borderless = false;

    if (win_mode && win_mode[0]) {
        if (!std::strcmp(win_mode, "fullscreen")) {
            is_fullscreen = true;
        } else if (!std::strcmp(win_mode, "borderless")) {
            is_fullscreen = true;
            is_borderless = true;
        } else if (!std::strcmp(win_mode, "windowed")) {
            is_fullscreen = false;
            is_borderless = false;
        }
    } else if (fullscreen && fullscreen[0] == '1') {
        is_fullscreen = true;
    }

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    if (is_fullscreen) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    }
    if (is_borderless) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    }

    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    if (!is_fullscreen) {
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowBordered(window, true);
        SDL_SetWindowSize(window, width_, height_);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SyncWindow(window);

#if defined(_WIN32)
        const SDL_PropertiesID wp_init = SDL_GetWindowProperties(window);
        HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(wp_init, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd) {
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            style &= ~(WS_POPUP);
            style |= (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLong(hwnd, GWL_STYLE, style);

            RECT rc = { 0, 0, width_, height_ };
            AdjustWindowRect(&rc, style, FALSE);
            int win_w = rc.right - rc.left;
            int win_h = rc.bottom - rc.top;

            int screen_w = GetSystemMetrics(SM_CXSCREEN);
            int screen_h = GetSystemMetrics(SM_CYSCREEN);
            int pos_x = (screen_w - win_w) / 2;
            int pos_y = (screen_h - win_h) / 2;

            SetWindowPos(hwnd, nullptr, pos_x, pos_y, win_w, win_h,
                         SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
#endif
    }

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
#endif
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    last_mouse_motion_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
}

void WindowSDL::Hide() {
    if (window) {
        SDL_HideWindow(window);
    }
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

std::string WindowSDL::GetTextInputPrompt() {
    std::scoped_lock lock{text_mutex};
    return text_prompt;
}

std::string WindowSDL::GetTextInputValue() {
    std::scoped_lock lock{text_mutex};
    return text;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    const bool pointer_menu = !text_active && !BbOverlay::CapturesInput() && BbMenuPointer::Active();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        bool event_handled = false;
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            BbMenuPointer::NoteMouse(event.motion.x, event.motion.y);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            BbMenuPointer::NoteMouse(event.button.x, event.button.y);
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL) {
            last_mouse_motion_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
            mouse_input_seen.store(true, std::memory_order_relaxed);
            if (mouse_cursor_hidden && !(pointer_menu && BbMenuPointer::WheelHoldsCursor())) {
                SDL_ShowCursor();
                mouse_cursor_hidden = false;
            }
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(event.gaxis.value) > 8000)) {
            BbMenuPointer::NoteDevice(true);
        } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_MOTION ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_WHEEL) {
            BbMenuPointer::NoteDevice(false);
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            window_focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            menu_released = false;
        } else if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            mouse_inside = event.type == SDL_EVENT_WINDOW_MOUSE_ENTER;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }

        // In the game's menus the cursor is a pointer: its clicks and the wheel go to the menu.
        if (pointer_menu && !relative_mouse_active) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    BbMenuPointer::Click();
                } else if (event.button.button == SDL_BUTTON_RIGHT && !BbMenuPointer::RightClickSwitchesPages()) {
                    BbMenuPointer::Back();
                }
                continue;
            }
            if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                BbMenuPointer::Wheel(event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
                                                                                     : event.wheel.y);
                continue;
            }
        }

        const bool mk_active = BbSettings::Get().mk_enabled.load();

        // Mouse look: click inside the game window takes the mouse (that initial click is consumed
        // to prevent accidental attacks/rolls upon gaining capture).
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !relative_mouse_active &&
            mk_active && !BbOverlay::CapturesInput() && !text_active) {
            CaptureMouse(true);
            continue;
        }

        // The mouse capture key (F1 by default) both releases and re-captures the pointer,
        // so the user can freely interact with the desktop/second monitor and come back.
        const int capture_key = BbSettings::Get().keybinds[BB_ACTION_MOUSE_CAPTURE_TOGGLE].load();
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && capture_key > 0 &&
            capture_key < SDL_SCANCODE_COUNT && event.key.scancode == capture_key) {
            if (relative_mouse_active) {
                CaptureMouse(false);
            } else if (mk_active && !BbOverlay::CapturesInput() && !text_active) {
                CaptureMouse(true);
            }
            continue;
        }

        // Focus loss immediately releases capture and frees the clipped cursor
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            CaptureMouse(false);
        }

        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (relative_mouse_active) {
                std::scoped_lock lock{mouse_delta_mutex};
                accumulated_mouse_dx += event.motion.xrel;
                accumulated_mouse_dy += event.motion.yrel;
            }
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            if (relative_mouse_active) {
                UpdateClipCursor(true);
            }
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
 
    // Periodically reload bbport.ini so external edits (launcher dialog) apply in real time.
    // Not while the in-game menu is open: it keeps its changes in memory and writes the file
    // when it closes, so a reload now would put the old values back (every change reverted
    // within half a second).
    static uint64_t last_ini_check_ms = 0;
    const uint64_t current_ticks = SDL_GetTicks();
    if (current_ticks - last_ini_check_ms >= 500) {
        last_ini_check_ms = current_ticks;
        if (!BbOverlay::CapturesInput()) {
            BbSettings::Reload();
        }
    }

    const bool overlay_capturing = BbOverlay::CapturesInput() || text_active;
    const bool mk_active = BbSettings::Get().mk_enabled.load();

    // The overlay menu, typing dialog, or disabling M&K in settings releases mouse capture
    if (relative_mouse_active && (overlay_capturing || !mk_active)) {
        CaptureMouse(false);
    }

    // The game's menus keep the cursor; the mouse they released is taken back when they close.
    if (pointer_menu) {
        if (relative_mouse_active) {
            CaptureMouse(false);
        }
        menu_released = window_focused;
    } else if (menu_released) {
        menu_released = false;
        if (mk_active && !overlay_capturing && window_focused && mouse_inside) {
            CaptureMouse(true);
        }
    }
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        BbMenuPointer::NoteWindow(w, h, window_focused, mouse_inside, relative_mouse_active);
    }

    if (!relative_mouse_active) {
        // Auto-hide cursor after 3 seconds of mouse idle when not in overlay menu
        if (!BbOverlay::CapturesInput()) {
            const uint64_t now_ms = SDL_GetTicks();
            const uint64_t last_mouse_ms = last_mouse_motion_ms.load(std::memory_order_relaxed);
            if (!mouse_cursor_hidden && (now_ms - last_mouse_ms >= 3000)) {
                SDL_HideCursor();
                mouse_cursor_hidden = true;
            }
        } else if (mouse_cursor_hidden) {
            SDL_ShowCursor();
            mouse_cursor_hidden = false;
        }
    }

    const bool wheel_hold = pointer_menu && !relative_mouse_active && BbMenuPointer::WheelHoldsCursor();
    if (wheel_hold && !mouse_cursor_hidden) {
        SDL_HideCursor();
        mouse_cursor_hidden = wheel_cursor_hidden = true;
    } else if (!wheel_hold && wheel_cursor_hidden) {
        wheel_cursor_hidden = false;
        if (mouse_cursor_hidden && !relative_mouse_active) {
            SDL_ShowCursor();
            mouse_cursor_hidden = false;
        }
    }

    return is_open;
}

void WindowSDL::UpdateClipCursor(bool clip) {
#if defined(_WIN32)
    if (!clip) {
        ClipCursor(nullptr);
        return;
    }
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd) {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            POINT pt_tl = { rc.left, rc.top };
            POINT pt_br = { rc.right, rc.bottom };
            ClientToScreen(hwnd, &pt_tl);
            ClientToScreen(hwnd, &pt_br);
            RECT clip_rc = { pt_tl.x, pt_tl.y, pt_br.x, pt_br.y };
            ClipCursor(&clip_rc);
        }
    }
#endif
}

void WindowSDL::CaptureMouse(bool capture) {
    if (capture == relative_mouse_active.load(std::memory_order_relaxed)) {
        return;
    }
    SDL_SetWindowRelativeMouseMode(window, capture);
    UpdateClipCursor(capture);
    if (capture) {
        SDL_HideCursor();
        mouse_cursor_hidden = true;
        std::scoped_lock lock{mouse_delta_mutex};
        accumulated_mouse_dx = 0.0f;
        accumulated_mouse_dy = 0.0f;
    } else {
        SDL_ShowCursor();
        mouse_cursor_hidden = false;
    }
    relative_mouse_active.store(capture, std::memory_order_release);
}

void WindowSDL::ConsumeMouseDelta(float* dx, float* dy) {
    std::scoped_lock lock{mouse_delta_mutex};
    if (dx) *dx = accumulated_mouse_dx;
    if (dy) *dy = accumulated_mouse_dy;
    accumulated_mouse_dx = 0.0f;
    accumulated_mouse_dy = 0.0f;
}

bool WindowSDL::MouseActiveWithin(uint64_t ms) const {
    if (!mouse_input_seen.load(std::memory_order_relaxed)) {
        return false; // the window's creation time is not mouse input
    }
    const uint64_t last = last_mouse_motion_ms.load(std::memory_order_relaxed);
    return SDL_GetTicks() - last <= ms;
}

} // namespace Frontend
