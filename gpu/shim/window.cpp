// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland, Win32 or Metal on macOS).
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#if defined(_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    // MoltenVK presents to a CAMetalLayer: SDL_Metal_CreateView makes the window's layer one.
    (void)wp;
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
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
    last_mouse_motion_ms = SDL_GetTicks();
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
}

WindowSDL::~WindowSDL() {
    SDL_DestroyWindow(window);
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
        if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL) {
            last_mouse_motion_ms = SDL_GetTicks();
            if (mouse_cursor_hidden) {
                SDL_ShowCursor();
                mouse_cursor_hidden = false;
            }
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            std::scoped_lock lock{mouse_delta_mutex};
            accumulated_mouse_dx += event.motion.xrel;
            accumulated_mouse_dy += event.motion.yrel;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
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
 
    // Periodically reload bbport.ini so external edits (launcher dialog) apply in real time
    static uint64_t last_ini_check_ms = 0;
    const uint64_t current_ticks = SDL_GetTicks();
    if (current_ticks - last_ini_check_ms >= 500) {
        last_ini_check_ms = current_ticks;
        BbSettings::Reload();
    }

    const bool overlay_capturing = BbOverlay::CapturesInput() || text_active;
    const bool mk_active = BbSettings::Get().mk_enabled.load();
    const bool want_relative = mk_active && !overlay_capturing;

    if (want_relative != relative_mouse_active) {
        relative_mouse_active = want_relative;
        SDL_SetWindowRelativeMouseMode(window, want_relative);
        if (want_relative) {
            SDL_HideCursor();
            mouse_cursor_hidden = true;
        } else {
            SDL_ShowCursor();
            mouse_cursor_hidden = false;
        }
    }

    if (!relative_mouse_active) {
        // Auto-hide cursor after 3 seconds of mouse idle when not in overlay menu
        if (!BbOverlay::CapturesInput()) {
            const uint64_t now_ms = SDL_GetTicks();
            if (!mouse_cursor_hidden && (now_ms - last_mouse_motion_ms >= 3000)) {
                SDL_HideCursor();
                mouse_cursor_hidden = true;
            }
        } else if (mouse_cursor_hidden) {
            SDL_ShowCursor();
            mouse_cursor_hidden = false;
        }
    }

    return is_open;
}

void WindowSDL::ConsumeMouseDelta(float* dx, float* dy) {
    std::scoped_lock lock{mouse_delta_mutex};
    if (dx) *dx = accumulated_mouse_dx;
    if (dy) *dy = accumulated_mouse_dy;
    accumulated_mouse_dx = 0.0f;
    accumulated_mouse_dy = 0.0f;
}

} // namespace Frontend
