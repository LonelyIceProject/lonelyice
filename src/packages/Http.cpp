#include "Http.h"
#include "TextUtil.h"
#include <fstream>
#include <Windows.h>
#include <winhttp.h>

namespace fs = std::filesystem;

namespace
{
    bool IsHttp(std::string const& url)
    {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    }

    fs::path LocalPath(std::string const& url)
    {
        std::string p = url.rfind("file:///", 0) == 0 ? url.substr(8) : url.rfind("file://", 0) == 0 ? url.substr(7) : url;
        return fs::u8path(p);
    }

    struct Handle
    {
        HINTERNET h = nullptr;
        ~Handle() { if (h) WinHttpCloseHandle(h); }
    };
}

std::string LonelyIce::Http::Resolve(std::string const& base, std::string const& url)
{
    if (IsHttp(url) || url.rfind("file://", 0) == 0 || fs::u8path(url).is_absolute())
        return url;
    std::size_t slash = base.find_last_of("/\\");
    return slash == std::string::npos ? url : base.substr(0, slash + 1) + url;
}

bool LonelyIce::Http::Get(std::string const& url, std::vector<uint8_t>& out, std::string& error, Progress const& progress)
{
    out.clear();
    if (!IsHttp(url))
    {
        std::ifstream in(LocalPath(url), std::ios::binary);
        if (!in)
        {
            error = "нет файла " + url;
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }

    std::wstring const wurl = Utf8ToWide(url);
    URL_COMPONENTS parts = { sizeof(parts) };
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
    {
        error = "неверный адрес " + url;
        return false;
    }

    Handle session{ WinHttpOpen(L"LonelyIce", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
    Handle connect{ session.h ? WinHttpConnect(session.h, host, parts.nPort, 0) : nullptr };
    std::wstring const object = std::wstring(path) + extra;
    Handle request{ connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr };
    if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request.h, nullptr))
    {
        error = "нет ответа от " + WideToUtf8(host) + " (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        error = url + ": HTTP " + std::to_string(status);
        return false;
    }
    wchar_t length[32] = {};
    size = sizeof(length);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length, nullptr, 10);

    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available))
        {
            error = "обрыв загрузки " + url;
            return false;
        }
        if (!available)
            break;
        std::size_t const at = out.size();
        out.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out.data() + at, available, &read))
        {
            error = "обрыв загрузки " + url;
            return false;
        }
        out.resize(at + read);
        if (progress && !progress(out.size(), total))
        {
            error = "отменено";
            return false;
        }
    }
    return true;
}
