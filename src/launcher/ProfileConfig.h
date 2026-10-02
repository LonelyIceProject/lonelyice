#ifndef LONELYICE_PROFILECONFIG_H
#define LONELYICE_PROFILECONFIG_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <fkYAML/node.hpp>

namespace LonelyIce
{
    class ProfileConfig
    {
    public:
        enum class Layer { Profile, Local };
        using Path = std::vector<std::string>;

        // A portable server.yaml and sibling local.yaml. Missing files start as empty mappings.
        bool Load(std::filesystem::path const& profilePath, std::string& error);
        bool Save(std::string& error);
        std::filesystem::path const& ProfilePath() const { return _profilePath; }
        std::filesystem::path const& LocalPath() const { return _localPath; }
        fkyaml::node Effective() const;
        std::optional<fkyaml::node> Get(Path const& path) const;
        std::optional<fkyaml::node> Get(Path const& path, Layer layer) const;
        // Mutators reject attempts to traverse an existing non-mapping value, preserving unknown fields.
        void Set(Path const& path, fkyaml::node value, Layer layer = Layer::Profile);
        void SetEffective(Path const& path, fkyaml::node value);
        void Remove(Path const& path, Layer layer = Layer::Profile);
        std::string String(Path const& path, std::string const& fallback = {}) const;
        bool Boolean(Path const& path, bool fallback = false) const;
        int64_t Integer(Path const& path, int64_t fallback = 0) const;

        // Core configuration values need scalar text while retaining typed YAML in the profile.
        static std::string Scalar(fkyaml::node const& value);
        static bool LoadYaml(std::filesystem::path const& path, fkyaml::node& root, std::string& error,
            bool missingAllowed = true);
        static bool SaveYamlAtomic(std::filesystem::path const& path, fkyaml::node const& root,
            std::string& error, bool privateFile = false);

    private:
        fkyaml::node _profile = fkyaml::node::mapping();
        fkyaml::node _local = fkyaml::node::mapping();
        std::filesystem::path _profilePath, _localPath;
        bool _profileChanged = false, _localChanged = false;
    };
}

#endif
