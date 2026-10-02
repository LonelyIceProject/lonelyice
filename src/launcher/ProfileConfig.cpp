#include "ProfileConfig.h"
#include "Platform.h"
#include <atomic>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace LonelyIce;

namespace
{
    std::string Quoted(std::string const& text)
    {
        std::string quoted = "\"";
        for (unsigned char character : text)
        {
            if (character == '\\' || character == '"')
            {
                quoted += '\\';
                quoted += char(character);
            }
            else if (character == '\n')
                quoted += "\\n";
            else if (character == '\r')
                quoted += "\\r";
            else if (character == '\t')
                quoted += "\\t";
            else if (character < 32 || character == 127)
            {
                constexpr char hex[] = "0123456789abcdef";
                quoted += "\\x";
                quoted += hex[character >> 4];
                quoted += hex[character & 15];
            }
            else
                quoted += char(character);
        }
        return quoted + '"';
    }

    bool Inline(fkyaml::node const& node)
    {
        return (!node.is_mapping() && !node.is_sequence()) || node.empty();
    }

    void Emit(std::ostream& output, fkyaml::node const& node, std::size_t indentation = 0)
    {
        if (indentation > 512)
            throw std::runtime_error("YAML configuration is nested too deeply or contains a cyclic alias.");
        std::string const space(indentation, ' ');
        if (node.is_mapping())
        {
            if (node.empty())
                output << "{}";
            for (auto const& [key, value] : node.as_map())
            {
                if (key.is_mapping() || key.is_sequence())
                    throw std::runtime_error("YAML configuration requires scalar mapping keys.");
                output << space;
                Emit(output, key);
                output << ':';
                if (Inline(value))
                {
                    output << ' ';
                    Emit(output, value, indentation + 2);
                    output << '\n';
                }
                else
                {
                    output << '\n';
                    Emit(output, value, indentation + 2);
                }
            }
        }
        else if (node.is_sequence())
        {
            if (node.empty())
                output << "[]";
            for (auto const& value : node.as_seq())
            {
                output << space << '-';
                if (Inline(value))
                {
                    output << ' ';
                    Emit(output, value, indentation + 2);
                    output << '\n';
                }
                else
                {
                    output << '\n';
                    Emit(output, value, indentation + 2);
                }
            }
        }
        else if (node.is_string())
            output << Quoted(node.get_value<std::string>());
        else if (node.is_boolean())
            output << (node.get_value<bool>() ? "true" : "false");
        else if (node.is_integer())
            output << node.get_value<int64_t>();
        else if (node.is_float_number())
        {
            double const number = node.get_value<double>();
            if (std::isnan(number))
                output << ".nan";
            else if (std::isinf(number))
                output << (number < 0 ? "-.inf" : ".inf");
            else
            {
                std::ostringstream scalar;
                scalar << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
                std::string const text = scalar.str();
                output << text;
                if (text.find_first_of(".eE") == std::string::npos)
                    output << ".0";
            }
        }
        else
            output << "null";
    }

    fkyaml::node Merge(fkyaml::node const& profile, fkyaml::node const& local)
    {
        if (!profile.is_mapping() || !local.is_mapping())
            return local;
        fkyaml::node merged = profile;
        for (auto const& [key, value] : local.as_map())
            merged[key] = merged.contains(key) ? Merge(merged[key], value) : value;
        return merged;
    }

    std::string PathText(ProfileConfig::Path const& path)
    {
        std::string text;
        for (std::string const& key : path)
        {
            if (!text.empty())
                text += '/';
            text += key;
        }
        return text;
    }

    std::optional<fkyaml::node> Find(fkyaml::node const& root, ProfileConfig::Path const& path)
    {
        fkyaml::node const* node = &root;
        for (std::string const& key : path)
        {
            if (!node->is_mapping())
                throw std::runtime_error("Expected a YAML mapping at " + PathText(path));
            if (!node->contains(key))
                return std::nullopt;
            node = &(*node)[key];
        }
        return *node;
    }
}

bool ProfileConfig::LoadYaml(fs::path const& path, fkyaml::node& root, std::string& error, bool missingAllowed)
{
    error.clear();
    std::error_code ec;
    if (!fs::exists(path, ec) && !ec && missingAllowed)
    {
        root = fkyaml::node::mapping();
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "Cannot read YAML configuration: " + Platform::PathToUtf8(path);
        return false;
    }
    try
    {
        std::vector<fkyaml::node> documents = fkyaml::node::deserialize_docs(input);
        if (documents.size() > 1)
            throw std::runtime_error("Use a single YAML document.");
        fkyaml::node parsed = documents.empty() ? fkyaml::node::mapping() : documents.front();
        if (parsed.is_null())
            parsed = fkyaml::node::mapping();
        if (!parsed.is_mapping())
            throw std::runtime_error("The document root must be a mapping.");
        root = std::move(parsed);
        return true;
    }
    catch (std::exception const&)
    {
        // Parser messages may echo a secret token. Explain the correction without echoing source values.
        error = "Invalid YAML configuration: " + Platform::PathToUtf8(path)
            + ". Use one document with a mapping at the root; check indentation, quotes and value types.";
        return false;
    }
}

