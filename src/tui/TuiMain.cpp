#include "TuiMain.h"
#include "TuiJobs.h"
#include "TuiSettingsView.h"
#include "StorageSetupView.h"
#include "LauncherRuntime.h"
#include "GameClient.h"
#include "ConfigEnv.h"
#include "Lang.h"
#include "PackageManager.h"
#include "ProfilePlugins.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace LonelyIce;
using namespace ftxui;

namespace
{
    std::string SafeText(std::string s)
    {
        for (char& c : s)
            if (static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t')
                c = ' ';
        return s;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    class TerminalLauncher
    {
    public:
        TerminalLauncher(LauncherSettings settings)
            : _screen(ScreenInteractive::Fullscreen()), _settings(std::move(settings)),
              _context(CreateLaunchContext(_settings)),
              _packages(std::make_shared<Packages::Manager>(_context.plugins)),
              _jobs([this] { Wake(); }, [this](std::string error) { Message(std::move(error)); }),
              _settingsView(_model, [this] { SaveSettings(); }, [this] { GuardDiscard(); })
        {
            _screen.TrackMouse(false);
            _screen.HandlePipedInput(false);
            _screen.ForceHandleCtrlC(false);
            _indexes = _settings.packageIndex;
            _settingsHolder = Container::Vertical({});
            ReloadModel();
            StorageSetupView::Host host;
            host.settings = &_settings;
            host.exe = _context.exe;
            host.setupDir = _context.exeDir / "setup";
            host.defaultRoot = _context.root;
            host.providers = [this] { return EnabledStorageProviders(_context.plugins); };
            host.env = [this](StorageChoice const& choice, fs::path const& root)
            {
                if (!choice.Remote())
                    return LocalDatabaseOverrides(root);
                for (auto const& provider : EnabledStorageProviders(_context.plugins))
                    if (provider.provider.id == choice.location)
                        return RemoteDatabaseOverrides(provider.provider, choice.remote,
                                                       provider.dir / "server" / Packages::Manager::Platform());
                return Platform::Env{};
            };
            host.serverRunning = [this] { return Running(); };
            host.operationBusy = [this]
            { return _jobs.Busy() || _model.ChangedCount() != 0 || _indexes != _settings.packageIndex; };
            host.committed = [this]
            {
                ReloadContext();
                ReloadModel();
            };
            host.wake = [this] { Wake(); };
            _storage = std::make_unique<StorageSetupView>(std::move(host));
            RefreshPlugins();
        }

        ~TerminalLauncher()
        {
            _jobs.Cancel();
        }

        int Run()
        {
            auto root = Build();
            std::error_code ec;
            if (_settings.sqlStamp.empty() || !fs::is_regular_file(_context.config, ec))
            {
                _storage->StartWizard();
                _tab = _tabSelection = 3;
            }
            _screen.Loop(root);
            _jobs.Cancel();
            return 0;
        }

    private:
        struct PluginRow
        {
            std::string id;
            std::optional<Packages::Local> local;
            std::optional<Packages::Package> package;
        };
        using Action = std::function<void()>;

        void Wake()
        {
            _screen.PostEvent(Event::Custom);
        }
        bool Busy() const
        {
            return _jobs.Busy() || (_storage && _storage->Busy());
        }
        bool Running() const
        {
            return _packages->ServerRunning();
        }
        bool Dirty() const
        {
            return _model.ChangedCount() || _indexes != _settings.packageIndex || (_storage && _storage->Dirty());
        }
        void Message(std::string message)
        {
            _message = SafeText(std::move(message));
        }

        void ReloadContext()
        {
            ResolveSettingsPaths(_settings);
            _context = CreateLaunchContext(_settings);
        }

        void ReloadModel()
        {
            // Detach components that reference old SetValue/SetDef storage before rebuilding it.
            _settingsHolder->DetachAllChildren();
            std::vector<std::string> locales;
            if (!_settings.clientPath.empty())
                for (auto const& locale : GameClient::Inspect(_settings.clientPath).locales)
                    locales.push_back(locale.name);
            _model.Load(_context.config, ReadPlugins(_context.plugins), _settings, locales);
            _settingsHolder->Add(_settingsView.Build());
        }

        bool SaveSettings()
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return false;
            }
            if (_indexes.find_first_of("\r\n") != std::string::npos || _indexes.find('\0') != std::string::npos)
            {
                Message(Tr("tui.invalid_index"));
                return false;
            }
            auto error = TuiSettingsView::Validate(_model);
            if (!error.empty())
            {
                Message(error);
                return false;
            }
            // Save the source list in the same settings write as SettingsModel. Restore it on failure so the draft
            // stays dirty.
            auto oldIndexes = _settings.packageIndex;
            _settings.packageIndex = _indexes;
            auto result = _model.Save(_settings);
            if (!result.error.empty())
            {
                _settings.packageIndex = oldIndexes;
                Message(result.error);
                return false;
            }
            _restartNeeded |= result.restart || result.reload || result.realmName;
            Message(Tr(_restartNeeded ? "tui.saved_restart" : "tui.saved"));
            return true;
        }

        void GuardDiscard()
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            ShowDialog(Tr("tui.unsaved"), Tr("tui.discard_question"),
                       {{Tr("tui.discard"), [this] { ReloadModel(); }}, {Tr("tui.cancel"), [] {}}});
        }

