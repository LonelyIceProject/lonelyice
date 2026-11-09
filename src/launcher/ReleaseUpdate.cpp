#include "ReleaseUpdate.h"
#include "CryptoHash.h"
#include "Lang.h"
#include "PackageManager.h"
#include "Platform.h"
#include "ServerLock.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <fkYAML/node.hpp>
#ifdef __linux__
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace LonelyIce;
using namespace LonelyIce::ReleaseUpdate;
using namespace std::chrono_literals;

namespace
{
    constexpr std::string_view Repository = "https://github.com/LonelyIceProject/lonelyice/releases/";
    constexpr char Latest[] = "https://api.github.com/repos/LonelyIceProject/lonelyice/releases/latest";
    constexpr std::string_view StagePrefix = ".lonelyice-update-";
    constexpr char ResultFile[] = ".lonelyice-update-result";
    constexpr std::size_t MaxArchive = 512 * 1024 * 1024;

    std::optional<std::array<unsigned, 3>> Version(std::string_view text)
    {
        std::array<unsigned, 3> parts{};
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            auto end = text.find('.');
            auto part = text.substr(0, end);
            if (part.empty() || (part.size() > 1 && part.front() == '0') ||
                (end == text.npos) != (i == parts.size() - 1))
                return std::nullopt;
            auto parsed = std::from_chars(part.data(), part.data() + part.size(), parts[i]);
            if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size())
                return std::nullopt;
            if (end != text.npos)
                text.remove_prefix(end + 1);
        }
        return parts;
    }

    std::string AssetName(std::string const& tag)
    {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#ifdef LONELYICE_GUI
        return "lonelyice-" + tag + "-windows-x64-desktop.zip";
#else
        return "lonelyice-" + tag + "-windows-x64-tui.zip";
#endif
#elif defined(__linux__) && defined(__x86_64__) && !defined(LONELYICE_GUI)
        return "lonelyice-" + tag + "-linux-x64-tui.tar.gz";
#else
        return {};
#endif
    }

    char const* Executable()
    {
#ifdef _WIN32
        return "LonelyIce.exe";
#else
        return "LonelyIce";
#endif
    }

    std::string Text(fkyaml::node const& node, char const* key)
    {
        return node.is_mapping() && node.contains(key) && node[key].is_string()
            ? node[key].get_value<std::string>() : std::string{};
    }

    std::string HexSha256(std::vector<uint8_t> const& bytes)
    {
        auto digest = Acore::Crypto::SHA256::GetDigestOf(bytes);
        std::string hex;
        for (uint8_t byte : digest)
        {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        return hex;
    }

    bool Hex(std::string_view text)
    {
        return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == text.npos;
    }

    bool StageName(std::string_view name)
    {
        return name.starts_with(StagePrefix) && name.size() > StagePrefix.size() &&
            name.substr(StagePrefix.size()).find_first_not_of("0123456789") == name.npos;
    }

    bool SafeComponent(std::string_view name)
    {
        return !name.empty() && name != "." && name != ".." &&
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == name.npos;
    }

    void EnsureTree(fs::path const& root)
    {
        if (fs::symlink_status(root).type() != fs::file_type::directory)
            throw std::runtime_error(Tr("release.invalid_archive"));
        for (auto const& item : fs::recursive_directory_iterator(root))
        {
            auto type = item.symlink_status().type();
            if (type != fs::file_type::directory && type != fs::file_type::regular)
                throw std::runtime_error(Tr("release.invalid_archive"));
        }
    }

    fs::path MakeStage(fs::path const& root)
    {
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            fs::path directory = root / (std::string(StagePrefix) + std::to_string(random()) + std::to_string(random()));
            if (fs::create_directory(directory))
                return directory;
        }
        throw std::runtime_error(Tr("release.stage_failed"));
    }

    void Write(fs::path const& path, std::string_view data)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out)
            throw std::runtime_error(Tr("release.stage_failed"));
    }

    void Extract(fs::path const& archive, fs::path const& payload)
    {
        fs::create_directory(payload);
        Platform::ChildOptions options;
#ifdef _WIN32
        auto windows = Platform::GetEnv("SystemRoot");
        if (!windows)
            throw std::runtime_error(Tr("release.invalid_archive"));
        options.exe = Platform::Utf8ToPath(*windows) / "System32" / "tar.exe";
#else
        options.exe = "/usr/bin/tar";
#endif
        options.args = { "-xf", Platform::PathToUtf8(archive), "-C", Platform::PathToUtf8(payload) };
        options.workDir = payload;
        Platform::Child tar;
        std::string error;
        if (!tar.Start(options, error))
            throw std::runtime_error(Tr("release.extract_failed", error));
        tar.CloseInput();
        char buffer[1024];
        while (tar.Read(buffer, sizeof(buffer)))
        {
        }
        tar.Wait(-1);
        if (tar.ExitCode() != 0)
            throw std::runtime_error(Tr("release.invalid_archive"));
        EnsureTree(payload);
        for (fs::path const& required : { fs::path(Executable()), fs::path("setup/sql.pak"), fs::path("setup/configs.pak"),
#ifdef _WIN32
            fs::path("common.dll"), fs::path("shared.dll"), fs::path("game.dll"), fs::path("database.dll"),
            fs::path("libcrypto-3-x64.dll"), fs::path("libssl-3-x64.dll")
#else
            fs::path("libcommon.so"), fs::path("libshared.so"), fs::path("libgame.so"), fs::path("libdatabase.so")
#endif
        })
            if (!fs::is_regular_file(payload / required) || fs::file_size(payload / required) == 0)
                throw std::runtime_error(Tr("release.invalid_archive"));
    }

    bool TargetRunning(fs::path const& exe)
    {
#ifdef __linux__
        std::error_code error;
        for (auto const& proc : fs::directory_iterator("/proc", error))
        {
            if (fs::read_symlink(proc.path() / "exe", error) == exe)
                return true;
        }
        return false;
#else
        return Platform::IsProcessRunning(exe);
#endif
    }

    std::string CoreAbi(fs::path const& dir)
    {
        std::ifstream in(dir / "plugin.json");
        auto node = fkyaml::node::deserialize(in);
        return node.is_mapping() && node.contains("core") ? Text(node["core"], "abi") : std::string{};
    }

    bool ManagedFile(std::string_view name)
    {
        return name == Executable() || name == "authserver" || name == "worldserver" ||
            name == "authserver.exe" || name == "worldserver.exe" ||
            name.ends_with(".dll") || name.ends_with(".so") ||
            name == "README-Linux.txt" || name == "cli.md" || name == "configuration.md" ||
            name == "building.md" || name == "terminal.md" ||
            name == "runtime-dependencies.txt" || name == "source-versions.json";
    }

    void CheckDestination(fs::path const& root, fs::path const& relative)
    {
        fs::path path = root;
        for (auto const& component : relative)
        {
            if (!SafeComponent(Platform::PathToUtf8(component)))
                throw std::runtime_error(Tr("release.invalid_archive"));
            path /= component;
            if (fs::is_symlink(fs::symlink_status(path)))
                throw std::runtime_error(Tr("release.invalid_archive"));
        }
    }

    struct Change
    {
        fs::path source, destination;
    };

    std::vector<Change> Changes(fs::path const& root, fs::path const& payload)
    {
        std::vector<Change> changes;
        for (auto const& entry : fs::directory_iterator(payload))
        {
            fs::path name = entry.path().filename();
            if (entry.is_regular_file() && ManagedFile(Platform::PathToUtf8(name)))
                changes.push_back({ name, name });
        }
        for (fs::path const& pack : { fs::path("setup/sql.pak"), fs::path("setup/configs.pak") })
            changes.push_back({ pack, pack });

        fs::path const plugins = "plugins";
        fs::path const disabled = plugins / ".disabled";
        CheckDestination(root, disabled);
        auto enabledPlugins = ReadPlugins(root / plugins);
        auto disabledPlugins = ReadPlugins(root / disabled);
        for (auto const& bundled : ReadPlugins(payload / plugins))
        {
            fs::path source = plugins / bundled.dir.filename();
            fs::path destination = source;
            auto find = [&](std::vector<PluginManifest> const& installed) -> PluginManifest const*
            {
                auto found = std::find_if(installed.begin(), installed.end(),
                    [&](PluginManifest const& plugin) { return plugin.id == bundled.id; });
                return found == installed.end() ? nullptr : &*found;
            };
            auto const* installed = find(enabledPlugins);
            if (!installed)
                installed = find(disabledPlugins);
            if (installed)
            {
                destination = installed->dir.lexically_relative(root);
                CheckDestination(root, destination);
                if (CoreAbi(installed->dir) == CoreAbi(bundled.dir) &&
                    Packages::Manager::CompareVersions(installed->version, bundled.version) >= 0)
                    continue;
            }
            // A directory with no readable manifest may belong to the user.
            else if (fs::exists(root / destination))
                continue;
            changes.push_back({ source, destination });
        }
        std::stable_partition(changes.begin(), changes.end(),
            [](Change const& change) { return change.destination != Executable(); });
        return changes;
    }

    void ApplyFiles(fs::path const& root, fs::path const& stage)
    {
        fs::path const incoming = stage / "incoming";
        fs::path const previous = stage / "previous";
        fs::path const payload = stage / "payload";
        EnsureTree(payload);
        auto changes = Changes(root, payload);
        std::string plan;
        for (auto const& change : changes)
        {
            CheckDestination(root, change.destination);
            auto const target = fs::symlink_status(root / change.destination);
            auto const source = fs::symlink_status(payload / change.source);
            if (fs::exists(target) && target.type() != source.type())
                throw std::runtime_error(Tr("release.stage_failed"));
            fs::create_directories((incoming / change.destination).parent_path());
            fs::create_directories((previous / change.destination).parent_path());
            fs::copy(payload / change.source, incoming / change.destination, fs::copy_options::recursive);
            plan += Platform::PathToUtf8(change.destination) + '\n';
        }
        Write(stage / "restore-plan", plan);

        std::vector<fs::path> attempted;
        try
        {
            for (auto const& change : changes)
            {
                fs::path const& relative = change.destination;
                attempted.push_back(relative);
                fs::path const target = root / relative;
                fs::create_directories(target.parent_path());
                if (fs::exists(target))
                    fs::rename(target, previous / relative);
                fs::rename(incoming / relative, target);
            }
        }
        catch (...)
        {
            std::error_code error;
            for (auto it = attempted.rbegin(); it != attempted.rend(); ++it)
            {
                fs::path const target = root / *it;
                if (!fs::exists(incoming / *it))
                    fs::rename(target, incoming / *it, error);
                if (error)
                    throw std::runtime_error(Tr("release.restore_failed", Platform::PathToUtf8(stage)));
                if (fs::exists(previous / *it))
                    fs::rename(previous / *it, target, error);
                if (error)
                    throw std::runtime_error(Tr("release.restore_failed", Platform::PathToUtf8(stage)));
            }
            fs::remove(stage / "restore-plan", error);
            throw;
        }
    }

