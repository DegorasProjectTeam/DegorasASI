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
#include <cstddef>
#include <string>
#include <vector>

// PROJECT INCLUDES
#include "DegorasASI/Helpers/json_utils.h"


// NAMESPACES
namespace dpasi
{
namespace json
{

namespace
{

// Position of the first value character for "key": <value>, tolerant of surrounding whitespace, or npos.
std::size_t valueStart(const std::string& j, const std::string& key)
{
    const std::string pat = "\"" + key + "\"";
    std::size_t pos = 0;
    while ((pos = j.find(pat, pos)) != std::string::npos)
    {
        std::size_t k = pos + pat.size();
        while (k < j.size() && std::isspace(static_cast<unsigned char>(j[k]))) ++k;
        if (k < j.size() && j[k] == ':')
        {
            ++k;
            while (k < j.size() && std::isspace(static_cast<unsigned char>(j[k]))) ++k;
            return k;
        }
        pos += pat.size();
    }
    return std::string::npos;
}

// Read a scalar token (number / bool) starting at s, up to the next structural delimiter.
std::string scalarToken(const std::string& j, std::size_t s)
{
    std::size_t i = s;
    while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != ']') ++i;
    std::string t = j.substr(s, i - s);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    return t;
}

// Return the substring of a balanced container ('{','}' or '[',']') starting at s (s must be the opener).
std::string balanced(const std::string& j, std::size_t s, char open, char close)
{
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = s; i < j.size(); ++i)
    {
        const char c = j[i];
        if (in_str)
        {
            if (c == '"' && j[i - 1] != '\\') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == open) ++depth;
        else if (c == close)
        {
            --depth;
            if (depth == 0) return j.substr(s, i - s + 1);
        }
    }
    return std::string();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

std::string prettify(const std::string& compact)
{
    std::string out;
    int indent = 0;
    bool in_str = false;
    const auto newline = [&]()
    {
        out += '\n';
        out.append(static_cast<std::size_t>(indent) * 4, ' ');
    };

    for (std::size_t i = 0; i < compact.size(); ++i)
    {
        const char c = compact[i];
        if (in_str)
        {
            out += c;
            if (c == '"' && compact[i - 1] != '\\') in_str = false;
            continue;
        }
        switch (c)
        {
            case '"': in_str = true; out += c; break;
            case '{': case '[': out += c; ++indent; newline(); break;
            case '}': case ']': --indent; newline(); out += c; break;
            case ',': out += c; newline(); break;
            case ':': out += ": "; break;
            case ' ': break;   // drop existing spacing; re-introduced deterministically above
            default:  out += c; break;
        }
    }
    return out;
}

bool getBool(const std::string& json, const std::string& key, bool def)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos) return def;
    return scalarToken(json, s) == "true";
}

int getInt(const std::string& json, const std::string& key, int def)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos) return def;
    try { return std::stoi(scalarToken(json, s)); }
    catch (...) { return def; }
}

std::int64_t getInt64(const std::string& json, const std::string& key, std::int64_t def)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos) return def;
    try { return static_cast<std::int64_t>(std::stoll(scalarToken(json, s))); }
    catch (...) { return def; }
}

double getDouble(const std::string& json, const std::string& key, double def)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos) return def;
    try { return std::stod(scalarToken(json, s)); }
    catch (...) { return def; }
}

std::string getString(const std::string& json, const std::string& key, const std::string& def)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos || json[s] != '"') return def;
    std::string out;
    for (std::size_t i = s + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) { out += json[i + 1]; ++i; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

std::string getObject(const std::string& json, const std::string& key)
{
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos || json[s] != '{') return std::string();
    return balanced(json, s, '{', '}');
}

std::vector<int> getIntArray(const std::string& json, const std::string& key)
{
    std::vector<int> values;
    const std::size_t s = valueStart(json, key);
    if (s == std::string::npos || json[s] != '[') return values;

    const std::string arr = balanced(json, s, '[', ']');
    std::size_t p = 1;   // skip the opening '['
    while (p < arr.size())
    {
        const std::size_t comma = arr.find(',', p);
        const std::size_t end = (comma == std::string::npos) ? arr.size() - 1 : comma;   // arr.back() == ']'
        const std::string token = arr.substr(p, end - p);
        try { values.push_back(std::stoi(token)); }
        catch (...) { /* Skip a non-numeric or empty entry; the extractors are best-effort by design. */ }
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
    return values;
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
