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
// ASSERTIONS MUST BE LIVE IN EVERY BUILD TYPE.
//
// This suite is assert-based and has no framework, so assert() IS the test. Release defines NDEBUG, which makes
// assert() expand to nothing and discards the WHOLE expression -- including any call inside it. Measured on this
// suite before the fix: objdump found zero references to _assert in all eleven Release objects, so the entire
// assertion layer was dead, and 610 of the 716 assert sites wrap a call whose side effect went with it. The visible
// symptoms were a suite that passed while doing nothing (Test_AsiCameraCapture reported "serial: (empty), 0
// controls, sensor 0x0" and still declared success, because assert(camera.doConnect() == ...) never opened the
// camera) and two tests that hung outright (UT_FramePump, Test_FrameCallback: assert(pump.start(...)) never started
// the pump, so a later loop waited on a counter that could not advance).
//
// BOTH LINES ARE REQUIRED, in this order, above the first #include. The bare #undef is NOT enough: if anything has
// already pulled in <cassert> while NDEBUG was defined, assert is already expanded away and stays dead. Re-including
// the header re-arms it, because assert.h does #undef assert and redefines the macro on every inclusion and
// <cassert> deliberately has no include guard. Verified by compiling both forms with -O3 -DNDEBUG: the two-line
// form fires, the bare #undef placed after an earlier <cassert> produces zero _assert references.
#undef NDEBUG
#include <cassert>


// C++ INCLUDES
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// PROJECT INCLUDES (module aggregators)
#include <DegorasASI/Modules/Common>
#include <DegorasASI/Modules/Helpers>


using namespace dpasi;
using namespace dpasi::types;

// ---------------------------------------------------------------------------------------------------------------------
// No-hardware self-check for the frame writers. A SYNTHETIC frame is better than a captured one here: the bytes are
// known in advance, so the files can be verified exactly rather than merely "looking plausible" -- and a width can be
// chosen that forces BMP row padding, which a real ASI frame never does (the vendor requires a width that is a
// multiple of 8, so a 24-bpp row is always 4-byte aligned already).
// ---------------------------------------------------------------------------------------------------------------------

