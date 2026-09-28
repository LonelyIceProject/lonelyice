#ifndef LONELYICE_COMMANDCATALOG_H
#define LONELYICE_COMMANDCATALOG_H

#include <string>
#include <utility>
#include <vector>

namespace LonelyIce
{
    struct CmdArgDef
    {
        std::string name;      // placeholder in the template, {name}
        char type;             // t = text, n = number, s = select
        std::string def;
        std::vector<std::pair<std::string, std::string>> options;
    };

    struct CmdDef
    {
        std::string title, desc;
        std::string tmpl;      // {p} = selected character; "gold" arg fills {copper}
        std::vector<CmdArgDef> args;
        bool danger = false;
    };

    struct CmdGroup
    {
        std::string id, name;
        std::vector<CmdDef> cmds;
    };

    std::vector<CmdGroup> const& CommandCatalog();
    std::string BuildCommand(CmdDef const& def, std::vector<std::string> const& values, std::string const& character);
}

#endif
