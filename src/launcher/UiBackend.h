#ifndef LONELYICE_UIBACKEND_H
#define LONELYICE_UIBACKEND_H

#include <RmlUi/Core/Types.h>
#include <cstdint>

namespace Rml
{
    class Context;
    class RenderInterface;
    class SystemInterface;
}

struct SDL_Window;

// SDL3 window + OpenGL 3.3 renderer for RmlUi, trimmed from RmlUi's SDL_GL3 sample backend (no SDL_image: textures are TGA).
namespace LonelyIce::UiBackend
{
    bool Initialize(char const* title, int width, int height);
    void Shutdown();

    Rml::SystemInterface* GetSystemInterface();
    Rml::RenderInterface* GetRenderInterface();
    SDL_Window* GetWindow();
    float GetDisplayScale();

    // Returns false when the user closed the window. Waits for input unless wakeups arrive.
    bool ProcessEvents(Rml::Context* context, double maxWaitSeconds);
    void Wake();                 // thread-safe, interrupts the wait in ProcessEvents
    void BeginFrame();
    void PresentFrame();
}

#endif