bool ProfileConfig::SaveYamlAtomic(fs::path const& path, fkyaml::node const& root, std::string& error, bool privateFile)
{
    error.clear();
    fs::path temporary;
    try
    {
        if (!path.parent_path().empty())
            fs::create_directories(path.parent_path());
        static std::atomic<uint64_t> sequence = 0;
#ifdef _WIN32
        unsigned long const processId = GetCurrentProcessId();
#else
        auto const processId = getpid();
#endif
        temporary = path;
        temporary += ".tmp-" + std::to_string(processId) + "-" + std::to_string(++sequence);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("open");
#ifndef _WIN32
        if (privateFile)
            fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#else
        (void)privateFile;
#endif
        // fkYAML's emitter leaves strings containing inline '#' comments unquoted; always quote strings.
        Emit(output, root);
        output << '\n';
        output.flush();
        output.close();
        if (!output)
            throw std::runtime_error("write");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("replace");
#else
        fs::rename(temporary, path);
#endif
        return true;
    }
    catch (std::exception const&)
    {
        std::error_code ignored;
        if (!temporary.empty())
            fs::remove(temporary, ignored);
        error = "Cannot save YAML configuration: " + Platform::PathToUtf8(path)
            + ". Check that its directory is writable and has free disk space.";
        return false;
    }
}

bool ProfileConfig::Load(fs::path const& profilePath, std::string& error)
{
    try
    {
        fs::path const portable = fs::absolute(profilePath).lexically_normal();
        if (portable.extension() != ".yaml" && portable.extension() != ".yml")
        {
            error = "Select a YAML server profile (.yaml or .yml). Legacy INI and CONF files are not supported.";
            return false;
        }
        fs::path const local = portable.parent_path() / "local.yaml";
        if (portable == local)
        {
            error = "Select server.yaml as the profile; local.yaml is loaded automatically as its local overrides.";
            return false;
        }
        fkyaml::node profileRoot, localRoot;
        if (!LoadYaml(portable, profileRoot, error) || !LoadYaml(local, localRoot, error))
            return false;
        _profilePath = portable;
        _localPath = local;
        _profile = std::move(profileRoot);
        _local = std::move(localRoot);
        _profileChanged = _localChanged = false;
        return true;
    }
    catch (fs::filesystem_error const&)
    {
        error = "Cannot resolve the YAML profile path. Select an accessible server.yaml file.";
        return false;
    }
}

bool ProfileConfig::Save(std::string& error)
{
    if (_profilePath.empty())
    {
        error = "No YAML server profile has been loaded.";
        return false;
    }
    if (_profileChanged)
    {
        if (!SaveYamlAtomic(_profilePath, _profile, error))
            return false;
        _profileChanged = false;
    }
    if (_localChanged)
    {
        if (!SaveYamlAtomic(_localPath, _local, error, true))
            return false;
        _localChanged = false;
    }
    error.clear();
    return true;
}

fkyaml::node ProfileConfig::Effective() const
{
    return Merge(_profile, _local);
}

std::optional<fkyaml::node> ProfileConfig::Get(Path const& path) const
{
    return Find(Effective(), path);
}

std::optional<fkyaml::node> ProfileConfig::Get(Path const& path, Layer layer) const
{
    return Find(layer == Layer::Local ? _local : _profile, path);
}

void ProfileConfig::Set(Path const& path, fkyaml::node value, Layer layer)
{
    if (path.empty())
        throw std::runtime_error("A YAML setting path cannot be empty.");
    fkyaml::node* node = layer == Layer::Local ? &_local : &_profile;
    for (std::size_t index = 0; index + 1 < path.size(); ++index)
    {
        if (!node->is_mapping())
            throw std::runtime_error("Expected a YAML mapping at " + PathText(path));
        if (!node->contains(path[index]))
            (*node)[path[index]] = fkyaml::node::mapping();
        node = &(*node)[path[index]];
    }
    if (!node->is_mapping())
        throw std::runtime_error("Expected a YAML mapping at " + PathText(path));
    (*node)[path.back()] = std::move(value);
    (layer == Layer::Local ? _localChanged : _profileChanged) = true;
}

void ProfileConfig::SetEffective(Path const& path, fkyaml::node value)
{
    Set(path, std::move(value), Get(path, Layer::Local) ? Layer::Local : Layer::Profile);
}

void ProfileConfig::Remove(Path const& path, Layer layer)
{
    if (path.empty())
        return;
    fkyaml::node* node = layer == Layer::Local ? &_local : &_profile;
    for (std::size_t index = 0; index + 1 < path.size(); ++index)
    {
        if (!node->is_mapping() || !node->contains(path[index]))
            return;
        node = &(*node)[path[index]];
    }
    if (node->is_mapping() && node->contains(path.back()))
    {
        node->as_map().erase(fkyaml::node(path.back()));
        (layer == Layer::Local ? _localChanged : _profileChanged) = true;
    }
}

std::string ProfileConfig::Scalar(fkyaml::node const& value)
{
    if (value.is_string())
        return value.get_value<std::string>();
    if (value.is_boolean())
        return value.get_value<bool>() ? "1" : "0";
    if (value.is_integer())
        return std::to_string(value.get_value<int64_t>());
    if (value.is_float_number())
        return fkyaml::node::serialize(value);
    throw std::runtime_error("Expected a scalar YAML setting value.");
}

std::string ProfileConfig::String(Path const& path, std::string const& fallback) const
{
    auto value = Get(path);
    if (!value)
        return fallback;
    if (!value->is_string())
        throw std::runtime_error("Expected a YAML string at " + PathText(path));
    return value->get_value<std::string>();
}

bool ProfileConfig::Boolean(Path const& path, bool fallback) const
{
    auto value = Get(path);
    if (!value)
        return fallback;
    if (!value->is_boolean())
        throw std::runtime_error("Expected a YAML boolean at " + PathText(path));
    return value->get_value<bool>();
}

int64_t ProfileConfig::Integer(Path const& path, int64_t fallback) const
{
    auto value = Get(path);
    if (!value)
        return fallback;
    if (!value->is_integer())
        throw std::runtime_error("Expected a YAML integer at " + PathText(path));
    return value->get_value<int64_t>();
}
