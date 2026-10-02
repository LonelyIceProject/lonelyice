#include "TuiSettingsView.h"
#include "Lang.h"
#include <ftxui/screen/terminal.hpp>
#include <algorithm>

using namespace LonelyIce;
using namespace ftxui;

TuiSettingsView::TuiSettingsView(SettingsModel& model, std::function<void()> save, std::function<void()> discard)
    : _model(model), _save(std::move(save)), _discard(std::move(discard))
{
}

void TuiSettingsView::SelectGroup(std::string const& id)
{
    auto const& groups = _model.Groups();
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (groups[i].id == id)
        {
            _group = static_cast<int>(i);
            return;
        }
}

Component TuiSettingsView::Build()
{
    _rows.clear();
    _groups.clear();
    Components pages;
    for (auto const& group : _model.Groups())
    {
        _groups.push_back(Tr(group.name));
        Components fields;
        for (auto& value : _model.Values())
        {
            if (value.def->group != group.id)
                continue;
            auto* v = &value;
            auto row = std::make_shared<Row>();
            _rows.push_back(row);
            Component field;
            if (v->def->type == 'b')
            {
                row->checked = v->cur == "1";
                CheckboxOption option;
                option.on_change = [row, v] { v->cur = row->checked ? "1" : "0"; };
                field = Checkbox(Tr("tui.enabled"), &row->checked, option);
            }
            else if (v->def->type == 's' && !v->def->options.empty())
            {
                bool found = false;
                for (auto const& [key, label] : v->def->options)
                {
                    if (key == v->cur)
                    {
                        row->selected = static_cast<int>(row->labels.size());
                        found = true;
                    }
                    row->values.push_back(key);
                    row->labels.push_back(Tr(label));
                }
                // Retain a config value unknown to the current schema until the user explicitly replaces it.
                if (!found)
                {
                    row->selected = static_cast<int>(row->labels.size());
                    row->values.push_back(v->cur);
                    row->labels.push_back(Tr("tui.current_value", v->cur));
                }
                DropdownOption option;
                option.radiobox.entries = &row->labels;
                option.radiobox.selected = &row->selected;
                option.radiobox.on_change = [row, v] { v->cur = row->values.at(row->selected); };
                field = Dropdown(option);
            }
            else
            {
                InputOption option;
                option.multiline = false;
                field = Input(&v->cur, option);
            }
            fields.push_back(Renderer(
                field,
                [this, field, v]
                {
                    auto const& d = *v->def;
                    std::string meta =
                        d.key + "  [" +
                        Tr(d.source == SetSource::Launcher ? "tui.apply_launcher" : "tui.apply." + d.apply) + "]";
                    if (d.type == 'n')
                    {
                        meta += " " + Tr(d.integer ? "tui.int" : "tui.number");
                        if (d.min)
                            meta += " >= " + std::to_string(*d.min);
                        if (d.max)
                            meta += " <= " + std::to_string(*d.max);
                    }
                    if (!d.def.empty())
                        meta += "  " + Tr("tui.default", d.def);
                    Elements parts{paragraph(Tr(d.label) + (_model.Changed(*v) ? " *" : "")) | bold,
                                   paragraph(meta) | dim, field->Render() | border};
                    if (!d.hint.empty())
                        parts.push_back(paragraph(Tr(d.hint)) | dim);
                    return vbox(std::move(parts));
                }));
        }
        if (fields.empty())
            fields.push_back(Renderer([] { return paragraph(Tr("tui.no_settings")); }));
        auto form = Container::Vertical(std::move(fields));
        pages.push_back(Renderer(form,
                                 [form, hint = group.hint, launcher = group.id == "launch"]
                                 {
                                     Elements parts{paragraph(Tr(hint)) | dim};
                                     if (launcher)
                                         parts.push_back(paragraph(Tr("tui.gui_settings_note")) | dim);
                                     parts.push_back(form->Render());
                                     return vbox(std::move(parts)) | vscroll_indicator | frame | flex;
                                 }));
    }
    _group = std::clamp(_group, 0, std::max(0, static_cast<int>(_groups.size()) - 1));
    auto menu = Menu(&_groups, &_group);
    auto tabs = Container::Tab(std::move(pages), &_group);
    auto save = Button(Tr("tui.save"), _save);
    auto discard = Button(Tr("tui.discard"), _discard);
    auto buttons = Container::Horizontal({save, discard});
    Components errorRows;
    for (auto const& error : _model.Errors())
        errorRows.push_back(Renderer(
            [error](bool focused)
            {
                auto element = paragraph(error) | color(Color::Red);
                return focused ? element | focus : element;
            }));
    auto errorsBody = Container::Vertical(std::move(errorRows));
    auto errors = Collapsible(
        Tr("tui.schema_errors", _model.Errors().size()),
        Renderer(errorsBody, [errorsBody]
                 { return errorsBody->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 3); }));
    auto shownErrors = Maybe(errors, [this] { return !_model.Errors().empty(); });
    auto root = Container::Vertical({shownErrors, menu, tabs, buttons});
    return Renderer(
        root,
        [this, menu, tabs, buttons, shownErrors]
        {
            for (std::size_t i = 0; i < _model.Groups().size(); ++i)
            {
                auto const& group = _model.Groups()[i];
                int changed = _model.ChangedCount(group.id);
                _groups[i] = Tr(group.name) + (changed ? " (" + std::to_string(changed) + " *)" : "");
            }
            Element body;
            if (Terminal::Size().dimx >= 90)
                body = hbox({menu->Render() | size(WIDTH, LESS_THAN, 26) | vscroll_indicator | frame, separator(),
                             tabs->Render() | flex}) |
                       flex;
            else
                body = vbox({menu->Render() | size(HEIGHT, LESS_THAN, 3) | frame, separator(), tabs->Render() | flex}) |
                       flex;
            return vbox({paragraph(Tr("tui.settings_count", _model.ChangedCount())) | bold,
                         paragraph(Tr("tui.apply_help")) | dim | size(HEIGHT, LESS_THAN, 2) | frame,
                         shownErrors->Render(), body, buttons->Render()});
        });
}

std::string TuiSettingsView::Validate(SettingsModel& model)
{
    for (auto const& value : model.Values())
        if (model.Changed(value))
            if (auto error = SettingsModel::ValidateValue(value); !error.empty())
                return error;
    return {};
}