        void GuardEdits(Action next)
        {
            if (!Dirty())
            {
                next();
                return;
            }
            std::vector<std::pair<std::string, Action>> buttons;
            if (!_storage || !_storage->Dirty())
                buttons.push_back({Tr("tui.save_continue"), [this, next]
                                   {
                                       if (SaveSettings())
                                           next();
                                   }});
            buttons.push_back({Tr("tui.discard_continue"), [this, next]
                               {
                                   _indexes = _settings.packageIndex;
                                   ReloadModel();
                                   if (_storage)
                                       _storage->Reload();
                                   next();
                               }});
            buttons.push_back({Tr("tui.cancel"), [] {}});
            ShowDialog(Tr("tui.unsaved"), Tr("tui.unsaved_question"), std::move(buttons));
        }

        void Navigate(int tab)
        {
            if (tab == _tab)
                return;
            GuardEdits([this, tab] { _tab = _tabSelection = tab; });
        }

        void ShowDialog(std::string title, std::string body, std::vector<std::pair<std::string, Action>> actions)
        {
            _dialogTitle = std::move(title);
            _dialogText = SafeText(std::move(body));
            _dialogBody->DetachAllChildren();
            std::istringstream input(_dialogText);
            for (std::string line; std::getline(input, line);)
            {
                _dialogBody->Add(Renderer(
                    [line](bool focused)
                    {
                        auto element = paragraph(line);
                        return focused ? element | focus | inverted : element;
                    }));
            }
            _dialogButtons->DetachAllChildren();
            for (auto& [label, action] : actions)
                _dialogButtons->Add(Button(label,
                                           [this, action = std::move(action)]
                                           {
                                               _dialog = false;
                                               action();
                                           }));
            _dialog = true;
            _dialogButtons->TakeFocus();
        }

        void Quit()
        {
            if (Busy())
            {
                ShowDialog(Tr("tui.quit"), Tr("tui.operation_running"),
                           {{Tr("tui.cancel_operation"),
                             [this]
                             {
                                 _jobs.Cancel();
                                 if (_storage)
                                     _storage->Cancel();
                                 Message(Tr("tui.cancelling"));
                             }},
                            {Tr("tui.back"), [] {}}});
                return;
            }
            GuardEdits([this] { _screen.ExitLoopClosure()(); });
        }

        void Tick()
        {
            _jobs.Poll();
            if (_storage)
                _storage->Tick();
        }

