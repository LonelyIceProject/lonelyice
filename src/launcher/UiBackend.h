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
struct SDL_Surface;

// SDL3 window + OpenGL 3.3 renderer for RmlUi, trimmed from RmlUi's SDL_GL3 sample backend (no SDL_image: textures are TGA).
namespace LonelyIce::UiBackend
{
    // width/height in dp; uiScale multiplies the Windows display scale, 0 picks the largest of 150/125/100 % that fits.
    // The window is clamped to the usable desktop area.
    bool Initialize(char const* title, int width, int height, float uiScale);
    float GetUiScale();
    void Shutdown();

    Rml::SystemInterface* GetSystemInterface();
    Rml::RenderInterface* GetRenderInterface();
    SDL_Window* GetWindow();
    float GetDpRatio();          // display scale * ui scale
    // Changes the ui scale live: the context's dp ratio and the window size follow.
    void SetUiScale(Rml::Context* context, float uiScale);
    // Ctrl + mouse wheel: called with +1 / -1.
    void SetZoomHandler(void (*handler)(void* user, int steps), void* user);

    // Returns false when the application must quit (session end). closeRequested is set when the user closed the window.
    // Waits for input up to maxWaitSeconds unless woken up.
    bool ProcessEvents(Rml::Context* context, double maxWaitSeconds, bool& closeRequested);
    void SetIcon(SDL_Surface* icon);
    void ShowWindow();       // restore, raise and focus
    void HideWindow();
    bool IsWindowVisible();
    void Wake();                 // thread-safe, interrupts the wait in ProcessEvents
    void BeginFrame();
    void PresentFrame();
}

#endif
