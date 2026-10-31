# Archives the previous game session's ZHMModLoader.log before Visual Studio
# relaunches the game (which makes the SDK truncate it).
#
# Run as an install script; expects NMA_GAME_DIR and NMA_ARCHIVE_DIR to be set
# by a preceding install(CODE). The copy is RAW (unscrubbed, may contain the
# short-lived OAuth token when HTTP logging is on) — <project>/logs/ is
# gitignored and local-only. The in-mod LogMirror writes scrubbed copies.
#
# Files are named after the log's last-write time, so relaunching without
# playing (log unchanged) does not create duplicates.

if (NOT DEFINED NMA_GAME_DIR OR NOT DEFINED NMA_ARCHIVE_DIR)
    return()
endif()

foreach (_nma_log "${NMA_GAME_DIR}/ZHMModLoader.log" "${NMA_GAME_DIR}/Retail/ZHMModLoader.log")
    if (NOT EXISTS "${_nma_log}")
        continue()
    endif()

    file(SIZE "${_nma_log}" _nma_size)
    if (_nma_size EQUAL 0)
        continue()
    endif()

    file(TIMESTAMP "${_nma_log}" _nma_ts "%Y%m%d-%H%M%S")
    set(_nma_dst "${NMA_ARCHIVE_DIR}/session-${_nma_ts}.log")
    if (EXISTS "${_nma_dst}")
        message(STATUS "NoMoreAltF4: previous game log already archived (${_nma_dst})")
        continue()
    endif()

    file(MAKE_DIRECTORY "${NMA_ARCHIVE_DIR}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy "${_nma_log}" "${_nma_dst}"
        RESULT_VARIABLE _nma_rc
    )
    if (_nma_rc EQUAL 0)
        message(STATUS "NoMoreAltF4: archived previous game log -> ${_nma_dst}")
    else()
        message(WARNING "NoMoreAltF4: failed to archive ${_nma_log} (exit ${_nma_rc})")
    endif()
endforeach()
