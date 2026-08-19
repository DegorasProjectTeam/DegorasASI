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
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
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

    // Closed explicitly: out.good() before the destructor runs would report success while the last block was still
    // buffered, so a disk that filled up mid-write would look like a clean save.
    out.close();
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

    out.close();   // see writeBmp: report only what actually reached the disk
    return out.good();
}

// -- FITS ------------------------------------------------------------------------------------------------------------

namespace
{

constexpr std::size_t kFitsCardLen = 80;     // every header card is exactly 80 characters
constexpr std::size_t kFitsBlockLen = 2880;  // and the file is a whole number of 2880-byte blocks

/// Format one card the way the standard lays it out: keyword in columns 1-8, "= " in 9-10, value from column 11.
std::string fitsCardText(const FitsCard& card)
{
    std::string key = card.key.substr(0, 8);
    for (char& c : key)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    key.resize(8, ' ');

    std::string text = key + "= ";
    // Numbers are right-justified through column 30, text is left-justified from column 11: both are what readers
    // expect, and misaligning them is the classic way to produce a file that some tools reject.
    const bool is_text = !card.value.empty() && card.value.front() == '\'';
    if (is_text)
        text += card.value;
    else
        text += std::string(card.value.size() < 20 ? 20 - card.value.size() : 0, ' ') + card.value;

    if (!card.comment.empty() && text.size() + 3 < kFitsCardLen)
        text += " / " + card.comment;

    text.resize(kFitsCardLen, ' ');   // truncates an over-long comment, which is the harmless half of the card
    return text;
}

/// A FITS text value: single-quoted, padded to at least 8 characters, with embedded quotes doubled.
std::string fitsQuote(const std::string& value)
{
    std::string escaped;
    for (const char c : value)
    {
        escaped += c;
        if (c == '\'')
            escaped += c;
    }
    if (escaped.size() < 8)
        escaped.resize(8, ' ');
    return "'" + escaped + "'";
}

/// The frame's timestamp as the FITS DATE-OBS form, yyyy-mm-ddThh:mm:ss.sss in UTC.
std::string fitsDateObs(const std::chrono::system_clock::time_point& stamp)
{
    const std::time_t secs = std::chrono::system_clock::to_time_t(stamp);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    const auto since = stamp.time_since_epoch();
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(since).count() % 1000;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
    return std::string(buf);
}

} // namespace

FitsCard fitsInt(const std::string& key, long long value, const std::string& comment)
{
    return FitsCard{key, std::to_string(value), comment};
}

FitsCard fitsReal(const std::string& key, double value, const std::string& comment)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10G", value);
    return FitsCard{key, std::string(buf), comment};
}

FitsCard fitsText(const std::string& key, const std::string& value, const std::string& comment)
{
    return FitsCard{key, fitsQuote(value), comment};
}

bool writeFits(const types::Frame& frame, const std::string& path, const FitsCards& extra)
{
    if (!isConsistent(frame))
        return false;

    const bool sixteen_bit = (frame.format == types::ImageFormat::RAW16);
    const bool colour = (frame.format == types::ImageFormat::RGB24);
    if (!sixteen_bit && !colour && frame.format != types::ImageFormat::RAW8 &&
        frame.format != types::ImageFormat::Y8)
        return false;

    // -- Header --------------------------------------------------------------------------------------------------
    // The mandatory cards come first and in this order; anything else may follow, and END closes the header.
    FitsCards cards;
    cards.push_back(FitsCard{"SIMPLE", "T", "conforms to FITS standard"});
    cards.push_back(fitsInt("BITPIX", sixteen_bit ? 16 : 8, "bits per sample"));
    cards.push_back(fitsInt("NAXIS", colour ? 3 : 2, "number of axes"));
    cards.push_back(fitsInt("NAXIS1", frame.width, "image width"));
    cards.push_back(fitsInt("NAXIS2", frame.height, "image height"));
    if (colour)
        cards.push_back(fitsInt("NAXIS3", 3, "colour planes, in R G B order"));

    if (sixteen_bit)
    {
        // BITPIX 16 is SIGNED in FITS while the sensor data is unsigned, so the samples are stored biased and the
        // reader undoes it. Every FITS reader applies BZERO, so the values come back exactly as captured.
        cards.push_back(fitsInt("BZERO", 32768, "unsigned samples stored as signed"));
        cards.push_back(fitsInt("BSCALE", 1, ""));
    }

    cards.push_back(fitsText("DATE-OBS", fitsDateObs(frame.timestamp), "UTC at frame retrieval"));
    if (frame.bin > 1)
    {
        cards.push_back(fitsInt("XBINNING", frame.bin, "horizontal binning"));
        cards.push_back(fitsInt("YBINNING", frame.bin, "vertical binning"));
    }
    cards.push_back(fitsText("CREATOR", "LibDegorasASI", "writing software"));
    cards.insert(cards.end(), extra.begin(), extra.end());

    std::string header;
    for (const FitsCard& card : cards)
        header += fitsCardText(card);
    header += std::string("END") + std::string(kFitsCardLen - 3, ' ');
    header.resize(((header.size() + kFitsBlockLen - 1) / kFitsBlockLen) * kFitsBlockLen, ' ');

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(header.data(), static_cast<std::streamsize>(header.size()));

    // -- Data ----------------------------------------------------------------------------------------------------
    // Big-endian, first axis varying fastest, rows BOTTOM-UP. Colour is stored plane by plane, not interleaved, so
    // the SDK's interleaved B,G,R has to be taken apart and emitted as R, then G, then B.
    std::string data;
    const std::size_t sample_bytes = sixteen_bit ? 2u : 1u;
    data.reserve(static_cast<std::size_t>(frame.width) * frame.height * (colour ? 3u : 1u) * sample_bytes);

    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * types::bytesPerPixel(frame.format);

    if (colour)
    {
        for (int plane = 0; plane < 3; ++plane)
        {
            const std::size_t channel = static_cast<std::size_t>(2 - plane);   // R,G,B out of B,G,R
            for (int y = frame.height - 1; y >= 0; --y)
            {
                const std::size_t row_at = static_cast<std::size_t>(y) * row_bytes;
                for (int x = 0; x < frame.width; ++x)
                    data += static_cast<char>(frame.data[row_at + static_cast<std::size_t>(x) * 3u + channel]);
            }
        }
    }
    else
    {
        for (int y = frame.height - 1; y >= 0; --y)
        {
            const std::size_t row_at = static_cast<std::size_t>(y) * row_bytes;
            if (!sixteen_bit)
            {
                data.append(reinterpret_cast<const char*>(frame.data.data() + row_at), row_bytes);
            }
            else
            {
                for (int x = 0; x < frame.width; ++x)
                {
                    const std::size_t at = row_at + static_cast<std::size_t>(x) * 2u;
                    const int biased = static_cast<int>(frame.data[at] | (frame.data[at + 1] << 8)) - 32768;
                    data += static_cast<char>((biased >> 8) & 0xFF);   // big-endian, high byte first
                    data += static_cast<char>(biased & 0xFF);
                }
            }
        }
    }

    const std::size_t tail = data.size() % kFitsBlockLen;
    if (tail != 0)
        data.resize(data.size() + (kFitsBlockLen - tail), '\0');   // blocks are padded with zeros, headers with spaces

    out.write(data.data(), static_cast<std::streamsize>(data.size()));

    out.close();   // see writeBmp: report only what actually reached the disk
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
