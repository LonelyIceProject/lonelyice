#ifndef LONELYICE_CONSOLE_TEXT_H
#define LONELYICE_CONSOLE_TEXT_H

#include <string>
#include <string_view>

namespace LonelyIce
{
    // The POSIX console appender resets its colour after '\n'. That reset consequently prefixes
    // the next line, including @@LI control messages. Remove complete SGR sequences before parsing.
    inline std::string StripConsoleColors(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();)
        {
            if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[')
            {
                std::size_t end = i + 2;
                while (end < text.size() && ((text[end] >= '0' && text[end] <= '9') ||
                    text[end] == ';' || text[end] == ':'))
                    ++end;
                if (end < text.size() && text[end] == 'm')
                {
                    i = end + 1;
                    continue;
                }
            }
            out += text[i++];
        }
        return out;
    }
}

#endif
