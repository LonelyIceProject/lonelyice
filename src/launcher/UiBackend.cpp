#include "UiBackend.h"
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <Windows.h>

namespace
{
    // RmlUi warnings and errors go to lonelyice-ui.log next to the exe.
    class LoggingSystemInterface : public SystemInterface_SDL
    {
    public:
        bool LogMessage(Rml::Log::Type type, Rml::String const& message) override
        {
            if (type <= Rml::Log::LT_WARNING)
            {
                static FILE* f = [] {
                    wchar_t path[MAX_PATH];
                    GetModuleFileNameW(nullptr, path, MAX_PATH);
                    std::wstring p = path;
                    p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"lonelyice-ui.log";
                    FILE* file = nullptr;
                    _wfopen_s(&file, p.c_str(), L"w");
                    return file;
                }();
                if (f)
                {
                    fprintf(f, "%s %s\n", type == Rml::Log::LT_WARNING ? "WARN " : "ERROR", message.c_str());
                    fflush(f);
                }
            }
            return true;
        }
    };

    struct BackendData
    {
        LoggingSystemInterface system;
        RenderInterface_GL3 render;
        SDL_Window* window = nullptr;
        SDL_GLContext gl = nullptr;
    };

    std::unique_ptr<BackendData> _data;
    Uint32 _wakeEvent = 0;
}

bool LonelyIce::UiBackend::Initialize(char const* title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return false;

    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    _wakeEvent = SDL_RegisterEvents(1);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.f)
        scale = 1.f;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, int(width * scale));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, int(height * scale));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    SDL_Window* window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!window)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window, int(760 * scale), int(540 * scale));

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl)
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "SDL_GL_CreateContext: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        return false;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    if (!RmlGL3::Initialize())
    {
        Rml::Log::Message(Rml::Log::LT_ERROR, "OpenGL 3.3 is not available");
        return false;
    }

    _data = std::make_unique<BackendData>();
    if (!_data->render)
    {
        _data.reset();
        return false;
    }

    _data->window = window;
    _data->gl = gl;
    _data->system.SetWindow(window);

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    _data->render.SetViewport(pw, ph);
    return true;
}

void LonelyIce::UiBackend::Shutdown()
{
    if (!_data)
        return;
    SDL_Window* window = _data->window;
    SDL_GLContext gl = _data->gl;
    _data.reset();
    RmlGL3::Shutdown();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

Rml::SystemInterface* LonelyIce::UiBackend::GetSystemInterface()
{
    return &_data->system;
}

Rml::RenderInterface* LonelyIce::UiBackend::GetRenderInterface()
{
    return &_data->render;
}

SDL_Window* LonelyIce::UiBackend::GetWindow()
{
    return _data ? _data->window : nullptr;
}

float LonelyIce::UiBackend::GetDisplayScale()
{
    float s = _data ? SDL_GetWindowDisplayScale(_data->window) : 1.f;
    return s > 0.f ? s : 1.f;
}

void LonelyIce::UiBackend::Wake()
{
    SDL_Event ev{};
    ev.type = _wakeEvent;
    SDL_PushEvent(&ev);
}

bool LonelyIce::UiBackend::ProcessEvents(Rml::Context* context, double maxWaitSeconds)
{
    bool running = true;
    SDL_Event ev;

    double wait = std::min(context->GetNextUpdateDelay(), maxWaitSeconds);
    bool has = wait > 0 ? SDL_WaitEventTimeout(&ev, int(wait * 1000)) : SDL_PollEvent(&ev);

    while (has)
    {
        switch (ev.type)
        {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                _data->render.SetViewport(ev.window.data1, ev.window.data2);
                RmlSDL::InputEventHandler(context, _data->window, ev);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                context->SetDensityIndependentPixelRatio(GetDisplayScale());
                break;
            default:
                if (ev.type != _wakeEvent)
                    RmlSDL::InputEventHandler(context, _data->window, ev);
                break;
        }
        has = SDL_PollEvent(&ev);
    }
    return running;
}

void LonelyIce::UiBackend::BeginFrame()
{
    _data->render.Clear();
    _data->render.BeginFrame();
}

void LonelyIce::UiBackend::PresentFrame()
{
    _data->render.EndFrame();
    SDL_GL_SwapWindow(_data->window);
}