#ifdef __linux__
    bool ReplaceProcess(fs::path const& exe, std::vector<std::string> args)
    {
        args.insert(args.begin(), Platform::PathToUtf8(exe));
        std::vector<char*> argv;
        for (std::string& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        return false;
    }
#endif
}

std::string ReleaseUpdate::CurrentVersion()
{
    return LONELYICE_VERSION;
}

std::optional<Release> ReleaseUpdate::Check(std::string& error, Http::Progress const& progress)
{
    error.clear();
    auto current = Version(CurrentVersion());
    if (!current)
    {
        error = Tr("release.unknown_version");
        return std::nullopt;
    }
    if (AssetName("").empty())
    {
        error = Tr("release.unsupported");
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    if (!Http::GetHttps(Latest, bytes, error, 1024 * 1024, progress))
        return std::nullopt;
    try
    {
        auto node = fkyaml::node::deserialize(std::string(bytes.begin(), bytes.end()));
        std::string tag = Text(node, "tag_name");
        auto latest = tag.starts_with('v') ? Version(std::string_view(tag).substr(1)) : std::nullopt;
        if (!latest || !node.contains("draft") || !node["draft"].is_boolean() || node["draft"].get_value<bool>() ||
            !node.contains("prerelease") || !node["prerelease"].is_boolean() || node["prerelease"].get_value<bool>() ||
            !node.contains("assets") || !node["assets"].is_sequence())
            throw std::runtime_error(Tr("release.invalid_release"));
        if (*latest <= *current)
            return std::nullopt;
        std::string const name = AssetName(tag);
        std::string const download = std::string(Repository) + "download/" + tag + "/" + name;
        for (auto const& asset : node["assets"].as_seq())
        {
            if (Text(asset, "name") != name)
                continue;
            std::string digest = Text(asset, "digest");
            if (Text(asset, "browser_download_url") != download || !digest.starts_with("sha256:") ||
                !Hex(std::string_view(digest).substr(7)) || !asset.contains("size") || !asset["size"].is_integer())
                throw std::runtime_error(Tr("release.invalid_release"));
            auto size = asset["size"].get_value<int64_t>();
            if (size <= 0 || size > int64_t(MaxArchive))
                throw std::runtime_error(Tr("release.invalid_release"));
            return Release{ tag.substr(1), std::string(Repository) + "tag/" + tag, download,
                            digest.substr(7), static_cast<std::size_t>(size) };
        }
        error = Tr("release.no_asset", tag);
    }
    catch (std::exception const&)
    {
        error = Tr("release.invalid_release");
    }
    return std::nullopt;
}

std::optional<Staged> ReleaseUpdate::Stage(Release const& release, fs::path const& root, std::string& error,
                                            Http::Progress const& progress)
{
    error.clear();
    fs::path directory;
    try
    {
        directory = MakeStage(root);
        fs::path const archive = directory / "release";
        {
            std::vector<uint8_t> bytes;
            if (!Http::GetHttps(release.download, bytes, error, release.size, progress))
                throw std::runtime_error(error);
            if (bytes.size() != release.size || HexSha256(bytes) != release.sha256)
                throw std::runtime_error(Tr("release.checksum"));
            Write(archive, std::string_view(reinterpret_cast<char const*>(bytes.data()), bytes.size()));
        }
        if (progress && !progress(release.size, release.size))
            throw std::runtime_error(Tr("pkg.http.cancelled"));
        Extract(archive, directory / "payload");
        if (progress && !progress(release.size, release.size))
            throw std::runtime_error(Tr("pkg.http.cancelled"));
        fs::remove(archive);
        return Staged{ directory, release.version };
    }
    catch (std::exception const& e)
    {
        error = e.what();
        if (!directory.empty())
            Discard({ directory, release.version });
        return std::nullopt;
    }
}

void ReleaseUpdate::Discard(Staged const& staged)
{
    if (!StageName(Platform::PathToUtf8(staged.directory.filename())))
        return;
    std::error_code error;
    fs::remove_all(staged.directory, error);
}

bool ReleaseUpdate::Install(Staged const& staged, fs::path const& profile, bool tui, std::string& error)
{
    Platform::ChildOptions options;
    options.exe = staged.directory / "payload" / Executable();
    options.workDir = options.exe.parent_path();
    options.args = { "--apply-update", staged.version, tui ? "tui" : "gui",
                     Platform::PathToUtf8(fs::absolute(profile)) };
    options.pipes = false;
#ifdef __linux__
    if (tui)
    {
        ReplaceProcess(options.exe, options.args);
        error = Tr("release.stage_failed");
        return false;
    }
#endif
    std::error_code ec;
    fs::remove(staged.directory / "ready", ec);
    if (ec)
    {
        error = Tr("release.stage_failed");
        return false;
    }
    Platform::Child helper;
    if (!helper.Start(options, error))
        return false;
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    do
    {
        if (fs::is_regular_file(staged.directory / "ready", ec))
            return true;
        std::this_thread::sleep_for(50ms);
    } while (helper.Running() && std::chrono::steady_clock::now() < deadline);
    helper.Kill();
    helper.Wait(1000);
    error = Tr("release.helper_failed");
    return false;
}

std::string ReleaseUpdate::TakeResult(fs::path const& root)
{
    fs::path const file = root / ResultFile;
    std::ifstream in(file, std::ios::binary);
    std::string stage, status, detail;
    if (!std::getline(in, stage) || !std::getline(in, status))
        return {};
    std::getline(in, detail);
    in.close();
    std::error_code error;
    bool const recovery = StageName(stage) && status != "ok" && fs::exists(root / stage / "restore-plan", error);
    if (StageName(stage) && !recovery)
        Discard({ root / stage, {} });
    // On Windows the helper can still have its own executable mapped as the new launcher starts.
    if (status != "ok" || !StageName(stage) || !fs::exists(root / stage, error))
        fs::remove(file, error);
    return status == "ok" ? Tr("release.installed", detail) : Tr("release.install_failed", detail);
}

int ReleaseUpdate::Apply(int argc, char** argv)
{
    if (argc != 5 || CurrentVersion() != argv[2] || !Version(argv[2]) ||
        (std::string_view(argv[3]) != "gui" && std::string_view(argv[3]) != "tui"))
        return 1;
    fs::path const exe = Platform::ExePath();
    fs::path const payload = exe.parent_path();
    fs::path const stage = payload.parent_path();
    fs::path const root = stage.parent_path();
    std::string const stageName = Platform::PathToUtf8(stage.filename());
    if (payload.filename() != "payload" || !StageName(stageName) ||
        fs::is_symlink(fs::symlink_status(stage)) || !fs::is_regular_file(root / Executable()) ||
        fs::exists(stage / "restore-plan"))
        return 1;

    bool const tui = std::string_view(argv[3]) == "tui";
    std::string status = "ok";
    std::string detail = argv[2];
    try
    {
        Platform::FileLock updateLock(root / ".lonelyice-update.lock");
        if (!updateLock.Held())
            return 1;
        Write(stage / "ready", {});
        auto const deadline = std::chrono::steady_clock::now() + 20s;
        while (TargetRunning(root / Executable()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(100ms);
        if (TargetRunning(root / Executable()))
        {
            Write(root / ResultFile, stageName + "\nerror\n" + Tr("release.other_instance") + "\n");
            return 1;
        }
        Packages::ServerLock serverLock(root / "plugins");
        if (!serverLock.Held())
            throw std::runtime_error(Tr("release.stop_server"));
        ApplyFiles(root, stage);
        std::error_code ignored;
        fs::remove_all(stage / "previous", ignored);
    }
    catch (std::exception const& e)
    {
        status = "error";
        detail = e.what();
    }

    try
    {
        Write(root / ResultFile, stageName + '\n' + status + '\n' + detail + '\n');
    }
    catch (std::exception const&)
    {
        // The launcher can still be opened if the result message could not be saved.
    }
    Platform::ChildOptions launch;
    launch.exe = root / Executable();
    launch.workDir = root;
    launch.args = { "--settings", argv[4] };
    if (tui)
        launch.args.insert(launch.args.begin(), "--tui");
    launch.pipes = false;
#ifdef __linux__
    if (tui)
        return ReplaceProcess(launch.exe, launch.args) ? 0 : 1;
#endif
    Platform::Child child;
    std::string error;
    return child.Start(launch, error) ? 0 : 1;
}
