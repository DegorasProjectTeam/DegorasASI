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
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

// PLATFORM INCLUDES
#ifdef _WIN32
#   include <windows.h>
#   include <process.h>
#else
#   include <fcntl.h>
#   include <sys/file.h>
#   include <unistd.h>
#endif

// PROJECT INCLUDES
#include "DegorasASI/ASI/asi_camera_lock.h"


// NAMESPACES
namespace dpasi
{
namespace asi
{

namespace
{

// The mutex byte and the payload live in DIFFERENT parts of the file, and that separation is load-bearing on Windows:
// LockFileEx takes an exclusive lock on a BYTE RANGE, and while it is held no other process may read that range. Lock
// byte 0 and write the description further in, and a second process can still read who the holder is in order to say
// so, while the range that decides ownership stays exclusive. flock() on POSIX locks the whole file but is purely
// advisory and does not block read(), so the same layout works there for free.
constexpr long long kLockByte    = 0;
constexpr long long kLockLength  = 1;
constexpr long long kPayloadAt   = 16;

std::mutex& holdMtx()
{
    static std::mutex mutex;
    return mutex;
}

#ifdef _WIN32
using NativeHandle = HANDLE;
#else
using NativeHandle = int;
#endif

/// Handles this process is holding, keyed by camera. Kept so the release path can close exactly the right one.
std::unordered_map<int, NativeHandle>& held()
{
    static std::unordered_map<int, NativeHandle> map;
    return map;
}

std::string tempDir()
{
#ifdef _WIN32
    char buffer[MAX_PATH + 1] = {0};
    const DWORD n = GetTempPathA(MAX_PATH, buffer);
    if (n == 0 || n > MAX_PATH)
        return std::string(".");
    return std::string(buffer);
#else
    const char* env = std::getenv("TMPDIR");
    if (env != nullptr && *env != '\0')
        return std::string(env) + "/";
    return std::string("/tmp/");
#endif
}

std::string lockPath(types::CameraId id)
{
    // Keyed on the camera id rather than the serial: the id is what the caller has before connecting, which is when
    // the claim has to be decided. Two cameras of the same model always get distinct ids from the SDK.
    return tempDir() + "dpasi-camera-" + std::to_string(types::toType(id)) + ".lock";
}

/// This process's identity, written into the lock so another process can name the holder.
std::string selfDescription()
{
#ifdef _WIN32
    char exe[MAX_PATH + 1] = {0};
    std::string name("unknown");
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0)
    {
        const std::string full(exe);
        const std::size_t slash = full.find_last_of("\\/");
        name = (slash == std::string::npos) ? full : full.substr(slash + 1);
    }
    return "pid " + std::to_string(static_cast<long long>(GetCurrentProcessId())) + " (" + name + ")";
#else
    return "pid " + std::to_string(static_cast<long long>(getpid()));
#endif
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

bool tryLockCameraOnHost(types::CameraId id)
{
    const std::lock_guard<std::mutex> lock(holdMtx());

    const int key = types::toType(id);
    if (held().count(key) != 0)
        return true;    // this process already holds it; the in-process registry is what refuses that case

    const std::string path = lockPath(id);
    const std::string me = selfDescription() + "\n";

#ifdef _WIN32
    // FILE_SHARE_READ | FILE_SHARE_WRITE so that a process which LOSES the race can still open the file to read who
    // won. Exclusion comes from LockFileEx below, not from the sharing mode.
    HANDLE handle = CreateFileA(path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return true;    // FAILS OPEN: no lock file, no protection, but the camera stays usable

    OVERLAPPED ov = {};
    ov.Offset = static_cast<DWORD>(kLockByte);
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                    static_cast<DWORD>(kLockLength), 0, &ov))
    {
        const DWORD err = GetLastError();
        CloseHandle(handle);
        // ERROR_LOCK_VIOLATION / ERROR_IO_PENDING mean somebody holds it. Anything else is a broken lock rather than
        // a busy camera, and a broken lock must not cost the caller its hardware.
        return !(err == ERROR_LOCK_VIOLATION || err == ERROR_IO_PENDING);
    }

    // Written past the locked range so a loser can read it. Not an error if it fails; it is only a diagnostic.
    OVERLAPPED wov = {};
    wov.Offset = static_cast<DWORD>(kPayloadAt);
    DWORD written = 0;
    WriteFile(handle, me.c_str(), static_cast<DWORD>(me.size()), &written, &wov);
    FlushFileBuffers(handle);

    held()[key] = handle;
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0)
        return true;    // FAILS OPEN, as above

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        const bool busy = (errno == EWOULDBLOCK || errno == EAGAIN);
        ::close(fd);
        return !busy;
    }

    ::pwrite(fd, me.c_str(), me.size(), static_cast<off_t>(kPayloadAt));
    ::fsync(fd);

    held()[key] = fd;
    return true;
#endif
}

void unlockCameraOnHost(types::CameraId id) noexcept
{
    const std::lock_guard<std::mutex> lock(holdMtx());

    const int key = types::toType(id);
    const auto it = held().find(key);
    if (it == held().end())
        return;         // idempotent, so a teardown path can call it unconditionally

    // Closing the handle releases the lock. Doing it explicitly rather than relying on process exit is what lets a
    // long-lived program connect, disconnect and let something else in without restarting.
#ifdef _WIN32
    CloseHandle(it->second);
#else
    ::close(it->second);
#endif
    held().erase(it);
}

std::string describeCameraHolder(types::CameraId id)
{
    {
        const std::lock_guard<std::mutex> lock(holdMtx());
        if (held().count(types::toType(id)) != 0)
            return std::string();   // us, which is never the interesting answer
    }

    const std::string path = lockPath(id);
    char buffer[256] = {0};

#ifdef _WIN32
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::string();

    // If the mutex byte can be taken, nobody holds the camera and whatever the payload says is a leftover from a
    // previous run. Reporting a holder that has gone is worse than reporting none.
    OVERLAPPED probe = {};
    probe.Offset = static_cast<DWORD>(kLockByte);
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                   static_cast<DWORD>(kLockLength), 0, &probe))
    {
        UnlockFileEx(handle, 0, static_cast<DWORD>(kLockLength), 0, &probe);
        CloseHandle(handle);
        return std::string();
    }

    OVERLAPPED rov = {};
    rov.Offset = static_cast<DWORD>(kPayloadAt);
    DWORD read = 0;
    const bool ok = ReadFile(handle, buffer, sizeof(buffer) - 1, &read, &rov) != 0;
    CloseHandle(handle);
    if (!ok || read == 0)
        return std::string();
    buffer[read] = '\0';
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return std::string();

    if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
    {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return std::string();
    }

    const ssize_t read = ::pread(fd, buffer, sizeof(buffer) - 1, static_cast<off_t>(kPayloadAt));
    ::close(fd);
    if (read <= 0)
        return std::string();
    buffer[read] = '\0';
#endif

    // The payload is fixed-length-free text; trim at the first control character so a short holder name is not
    // followed by whatever the previous, longer holder left behind.
    std::string holder(buffer);
    const std::size_t end = holder.find_first_of("\r\n");
    if (end != std::string::npos)
        holder.resize(end);
    return holder;
}

// ---------------------------------------------------------------------------------------------------------------------

}} // END NAMESPACES

// ---------------------------------------------------------------------------------------------------------------------
