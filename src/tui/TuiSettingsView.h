#ifndef LONELYICE_TUISETTINGSVIEW_H
#define LONELYICE_TUISETTINGSVIEW_H

#include "SettingsModel.h"
#include <ftxui/component/component.hpp>
#include <functional>
#include <memory>

namespace LonelyIce
{
    class TuiSettingsView
    {
    public:
        TuiSettingsView(SettingsModel& model, std::function<void()> save, std::function<void()> discard);
        ftxui::Component Build();
        void SelectGroup(std::string const& id);
        // Full-input validation before SettingsModel::Save, which otherwise clamps or partially parses numbers.
        static std::string Validate(SettingsModel& model);

    private:
        struct Row
        {
            bool checked = false;
            int selected = 0;
            std::vector<std::string> labels, values;
        };
        SettingsModel& _model;
        std::function<void()> _save, _discard;
        std::vector<std::string> _groups;
        std::vector<std::shared_ptr<Row>> _rows;
        int _group = 0;
    };
} // namespace LonelyIce
#endif
