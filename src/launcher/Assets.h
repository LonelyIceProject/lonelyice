#ifndef LONELYICE_ASSETS_H
#define LONELYICE_ASSETS_H

#include <RmlUi/Core/FileInterface.h>
#include <string>

namespace LonelyIce
{
    // Serves ui/ and fonts/ from resources linked into the exe.
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