        void LoadCatalog()
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            auto manager = std::make_shared<Packages::Manager>(*_packages);
            auto indexes = _indexes;
            Message(Tr("tui.loading_catalog"));
            _jobs.Start(
                [this, manager, indexes](auto const& cancelled)
                {
                    std::string error;
                    bool ok =
                        manager->LoadIndex(indexes, error, [&cancelled](uint64_t, uint64_t) { return !cancelled; });
                    return [this, manager, ok, error]
                    {
                        _packages = manager;
                        RefreshPlugins();
                        std::string failures;
                        for (auto const& source : manager->Sources())
                            if (!source.ok)
                                failures += source.location + ": " + source.error + "\n";
                        Message(!failures.empty() ? failures : ok ? Tr("tui.catalog_loaded") : error);
                    };
                });
        }

        void RefreshPlugins()
        {
            std::string keep = _pluginSelection >= 0 && _pluginSelection < static_cast<int>(_pluginRows.size())
                                   ? _pluginRows[_pluginSelection].id
                                   : "";
            std::map<std::string, PluginRow> all;
            for (auto const& local : _packages->Installed())
            {
                all[local.manifest.id].id = local.manifest.id;
                all[local.manifest.id].local = local;
            }
            for (auto const& package : _packages->Available())
            {
                auto& row = all[package.id];
                row.id = package.id;
                if (!row.package || Packages::Manager::CompareVersions(package.version, row.package->version) > 0)
                    row.package = package;
            }
            _pluginRows.clear();
            _pluginLabels.clear();
            auto query = Lower(_query);
            for (auto const& [id, row] : all)
            {
                std::string name = row.local ? row.local->manifest.name : row.package ? row.package->name : id;
                if (!query.empty() &&
                    Lower(id + " " + name + " " + (row.package ? row.package->description : "")).find(query) ==
                        std::string::npos)
                    continue;
                if (_pluginFilter == 1 && !row.local)
                    continue;
                if (_pluginFilter == 2 && !row.package)
                    continue;
                _pluginRows.push_back(row);
                _pluginLabels.push_back(SafeText(
                    id + " " + (row.local ? row.local->manifest.version + (row.local->enabled ? " [+]" : " [-]") : "") +
                    (row.package ? " -> " + row.package->version : "")));
            }
            _pluginSelection = std::clamp(_pluginSelection, 0, std::max(0, static_cast<int>(_pluginRows.size()) - 1));
            for (std::size_t i = 0; i < _pluginRows.size(); ++i)
                if (_pluginRows[i].id == keep)
                    _pluginSelection = static_cast<int>(i);
        }

        PluginRow const* SelectedPlugin() const
        {
            return _pluginSelection >= 0 && _pluginSelection < static_cast<int>(_pluginRows.size())
                       ? &_pluginRows[_pluginSelection]
                       : nullptr;
        }

        void ConfigurePlugin()
        {
            auto row = SelectedPlugin();
            if (!row || !row->local)
            {
                Message(Tr("tui.select_installed"));
                return;
            }
            auto plugin = row->local->manifest;
            if (!row->local->enabled)
            {
                Message(Tr("tui.enable_before_configure"));
                return;
            }
            if (plugin.storage)
            {
                GuardEdits(
                    [this, id = plugin.storage->id]
                    {
                        if (!_storage->SelectProvider(id))
                        {
                            Message(Tr("tui.provider_unavailable"));
                            return;
                        }
                        _tab = _tabSelection = 3;
                    });
                return;
            }
            auto schema = ReadPluginSettings(plugin);
            if (!schema.error.empty())
            {
                Message(schema.error);
                return;
            }
            if (schema.fields.empty() || plugin.configDist.empty())
            {
                Message(Tr("tui.no_settings"));
                return;
            }
            GuardEdits(
                [this, id = plugin.id]
                {
                    _settingsView.SelectGroup("plugin:" + id);
                    _tab = _tabSelection = 2;
                });
        }

        bool ActiveProvider(std::string const& id)
        {
            for (auto const& local : _packages->Installed())
                if (local.manifest.id == id && local.manifest.storage &&
                    local.manifest.storage->id == _settings.location)
                    return true;
            return false;
        }

        bool HasDependents(std::string const& id)
        {
            auto dependents = _packages->Dependents(id);
            if (dependents.empty())
                return false;
            std::string message = Tr("tui.dependents_blocked") + "\n";
            for (auto const& dependent : dependents)
                message += dependent + "\n";
            ShowDialog(Tr("tui.dependents"), message, {{Tr("tui.back"), [] {}}});
            return true;
        }

        void PackageAction(std::string action)
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            if (Dirty())
            {
                GuardEdits([this, action] { PackageAction(action); });
                return;
            }
            auto row = SelectedPlugin();
            if (!row)
            {
                Message(Tr("tui.select_plugin"));
                return;
            }
            std::string id = row->id;
            if ((action == "remove" || action == "disable") && ActiveProvider(id))
            {
                Message(Tr("tui.active_provider"));
                return;
            }
            if ((action == "remove" || action == "disable") && HasDependents(id))
                return;
            if ((action == "enable" || action == "disable" || action == "remove") && !row->local)
            {
                Message(Tr("tui.select_installed"));
                return;
            }
            if (action == "install" || action == "update")
            {
                auto manager = _packages;
                _jobs.Start(
                    [this, manager, id, action](auto const& cancelled)
                    {
                        auto plan = cancelled ? Packages::Plan{} : manager->Resolve({{id, "*"}}, action == "update");
                        bool wasCancelled = cancelled;
                        return [this, id, action, plan, wasCancelled]
                        {
                            if (wasCancelled)
                            {
                                Message(Tr("tui.cancelled"));
                                return;
                            }
                            if (!plan.error.empty())
                            {
                                Message(plan.error);
                                return;
                            }
                            if (plan.steps.empty())
                            {
                                Message(Tr("tui.no_updates"));
                                return;
                            }
                            ConfirmPackage(action, id, plan);
                        };
                    });
            }
            else
                ConfirmPackage(action, id, {});
        }

        void ConfirmPackage(std::string action, std::string id, Packages::Plan plan)
        {
            std::string description = Tr("tui.package_question", id) + "\n";
            for (auto const& step : plan.steps)
                description += step.package.id + ": " + (step.from.empty() ? Tr("tui.new") : step.from) + " -> " +
                               step.package.version + "\n";
            if (action == "remove" || action == "disable")
            {
                auto deps = _packages->Dependents(id);
                if (!deps.empty())
                {
                    description += Tr("tui.dependents") + "\n";
                    for (auto const& dep : deps)
                        description += dep + "\n";
                }
            }
            if (Running())
            {
                Message(Tr("tui.stop_external"));
                return;
            }
            description += "\n" + Tr("tui.package_stopped");
            ShowDialog(Tr("tui.confirm") + ": " + Tr("tui." + action), description,
                       {{Tr("tui.apply"), [this, action, id, plan] { MutatePackage(action, id, plan); }},
                        {Tr("tui.cancel"), [] {}}});
        }

        void MutatePackage(std::string action, std::string id, Packages::Plan plan)
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            if ((action == "remove" || action == "disable") && ActiveProvider(id))
            {
                Message(Tr("tui.active_provider"));
                return;
            }
            if ((action == "remove" || action == "disable") && HasDependents(id))
                return;
            if (Running())
            {
                Message(Tr("tui.stop_external"));
                return;
            }
            auto manager = std::make_shared<Packages::Manager>(*_packages);
            auto profilePath = _settings.file;
            Message(Tr("tui.applying"));
            _jobs.Start(
                [this, action, id, plan, manager, profilePath](auto const& cancelled)
                {
                    std::string error;
                    if (manager->ServerRunning())
                        error = Tr("tui.stop_external");
                    if ((action == "remove" || action == "disable") && !manager->Dependents(id).empty())
                        error = Tr("tui.dependents_blocked");
                    bool ok = false;
                    if (error.empty() && !cancelled)
                    {
                        if (action == "install" || action == "update")
                            ok = manager->Install(plan, error, {},
                                                  [&cancelled](uint64_t, uint64_t) { return !cancelled; });
                        else if (action == "remove")
                            ok = manager->Remove(id, error);
                        else
                            ok = manager->SetEnabled(id, action == "enable", error);
                    }
                    else if (error.empty())
                        error = Tr("tui.cancelled");
                    std::string profileError;
                    if (ok)
                    {
                        try
                        {
                            ProfileConfig profile;
                            if (profile.Load(profilePath, profileError))
                            {
                                std::vector<std::string> affected{ id };
                                for (auto const& step : plan.steps)
                                    affected.push_back(step.package.id);
                                // Update only this operation's entries, preserving an imported desired list.
                                SnapshotProfilePlugins(profile, *manager, affected);
                                profile.Save(profileError);
                            }
                        }
                        catch (std::exception const& e) { profileError = e.what(); }
                    }
                    return [this, manager, ok, error, profileError]
                    {
                        _packages = manager;
                        RefreshPlugins();
                        // Rescan actual disk state even after rollback/error; never infer installed schemas from
                        // catalog metadata.
                        ReloadModel();
                        _storage->Reload();
                        _restartNeeded |= ok;
                        Message(!profileError.empty() ? Tr("profile.plugins.save_failed", profileError)
                            : ok ? Tr("profile.plugins.saved") : error);
                    };
                });
        }

        void ReviewPluginProfile()
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            if (Dirty())
            {
                GuardEdits([this] { ReviewPluginProfile(); });
                return;
            }
            auto manager = std::make_shared<Packages::Manager>(*_packages);
            auto path = _settings.file;
            auto indexes = _settings.packageIndex;
            auto storage = _settings.location;
            Message(Tr("tui.loading_catalog"));
            _jobs.Start([this, manager, path, indexes, storage](auto const& cancelled)
            {
                ProfileConfig profile;
                std::string error;
                ProfilePluginPlan plan;
                if (!profile.Load(path, error))
                    plan.error = error;
                else
                {
                    std::string catalogError;
                    plan = PreviewProfilePlugins(profile, *manager, storage);
                    if (!plan.error.empty())
                        manager->LoadIndex(indexes, catalogError, [&cancelled](uint64_t, uint64_t) { return !cancelled; });
                    if (cancelled)
                        plan.error = Tr("tui.cancelled");
                    else
                    {
                        plan = PreviewProfilePlugins(profile, *manager, storage);
                        if (!plan.error.empty() && !catalogError.empty())
                            plan.error += "\n" + catalogError;
                    }
                }
                return [this, manager, plan]
                {
                    _packages = manager;
                    RefreshPlugins();
                    if (!plan.error.empty())
                    {
                        Message(plan.error);
                        return;
                    }
                    ShowDialog(Tr("profile.plugins.preview"), DescribeProfilePlugins(plan),
                        { { Tr("tui.apply"), [this, plan] { ApplyPluginProfile(plan); } },
                          { Tr("tui.cancel"), [] {} } });
                };
            });
        }

        void ApplyPluginProfile(ProfilePluginPlan plan)
        {
            if (Busy())
            {
                Message(Tr("tui.busy"));
                return;
            }
            if (Running())
            {
                Message(Tr("tui.stop_external"));
                return;
            }
            auto manager = std::make_shared<Packages::Manager>(*_packages);
            auto path = _settings.file;
            Message(Tr("tui.applying"));
            _jobs.Start([this, manager, path, plan](auto const& cancelled)
            {
                ProfileConfig profile;
                std::string error;
                bool ok = profile.Load(path, error) && ApplyProfilePlugins(profile, *manager, plan, error, {},
                    [&cancelled](uint64_t, uint64_t) { return !cancelled; }, [&cancelled] { return bool(cancelled); });
                return [this, manager, ok, error]
                {
                    _packages = manager;
                    RefreshPlugins();
                    ReloadModel();
                    _storage->Reload();
                    _restartNeeded |= ok;
                    Message(ok ? Tr("profile.plugins.applied") : Tr("profile.plugins.failed", error));
                };
            });
        }

        Component OverviewPage()
        {
            auto rows = Container::Vertical({});
            rows->Add(Button(Tr("tui.wizard.start"), [this]
            {
                if (Busy())
                {
                    Message(Tr("tui.busy"));
                    return;
                }
                GuardEdits([this]
                {
                    _storage->StartWizard();
                    _tab = _tabSelection = 3;
                });
            }));
            std::vector<std::function<std::string()>> values{
                [] { return Tr("tui.overview_help"); },
                [this] { return Tr("tui.path_settings", Platform::PathToUtf8(_settings.file)); },
                [this] { return Tr("tui.path_root", Platform::PathToUtf8(_context.root)); },
                [this] { return Tr("tui.path_config", Platform::PathToUtf8(_context.config)); },
                [this] { return Tr("tui.path_plugins", Platform::PathToUtf8(_context.plugins)); },
                [this] { return Tr("tui.path_client", Platform::PathToUtf8(_settings.clientPath)); },
                [this]
                {
                    return Tr("tui.storage_current", _settings.location,
                              Tr(_settings.dataCache ? "tui.yes" : "tui.no"));
                },
                [this]
                {
                    std::error_code ec;
                    bool config = fs::is_regular_file(_context.config, ec);
                    return Tr(config ? "tui.config_present" : "tui.config_missing");
                },
                [this] { return Tr(Running() ? "tui.external_running" : "tui.external_stopped"); },
                [this] { return _restartNeeded ? Tr("tui.restart_needed") : ""; }};
            for (auto const& value : values)
                rows->Add(Renderer(
                    [value](bool focused)
                    {
                        auto element = paragraph(SafeText(value()));
                        return focused ? element | focus | inverted : element;
                    }));
            return Renderer(rows, [rows] { return rows->Render() | vscroll_indicator | frame | flex; });
        }

        Component PluginsPage()
        {
            InputOption searchOptions;
            searchOptions.multiline = false;
            searchOptions.on_change = [this] { RefreshPlugins(); };
            auto search = Input(&_query, Tr("tui.search"), searchOptions);
            InputOption indexOptions;
            indexOptions.multiline = false;
            auto indexes = Input(&_indexes, Tr("tui.indexes"), indexOptions);
            auto load = Button(Tr("tui.load_catalog"), [this] { LoadCatalog(); });
            auto saveIndexes = Button(Tr("tui.save_indexes"), [this] { SaveSettings(); });
            auto options = DropdownOption{};
            options.radiobox.entries = &_pluginFilters;
            options.radiobox.selected = &_pluginFilter;
            options.radiobox.on_change = [this] { RefreshPlugins(); };
            auto filter = Dropdown(options);
            auto list = Menu(&_pluginLabels, &_pluginSelection);
            auto actions = Container::Vertical({Button(Tr("tui.install"), [this] { PackageAction("install"); }),
                                                Button(Tr("tui.update"), [this] { PackageAction("update"); }),
                                                Button(Tr("tui.enable"), [this] { PackageAction("enable"); }),
                                                Button(Tr("tui.disable"), [this] { PackageAction("disable"); }),
                                                Button(Tr("tui.remove"), [this] { PackageAction("remove"); }),
                                                Button(Tr("tui.configure"), [this] { ConfigurePlugin(); })});
            auto profile = Button(Tr("profile.plugins.preview"), [this] { ReviewPluginProfile(); });
            auto repoActions = Container::Horizontal({load, saveIndexes, profile});
            auto root = Container::Vertical({indexes, repoActions, search, filter, list, actions});
            return Renderer(root,
                            [this, indexes, repoActions, search, filter, list, actions]
                            {
                                auto row = SelectedPlugin();
                                std::string info = Tr("tui.offline_catalog");
                                if (row)
                                {
                                    info = row->id + "\n" +
                                           (row->local     ? row->local->manifest.description
                                            : row->package ? row->package->description
                                                           : "");
                                    if (row->package)
                                        info += "\n" + Tr("tui.source", row->package->source);
                                }
                                Element detail = vbox({paragraph(SafeText(info)) | size(HEIGHT, LESS_THAN, 5) | frame,
                                                       actions->Render() | frame}) |
                                                 flex;
                                Element items = list->Render() | vscroll_indicator | frame | flex;
                                Element body = Terminal::Size().dimx >= 85
                                                   ? hbox({items, separator(), detail}) | flex
                                                   : vbox({items | flex, detail | size(HEIGHT, LESS_THAN, 7)}) | flex;
                                return vbox(
                                    {paragraph(Tr("tui.indexes") + (_indexes != _settings.packageIndex ? " *" : "")),
                                     indexes->Render() | border, repoActions->Render(), search->Render() | border,
                                     filter->Render(), body});
                            });
        }

        Component Build()
        {
            _tabs = {Tr("tui.overview"), Tr("tui.plugins"), Tr("tui.settings"), Tr("tui.setup")};
            _pluginFilters = {Tr("tui.all"), Tr("tui.installed"), Tr("tui.available")};
            MenuOption menuOptions = MenuOption::Horizontal();
            menuOptions.on_change = [this]
            {
                int target = _tabSelection;
                _tabSelection = _tab;
                Navigate(target);
            };
            auto menu = Menu(&_tabs, &_tabSelection, menuOptions);
            auto settings = CatchEvent(_settingsHolder, [this](Event e) { return Busy() && e != Event::Custom; });
            auto storage =
                CatchEvent(_storage->Build(), [this](Event e) { return _jobs.Busy() && e != Event::Custom; });
            auto pages = Container::Tab({OverviewPage(), PluginsPage(), settings, storage}, &_tab);
            auto quit = Button(Tr("tui.quit"), [this] { Quit(); });
            auto help = Button(Tr("tui.help"), [this] { Help(); });
            auto footer = Container::Horizontal({help, quit});
            auto container = Container::Vertical({menu, pages, footer});
            auto main = Renderer(
                container,
                [this, menu, pages, footer]
                {
                    auto dimensions = Terminal::Size();
                    if (dimensions.dimx < 48 || dimensions.dimy < 20)
                        return vbox({paragraph(Tr("tui.minimum")), paragraph(Tr("tui.keys")), footer->Render()}) |
                               border;
                    return vbox({text("LonelyIce") | bold, menu->Render(), separator(), pages->Render() | flex,
                                 separator(),
                                 paragraph(Busy() ? Tr("tui.busy") : _message) | size(HEIGHT, LESS_THAN, 3) | frame,
                                 paragraph(Tr("tui.keys")) | dim, footer->Render()}) |
                           border;
                });
            _dialogButtons = Container::Vertical({});
            _dialogBody = Container::Vertical({});
            auto dialogContent = Container::Vertical({_dialogBody, _dialogButtons});
            auto dialog = Renderer(dialogContent,
                                   [this]
                                   {
                                       return vbox({paragraph(_dialogTitle) | bold, separator(),
                                                    _dialogBody->Render() | vscroll_indicator | frame | flex,
                                                    separator(), _dialogButtons->Render()}) |
                                              size(WIDTH, LESS_THAN, std::max(20, Terminal::Size().dimx - 6)) |
                                              size(HEIGHT, LESS_THAN, std::max(8, Terminal::Size().dimy - 4)) | border |
                                              center;
                                   });
            auto modal = Modal(main, dialog, &_dialog);
            return CatchEvent(modal,
                              [this](Event event)
                              {
                                  if (event == Event::Custom)
                                  {
                                      Tick();
                                      return true;
                                  }
                                  if (event == Event::CtrlC || event == Event::F10)
                                  {
                                      Quit();
                                      return true;
                                  }
                                  if (_dialog && event == Event::Escape)
                                  {
                                      _dialog = false;
                                      return true;
                                  }
                                  if (_dialog)
                                      return false;
                                  if (event == Event::F1)
                                  {
                                      Help();
                                      return true;
                                  }
                                  auto dimensions = Terminal::Size();
                                  if (dimensions.dimx < 48 || dimensions.dimy < 20)
                                      return true;
                                  if (event == Event::F2)
                                  {
                                      Navigate(0);
                                      return true;
                                  }
                                  if (event == Event::F3)
                                  {
                                      Navigate(1);
                                      return true;
                                  }
                                  if (event == Event::F4)
                                  {
                                      Navigate(2);
                                      return true;
                                  }
                                  if (event == Event::F5)
                                  {
                                      Navigate(3);
                                      return true;
                                  }
                                  if (event == Event::CtrlS)
                                  {
                                      SaveSettings();
                                      return true;
                                  }
                                  if (event == Event::Escape && _jobs.Busy())
                                  {
                                      _jobs.Cancel();
                                      Message(Tr("tui.cancelling"));
                                      return true;
                                  }
                                  return false;
                              });
        }

        void Help()
        {
            ShowDialog(Tr("tui.help"), Tr("tui.help_text"), {{Tr("tui.back"), [] {}}});
        }

        ScreenInteractive _screen;
        LauncherSettings _settings;
        LaunchContext _context;
        std::shared_ptr<Packages::Manager> _packages;
        TuiJobs _jobs;
        SettingsModel _model;
        TuiSettingsView _settingsView;
        std::unique_ptr<StorageSetupView> _storage;
        Component _settingsHolder, _dialogButtons, _dialogBody;
        std::vector<std::string> _tabs, _pluginLabels, _pluginFilters;
        std::vector<PluginRow> _pluginRows;
        std::string _indexes, _query, _message, _dialogTitle, _dialogText;
        int _tab = 0, _tabSelection = 0, _pluginSelection = 0, _pluginFilter = 0;
        bool _dialog = false, _restartNeeded = false;
    };
} // namespace

