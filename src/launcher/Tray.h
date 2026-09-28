#ifndef LONELYICE_TRAY_H
#define LONELYICE_TRAY_H

#include <functional>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Tray;
struct SDL_TrayMenu;
struct SDL_TrayEntry;

namespace LonelyIce
{
    // Notification-area icon with a menu. Callbacks run on the UI thread while SDL pumps events.
    class Tray
    {
    public:
        struct Item
        {
            std::string id;         // empty = separator
            std::string label;
            std::function<void()> action;
        };

        bool Create(SDL_Surface* icon, std::vector<Item> items);
        void Destroy();
        void SetLabel(std::string const& id, std::string const& label);
        void SetEnabled(std::string const& id, bool enabled);
        void SetTooltip(std::string const& text);
        void Invoke(std::string const& id);     // runs an entry's action as if clicked

    private:
        struct Entry
        {
            std::string id;
            SDL_TrayEntry* entry = nullptr;
            std::function<void()> action;
        };

        SDL_Tray* _tray = nullptr;
        std::vector<Entry> _entries;
    };
}

#endif
