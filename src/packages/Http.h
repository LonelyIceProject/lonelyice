#ifndef LONELYICE_HTTP_H
#define LONELYICE_HTTP_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace LonelyIce::Http
{
    // done, total (0 when unknown); returning false cancels
    using Progress = std::function<bool(uint64_t done, uint64_t total)>;

    // Fetches http(s) URLs, file:// URLs and local paths into memory. Follows redirects.
    bool Get(std::string const& url, std::vector<uint8_t>& out, std::string& error, Progress const& progress = {});

    // url relative to base ("pkg/a.zip" next to "https://host/repo/index.json"); absolute urls pass through.
    std::string Resolve(std::string const& base, std::string const& url);
}

#endif