int TuiMain(int argc, char** argv)
{
    Platform::UseParentConsole(true, true);
    auto languageOverride = Platform::GetEnv("LONELYICE_LANG");
    Lang::Init();
    fs::path settingsFile = Platform::ExePath().parent_path() / "server.yaml";
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--tui" || arg == "-nw" || arg == "--nw")
            continue;
        if (arg == "--help" || arg == "-h")
        {
            std::cout << "LonelyIce --tui | -nw | --nw [--settings <server.yaml>]\n";
            return 0;
        }
        if (arg == "--settings" && i + 1 < argc)
            settingsFile = Platform::Utf8ToPath(argv[++i]);
        else
        {
            std::cerr << Tr("tui.invalid_option", arg) << '\n';
            return 2;
        }
    }
#ifdef _WIN32
    bool const tty = _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    bool const tty = isatty(fileno(stdin)) && isatty(fileno(stdout));
#endif
    if (!tty)
    {
        std::cerr << Tr("tui.requires_tty") << '\n';
        return 2;
    }
    try
    {
        LauncherSettings settings;
        settings.file = fs::absolute(settingsFile).lexically_normal();
        std::string loadError;
        if (!settings.Load(&loadError))
        {
            std::cerr << loadError << '\n';
            return 2;
        }
        ResolveSettingsPaths(settings);
        if ((!languageOverride || languageOverride->empty()) && !settings.language.empty())
            Lang::Set(settings.language);
        return TerminalLauncher(std::move(settings)).Run();
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
