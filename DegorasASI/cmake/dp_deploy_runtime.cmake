# ----------------------------------------------------------------------------------------------------------------------
# dp_deploy_runtime.cmake -- resolve and copy an executable's whole runtime DLL closure next to it.
#
# Run in SCRIPT MODE from a POST_BUILD step:
#
#   ${CMAKE_COMMAND} -DDP_BINARY=<exe> -DDP_DEST=<dir> -DDP_SEARCH_DIRS=<a;b;c> [-DDP_MODULES=<plugin;...>]
#                    -P <this file>
#
# WHY NOT $<TARGET_RUNTIME_DLLS:tgt>. That generator expression only knows about IMPORTED CMake targets in the link
# closure. Qt and OpenCV link most of their own dependencies by absolute path rather than through imported targets,
# so it silently misses them: measured on this project it produced 18 DLLs and left out icu*, harfbuzz, freetype,
# png, zstd, brotli, pcre2, double-conversion, md4c, tbb and the image codecs. That is not merely incomplete -- SIX
# of those names (libharfbuzz-0, libzstd, libbrotlicommon, libbrotlidec, libmd4c, libdouble-conversion) also exist
# in the MSYS2 prefix, so the gap is exactly where a vcpkg-built Qt could end up loading MSYS2's copy of its own
# dependency. file(GET_RUNTIME_DEPENDENCIES) reads the PE import tables recursively instead, so the closure is
# derived from the binaries themselves and there is no list to maintain or to drift.
#
# DP_SEARCH_DIRS IS AN ORDERED PREFERENCE, not just a set of places to look: the first directory that has a given
# DLL name wins. Pass the package prefix before the compiler prefix, and provenance is decided here rather than by
# whatever PATH happens to look like at run time.
# ----------------------------------------------------------------------------------------------------------------------

# Script mode needs its own minimum, and CMP0207 needs to be NEW: it makes file(GET_RUNTIME_DEPENDENCIES) normalise
# paths to forward slashes before matching, which stops it warning on every backslash path an import table yields
# (C:\Windows\system32/kernel32.dll and friends).
cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT DP_BINARY OR NOT DP_DEST)
    message(FATAL_ERROR "dp_deploy_runtime.cmake: DP_BINARY and DP_DEST are required.")
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${DP_BINARY}
    MODULES ${DP_MODULES}
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    CONFLICTING_DEPENDENCIES_PREFIX _conflict
    DIRECTORIES ${DP_SEARCH_DIRS}
    # The API sets are the OS itself and are never deployed. Dropping them PRE avoids resolving them at all.
    PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*" "hvsifiletrust" "pdmutilities")

# Anything that resolved inside the Windows directory is an OS DLL and must NOT be copied: putting kernel32.dll next
# to an executable ranges from useless to actively harmful.
#
# Done with an explicit prefix test rather than POST_EXCLUDE_REGEXES, because CMake's regex engine has no
# case-insensitive flag. "(?i)" is not a CMake construct -- it is matched literally -- so a regex written that way
# never matches and every system DLL gets copied, silently. Windows also reports these paths with inconsistent case
# (C:/Windows/system32/kernel32.dll alongside C:/Windows/system32/DWrite.dll), so the comparison is lowered.
file(TO_CMAKE_PATH "$ENV{SystemRoot}" _win_root)
if(NOT _win_root)
    set(_win_root "C:/Windows")
endif()
string(TOLOWER "${_win_root}/" _win_root_l)

get_filename_component(_dest_real "${DP_DEST}" REALPATH)
string(TOLOWER "${_dest_real}" _dest_real_l)

# CONFLICTS ARE RESOLUTIONS, not failures. A name found in more than one search directory lands in
# _conflict_<name> instead of _resolved, and if it is left there it never gets copied -- which matters on the second
# and later builds, because the destination is itself a search directory, so everything already deployed becomes
# "conflicting" and would never be refreshed again. A vcpkg upgrade would then leave a stale DLL beside new headers,
# which is a genuinely nasty way to spend an afternoon. DP_SEARCH_DIRS is an ordered preference precisely so that
# picking the first candidate is the right answer, so fold the winners back in.
set(_to_copy ${_resolved})
foreach(_name IN LISTS _conflict_FILENAMES)
    foreach(_cand IN LISTS _conflict_${_name})
        string(TOLOWER "${_cand}" _cand_l)
        if(NOT _cand_l MATCHES "^${_win_root_l}")
            list(APPEND _to_copy "${_cand}")
            break()
        endif()
    endforeach()
endforeach()

set(_copied 0)
set(_skipped 0)
foreach(_dll IN LISTS _to_copy)
    string(TOLOWER "${_dll}" _dll_l)
    if(_dll_l MATCHES "^${_win_root_l}")
        math(EXPR _skipped "${_skipped} + 1")
        continue()
    endif()
    get_filename_component(_dll_dir "${_dll}" DIRECTORY)
    get_filename_component(_dll_dir "${_dll_dir}" REALPATH)
    string(TOLOWER "${_dll_dir}" _dll_dir_l)
    if(_dll_dir_l STREQUAL _dest_real_l)
        continue()   # already where it needs to be
    endif()
    file(COPY "${_dll}" DESTINATION "${DP_DEST}" FOLLOW_SYMLINK_CHAIN)
    math(EXPR _copied "${_copied} + 1")
endforeach()

message(STATUS "[DEGORAS] deployed ${_copied} runtime DLLs to ${DP_DEST} (${_skipped} OS DLLs left alone)")

# A conflict means two search directories both offer the same DLL name. The first one won, which is the intended
# behaviour, but a genuine one is worth saying out loud: it means a package exists in both the vcpkg and the MSYS2
# prefix, and going unnoticed is exactly how that becomes a day of confusion.
#
# Two kinds are not genuine and would bury the real ones. OS DLLs, which are skipped above anyway. And conflicts won
# by the DESTINATION itself: the destination is a search directory too, because the library being deployed alongside
# lives there, so from the second build onwards everything already copied is legitimately found there as well.
if(_conflict_FILENAMES)
    foreach(_name IN LISTS _conflict_FILENAMES)
        list(GET _conflict_${_name} 0 _winner)
        get_filename_component(_winner_dir "${_winner}" DIRECTORY)
        get_filename_component(_winner_dir "${_winner_dir}" REALPATH)
        string(TOLOWER "${_winner_dir}" _winner_dir_l)
        string(TOLOWER "${_winner}" _winner_l)
        if(NOT _winner_l MATCHES "^${_win_root_l}" AND NOT _winner_dir_l STREQUAL _dest_real_l)
            message(STATUS "[DEGORAS] ${_name}: in more than one search directory, took ${_winner}")
        endif()
    endforeach()
endif()

# Unresolved names are only a problem if they are not part of the OS. The PRE excludes drop the API sets, so anything
# left here is a genuinely missing dependency and the deployed tree will not run without help from PATH.
if(_unresolved)
    message(WARNING "[DEGORAS] unresolved runtime dependencies (the deployed tree is NOT self-contained): "
                    "${_unresolved}")
endif()

# **********************************************************************************************************************
