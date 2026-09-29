#include "UiBackend.h"
#include "Lang.h"
#include "Png.h"
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <Windows.h>

namespace
{
    // RmlUi warnings and errors go to lonelyice-ui.log next to the exe.
    class LoggingSystemInterface : public SystemInterface_SDL
    {
    public:
        // Markup text "@{key}" becomes Tr("key"). Inside a data binding ({{ ... }}) it is left for the text the
        // binding produces, which RmlUi passes through here again: a translation may hold quotes.
        int TranslateString(Rml::String& translated, Rml::String const& input) override
        {
            translated.clear();
            int count = 0, depth = 0;
            for (std::size_t i = 0; i < input.size();)
            {
                if (input.compare(i, 2, "{{") == 0 || input.compare(i, 2, "}}") == 0)
                {
                    depth += input[i] == '{' ? 1 : -1;
                    translated.append(input, i, 2);
                    i += 2;
                    continue;
                }
                std::size_t const end = depth == 0 && input.compare(i, 2, "@{") == 0 ? input.find('}', i + 2) : Rml::String::npos;
                if (end == Rml::String::npos)
                {
                    translated += input[i++];
                    continue;
                }
                translated += LonelyIce::Tr(std::string_view(input).substr(i + 2, end - i - 2));
                i = end + 1;
                ++count;
            }
            return count;
        }

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

    // The GL3 sample renderer reads TGA only; plugin icons are PNG files on disk.
    class Renderer : public RenderInterface_GL3
    {
    public:
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, Rml::String const& source) override
        {
            if (source.size() < 4 || _stricmp(source.c_str() + source.size() - 4, ".png") != 0)
                return RenderInterface_GL3::LoadTexture(dimensions, source);

            std::ifstream in(std::filesystem::u8path(source), std::ios::binary);
            std::vector<uint8_t> file(std::istreambuf_iterator<char>(in), {});
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (file.empty() || !LonelyIce::Png::Decode(file.data(), file.size(), rgba, w, h))
            {
                Rml::Log::Message(Rml::Log::LT_WARNING, "Cannot read image %s", source.c_str());
                return 0;
            }
            for (std::size_t i = 0; i < rgba.size(); i += 4)
                for (std::size_t j = 0; j < 3; ++j)
                    rgba[i + j] = uint8_t(rgba[i + j] * rgba[i + 3] / 255);    // premultiplied alpha
            dimensions = { w, h };
            return GenerateTexture({ rgba.data(), rgba.size() }, dimensions);
        }
    };

    struct BackendData
    {
        LoggingSystemInterface system;
        Renderer render;
        SDL_Window* window = nullptr;
        SDL_GLContext gl = nullptr;
    };

    std::unique_ptr<BackendData> _data;
    Uint32 _wakeEvent = 0;
    float _uiScale = 1.f;
    void (*_zoomHandler)(void*, int) = nullptr;
    void* _zoomUser = nullptr;

    float DisplayScale(SDL_Window* window)
    {
        float s = window ? SDL_GetWindowDisplayScale(window) : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        return s > 0.f ? s : 1.f;
    }

    // Fits w x h (pixels) into the usable area of the window's display, leaving a margin.
    void FitToDisplay(SDL_DisplayID display, int& w, int& h)
    {
        SDL_Rect area{};
        if (!SDL_GetDisplayUsableBounds(display, &area) || area.w <= 0)
            return;
        float k = std::min({ 1.f, area.w * 0.96f / w, area.h * 0.94f / h });
        w = int(w * k);
        h = int(h * k);
    }
}

