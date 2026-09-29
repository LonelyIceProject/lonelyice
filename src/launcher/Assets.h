#ifndef LONELYICE_ASSETS_H
#define LONELYICE_ASSETS_H

#include <RmlUi/Core/FileInterface.h>
#include <string>
#include <string_view>
#include <vector>

namespace LonelyIce
{
    // Embedded assets without RmlUi (also for the server and tool modes); LONELYICE_ASSETS applies as well.
    bool ReadAsset(std::string const& path, std::string& out);
    // Asset paths under a folder ("lang/en/"), sorted.
    std::vector<std::string> ListAssets(std::string_view folder);

    // Serves ui/ and fonts/ from arrays linked into the executable.
    // If LONELYICE_ASSETS points to the assets folder, files are read from disk instead (for editing the UI live).
    class AssetFileInterface : public Rml::FileInterface
    {
    public:
        AssetFileInterface();

        Rml::FileHandle Open(Rml::String const& path) override;
        void Close(Rml::FileHandle file) override;
        size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
        bool Seek(Rml::FileHandle file, long offset, int origin) override;
        size_t Tell(Rml::FileHandle file) override;
        size_t Length(Rml::FileHandle file) override;

    private:
        std::string _diskRoot;
    };
}

#endif
