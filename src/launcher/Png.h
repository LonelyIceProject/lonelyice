#ifndef LONELYICE_PNG_H
#define LONELYICE_PNG_H

#include <cstdint>
#include <vector>

namespace LonelyIce::Png
{
    // Decodes a non-interlaced 8-bit PNG (gray, gray+alpha, RGB, RGBA or palette) into RGBA rows, top row first.
    bool Decode(uint8_t const* data, std::size_t size, std::vector<uint8_t>& rgba, int& width, int& height);
}

#endif
