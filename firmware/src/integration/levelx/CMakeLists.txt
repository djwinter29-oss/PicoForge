set(PF_LEVELX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/levelx")
if(NOT EXISTS "${PF_LEVELX_ROOT}/common/inc/lx_api.h")
    message(FATAL_ERROR "Initialize the pinned LevelX submodule: git submodule update --init firmware/third_party/levelx")
endif()
file(GLOB PF_LEVELX_SOURCES CONFIGURE_DEPENDS "${PF_LEVELX_ROOT}/common/src/lx_nor_flash_*.c")
list(FILTER PF_LEVELX_SOURCES EXCLUDE REGEX "/lx_nor_flash_simulator\\.c$")
add_library(pf_levelx_vendor STATIC ${PF_LEVELX_SOURCES})
target_include_directories(pf_levelx_vendor PUBLIC
    "${PF_LEVELX_ROOT}/common/inc" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(pf_levelx_vendor PUBLIC LX_INCLUDE_USER_DEFINE_FILE)

add_library(pf_levelx_nor STATIC levelx_nor.c)
target_include_directories(pf_levelx_nor PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pf_levelx_nor PUBLIC pf_levelx_vendor pf_nor_flash)
