#include "Png.h"
#include <cstdlib>
#include <cstring>
#include <miniz.h>

namespace
{
    uint32_t Be32(uint8_t const* p)
    {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }

    uint8_t Paeth(int a, int b, int c)
    {
        int const p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
    }
}

bool LonelyIce::Png::Decode(uint8_t const* data, std::size_t size, std::vector<uint8_t>& rgba, int& width, int& height)
{
    static uint8_t const signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (size < 8 || std::memcmp(data, signature, 8) != 0)
        return false;

    uint32_t w = 0, h = 0;
    int depth = 0, color = -1, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (std::size_t pos = 8; pos + 12 <= size;)
    {
        uint32_t const len = Be32(data + pos);
        uint8_t const* type = data + pos + 4;
        uint8_t const* body = data + pos + 8;
        if (len > size - pos - 12)
            return false;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13)
        {
            w = Be32(body);
            h = Be32(body + 4);
            depth = body[8];
            color = body[9];
            interlace = body[12];
        }
        else if (!std::memcmp(type, "PLTE", 4))
            palette.assign(body, body + len);
        else if (!std::memcmp(type, "tRNS", 4))
            trns.assign(body, body + len);
        else if (!std::memcmp(type, "IDAT", 4))
            idat.insert(idat.end(), body, body + len);
        else if (!std::memcmp(type, "IEND", 4))
            break;
        pos += 12 + len;
    }

    int channels = 0;
    switch (color)
    {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return false;
    }
    if (depth != 8 || interlace != 0 || !w || !h || w > 4096 || h > 4096 || (color == 3 && palette.empty()))
        return false;

    std::size_t const stride = std::size_t(w) * channels;
    std::vector<uint8_t> raw((stride + 1) * h);
    mz_ulong rawSize = mz_ulong(raw.size());
    if (mz_uncompress(raw.data(), &rawSize, idat.data(), mz_ulong(idat.size())) != MZ_OK || rawSize != raw.size())
        return false;

    // Undo the per-row filters in place.
    std::vector<uint8_t> prev(stride, 0);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* row = raw.data() + y * (stride + 1);
        uint8_t const filter = row[0];
        uint8_t* px = row + 1;
        for (std::size_t i = 0; i < stride; ++i)
        {
            int const a = i >= std::size_t(channels) ? px[i - channels] : 0, b = prev[i], c = i >= std::size_t(channels) ? prev[i - channels] : 0;
            switch (filter)
            {
                case 0: break;
                case 1: px[i] = uint8_t(px[i] + a); break;
                case 2: px[i] = uint8_t(px[i] + b); break;
                case 3: px[i] = uint8_t(px[i] + ((a + b) >> 1)); break;
                case 4: px[i] = uint8_t(px[i] + Paeth(a, b, c)); break;
                default: return false;
            }
        }
        std::memcpy(prev.data(), px, stride);
    }

    width = int(w);
    height = int(h);
    rgba.resize(std::size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t const* px = raw.data() + y * (stride + 1) + 1;
        uint8_t* out = rgba.data() + std::size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x, out += 4, px += channels)
        {
            switch (color)
            {
                case 0: out[0] = out[1] = out[2] = px[0]; out[3] = 255; break;
                case 4: out[0] = out[1] = out[2] = px[0]; out[3] = px[1]; break;
                case 2: out[0] = px[0]; out[1] = px[1]; out[2] = px[2]; out[3] = 255; break;
                case 6: std::memcpy(out, px, 4); break;
                case 3:
                {
                    std::size_t const i = px[0];
                    if (i * 3 + 2 >= palette.size())
                        return false;
                    out[0] = palette[i * 3];
                    out[1] = palette[i * 3 + 1];
                    out[2] = palette[i * 3 + 2];
                    out[3] = i < trns.size() ? trns[i] : 255;
                    break;
                }
            }
        }
    }
    return true;
}
