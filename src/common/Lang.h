#ifndef LONELYICE_LANG_H
#define LONELYICE_LANG_H

#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>

// User-facing text. Strings live in assets/lang/<code>/*.lang ("key = value" lines, "#" comments, "\n" for a line
// break) and are looked up by key: the current language, then English, then the key itself (so text that is not a
// key, such as a plugin's own label, passes through unchanged). Arguments fill {0}, {1}... (fmt syntax).
namespace LonelyIce::Lang
{
    struct Info
    {
        std::string code;       // en, de, es, fr, ru
        std::string name;       // in its own language
    };

    std::vector<Info> const& Available();

    // Picks the language: LONELYICE_LANG, then launcher.language of server.yaml next to the exe, else English.
    // Sets LONELYICE_LANG so child processes follow.
    void Init();
    // Switches to code (one of Available(), else English).
    void Set(std::string const& code);
    std::string Code();

    std::string Get(std::string_view key);
    bool Has(std::string_view key);

    // Text in the current language from a map of language code -> text (plugin manifests): the current
    // language, then English, then any.
    std::string Pick(std::map<std::string, std::string> const& texts);
}

namespace LonelyIce
{
    inline std::string Tr(std::string_view key)
    {
        return Lang::Get(key);
    }

    template <typename... Args>
    std::string Tr(std::string_view key, Args&&... args)
    {
        std::string const text = Lang::Get(key);
        try
        {
            return fmt::format(fmt::runtime(text), std::forward<Args>(args)...);
        }
        catch (fmt::format_error const&)
        {
            return text;
        }
    }
}

#endif
