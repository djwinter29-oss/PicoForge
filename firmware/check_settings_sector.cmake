# Post-link guard: fail if the firmware image reaches the reserved settings sector (last sector of flash).
# Inputs: PF_FLASH_SIZE_BYTES (CMake arithmetic, physical size), PF_SECTOR_SIZE, and either PF_IMAGE_END or
# PF_NM + PF_ELF (reads __flash_binary_end). The linker FLASH region is also shortened by one sector, so this
# independently confirms the linked image.
if(NOT DEFINED PF_IMAGE_END)
    execute_process(COMMAND "${PF_NM}" "${PF_ELF}" OUTPUT_VARIABLE pf_symbols RESULT_VARIABLE pf_result)
    if(NOT pf_result EQUAL 0 OR NOT pf_symbols MATCHES "([0-9a-fA-F]+) [A-Za-z] __flash_binary_end")
        message(FATAL_ERROR "settings sector guard: cannot read __flash_binary_end from ${PF_ELF}")
    endif()
    set(PF_IMAGE_END "0x${CMAKE_MATCH_1}")
endif()
math(EXPR pf_image_end "${PF_IMAGE_END}")
math(EXPR pf_settings_start "0x10000000 + (${PF_FLASH_SIZE_BYTES}) - ${PF_SECTOR_SIZE}")
if(pf_image_end GREATER pf_settings_start)
    message(FATAL_ERROR "firmware image end ${pf_image_end} overlaps the reserved settings sector at ${pf_settings_start}")
endif()
message(STATUS "settings sector guard: image ends at ${pf_image_end}, reserved sector starts at ${pf_settings_start}")
