#include "StorageForm.h"
#include "Lang.h"
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <algorithm>

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    // A check starts this long after the connection fields stop changing.
    constexpr uint64_t TypingPauseMs = 900;

    // The same place: the location and, for a database server, its connection (the cache does not matter).
    bool SamePlace(StorageChoice const& a, StorageChoice const& b)
    {
        return a.location == b.location && (!a.Remote() || a.remote == b.remote);
    }
}

StorageForm::StorageForm(std::string prefix, Host host) : _p(std::move(prefix)), _host(std::move(host)), _check(_host.wake)
{
}

void StorageForm::Bind(Rml::DataModelConstructor& c, bool registerTypes)
{
    if (registerTypes)
    {
        if (auto s = c.RegisterStruct<LocRow>())
        {
            s.RegisterMember("id", &LocRow::id);
            s.RegisterMember("title", &LocRow::title);
            s.RegisterMember("detail", &LocRow::detail);
        }
        c.RegisterArray<std::vector<LocRow>>();
    }
    c.Bind(_p + "locs", &_locs);
    c.Bind(_p + "loc", &_loc);
    c.Bind(_p + "remote", &_remote);
    c.Bind(_p + "cache", &_cache);
    c.Bind(_p + "cache_desc", &_cacheDesc);
    c.Bind(_p + "host", &_host_);
    c.Bind(_p + "port", &_port);
    c.Bind(_p + "user", &_user);
    c.Bind(_p + "pass", &_pass);
    c.Bind(_p + "prefix", &_prefix);
    c.Bind(_p + "state", &_state);
    c.Bind(_p + "note", &_note);
    c.BindEventCallback(_p + "pick", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const& args)
    {
        if (args.empty())
            return;
        _loc = args[0].Get<Rml::String>();
        for (StorageProviderInfo const& p : _providers)
            if (p.provider.id == _loc && _port.empty())
                _port = p.provider.port;
        Refresh();
        Check();
    });
    c.BindEventCallback(_p + "check", [this](Rml::DataModelHandle, Rml::Event&, Rml::VariantList const&) { Check(); });
}

void StorageForm::Dirty()
{
    if (_model)
        _model.DirtyAllVariables();
}

void StorageForm::Load(StorageChoice const& choice)
{
    _providers = _host.providers ? _host.providers() : std::vector<StorageProviderInfo>();
    _loc = choice.location;
    _cache = choice.cache;
    _host_ = choice.remote.host;
    _port = choice.remote.port;
    _user = choice.remote.user;
    _pass = choice.remote.password;
    _prefix = choice.remote.prefix;
    for (StorageProviderInfo const& p : _providers)
        if (p.provider.id == _loc && _port.empty())
            _port = p.provider.port;
    _result.reset();
    _checked.reset();
    _seen = Choice();
    _editedAt = 0;
    Refresh();
    Check();
}

StorageChoice StorageForm::Choice() const
{
    StorageChoice c;
    c.location = _loc;
    c.cache = _remote || _cache;
    c.remote = { _host_, _port, _user, _pass, _prefix };
    return c;
}

std::string StorageForm::LocationTitle(std::string const& location) const
{
    for (LocRow const& l : _locs)
        if (l.id == location)
            return l.title;
    return location;
}

bool StorageForm::Checkable(StorageChoice const& c) const
{
    return !c.Remote() || (!c.remote.host.empty() && !c.remote.user.empty() && !c.remote.prefix.empty());
}

void StorageForm::Check()
{
    StorageChoice const now = Choice();
    _result.reset();
    _editedAt = 0;
    if (!Checkable(now))
    {
        _checked.reset();
        _state.clear();
        _note = Tr("storage.check.fill");
        Dirty();
        return;
    }
    _checked = now;
    _check.Start(_host.exe, _host.env ? _host.env(now) : Platform::Env());
    _state = "run";
    _note = Tr("storage.check.running");
    Dirty();
}

bool StorageForm::Tick()
{
    bool changed = false;
    StorageChoice const now = Choice();
    if (std::optional<StorageState> r = _check.Take(); r && _checked && SamePlace(*_checked, now))
    {
        _result = r;
        changed = true;
        Relocalize();
    }
    if (!SamePlace(now, _seen))
    {
        _seen = now;
        _editedAt = Platform::TickMs();
        if (_result || _check.IsRunning())
        {
            _result.reset();
            _state.clear();
            _note.clear();
            changed = true;
            Dirty();
        }
    }
    if (_editedAt && Platform::TickMs() - _editedAt >= TypingPauseMs)
    {
        _editedAt = 0;
        if (!_checked || !SamePlace(*_checked, now) || !_result)
        {
            Check();
            changed = true;
        }
    }
    return changed;
}

void StorageForm::Refresh()
{
    _locs.clear();
    _locs.push_back({ "local", Tr("storage.local"), Tr("storage.local.detail") });
    for (StorageProviderInfo const& p : _providers)
        _locs.push_back({ p.provider.id, p.provider.name, Tr("storage.remote.detail") });
    if (std::none_of(_locs.begin(), _locs.end(), [&](LocRow const& l) { return l.id == _loc; }))
        _loc = "local";
    _remote = _loc != "local";
    _cacheDesc = Tr(_remote ? "storage.cache.remote" : "storage.cache.desc");
    Dirty();
}

void StorageForm::Relocalize()
{
    if (!_locs.empty())
        Refresh();
    if (_check.IsRunning())
    {
        _state = "run";
        _note = Tr("storage.check.running");
    }
    else if (_result)
    {
        StorageState const& r = *_result;
        _state = !r.reached ? "bad" : r.databases ? "ok" : "warn";
        _note = !r.reached ? r.error : !r.databases ? Tr("storage.check.empty")
            : Tr(r.HasDbc() ? "storage.check.ready_cache" : "storage.check.ready");
    }
    else if (!Checkable(Choice()))
    {
        _state.clear();
        _note = Tr("storage.check.fill");
    }
    Dirty();
}
