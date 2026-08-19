/*
 *  LibDegorasASI - An extensible C++ library for controlling ZWO ASI astronomy cameras.
 *
 *  Developed as free software by and for the Spanish Navy Observatory SLR station (SFEL) in San Fernando.
 *
 *  Copyright (C) 2024-2026 Degoras Project Team
 *                          < Ángel Vera Herrera, avera@roa.es - angelvh.engr@gmail.com >
 *                          < Jesús Relinque Madroñal, jrelinque@roa.es >
 *
 *  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
 *  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along with this program. If not, see
 *  <https://www.gnu.org/licenses/>.
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

// C++ INCLUDES
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

// PROJECT INCLUDES
#include "LibDegorasASI/Helpers/frame_writer.h"


// NAMESPACES
namespace dpasi
{
namespace imgio
{

namespace
{

/// The ramp used by framePreview(), from darkest to brightest.
const char* const kPreviewRamp = " .:-=+*#%@";
constexpr int kPreviewRampLen = 10;

/// Whether the frame's geometry and buffer agree, so no writer indexes past the end.
bool isConsistent(const types::Frame& frame)
{
    return !frame.empty() && frame.width > 0 && frame.height > 0 &&
           frame.data.size() >= frame.expectedBytes() && frame.expectedBytes() > 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

bool writeBmp(const types::Frame& frame, const std::string& path)
{
    if (frame.format != types::ImageFormat::RGB24 || !isConsistent(frame))
        return false;

    const std::uint32_t row_bytes = static_cast<std::uint32_t>(frame.width) * 3u;
    const std::uint32_t padding = (4u - (row_bytes % 4u)) % 4u;
    const std::uint32_t image_bytes = (row_bytes + padding) * static_cast<std::uint32_t>(frame.height);
    const std::uint32_t offset = 14u + 40u;                 // file header + info header; no palette at 24 bpp
    const std::uint32_t file_bytes = offset + image_bytes;

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    const auto u16 = [&out](std::uint16_t v)
    {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };
    const auto u32 = [&out](std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.put(static_cast<char>((v >> (8 * i)) & 0xFF));
    };

    // BITMAPFILEHEADER
    out.put('B');
    out.put('M');
    u32(file_bytes);
    u16(0);
    u16(0);
    u32(offset);

    // BITMAPINFOHEADER
    u32(40);
    u32(static_cast<std::uint32_t>(frame.width));
    u32(static_cast<std::uint32_t>(frame.height));
    u16(1);                                                 // planes
    u16(24);                                                // bits per pixel
    u32(0);                                                 // BI_RGB, uncompressed
    u32(image_bytes);
    u32(2835);                                              // ~72 dpi, in pixels per metre
    u32(2835);
    u32(0);                                                 // palette entries used
    u32(0);                                                 // palette entries considered important

    // Rows are stored bottom-up. Note the padding below never actually fires on an ASI frame: the vendor requires a
    // width that is a multiple of 8, so a 24-bpp row is a multiple of 24 bytes and already 4-byte aligned. It stays
    // because a writer that silently corrupted a narrower image would be a trap for whoever reuses this next.
    const std::vector<char> pad(padding, 0);
    for (int y = frame.height - 1; y >= 0; --y)
    {
        const std::size_t row_at = static_cast<std::size_t>(y) * row_bytes;
        out.write(reinterpret_cast<const char*>(frame.data.data() + row_at),
                  static_cast<std::streamsize>(row_bytes));
        if (padding != 0)
            out.write(pad.data(), static_cast<std::streamsize>(padding));
    }
    return out.good();
}

bool writePgm(const types::Frame& frame, const std::string& path)
{
    if (!isConsistent(frame))
        return false;

    const bool sixteen_bit = (frame.format == types::ImageFormat::RAW16);
    if (!sixteen_bit && frame.format != types::ImageFormat::RAW8 && frame.format != types::ImageFormat::Y8)
        return false;

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;

    out << "P5\n" << frame.width << " " << frame.height << "\n" << (sixteen_bit ? 65535 : 255) << "\n";

    const std::size_t bytes = frame.expectedBytes();
    if (!sixteen_bit)
    {
        out.write(reinterpret_cast<const char*>(frame.data.data()), static_cast<std::streamsize>(bytes));
    }
    else
    {
        for (std::size_t i = 0; i + 1 < bytes; i += 2)
        {
            out.put(static_cast<char>(frame.data[i + 1]));   // high byte first: PGM samples are big-endian
            out.put(static_cast<char>(frame.data[i]));
        }
    }
    return out.good();
}

bool writeFrame(const types::Frame& frame, const std::string& path)
{
    return (frame.format == types::ImageFormat::RGB24) ? writeBmp(frame, path) : writePgm(frame, path);
}

std::string extensionFor(types::ImageFormat format)
{
    return (format == types::ImageFormat::RGB24) ? std::string("bmp") : std::string("pgm");
}

std::string framePreview(const types::Frame& frame, int columns)
{
    if (!isConsistent(frame) || columns <= 0)
        return std::string();

    const std::size_t bpp = types::bytesPerPixel(frame.format);
    const bool sixteen_bit = (frame.format == types::ImageFormat::RAW16);

    // Halved because a character cell is roughly twice as tall as it is wide.
    const int rows = (frame.height * columns) / (frame.width * 2);
    if (rows <= 0)
        return std::string();

    std::string out;
    out.reserve(static_cast<std::size_t>(rows) * (static_cast<std::size_t>(columns) + 1));

    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < columns; ++c)
        {
            // Nearest-neighbour sampling; a glance does not warrant averaging.
            const int x = (c * frame.width) / columns;
            const int y = (r * frame.height) / rows;
            const std::size_t at = (static_cast<std::size_t>(y) * frame.width + x) * bpp;
            if (at + bpp > frame.data.size())
                continue;

            int value = 0;
            if (sixteen_bit)
                value = (frame.data[at] | (frame.data[at + 1] << 8)) >> 8;                  // top 8 bits
            else if (bpp == 3)
                value = (frame.data[at] + frame.data[at + 1] + frame.data[at + 2]) / 3;     // B,G,R, close enough
            else
                value = frame.data[at];

            out += kPreviewRamp[(value * (kPreviewRampLen - 1)) / 255];
        }
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
