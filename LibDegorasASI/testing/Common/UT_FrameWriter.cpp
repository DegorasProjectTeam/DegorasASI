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
#include <cassert>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// PROJECT INCLUDES (module aggregators)
#include <LibDegorasASI/Modules/Common>
#include <LibDegorasASI/Modules/Helpers>


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
    testPreview();

    std::cout << "UT_FrameWriter: ALL CHECKS PASSED" << std::endl;
    return 0;
}
