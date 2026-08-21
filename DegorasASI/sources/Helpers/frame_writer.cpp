/*
 *  DegorasASI - An extensible C++ library for controlling ZWO ASI astronomy cameras.
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
#include "DegorasASI/Helpers/frame_writer.h"


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

/// Width of the map produced by the one-argument framePreview(): wide enough to make out a star field, narrow enough
/// for any terminal.
constexpr int kDefaultPreviewColumns = 64;

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

/// Which photosite of the 2x2 mosaic cell an axis starts on. Written as a loop-free positive modulo because
/// Frame is a plain struct a caller may fill by hand, and C++ gives a NEGATIVE remainder for a negative left
/// operand -- an XBAYROFF of -1 would be rejected or misread rather than merely wrong.
int mosaicOffset(int start)
{
    return ((start % 2) + 2) % 2;
}

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

FitsCard fitsText(const std::string& key, const std::string& value)
{
    return fitsText(key, value, std::string());
}

FitsCard fitsText(const std::string& key, const std::string& value, const std::string& comment)
{
    return FitsCard{key, fitsQuote(value), comment};
}

FitsCard fitsBayerPattern(types::BayerPattern pattern)
{
    // The four names spelled out, NOT built by appending to toString(pattern). The SDK names a pattern by its first
    // ROW ("RG", "BG", ...) while FITS names it by the whole 2x2 CELL, and the second row is not the same for all
    // four: only RG happens to be completed by "GB". Appending it blindly yields BGGB, GRGB and GBGB, none of which
    // are Bayer patterns at all -- a reader meeting one falls back to its own guess, which is how a mosaic error
    // reaches a file that looks correctly labelled.
    const char* name = "RGGB";
    switch (pattern)
    {
        case types::BayerPattern::RG: name = "RGGB"; break;
        case types::BayerPattern::BG: name = "BGGR"; break;
        case types::BayerPattern::GR: name = "GRBG"; break;
        case types::BayerPattern::GB: name = "GBRG"; break;
    }
    return fitsText("BAYERPAT", name, "colour filter array, sensor top-left");
}

bool writeFits(const types::Frame& frame, const std::string& path)
{
    return writeFits(frame, path, FitsCards());
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
    // Samples are written as 16-bit ALWAYS, including for an 8-bit frame. BITPIX 8 is perfectly legal, and astropy
    // accepts it in strict mode, but 8-bit FITS is rare in astronomy and widely unimplemented -- ZWO's own ASIStudio
    // rejects it outright with "8 bits not supported". A file that is standards-correct but that the observatory's
    // tools cannot open is of no use, and an 8-bit value fits a 16-bit sample exactly, so nothing is lost.
    //
    // Values are NOT rescaled to fill the 16-bit range: they stay the sensor's own ADU, which is what photometry
    // needs. DATAMIN/DATAMAX below tell a viewer the real range so it can stretch the display sensibly.
    std::uint16_t data_min = 0xFFFF;
    std::uint16_t data_max = 0;
    {
        const std::size_t samples = frame.expectedBytes() / (sixteen_bit ? 2u : 1u);
        for (std::size_t i = 0; i < samples; ++i)
        {
            const std::uint16_t v = sixteen_bit
                ? static_cast<std::uint16_t>(frame.data[i * 2] | (frame.data[i * 2 + 1] << 8))
                : static_cast<std::uint16_t>(frame.data[i]);
            if (v < data_min) data_min = v;
            if (v > data_max) data_max = v;
        }
    }

    FitsCards cards;
    cards.push_back(FitsCard{"SIMPLE", "T", "conforms to FITS standard"});
    cards.push_back(fitsInt("BITPIX", 16, "bits per sample"));
    cards.push_back(fitsInt("NAXIS", colour ? 3 : 2, "number of axes"));
    cards.push_back(fitsInt("NAXIS1", frame.width, "image width"));
    cards.push_back(fitsInt("NAXIS2", frame.height, "image height"));
    if (colour)
        cards.push_back(fitsInt("NAXIS3", 3, "colour planes, in R G B order"));

    // BITPIX 16 is SIGNED in FITS while the sensor data is unsigned, so the samples are stored biased and the reader
    // undoes it. Every FITS reader applies BZERO, so the values come back exactly as captured.
    cards.push_back(fitsInt("BZERO", 32768, "unsigned samples stored as signed"));
    cards.push_back(fitsInt("BSCALE", 1, ""));
    cards.push_back(fitsInt("DATAMIN", data_min, "smallest sample present"));
    cards.push_back(fitsInt("DATAMAX", data_max, "largest sample present"));

    cards.push_back(fitsText("DATE-OBS", fitsDateObs(frame.timestamp), "UTC at frame retrieval"));
    if (frame.bin > 1)
    {
        cards.push_back(fitsInt("XBINNING", frame.bin, "horizontal binning"));
        cards.push_back(fitsInt("YBINNING", frame.bin, "vertical binning"));
    }
    cards.push_back(fitsText("CREATOR", "DegorasASI", "writing software"));

    // Says which end of the sensor the first stored row came from, so a reader can place the image the right way up
    // AND work out the phase of the colour mosaic. Without it the two are unrecoverable: see the block above the data
    // loop for why this is the single most important card in the file for a colour camera.
    cards.push_back(fitsText("ROWORDER", "TOP-DOWN", "first stored row is the sensor top row"));

    // A BAYERPAT on a binned frame is a lie the writer refuses to tell. Binning sums neighbouring photosites, which
    // destroys the mosaic: the result is a grey image, and a reader that believes the card will demosaic noise into
    // false colour. The caller cannot be trusted to remember this, and the frame knows its own binning, so the card
    // is dropped here rather than in every caller. Same for RGB24, which the SDK has already demosaiced.
    const bool mosaic_gone = (frame.bin > 1) || colour;
    bool wrote_mosaic = false;
    for (const FitsCard& card : extra)
    {
        // Matched the way the card will actually be WRITTEN -- truncated to eight columns and upper-cased, exactly as
        // fitsCardText() does it. Comparing the caller's spelling instead would let "bayerpat" slip through the guard
        // and still land in the file as BAYERPAT.
        std::string key = card.key.substr(0, 8);
        for (char& c : key)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (mosaic_gone && key == "BAYERPAT")
            continue;

        // The offsets are never taken from the caller: the frame carries its own origin, and a stale or guessed
        // offset is worse than none. They are re-derived below from what the frame actually says.
        if (key == "XBAYROFF" || key == "YBAYROFF")
            continue;

        cards.push_back(card);
        wrote_mosaic = wrote_mosaic || (key == "BAYERPAT");
    }

    // A windowed capture whose origin is ODD starts on a different photosite of the 2x2 cell, which shifts the mosaic
    // exactly as reversing the rows would. The vendor accepts an odd origin without complaint and says nothing about
    // it, so the only safe assumption is none: derive the offset from the frame and let the reader apply it. Written
    // only alongside a BAYERPAT, since without a mosaic the cards mean nothing.
    if (wrote_mosaic)
    {
        cards.push_back(fitsInt("XBAYROFF", mosaicOffset(frame.start_x), "mosaic column offset of the first pixel"));
        cards.push_back(fitsInt("YBAYROFF", mosaicOffset(frame.start_y), "mosaic row offset of the first pixel"));
    }

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
    // Big-endian, first axis varying fastest, rows TOP-DOWN -- stored exactly as the sensor delivers them, and
    // declared as such by the ROWORDER card above. Colour is stored plane by plane, not interleaved, so the SDK's
    // interleaved B,G,R has to be taken apart and emitted as R, then G, then B.
    //
    // TOP-DOWN rather than the bottom-up "first pixel at lower left" that FITS is famous for, and this cost us a bug
    // worth recording. That rule is NOT in the standard: FITS 4.0 mandates only that axis 1 varies fastest, and the
    // lower-left recommendation comes from WCS Paper I section 5.1, which calls it "a convention of convenience".
    // What the standard leaves open, practice has closed the other way -- every reader checked (Siril, PixInsight,
    // DeepSkyStacker, ASTAP, KStars) assumes TOP-DOWN when ROWORDER is absent, because that is what camera drivers
    // emit, and ZWO's own ASIStudio does not implement ROWORDER at all.
    //
    // On a colour camera this is not cosmetic. Reversing the rows shifts the Bayer mosaic by one row whenever the
    // height is even -- which is always, since isRoiAligned() requires it -- turning a native RGGB sensor into GBRG
    // in the file. Declare RGGB over that and a reader fills its red channel from green photosites: measured on an
    // ASI224MC, a red source came out green. Storing the rows as the sensor sends them makes the declared pattern
    // true by construction, with no dependence on the height being even and none on ROWORDER surviving downstream
    // (ASTAP strips it on purpose). The cost is that Siril, which always renders bottom-up, displays the frame
    // inverted -- as it does for every INDI, N.I.N.A. and SharpCap file, for the same reason.
    std::string data;
    data.reserve(static_cast<std::size_t>(frame.width) * frame.height * (colour ? 3u : 1u) * 2u);

    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * types::bytesPerPixel(frame.format);

    // Every sample goes out as a biased, big-endian 16-bit word, whatever width it arrived in.
    const auto emit = [&data](std::uint16_t value)
    {
        const int biased = static_cast<int>(value) - 32768;
        data += static_cast<char>((biased >> 8) & 0xFF);
        data += static_cast<char>(biased & 0xFF);
    };

    if (colour)
    {
        for (int plane = 0; plane < 3; ++plane)
        {
            const std::size_t channel = static_cast<std::size_t>(2 - plane);   // R,G,B out of B,G,R
            for (int y = 0; y < frame.height; ++y)
            {
                const std::size_t row_at = static_cast<std::size_t>(y) * row_bytes;
                for (int x = 0; x < frame.width; ++x)
                    emit(frame.data[row_at + static_cast<std::size_t>(x) * 3u + channel]);
            }
        }
    }
    else
    {
        for (int y = 0; y < frame.height; ++y)
        {
            const std::size_t row_at = static_cast<std::size_t>(y) * row_bytes;
            for (int x = 0; x < frame.width; ++x)
            {
                if (sixteen_bit)
                {
                    const std::size_t at = row_at + static_cast<std::size_t>(x) * 2u;
                    emit(static_cast<std::uint16_t>(frame.data[at] | (frame.data[at + 1] << 8)));
                }
                else
                {
                    emit(frame.data[row_at + static_cast<std::size_t>(x)]);
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

std::string framePreview(const types::Frame& frame)
{
    return framePreview(frame, kDefaultPreviewColumns);
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