namespace
{

std::vector<std::uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t le32(const std::vector<std::uint8_t>& d, std::size_t at)
{
    return std::uint32_t(d[at]) | (std::uint32_t(d[at + 1]) << 8) |
           (std::uint32_t(d[at + 2]) << 16) | (std::uint32_t(d[at + 3]) << 24);
}

std::uint16_t le16(const std::vector<std::uint8_t>& d, std::size_t at)
{
    return static_cast<std::uint16_t>(std::uint16_t(d[at]) | (std::uint16_t(d[at + 1]) << 8));
}

/// A frame whose every byte is a known function of its position, so any reordering shows up.
Frame makeFrame(ImageFormat format, int w, int h)
{
    Frame frame;
    frame.format = format;
    frame.width = w;
    frame.height = h;
    frame.bin = 1;
    frame.data.resize(frame.expectedBytes());
    for (std::size_t i = 0; i < frame.data.size(); ++i)
        frame.data[i] = static_cast<PixelByte>(i % 251);   // a prime stride, so patterns do not align with rows
    return frame;
}

void testBmpHeaderAndPixels()
{
    std::cout << "  BMP header and pixel order\n";

    const int w = 8, h = 4;
    const Frame frame = makeFrame(ImageFormat::RGB24, w, h);
    const std::string path = "ut_frame_writer_rgb24.bmp";
    assert(imgio::writeBmp(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    const std::uint32_t row = static_cast<std::uint32_t>(w) * 3u;
    const std::uint32_t pad = (4u - (row % 4u)) % 4u;
    const std::uint32_t image_bytes = (row + pad) * static_cast<std::uint32_t>(h);

    assert(d.size() == 54u + image_bytes);
    assert(d[0] == 'B' && d[1] == 'M');
    assert(le32(d, 2) == d.size());              // declared file size matches reality
    assert(le32(d, 10) == 54u);                  // pixel data offset
    assert(le32(d, 14) == 40u);                  // info header size
    assert(static_cast<int>(le32(d, 18)) == w);
    assert(static_cast<int>(le32(d, 22)) == h);
    assert(le16(d, 26) == 1);                    // planes
    assert(le16(d, 28) == 24);                   // bits per pixel
    assert(le32(d, 30) == 0);                    // BI_RGB, uncompressed
    assert(le32(d, 34) == image_bytes);

    // Rows are BOTTOM-UP: the first row in the file must be the LAST row of the frame, byte for byte. The bytes are
    // copied verbatim, which is what makes the ASI B,G,R order land correctly in a BMP without a swap.
    for (std::uint32_t x = 0; x < row; ++x)
    {
        assert(d[54u + x] == frame.data[static_cast<std::size_t>(h - 1) * row + x]);
        assert(d[54u + (row + pad) * (h - 1) + x] == frame.data[x]);   // last file row == first frame row
    }
    std::remove(path.c_str());
}

void testBmpPaddingPath()
{
    std::cout << "  BMP row padding (unreachable with ASI geometry, still correct)\n";

    // Width 5 gives a 15-byte row, so 1 byte of padding per row. No ASI camera can produce this -- width must be a
    // multiple of 8 -- but the writer is a public utility and must not corrupt anything narrower.
    const int w = 5, h = 3;
    const Frame frame = makeFrame(ImageFormat::RGB24, w, h);
    const std::string path = "ut_frame_writer_pad.bmp";
    assert(imgio::writeBmp(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    const std::uint32_t row = 15u, pad = 1u;
    assert(pad == (4u - (row % 4u)) % 4u);
    assert(d.size() == 54u + (row + pad) * static_cast<std::uint32_t>(h));
    assert(le32(d, 34) == (row + pad) * static_cast<std::uint32_t>(h));

    for (int y = 0; y < h; ++y)
    {
        const std::size_t file_row = 54u + static_cast<std::size_t>(row + pad) * y;
        const std::size_t frame_row = static_cast<std::size_t>(h - 1 - y) * row;
        for (std::uint32_t x = 0; x < row; ++x)
            assert(d[file_row + x] == frame.data[frame_row + x]);
        assert(d[file_row + row] == 0);          // the pad byte must be zero, not stale data
    }
    std::remove(path.c_str());
}

void testPgm8Bit()
{
    std::cout << "  PGM 8-bit\n";

    for (const ImageFormat format : {ImageFormat::RAW8, ImageFormat::Y8})
    {
        const int w = 8, h = 4;
        const Frame frame = makeFrame(format, w, h);
        const std::string path = "ut_frame_writer_8.pgm";
        assert(imgio::writePgm(frame, path));

        const std::vector<std::uint8_t> d = readFile(path);
        const std::string header = "P5\n8 4\n255\n";
        assert(d.size() == header.size() + frame.data.size());
        assert(std::string(d.begin(), d.begin() + header.size()) == header);

        // 8-bit samples go out untouched and in order: no swap, no shift, no row reversal.
        for (std::size_t i = 0; i < frame.data.size(); ++i)
            assert(d[header.size() + i] == frame.data[i]);
        std::remove(path.c_str());
    }
}

void testPgm16BitIsByteSwapped()
{
    std::cout << "  PGM 16-bit is byte-swapped to big-endian\n";

    const int w = 8, h = 2;
    const Frame frame = makeFrame(ImageFormat::RAW16, w, h);
    const std::string path = "ut_frame_writer_16.pgm";
    assert(imgio::writePgm(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    const std::string header = "P5\n8 2\n65535\n";
    assert(d.size() == header.size() + frame.data.size());
    assert(std::string(d.begin(), d.begin() + header.size()) == header);

    // The SDK delivers little-endian; PGM demands big-endian. Each pair must appear swapped -- and the VALUES must be
    // unchanged, because RAW16 already spans the full 16-bit range and shifting it would darken the image 16-fold.
    for (std::size_t i = 0; i + 1 < frame.data.size(); i += 2)
    {
        assert(d[header.size() + i]     == frame.data[i + 1]);
        assert(d[header.size() + i + 1] == frame.data[i]);
    }
    std::remove(path.c_str());
}

void testWrongFormatsAreRefused()
{
    std::cout << "  mismatched formats and empty frames are refused\n";

    // Each writer takes only what it can represent, rather than producing a file that opens but is wrong.
    assert(!imgio::writeBmp(makeFrame(ImageFormat::RAW8, 8, 4), "ut_should_not_exist.bmp"));
    assert(!imgio::writeBmp(makeFrame(ImageFormat::RAW16, 8, 4), "ut_should_not_exist.bmp"));
    assert(!imgio::writePgm(makeFrame(ImageFormat::RGB24, 8, 4), "ut_should_not_exist.pgm"));

    const Frame empty;
    assert(!imgio::writeBmp(empty, "ut_should_not_exist.bmp"));
    assert(!imgio::writePgm(empty, "ut_should_not_exist.pgm"));
    assert(!imgio::writeFrame(empty, "ut_should_not_exist.bmp"));

    // A frame whose buffer is shorter than its geometry claims must not be read past the end.
    Frame truncated = makeFrame(ImageFormat::RGB24, 8, 4);
    truncated.data.resize(truncated.data.size() / 2);
    assert(!imgio::writeBmp(truncated, "ut_should_not_exist.bmp"));
    assert(imgio::framePreview(truncated).empty());

    assert(readFile("ut_should_not_exist.bmp").empty());
    assert(readFile("ut_should_not_exist.pgm").empty());
}

void testWriteFrameDispatch()
{
    std::cout << "  writeFrame dispatches on format\n";

    assert(imgio::extensionFor(ImageFormat::RGB24) == "bmp");
    assert(imgio::extensionFor(ImageFormat::RAW8) == "pgm");
    assert(imgio::extensionFor(ImageFormat::RAW16) == "pgm");
    assert(imgio::extensionFor(ImageFormat::Y8) == "pgm");

    const std::string bmp = "ut_dispatch.bmp";
    const std::string pgm = "ut_dispatch.pgm";
    assert(imgio::writeFrame(makeFrame(ImageFormat::RGB24, 8, 4), bmp));
    assert(imgio::writeFrame(makeFrame(ImageFormat::RAW16, 8, 4), pgm));
    assert(readFile(bmp)[0] == 'B');
    assert(readFile(pgm)[0] == 'P');
    std::remove(bmp.c_str());
    std::remove(pgm.c_str());
}

/// Read a FITS keyword's raw value text from the 80-column cards, or an empty string if absent.
std::string fitsValue(const std::vector<std::uint8_t>& d, const std::string& key)
{
    for (std::size_t at = 0; at + 80 <= d.size(); at += 80)
    {
        const std::string card(d.begin() + at, d.begin() + at + 80);
        if (card.compare(0, 3, "END") == 0)
            break;
        std::string name = card.substr(0, 8);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name != key)
            continue;
        // A text value ends at its closing quote, NOT at column 30: the comment starts right after it. Slicing a fixed
        // 20 columns would drag part of the comment in.
        const std::string rest = card.substr(10);
        const std::size_t begin = rest.find_first_not_of(' ');
        if (begin == std::string::npos)
            return std::string();
        if (rest[begin] == '\'')
        {
            const std::size_t close = rest.find('\'', begin + 1);
            return (close == std::string::npos) ? std::string() : rest.substr(begin, close - begin + 1);
        }
        std::string value = rest.substr(begin, rest.find(" /", begin) - begin);
        const std::size_t last = value.find_last_not_of(' ');
        return (last == std::string::npos) ? std::string() : value.substr(0, last + 1);
    }
    return std::string();
}

/// Undo what a FITS reader does to a sample: big-endian signed, plus BZERO.
int fitsSample(const std::vector<std::uint8_t>& d, std::size_t at)
{
    return static_cast<std::int16_t>((d[at] << 8) | d[at + 1]) + 32768;
}

std::size_t fitsHeaderBytes(const std::vector<std::uint8_t>& d)
{
    for (std::size_t at = 0; at + 80 <= d.size(); at += 80)
        if (std::string(d.begin() + at, d.begin() + at + 3) == "END")
            return ((at + 80 + 2879) / 2880) * 2880;
    return 0;
}

void testFitsStructure()
{
    std::cout << "  FITS block structure and mandatory cards\n";

    const Frame frame = makeFrame(ImageFormat::RAW8, 16, 8);
    const std::string path = "ut_frame_writer.fits";
    assert(imgio::writeFits(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);

    // The whole file is a whole number of 2880-byte blocks: header AND data. A file that is not is simply not FITS.
    assert(d.size() % 2880 == 0);
    const std::size_t header_bytes = fitsHeaderBytes(d);
    assert(header_bytes > 0 && header_bytes % 2880 == 0);

    // Every card is exactly 80 columns, and the mandatory ones come first and in order.
    assert(fitsValue(d, "SIMPLE") == "T");
    // 16-bit ALWAYS, even for an 8-bit frame: BITPIX 8 is legal but widely unimplemented, and ZWO's own ASIStudio
    // refuses it outright. An 8-bit value fits a 16-bit sample exactly, so promoting it loses nothing.
    assert(fitsValue(d, "BITPIX") == "16");
    assert(fitsValue(d, "NAXIS") == "2");
    assert(fitsValue(d, "NAXIS1") == "16");
    assert(fitsValue(d, "NAXIS2") == "8");
    assert(fitsValue(d, "BZERO") == "32768");
    assert(!fitsValue(d, "DATAMIN").empty());
    assert(!fitsValue(d, "DATAMAX").empty());
    assert(!fitsValue(d, "DATE-OBS").empty());

    // Header padding is SPACES, data padding is zeros: the standard is specific about each.
    assert(d[header_bytes - 1] == ' ');
    std::remove(path.c_str());
}

void testFitsRowOrderAndValues()
{
    std::cout << "  FITS row order and 8-bit samples\n";

    const int w = 16, h = 8;
    const Frame frame = makeFrame(ImageFormat::RAW8, w, h);
    const std::string path = "ut_frame_writer_rows.fits";
    assert(imgio::writeFits(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    const std::size_t at = fitsHeaderBytes(d);

    // Rows go out TOP-DOWN, exactly as the sensor delivers them: the first row of DATA is the FIRST row of the frame.
    // This is not a free choice. Reversing them would shift the Bayer mosaic by one row on any even height -- which
    // is every height, since isRoiAligned() demands it -- and readers assume top-down when ROWORDER is absent, so a
    // reversed frame would come out both upside down and mis-coloured. Measured on an ASI224MC: red rendered green.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            assert(fitsSample(d, at + (static_cast<std::size_t>(y) * w + x) * 2u) ==
                   frame.data[static_cast<std::size_t>(y) * w + x]);

    // And the file must SAY so, or the order is unrecoverable and every reader falls back to a guess.
    assert(fitsValue(d, "ROWORDER") == "'TOP-DOWN'");
    std::remove(path.c_str());
}

void testFits16BitBiasRoundTrips()
{
    std::cout << "  FITS 16-bit BZERO round-trip\n";

    const int w = 8, h = 4;
    Frame frame;
    frame.format = ImageFormat::RAW16;
    frame.width = w; frame.height = h; frame.bin = 1;
    frame.data.resize(frame.expectedBytes());

    // Values spanning the whole unsigned range, including both ends, since those are exactly what a naive signed
    // BITPIX 16 would mangle.
    const std::vector<std::uint16_t> values = {0, 1, 32767, 32768, 32769, 65534, 65535, 12345,
                                               100, 200, 300, 400, 500, 600, 700, 800,
                                               900, 1000, 1100, 1200, 1300, 1400, 1500, 1600,
                                               1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        frame.data[i * 2]     = static_cast<PixelByte>(values[i] & 0xFF);
        frame.data[i * 2 + 1] = static_cast<PixelByte>((values[i] >> 8) & 0xFF);
    }

    const std::string path = "ut_frame_writer_16.fits";
    assert(imgio::writeFits(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    assert(fitsValue(d, "BITPIX") == "16");
    assert(fitsValue(d, "BZERO") == "32768");    // BITPIX 16 is SIGNED in FITS; the bias makes it carry unsigned data
    assert(fitsValue(d, "BSCALE") == "1");

    // Undo what a reader does -- big-endian signed, plus BZERO -- and the original values must come back exactly.
    const std::size_t at = fitsHeaderBytes(d);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const int recovered = fitsSample(d, at + (static_cast<std::size_t>(y) * w + x) * 2u);
            const std::size_t src = (static_cast<std::size_t>(y) * w + x) * 2u;   // top-down
            assert(recovered == (frame.data[src] | (frame.data[src + 1] << 8)));
        }
    }
    std::remove(path.c_str());
}

void testFitsColourPlanes()
{
    std::cout << "  FITS colour de-interleaving\n";

    const int w = 8, h = 4;
    const Frame frame = makeFrame(ImageFormat::RGB24, w, h);
    const std::string path = "ut_frame_writer_rgb.fits";
    assert(imgio::writeFits(frame, path));

    const std::vector<std::uint8_t> d = readFile(path);
    assert(fitsValue(d, "NAXIS") == "3");
    assert(fitsValue(d, "NAXIS3") == "3");

    // FITS stores colour PLANE BY PLANE in R,G,B order, while the SDK delivers pixels interleaved as B,G,R. Both the
    // de-interleaving and the channel reversal have to happen, and getting only one of them right swaps red and blue.
    const std::size_t at = fitsHeaderBytes(d);
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    for (int p = 0; p < 3; ++p)
    {
        const std::size_t channel = static_cast<std::size_t>(2 - p);   // plane 0 is R, which is byte 2 of B,G,R
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const int got = fitsSample(d, at + (p * plane + static_cast<std::size_t>(y) * w + x) * 2u);
                const std::size_t src = (static_cast<std::size_t>(y) * w + x) * 3u + channel;
                assert(got == frame.data[src]);
            }
        }
    }
    std::remove(path.c_str());
}

void testFitsBayerPatternNames()
{
    std::cout << "  FITS BAYERPAT names all four mosaics\n";

    // The bug this pins down: the SDK names a pattern by its first ROW, FITS by the whole 2x2 CELL, and only RG is
    // completed by "GB". Building the string as toString(pattern) + "GB" gave RGGB, BGGB, GRGB, GBGB -- three of
    // which are not Bayer patterns at all. It went unnoticed because the camera on the bench is an RG sensor.
    const struct { BayerPattern pattern; const char* name; } expected[] = {
        {BayerPattern::RG, "RGGB"},
        {BayerPattern::BG, "BGGR"},
        {BayerPattern::GR, "GRBG"},
        {BayerPattern::GB, "GBRG"},
    };
    for (const auto& e : expected)
    {
        const imgio::FitsCard card = imgio::fitsBayerPattern(e.pattern);
        assert(card.key == "BAYERPAT");
        assert(card.value.find(e.name) != std::string::npos);
    }
}

void testFitsDropsMosaicCardWhenThereIsNoMosaic()
{
    std::cout << "  FITS drops BAYERPAT when binned or already demosaiced\n";

    imgio::FitsCards cards;
    cards.push_back(imgio::fitsBayerPattern(BayerPattern::RG));

    // Binning sums neighbouring photosites, so the mosaic is gone however the caller labels the frame.
    Frame binned = makeFrame(ImageFormat::RAW8, 8, 4);
    binned.bin = 2;
    const std::string binned_path = "ut_frame_writer_binned.fits";
    assert(imgio::writeFits(binned, binned_path, cards));
    assert(fitsValue(readFile(binned_path), "BAYERPAT").empty());
    std::remove(binned_path.c_str());

    // RGB24 has already been demosaiced by the SDK; labelling it would have a reader demosaic it twice.
    const Frame colour = makeFrame(ImageFormat::RGB24, 8, 4);
    const std::string colour_path = "ut_frame_writer_demosaiced.fits";
    assert(imgio::writeFits(colour, colour_path, cards));
    assert(fitsValue(readFile(colour_path), "BAYERPAT").empty());
    std::remove(colour_path.c_str());

    // But an unbinned raw frame keeps it, or the reader has no way to recover colour at all.
    const Frame raw = makeFrame(ImageFormat::RAW8, 8, 4);
    const std::string raw_path = "ut_frame_writer_mosaic.fits";
    assert(imgio::writeFits(raw, raw_path, cards));
    const std::vector<std::uint8_t> raw_d = readFile(raw_path);
    assert(fitsValue(raw_d, "BAYERPAT") == "'RGGB    '");
    assert(fitsValue(raw_d, "XBAYROFF") == "0");     // full frame: the mosaic starts where the sensor does
    assert(fitsValue(raw_d, "YBAYROFF") == "0");
    std::remove(raw_path.c_str());
}

void testFitsMosaicOffsetFollowsRoiOrigin()
{
    std::cout << "  FITS mosaic offset follows an odd ROI origin\n";

    imgio::FitsCards cards;
    cards.push_back(imgio::fitsBayerPattern(BayerPattern::RG));
    // A caller's own offsets are ignored: the frame knows where it came from, and a stale guess is worse than none.
    cards.push_back(imgio::fitsInt("XBAYROFF", 7, "nonsense a caller should not be able to inject"));

    // An ODD origin starts the window on a different photosite of the 2x2 cell. The vendor accepts one without
    // complaint, so a reader has to be told, or it demosaics a windowed capture exactly one photosite out of phase --
    // the same class of error that made a red source render green when the row order was reversed.
    Frame frame = makeFrame(ImageFormat::RAW8, 8, 4);
    frame.start_x = 5;
    frame.start_y = 12;
    const std::string path = "ut_frame_writer_offset.fits";
    assert(imgio::writeFits(frame, path, cards));

    const std::vector<std::uint8_t> d = readFile(path);
    assert(fitsValue(d, "XBAYROFF") == "1");         // 5 is odd
    assert(fitsValue(d, "YBAYROFF") == "0");         // 12 is even
    std::remove(path.c_str());

    // And a frame with no mosaic gets no offsets, because they would describe nothing.
    Frame binned = makeFrame(ImageFormat::RAW8, 8, 4);
    binned.bin = 2;
    binned.start_x = 5;
    const std::string binned_path = "ut_frame_writer_offset_binned.fits";
    assert(imgio::writeFits(binned, binned_path, cards));
    assert(fitsValue(readFile(binned_path), "XBAYROFF").empty());
    std::remove(binned_path.c_str());
}

void testFitsCards()
{
    std::cout << "  FITS card formatting\n";

    const Frame frame = makeFrame(ImageFormat::RAW8, 8, 4);
    const std::string path = "ut_frame_writer_cards.fits";
    imgio::FitsCards extra;
    extra.push_back(imgio::fitsInt("GAIN", 450, "sensor gain"));
    extra.push_back(imgio::fitsReal("EXPTIME", 0.25, "seconds"));
    extra.push_back(imgio::fitsText("BAYERPAT", "RGGB", "colour filter array"));
    extra.push_back(imgio::fitsText("LONGNAME", "a value that is definitely longer than eight characters"));
    assert(imgio::writeFits(frame, path, extra));

    const std::vector<std::uint8_t> d = readFile(path);
    assert(fitsValue(d, "GAIN") == "450");
    assert(fitsValue(d, "EXPTIME") == "0.25");
    assert(fitsValue(d, "BAYERPAT") == "'RGGB    '");     // text is quoted and padded to eight characters

    // Every card must be exactly 80 columns, or every card after it is misaligned.
    const std::size_t header_bytes = fitsHeaderBytes(d);
    assert(header_bytes % 80 == 0);
    for (std::size_t at = 0; at + 80 <= header_bytes; at += 80)
        for (std::size_t i = 0; i < 80; ++i)
            assert(d[at + i] >= 32 && d[at + i] <= 126);   // printable ASCII only, as the standard requires

    std::remove(path.c_str());
}

void testPreview()
{
    std::cout << "  ASCII preview\n";

    // A frame that is black on the left and white on the right must render dark on the left and bright on the right,
    // which is the only property a glance actually needs to be trustworthy.
    Frame frame;
    frame.format = ImageFormat::RAW8;
    frame.width = 64;
    frame.height = 32;
    frame.bin = 1;
    frame.data.assign(frame.expectedBytes(), 0);
    for (int y = 0; y < frame.height; ++y)
        for (int x = frame.width / 2; x < frame.width; ++x)
            frame.data[static_cast<std::size_t>(y) * frame.width + x] = 255;

    const std::string preview = imgio::framePreview(frame, 32);
    assert(!preview.empty());

    const std::size_t eol = preview.find('\n');
    assert(eol == 32);                                   // the requested width, then a newline
    const std::string first = preview.substr(0, eol);
    assert(first.front() == ' ');                        // black -> the dimmest ramp character
    assert(first.back() == '@');                         // white -> the brightest

    // The aspect ratio is halved, because a character cell is about twice as tall as it is wide.
    const int rows = static_cast<int>(std::count(preview.begin(), preview.end(), '\n'));
    assert(rows == (frame.height * 32) / (frame.width * 2));

    assert(imgio::framePreview(frame, 0).empty());
    assert(imgio::framePreview(Frame(), 32).empty());
}

} // namespace

int main()
{
    testBmpHeaderAndPixels();
    testBmpPaddingPath();
    testPgm8Bit();
    testPgm16BitIsByteSwapped();
    testWrongFormatsAreRefused();
    testWriteFrameDispatch();
    testFitsStructure();
    testFitsRowOrderAndValues();
    testFits16BitBiasRoundTrips();
    testFitsColourPlanes();
    testFitsBayerPatternNames();
    testFitsDropsMosaicCardWhenThereIsNoMosaic();
    testFitsMosaicOffsetFollowsRoiOrigin();
    testFitsCards();
    testPreview();

    std::cout << "UT_FrameWriter: ALL CHECKS PASSED" << std::endl;
    return 0;
}
