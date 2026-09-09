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
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// OPENCV INCLUDES
#include <opencv2/imgcodecs.hpp>

// PROJECT INCLUDES
#include <DegorasASI/Modules/Helpers>

#include "live_controller.h"


using namespace dpasi;

// NAMESPACES
namespace live
{

namespace
{

// ---------------------------------------------------------------------------------------------------------------------
// NO COMMAND HERE MAY DEPEND ON SHIFT, AND NO KEY MAY DIFFER FROM ANOTHER ONLY BY CASE.
//
// The Qt backend of highgui throws the case away. In OpenCV 4.12's window_QT.cpp the key handler starts from
// QKeyEvent::key(), which is modifier-independent, and translates it with QTest::keyToAscii(), whose table returns
// LOWER case for every letter -- Qt's own source spells it out: "case Qt::Key_A: return 0x61; // 0x41 == 'A',
// 0x61 == 'a'". Modifiers are consulted only to discard Ctrl. So 'r' and shift+R arrive as the same code, and any
// upper-case case label is dead code.
//
// This was not theoretical. The map used to read lower case as "less" and upper case as "more", so exposure, gain and
// both white-balance channels COULD ONLY GO DOWN. Somebody who nudged the white balance down found no way back up,
// the red and blue channels ended at their minimum of 1, and the picture went pure green -- which then looked like a
// colour-handling bug in the library and was not one.
//
// So: the continuous controls are on PAIRS OF DIGITS, one key per direction, and anything that used to need shift for
// its opposite is now a CYCLE that a single key walks all the way round.
// ---------------------------------------------------------------------------------------------------------------------

// A tenth either way. Multiplicative rather than a fixed step because exposure spans microseconds to seconds: a step
// large enough to matter at one second is meaningless at one millisecond, and vice versa.
constexpr double kExposureUp = 1.1;
constexpr double kExposureDown = 0.9;

constexpr long long kGainStep = 10;
constexpr long long kWhiteBalanceStep = 1;

// Reticle nudging. The coarse step is a whole photosite; the fine one a tenth, because the centroid of a return does
// not land on an integer and a mark that can only sit on integers cannot be aligned with one.
constexpr double kCoarseNudge = 1.0;
constexpr double kFineNudge = 0.1;

// How near a click has to be, in sensor pixels, to grab a reticle rather than miss it.
constexpr double kGrabRadius = 24.0;

// Steps for the shape keys. Whole pixels: a sub-pixel gap is not a thing anyone needs.
constexpr double kGapStep = 1.0;
constexpr double kCircleStep = 2.0;

// The colours on offer. A short list rather than a colour picker, because highgui has no dialogs and because these
// are the ones that stay legible over a star field: red first, which is the default and the one that preserves dark
// adaptation at a telescope.
struct PaletteEntry
{
    const char* name;
    int red;
    int green;
    int blue;
};

const PaletteEntry kPalette[] = {
    {"red",     255,   0,   0},
    {"green",     0, 255,   0},
    {"cyan",      0, 255, 255},
    {"yellow",  255, 255,   0},
    {"magenta", 255,   0, 255},
    {"white",   255, 255, 255},
    {"black",     0,   0,   0},
};

constexpr int kPaletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

/// @brief The palette entry a reticle's colour matches, or -1 when it matches none.
int paletteIndexOf(const Reticle& item)
{
    for (int i = 0; i < kPaletteSize; ++i)
    {
        if (kPalette[i].red == item.red && kPalette[i].green == item.green && kPalette[i].blue == item.blue)
            return i;
    }
    return -1;
}

/// @brief A reticle's colour in words, for the HUD and the menu.
std::string colourNameOf(const Reticle& item)
{
    const int index = paletteIndexOf(item);
    if (index >= 0)
        return kPalette[index].name;
    return std::to_string(item.red) + "," + std::to_string(item.green) + "," + std::to_string(item.blue);
}

// One wheel notch, or one press of the zoom key. A factor rather than an increment, so the steps feel even across the
// whole range instead of crawling at 1x and leaping at 20x.
constexpr double kZoomFactor = 1.25;

// Arrow keys as cv::waitKeyEx reports them on Windows. Named rather than inline because they are PLATFORM-SPECIFIC
// and the digit keys below exist precisely so the feature does not depend on them: if a build reports different
// codes, 4/6/8/2 still move the reticle and only this table needs revisiting.
constexpr int kKeyLeft  = 2424832;
constexpr int kKeyUp    = 2490368;
constexpr int kKeyRight = 2555904;
constexpr int kKeyDown  = 2621440;

// One step of the thickness keys, in canvas pixels.
constexpr int kThicknessStep = 1;

constexpr int kKeyTab = 9;
constexpr int kKeyBackspace = 8;
constexpr int kKeyDelete = 3014656;

std::string fixed2(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << v;
    return os.str();
}

std::string fixed1(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v;
    return os.str();
}

}   // namespace

// ---------------------------------------------------------------------------------------------------------------------

LiveController::MenuEntry::MenuEntry() :
    action(MenuAction::ADD_HERE),
    palette(-1)
{
}

// ---------------------------------------------------------------------------------------------------------------------

LiveController::LiveController(LiveModel& model, LiveView& view) :
    model_(model),
    view_(view),
    shots_(0),
    fine_(false),
    reticle_file_(),
    menu_entries_(),
    entry_target_(EntryTarget::NONE),
    grabbed_(false),
    grab_dx_(0.0),
    grab_dy_(0.0)
{
}

// ---------------------------------------------------------------------------------------------------------------------

void LiveController::applyEntry()
{
    std::string text;
    if (!this->view_.takeEntry(text))
        return;

    const EntryTarget target = this->entry_target_;
    this->entry_target_ = EntryTarget::NONE;

    if (text.empty())
        return;

    double value = 0.0;
    try
    {
        value = std::stod(text);
    }
    catch (const std::exception&)
    {
        // The box only accepts digits and one point, so this needs a lone "." or an overflowing run of digits to
        // happen at all. Saying so beats setting something arbitrary.
        std::cout << "  '" << text << "' is not a number I can use\n";
        return;
    }

    if (target == EntryTarget::EXPOSURE)
    {
        // MILLISECONDS, matching the HUD, the slider and the --exposure switch. Microseconds would be the
        // camera's own unit and the wrong one to ask a person for: every value anybody types here has three
        // zeroes on the end of it.
        const long long microseconds = static_cast<long long>(value * 1000.0);
        if (microseconds <= 0)
        {
            std::cout << "  exposure must be greater than zero\n";
            return;
        }
        this->model_.requestExposure(microseconds);
        this->view_.showExposureOnSlider(microseconds);
        std::cout << "  exposure set to " << value << " ms\n";
    }
    else if (target == EntryTarget::GAIN)
    {
        this->model_.requestGain(static_cast<long long>(value));
        std::cout << "  gain set to " << static_cast<long long>(value) << "\n";
    }
}

void LiveController::pumpSliders()
{
    long long microseconds = 0;
    if (this->view_.takeExposureRequest(microseconds))
        this->model_.requestExposure(microseconds);

    long long gain = 0;
    if (this->view_.takeGainRequest(gain))
        this->model_.requestGain(gain);
}

void LiveController::pumpMouse()
{
    ReticleSet& set = this->view_.reticles();
    const FrameGeometry& geometry = this->view_.geometry();

    // Zoom at the cursor, so the thing being examined does not slide out from under it.
    int notches = 0;
    if (this->view_.takeWheel(notches))
    {
        int anchor_x = 0;
        int anchor_y = 0;
        if (!this->view_.probePointInFrame(anchor_x, anchor_y))
        {
            anchor_x = this->view_.frameWidth() / 2;
            anchor_y = this->view_.frameHeight() / 2;
        }
        const double factor = (notches > 0) ? kZoomFactor : (1.0 / kZoomFactor);
        for (int i = 0; i < std::abs(notches); ++i)
            this->view_.zoomBy(factor, anchor_x, anchor_y);
    }

    int drag_x = 0;
    int drag_y = 0;
    if (this->view_.takeRightDrag(drag_x, drag_y))
        this->view_.panBy(drag_x, drag_y);

    // A right CLICK, as opposed to a right drag, opens the menu -- or closes it, so the same button both ways.
    int click_x = 0;
    int click_y = 0;
    if (this->view_.takeRightClick(click_x, click_y))
    {
        if (this->view_.menuOpen())
            this->view_.closeMenu();
        else
            this->openMenuAt(click_x, click_y);
    }

    int choice = 0;
    if (this->view_.takeMenuChoice(choice))
        this->applyMenuChoice(choice);

    int x = 0;
    int y = 0;
    if (this->view_.takeMousePressInFrame(x, y))
    {
        // The click arrives in frame coordinates and the reticles live in sensor coordinates, so it is converted
        // rather than compared directly -- which is what keeps selection working at any binning or ROI.
        double sensor_x = 0.0;
        double sensor_y = 0.0;
        frameToSensor(geometry, x, y, sensor_x, sensor_y);

        // A press that lands nowhere near a reticle DESELECTS rather than creating one. Creation is a key, so that
        // clicking on the image to look at something cannot leave a mark behind.
        if (!set.selectNear(sensor_x, sensor_y, geometry, kGrabRadius))
        {
            set.deselect();
            this->grabbed_ = false;
        }
        else
        {
            // Remember where inside the mark the press landed. Everything after this moves the mark BY the
            // pointer's travel rather than TO the pointer, so selecting one does not shift it -- which was the
            // whole complaint: a click merely to select, before locking, nudged the mark by up to the grab
            // radius and undid the alignment it was about to protect.
            double centre_x = 0.0;
            double centre_y = 0.0;
            this->view_.reticles().resolvePosition(set.selected(), geometry, centre_x, centre_y);
            this->grab_dx_ = centre_x - sensor_x;
            this->grab_dy_ = centre_y - sensor_y;
            this->grabbed_ = true;
        }
    }

    // Held button plus a grabbed selection is a drag, offset preserved.
    // Not while the menu is open: the press that chose an item must not also drag whatever is under it.
    if (!this->view_.mouseHeld())
    {
        this->grabbed_ = false;
    }
    else if (this->grabbed_ && set.hasSelection() && !this->view_.menuOpen())
    {
        int cursor_x = 0;
        int cursor_y = 0;
        if (this->view_.probePointInFrame(cursor_x, cursor_y))
        {
            double sensor_x = 0.0;
            double sensor_y = 0.0;
            frameToSensor(geometry, cursor_x, cursor_y, sensor_x, sensor_y);
            set.placeSelected(sensor_x + this->grab_dx_, sensor_y + this->grab_dy_, geometry);
        }
    }
}

void LiveController::openMenuAt(int canvas_x, int canvas_y)
{
    ReticleSet& set = this->view_.reticles();
    const FrameGeometry& geometry = this->view_.geometry();

    // Selecting first is what makes the rest of the menu unambiguous: "delete this one" and "colour" then plainly
    // mean the reticle the pointer is on, and the same click leaves the keyboard pointed at it too.
    int frame_x = 0;
    int frame_y = 0;
    bool on_reticle = false;
    if (this->view_.canvasToFrame(canvas_x, canvas_y, frame_x, frame_y))
    {
        double sensor_x = 0.0;
        double sensor_y = 0.0;
        frameToSensor(geometry, frame_x, frame_y, sensor_x, sensor_y);
        on_reticle = set.selectNear(sensor_x, sensor_y, geometry, kGrabRadius);
    }

    std::vector<std::string> labels;
    this->menu_entries_.clear();

    auto add = [&labels, this](const std::string& label, MenuAction action, int palette)
    {
        labels.push_back(label);
        MenuEntry entry;
        entry.action = action;
        entry.palette = palette;
        this->menu_entries_.push_back(entry);
    };
    auto addSeparator = [&labels, this]()
    {
        // A separator carries a default entry that is never reached: the view refuses to report "-" as a choice.
        labels.push_back("-");
        this->menu_entries_.push_back(MenuEntry());
    };

    add("Add reticle here", MenuAction::ADD_HERE, -1);
    if (set.hasCentred())
        add("Remove centred reticle", MenuAction::REMOVE_CENTRED, -1);
    else
        add("Add centred reticle", MenuAction::ADD_CENTRED, -1);

    if (on_reticle)
    {
        // Labelled rather than hidden when locked. An item that vanishes reads as a bug; one that says why it
        // will refuse teaches the lock exists, and the entry right below it is how to undo that.
        add(set.isSelectedLocked() ? "Delete this reticle (locked)" : "Delete this reticle",
            MenuAction::DELETE_SELECTED, -1);
    }
    if (set.size() > 0)
        add("Delete all reticles", MenuAction::DELETE_ALL, -1);

    if (set.hasSelection())
    {
        addSeparator();
        add(set.isSelectedLocked() ? "Unlock this reticle" : "Lock this reticle", MenuAction::TOGGLE_LOCK, -1);
        add(std::string("Shape: ") + toString(set.at(set.selected()).shape) + " (change)",
            MenuAction::CYCLE_SHAPE, -1);

        addSeparator();
        add("Thicker line", MenuAction::THICKER, -1);
        add("Thinner line", MenuAction::THINNER, -1);
        add("Add circle", MenuAction::ADD_CIRCLE, -1);
        if (!set.at(set.selected()).circles.empty())
            add("Remove circle", MenuAction::REMOVE_CIRCLE, -1);

        addSeparator();
        const int current = paletteIndexOf(set.at(set.selected()));
        for (int i = 0; i < kPaletteSize; ++i)
        {
            // The current colour is marked, so the menu also answers "which one is it now?".
            const std::string mark = (i == current) ? "* " : "  ";
            add(mark + std::string("Colour: ") + kPalette[i].name, MenuAction::SET_COLOUR, i);
        }
    }

    this->view_.openMenu(canvas_x, canvas_y, labels);
}

void LiveController::applyMenuChoice(int index)
{
    if (index < 0 || index >= static_cast<int>(this->menu_entries_.size()))
        return;

    ReticleSet& set = this->view_.reticles();
    const FrameGeometry& geometry = this->view_.geometry();
    const MenuEntry entry = this->menu_entries_[static_cast<std::size_t>(index)];

    switch (entry.action)
    {
        case MenuAction::ADD_HERE:
        {
            // Where the MENU was opened, not where the pointer is now: the pointer has moved down the menu to reach
            // the item, so using its current position would put the reticle on the menu itself.
            int frame_x = 0;
            int frame_y = 0;
            if (!this->view_.menuAnchorInFrame(frame_x, frame_y))
                return;
            double sensor_x = 0.0;
            double sensor_y = 0.0;
            frameToSensor(geometry, frame_x, frame_y, sensor_x, sensor_y);
            set.addAt(sensor_x, sensor_y);
            return;
        }

        case MenuAction::ADD_CENTRED:
            if (!set.hasCentred())
                set.addCentred();
            return;

        case MenuAction::REMOVE_CENTRED:
            set.removeCentred();
            return;

        case MenuAction::DELETE_SELECTED:
            if (!set.removeSelected() && set.isSelectedLocked())
                std::cout << "  that reticle is LOCKED; unlock it first\n";
            return;

        case MenuAction::TOGGLE_LOCK:
            std::cout << "  reticle " << (set.selected() + 1) << ": "
                      << (set.toggleSelectedLock() ? "LOCKED (will not move or be deleted)" : "unlocked") << "\n";
            return;

        case MenuAction::CYCLE_SHAPE:
            std::cout << "  reticle " << (set.selected() + 1) << " shape: "
                      << toString(set.cycleSelectedShape()) << "\n";
            return;

        case MenuAction::DELETE_ALL:
            set.clear();
            return;

        case MenuAction::SET_COLOUR:
            if (entry.palette >= 0 && entry.palette < kPaletteSize)
                set.setSelectedColour(kPalette[entry.palette].red, kPalette[entry.palette].green,
                                      kPalette[entry.palette].blue);
            return;

        case MenuAction::THICKER:
            set.resizeSelectedThickness(kThicknessStep);
            return;

        case MenuAction::THINNER:
            set.resizeSelectedThickness(-kThicknessStep);
            return;

        case MenuAction::ADD_CIRCLE:
            set.addCircleToSelected();
            return;

        case MenuAction::REMOVE_CIRCLE:
            set.removeCircleFromSelected();
            return;
    }
}

void LiveController::setReticleFile(const std::string& path)
{
    this->reticle_file_ = path;
}

bool LiveController::loadReticles()
{
    if (this->reticle_file_.empty())
        return false;
    return this->view_.reticles().load(this->reticle_file_);
}

bool LiveController::saveReticles() const
{
    if (this->reticle_file_.empty())
        return false;
    return this->view_.reticles().save(this->reticle_file_);
}

std::string LiveController::selectedReticleText() const
{
    const ReticleSet& set = this->view_.reticles();
    if (!set.hasSelection())
    {
        if (set.size() == 0)
            return std::string();
        return "reticles " + std::to_string(set.size()) + ", none selected";
    }

    const std::size_t index = set.selected();
    const Reticle& item = set.at(index);
    double x = 0.0;
    double y = 0.0;
    set.resolvePosition(index, this->view_.geometry(), x, y);

    // The numbers are on screen because that is what makes a fine adjustment an adjustment rather than a guess.
    std::string text = "reticle " + std::to_string(index + 1) + "/" + std::to_string(set.size()) +
                       (item.centred ? " [centred]" : "") +
                       "  sensor " + fixed2(x) + "," + fixed2(y) +
                       "  gap " + fixed1(item.gap) +
                       "  " + colourNameOf(item) + " x" + std::to_string(item.thickness);
    if (!item.circles.empty())
        text += "  circle " + fixed1(item.circles.back());
    text += this->fine_ ? "  step 0.1" : "  step 1.0";
    return text;
}

bool LiveController::handleKey(int key, types::Frame& frame, const cv::Mat& display)
{
    // THE TYPED BOX SWALLOWS EVERYTHING while it is open, and that is the point rather than an oversight. The
    // digits are exposure and gain shortcuts in the normal map, so without this a value could not be typed at
    // all: pressing 3 would drop the gain instead of entering a three.
    if (this->view_.entryOpen())
    {
        this->view_.entryKey(key);
        this->applyEntry();
        return true;
    }

    // Reticles first, so their keys are handled whatever the rest of the map does.
    ReticleSet& set = this->view_.reticles();
    const FrameGeometry& geometry = this->view_.geometry();
    const double step = this->fine_ ? kFineNudge : kCoarseNudge;

    switch (key)
    {
        case 'v':
            this->view_.toggleReticles();
            std::cout << "  reticles: " << (this->view_.reticlesVisible() ? "shown" : "hidden") << "\n";
            return true;

        case 'x':
            if (set.hasCentred())
            {
                set.removeCentred();
                std::cout << "  centred reticle removed\n";
            }
            else
            {
                set.addCentred();
                std::cout << "  centred reticle added\n";
            }
            return true;

        case 'n':
        {
            int x = 0;
            int y = 0;
            if (this->view_.probePointInFrame(x, y))
            {
                double sensor_x = 0.0;
                double sensor_y = 0.0;
                frameToSensor(geometry, x, y, sensor_x, sensor_y);
                set.addAt(sensor_x, sensor_y);
                std::cout << "  reticle " << set.size() << " added at sensor "
                          << sensor_x << "," << sensor_y << "\n";
            }
            else
            {
                std::cout << "  point at the image first; a new reticle goes where the cursor is\n";
            }
            return true;
        }

        case kKeyTab:
            // Cycles by INDEX rather than by position: two reticles can sit on the same pixel, and selecting by
            // proximity would then never reach the second one.
            if (set.size() > 0)
                set.selectIndex(set.hasSelection() ? (set.selected() + 1) % set.size() : 0);
            return true;

        case kKeyDelete:
        case kKeyBackspace:
            // removeSelected() returns false for two different reasons and the difference matters to whoever is
            // pressing the key: nothing selected is a miss, a locked mark is a refusal. Saying so is what stops
            // the lock looking like the delete key has stopped working.
            if (set.removeSelected())
                std::cout << "  reticle removed, " << set.size() << " left\n";
            else if (set.isSelectedLocked())
                std::cout << "  that reticle is LOCKED; press B to unlock it first\n";
            return true;

        case 'b':
            if (set.hasSelection())
                std::cout << "  reticle " << (set.selected() + 1) << ": "
                          << (set.toggleSelectedLock() ? "LOCKED (will not move or be deleted)" : "unlocked")
                          << "\n";
            else
                std::cout << "  select a reticle first; click it or press TAB\n";
            return true;

        case 'r':
            if (set.hasSelection())
                std::cout << "  reticle " << (set.selected() + 1) << " shape: "
                          << toString(set.cycleSelectedShape()) << "\n";
            else
                std::cout << "  select a reticle first; click it or press TAB\n";
            return true;

        case 'f':
            this->fine_ = !this->fine_;
            std::cout << "  reticle step: " << (this->fine_ ? "0.1 px (fine)" : "1.0 px") << "\n";
            return true;

        // Arrows only. The digits used to double for these as insurance against the arrow codes being
        // platform-specific, but they do arrive here -- an arrow is not a translatable key, so the Qt handler falls
        // through to nativeVirtualKey() -- and the digit row is worth more as the camera controls below.
        case kKeyLeft:  set.nudgeSelected(-step, 0.0, geometry); return true;
        case kKeyRight: set.nudgeSelected(step, 0.0, geometry);  return true;
        case kKeyUp:    set.nudgeSelected(0.0, -step, geometry); return true;
        case kKeyDown:  set.nudgeSelected(0.0, step, geometry);  return true;

        // Zoom from the keyboard as well as the wheel: whether a wheel event reaches highgui depends on the backend,
        // and a magnifier that might not work is worse than one with two ways in. Two distinct keys rather than one
        // key and its shifted twin, for the reason at the top of this file.
        case 'z': this->view_.zoomBy(1.0 / kZoomFactor, this->view_.frameWidth() / 2.0,
                                     this->view_.frameHeight() / 2.0); return true;
        case '+': this->view_.zoomBy(kZoomFactor, this->view_.frameWidth() / 2.0,
                                     this->view_.frameHeight() / 2.0); return true;
        case '0': this->view_.resetView(); return true;

        case '[': set.resizeSelectedThickness(-kThicknessStep); return true;
        case ']': set.resizeSelectedThickness(kThicknessStep);  return true;
        case '-': set.resizeSelectedGap(-kGapStep); return true;
        case '=': set.resizeSelectedGap(kGapStep);  return true;

        // Cycles the palette rather than opening a picker, because highgui has no dialogs. One direction only, and
        // that is enough because it wraps; the menu offers the same colours by name for anyone who would rather pick.
        case 'c':
        {
            if (!set.hasSelection())
                return true;
            const int current = paletteIndexOf(set.at(set.selected()));
            const int next = ((current < 0 ? 0 : current) + 1) % kPaletteSize;
            set.setSelectedColour(kPalette[next].red, kPalette[next].green, kPalette[next].blue);
            std::cout << "  reticle colour: " << kPalette[next].name << "\n";
            return true;
        }

        case 'o':
            set.addCircleToSelected();
            return true;

        case 'u':
            set.removeCircleFromSelected();
            return true;

        case ',': set.resizeSelectedCircle(-kCircleStep); return true;
        case '.': set.resizeSelectedCircle(kCircleStep);  return true;

        case 'k':
            std::cout << "  " << (this->saveReticles() ? ("reticles saved: " + this->reticle_file_)
                                                       : std::string("reticles NOT saved")) << "\n";
            return true;

        default:
            break;
    }

    switch (key)
    {
        case -1:
            return true;

        case 27:                    // ESC
        case 'q':
            return false;

        case 'h':
            this->printKeys();
            return true;

        case 'a':
            this->view_.toggleStretch();
            std::cout << "  auto-stretch: " << (this->view_.stretchEnabled() ? "on" : "off") << "\n";
            return true;

        case 'd':
            this->view_.toggleDemosaic();
            std::cout << "  demosaic: " << (this->view_.demosaicEnabled() ? "on" : "off") << "\n";
            return true;

        case 'i':
            this->view_.toggleHud();
            return true;

        case 'l':
            this->view_.toggleHistogram();
            std::cout << "  histogram: " << (this->view_.histogramVisible() ? "shown" : "hidden") << "\n";
            return true;

        // Orientation. Applied to the DISPLAY and not to the camera, even though the camera has a FLIP control: a
        // hardware flip changes what a sensor coordinate means, which would drag every reticle off its photosite, and
        // on a colour sensor it also shifts the Bayer phase without the SDK reporting the new one, so the demosaic
        // would come out wrong. Flipping the picture instead leaves the sensor as the one fixed frame of reference.
        //
        // One key each, cycling through every state, rather than a key and its shifted opposite.
        case 'm':
            this->view_.cycleFlip();
            std::cout << "  orientation: " << (this->view_.orientationText().empty() ? "none"
                                                                                    : this->view_.orientationText())
                      << "\n";
            return true;

        case 't':
            this->view_.rotateBy(1);
            std::cout << "  orientation: " << (this->view_.orientationText().empty() ? "none"
                                                                                    : this->view_.orientationText())
                      << "\n";
            return true;

        // THE CAMERA CONTROLS, one digit per direction. These only enqueue: during a long exposure the change lands
        // on the next frame, and the HUD shows the value read back from the camera when it does.
        case 'e':
        {
            // Milliseconds, with one decimal place available for the sub-millisecond end the slider gave up.
            const double lo_ms = static_cast<double>(this->view_.exposureMinUs()) / 1000.0;
            const double hi_ms = static_cast<double>(this->view_.exposureMaxUs()) / 1000.0;
            std::ostringstream hint;
            hint << "milliseconds, " << fixed1(lo_ms) << " to " << static_cast<long long>(hi_ms);
            this->entry_target_ = EntryTarget::EXPOSURE;
            this->view_.openEntry("Exposure:", hint.str());
            return true;
        }

        case 'g':
        {
            std::ostringstream hint;
            hint << "0 to " << this->view_.gainMax();
            this->entry_target_ = EntryTarget::GAIN;
            this->view_.openEntry("Gain:", hint.str());
            return true;
        }

        case 'w':
            this->view_.cycleExposureBand();
            std::cout << "  exposure slider band: " << this->view_.exposureBandName() << "\n";
            return true;

        case '1': this->model_.requestExposureScale(kExposureDown); return true;
        case '2': this->model_.requestExposureScale(kExposureUp);   return true;

        case '3': this->model_.requestGainDelta(-kGainStep); return true;
        case '4': this->model_.requestGainDelta(kGainStep);  return true;

        case '5': this->model_.requestWbRedDelta(-kWhiteBalanceStep);  return true;
        case '6': this->model_.requestWbRedDelta(kWhiteBalanceStep);   return true;
        case '7': this->model_.requestWbBlueDelta(-kWhiteBalanceStep); return true;
        case '8': this->model_.requestWbBlueDelta(kWhiteBalanceStep);  return true;

        // The way out of a camera left in a useless state by whoever had it last -- which is not hypothetical: this
        // one hands back a white balance of 1/1, with no red and no blue in the picture at all.
        case '9':
            this->model_.requestResetControls();
            std::cout << "  exposure, gain, offset and white balance back to the camera's own defaults\n";
            return true;

        case 's':
        {
            if (frame.empty())
            {
                std::cout << "  nothing to save yet\n";
                return true;
            }
            // Saved through the library's own writer, not through OpenCV: it is the one that knows the vendor's
            // channel order and the mosaic phase, it keeps full depth, and it keeps these files identical to the
            // other examples'.
            const std::string path = "live_" + std::to_string(++this->shots_) + "." +
                                     imgio::extensionFor(frame.format);
            std::cout << "  saved: " << (imgio::writeFrame(frame, path) ? path : "WRITE FAILED") << "\n";
            return true;
        }

        case 'p':
        {
            if (display.empty())
            {
                std::cout << "  nothing on screen yet\n";
                return true;
            }
            // The screen image, stretch and all. Deliberately NOT a substitute for S: this is 8-bit and already
            // transformed, so it is a look, not data.
            const std::string path = "live_view_" + std::to_string(++this->shots_) + ".png";
            std::cout << "  " << (cv::imwrite(path, display) ? ("saved: " + path) : "PNG WRITE FAILED") << "\n";
            return true;
        }

        default:
            return true;
    }
}

void LiveController::printKeys() const
{
    // No command needs shift, and no two commands differ only by case: see the note at the top of this file for why
    // that is a hard rule here rather than a style choice.
    std::cout <<
        "Keys, while the window has focus. NOTHING here needs shift.\n"
        "\n"
        "Camera (a change during a long exposure lands on the NEXT frame; the HUD shows the value read back):\n"
        "  1 / 2          exposure -10% / +10%   (multiplicative: a fixed step is useless across us..s)\n"
        "  E              type an exposure, in milliseconds, into a box\n"
        "  G              type a gain into a box\n"
        "  W              exposure slider band: 1-100 ms -> 0.1-2 s -> 2-60 s -> round again\n"
        "  3 / 4          gain     -10  / +10\n"
        "  5 / 6          WB red   -1   / +1     (colour cameras only)\n"
        "  7 / 8          WB blue  -1   / +1\n"
        "  9              exposure, gain, offset and WB back to the CAMERA DEFAULTS\n"
        "                 (the way out of a camera the last session left unusable -- this one opens with\n"
        "                  WB 1/1, i.e. no red and no blue at all, against defaults of 52/95)\n"
        "\n"
        "Display:\n"
        "  A              toggle the percentile auto-stretch\n"
        "  D              toggle demosaicing (raw Bayer <-> colour)\n"
        "  I              toggle the HUD\n"
        "  L              toggle the histogram (levels), with the clipping percentages\n"
        "  M              flip: none -> horizontal -> vertical -> both -> none\n"
        "  T              rotate 90 degrees clockwise (four presses come back round)\n"
        "\n"
        "Reticles: a full-span cross or X with a central gap, in sensor coordinates and sub-pixel.\n"
        "  right-click    MENU: add here, add centred, delete, lock, shape, colour, thickness, circles\n"
        "  V              show / hide every reticle\n"
        "  X              add / remove the CENTRED reticle\n"
        "  N              new reticle where the cursor is\n"
        "  click          select the reticle under the cursor; drag to move it\n"
        "  TAB            select the next reticle\n"
        "  DEL / BACKSPC  remove the selected reticle (refused while it is locked)\n"
        "  B              lock / unlock it: a locked reticle will not move, drag or be deleted\n"
        "  R              shape: upright cross <-> diagonal X\n"
        "  arrows         move the selected reticle\n"
        "  F              fine step: 0.1 px instead of 1 px\n"
        "  C              next colour (red, green, cyan, yellow, magenta, white, black)\n"
        "  [ / ]          lines thinner / thicker\n"
        "  - / =          central gap smaller / larger\n"
        "  O / U          add a circle / remove the last one\n"
        "  , / .          last circle radius smaller / larger\n"
        "  K              save the reticles now (they are also saved on exit)\n"
        "\n"
        "View (the reticles are stored against the SENSOR, so they survive all of this):\n"
        "  wheel, + / Z   zoom in / out, digital, nearest-neighbour so a photosite stays a square\n"
        "  right-drag     pan\n"
        "  0              back to the whole frame\n"
        "\n"
        "Files:\n"
        "  S              save the frame through the library's own writer (FITS/BMP, full depth)\n"
        "  P              save what is on screen as PNG (8-bit, stretched -- for a quick look)\n"
        "  H              print these keys again\n"
        "  Q / ESC        quit\n"
        "\n"
        "Sliders: exposure (logarithmic) and gain. Hover the image to read the pixel under the cursor.\n"
        "The window stays responsive throughout a long exposure; the progress bar shows what is left of it.\n";
}

// ---------------------------------------------------------------------------------------------------------------------

}   // END NAMESPACE

// ---------------------------------------------------------------------------------------------------------------------
