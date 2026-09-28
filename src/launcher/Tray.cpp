#include "Tray.h"
#include <SDL3/SDL.h>

bool LonelyIce::Tray::Create(SDL_Surface* icon, std::vector<Item> items)
{
    _tray = SDL_CreateTray(icon, "LonelyIce");
    if (!_tray)
        return false;

    SDL_TrayMenu* menu = SDL_CreateTrayMenu(_tray);
    _entries.reserve(items.size());
    for (Item& item : items)
    {
        Entry e;
        e.id = item.id;
        e.action = std::move(item.action);
        e.entry = SDL_InsertTrayEntryAt(menu, -1, item.id.empty() ? nullptr : item.label.c_str(), SDL_TRAYENTRY_BUTTON);
        _entries.push_back(std::move(e));
    }

    // Entries are stable now (reserved), so their addresses can be callback userdata.
    for (Entry& e : _entries)
    {
        if (!e.entry || !e.action)
            continue;
        SDL_SetTrayEntryCallback(e.entry, [](void* userdata, SDL_TrayEntry*)
        {
            static_cast<Entry*>(userdata)->action();
        }, &e);
    }
    return true;
}

void LonelyIce::Tray::Destroy()
{
    if (_tray)
        SDL_DestroyTray(_tray);
    _tray = nullptr;
    _entries.clear();
}

void LonelyIce::Tray::SetLabel(std::string const& id, std::string const& label)
{
    for (Entry& e : _entries)
        if (e.id == id && e.entry)
            SDL_SetTrayEntryLabel(e.entry, label.c_str());
}

void LonelyIce::Tray::SetEnabled(std::string const& id, bool enabled)
{
    for (Entry& e : _entries)
        if (e.id == id && e.entry)
            SDL_SetTrayEntryEnabled(e.entry, enabled);
}

void LonelyIce::Tray::SetTooltip(std::string const& text)
{
    if (_tray)
        SDL_SetTrayTooltip(_tray, text.c_str());
}


void LonelyIce::Tray::Invoke(std::string const& id)
{
    for (Entry& e : _entries)
        if (e.id == id && e.action)
            e.action();
}