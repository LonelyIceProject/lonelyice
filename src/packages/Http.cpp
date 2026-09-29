#include "Http.h"
#include "Lang.h"
#include <curl/curl.h>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace
{
    bool IsHttp(std::string const& url)
    {
        return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    }

    fs::path LocalPath(std::string const& url)
    {
#ifdef _WIN32
        // file:///C:/dir -> C:/dir
        std::string p = url.rfind("file:///", 0) == 0 ? url.substr(8) : url.rfind("file://", 0) == 0 ? url.substr(7) : url;
#else
        // file:///home/dir -> /home/dir
        std::string p = url.rfind("file://", 0) == 0 ? url.substr(7) : url;
#endif
        return fs::u8path(p);
    }

    // curl_global_init is not guaranteed to be thread-safe everywhere; a function-local static runs it exactly once.
    bool CurlReady()
    {
        static bool const ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        return ready;
    }

    struct EasyDeleter
    {
        void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
    };

    struct UrlDeleter
    {
        void operator()(CURLU* url) const { curl_url_cleanup(url); }
    };

    struct Transfer
    {
        std::vector<uint8_t>* out = nullptr;
        LonelyIce::Http::Progress const* progress = nullptr;
    };

    size_t OnWrite(char* data, size_t size, size_t count, void* user)
    {
        Transfer& transfer = *static_cast<Transfer*>(user);
        try
        {
            transfer.out->insert(transfer.out->end(), data, data + size * count);
        }
        catch (...)
        {
            return 0; // out of memory: curl reports a write error
        }
        return size * count;
    }

    int OnProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
    {
        Transfer& transfer = *static_cast<Transfer*>(user);
        if (!*transfer.progress)
            return 0;
        // Non-zero aborts the transfer with CURLE_ABORTED_BY_CALLBACK.
        return (*transfer.progress)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal)) ? 0 : 1;
    }

    std::string HostOf(std::string const& url)
    {
        std::unique_ptr<CURLU, UrlDeleter> parsed(curl_url());
        char* host = nullptr;
        if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK
            || curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0) != CURLUE_OK)
            return {};
        std::string result = host;
        curl_free(host);
        return result;
    }
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
            error = Tr("pkg.http.no_file", url);
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }

    std::string const host = CurlReady() ? HostOf(url) : std::string();
    if (host.empty())
    {
        error = Tr("pkg.http.bad_url", url);
        return false;
    }
    std::unique_ptr<CURL, EasyDeleter> curl(curl_easy_init());
    if (!curl)
    {
        error = Tr("pkg.http.no_response", host, curl_easy_strerror(CURLE_FAILED_INIT));
        return false;
    }

    Transfer transfer{ &out, &progress };
    char details[CURL_ERROR_SIZE] = {};
    CURL* const c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "LonelyIce");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    // Give up when less than 1 byte/s arrives for a minute.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    // Peer and host verification stay on (curl's default). Schannel checks revocation, which fails outright when the
    // CRL/OCSP servers are unreachable; best effort keeps the check without that failure. Other TLS backends ignore it.
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, details);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, OnWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, progress ? 0L : 1L);

    CURLcode const result = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK)
    {
        out.clear();
        if (result == CURLE_ABORTED_BY_CALLBACK)
            error = Tr("pkg.http.cancelled");
        else if (result == CURLE_URL_MALFORMAT || result == CURLE_UNSUPPORTED_PROTOCOL)
            error = Tr("pkg.http.bad_url", url);
        else if (status == 0)
            error = Tr("pkg.http.no_response", host, details[0] ? details : curl_easy_strerror(result));
        else
            error = Tr("pkg.http.interrupted", url);
        return false;
    }
    if (status >= 400)
    {
        out.clear();
        error = Tr("pkg.http.status", url, status);
        return false;
    }
    return true;
}