bool LonelyIce::UiBackend::Initialize(char const* title, int width, int height, float uiScale)
{
    _uiScale = uiScale;
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return false;
    _wakeEvent = SDL_RegisterEvents(1);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    float scale = DisplayScale(nullptr);
    if (_uiScale <= 0.f)
    {
        SDL_Rect area{};
        bool known = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area) && area.w > 0;
        _uiScale = 1.f;
        for (float s : { 1.5f, 1.25f })
        {
            if (known && width * scale * s <= area.w * 0.96f && height * scale * s <= area.h * 0.94f)
            {
                _uiScale = s;
                break;
            }
        }
    }
    int w = int(width * scale * _uiScale), h = int(height * scale * _uiScale);
    FitToDisplay(SDL_GetPrimaryDisplay(), w, h);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
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
    SDL_SetWindowMinimumSize(window, int(640 * scale), int(460 * scale));

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

float LonelyIce::UiBackend::GetUiScale()
{
    return _uiScale;
}

float LonelyIce::UiBackend::GetDpRatio()
{
    return DisplayScale(_data ? _data->window : nullptr) * _uiScale;
}

void LonelyIce::UiBackend::SetUiScale(Rml::Context* context, float uiScale)
{
    if (!_data || uiScale <= 0.f || uiScale == _uiScale)
        return;
    float k = uiScale / _uiScale;
    _uiScale = uiScale;
    context->SetDensityIndependentPixelRatio(GetDpRatio());

    SDL_Window* window = _data->window;
    if (SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))
        return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(window, &x, &y);
    SDL_GetWindowSize(window, &w, &h);
    int nw = int(w * k), nh = int(h * k);
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    FitToDisplay(display, nw, nh);
    SDL_SetWindowSize(window, nw, nh);

    SDL_Rect area{};
    if (SDL_GetDisplayUsableBounds(display, &area))
    {
        int nx = std::clamp(x - (nw - w) / 2, area.x, std::max(area.x, area.x + area.w - nw));
        int ny = std::clamp(y - (nh - h) / 2, area.y, std::max(area.y, area.y + area.h - nh));
        SDL_SetWindowPosition(window, nx, ny);
    }
}

void LonelyIce::UiBackend::SetZoomHandler(void (*handler)(void*, int), void* user)
{
    _zoomHandler = handler;
    _zoomUser = user;
}

void LonelyIce::UiBackend::Wake()
{
    SDL_Event ev{};
    ev.type = _wakeEvent;
    SDL_PushEvent(&ev);
}

bool LonelyIce::UiBackend::ProcessEvents(Rml::Context* context, double maxWaitSeconds, bool& closeRequested)
{
    bool running = true;
    closeRequested = false;
    SDL_Event ev;

    double wait = std::min(context->GetNextUpdateDelay(), maxWaitSeconds);
    bool has = wait > 0 ? SDL_WaitEventTimeout(&ev, int(wait * 1000)) : SDL_PollEvent(&ev);

    while (has)
    {
        switch (ev.type)
        {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                closeRequested = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                _data->render.SetViewport(ev.window.data1, ev.window.data2);
                RmlSDL::InputEventHandler(context, _data->window, ev);
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                context->SetDensityIndependentPixelRatio(GetDpRatio());
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if ((SDL_GetModState() & SDL_KMOD_CTRL) && _zoomHandler)
                {
                    if (ev.wheel.y != 0.f)
                        _zoomHandler(_zoomUser, ev.wheel.y > 0.f ? 1 : -1);
                }
                else
                    RmlSDL::InputEventHandler(context, _data->window, ev);
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

void LonelyIce::UiBackend::SetIcon(SDL_Surface* icon)
{
    if (_data && icon)
        SDL_SetWindowIcon(_data->window, icon);
}

void LonelyIce::UiBackend::ShowWindow()
{
    if (!_data)
        return;
    SDL_ShowWindow(_data->window);
    SDL_RestoreWindow(_data->window);
    SDL_RaiseWindow(_data->window);
}

void LonelyIce::UiBackend::HideWindow()
{
    if (_data)
        SDL_HideWindow(_data->window);
}

bool LonelyIce::UiBackend::IsWindowVisible()
{
    return _data && !(SDL_GetWindowFlags(_data->window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
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
