# Match PicoUart's local-build version policy; runnable without the SDK via cmake -P.
if(NOT PF_FIRMWARE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-(dev|rc[1-9][0-9]*))?$")
    message(FATAL_ERROR
        "PF_FIRMWARE_VERSION must be MAJOR.MINOR.PATCH with optional -dev or -rcN "
        "(got '${PF_FIRMWARE_VERSION}')")
endif()
set(PF_FIRMWARE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PF_FIRMWARE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PF_FIRMWARE_VERSION_PATCH "${CMAKE_MATCH_3}")

foreach(part MAJOR MINOR PATCH)
    set(value "${PF_FIRMWARE_VERSION_${part}}")
    string(LENGTH "${value}" length)
    if(length GREATER 3 OR value GREATER 255)
        message(FATAL_ERROR "PF_FIRMWARE_VERSION ${part} must be 0-255 (got '${value}')")
    endif()
endforeach()

# USB BCD encodes major.minor only. Larger versions are valid for local builds.
if(PF_FIRMWARE_VERSION_MAJOR GREATER 99 OR PF_FIRMWARE_VERSION_MINOR GREATER 99)
    message(WARNING "PF_FIRMWARE_VERSION major/minor > 99: USB bcdDevice will be 0x0000")
    set(PF_BCD_DEVICE 0)
else()
    math(EXPR PF_BCD_DEVICE
        "((${PF_FIRMWARE_VERSION_MAJOR} / 10) * 4096) + ((${PF_FIRMWARE_VERSION_MAJOR} % 10) * 256) + ((${PF_FIRMWARE_VERSION_MINOR} / 10) * 16) + (${PF_FIRMWARE_VERSION_MINOR} % 10)")
endif()

if(CMAKE_SCRIPT_MODE_FILE)
    message("RESULT major=${PF_FIRMWARE_VERSION_MAJOR} minor=${PF_FIRMWARE_VERSION_MINOR} patch=${PF_FIRMWARE_VERSION_PATCH} bcd=${PF_BCD_DEVICE}")
endif()
