cmake_minimum_required(VERSION 3.16)

# This project builds offline. Every third-party component is committed under
# components/ instead of being fetched from the ESP component registry, and
# there is deliberately no idf_component.yml declaring registry dependencies.
#
# Fail here with something readable rather than a hundred lines of "esp_camera.h:
# No such file or directory" if that tree ever goes missing.
foreach(vendored esp32-camera esp-modbus)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/${vendored}/CMakeLists.txt")
        message(FATAL_ERROR
            "components/${vendored} is missing.\n"
            "Restore it from version control, or regenerate the vendored tree with\n"
            "    pwsh tools/vendor_components.ps1\n"
            "which is the only step in this project that needs network access.")
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_cam_gateway)

idf_build_get_property(idf_ver IDF_VER)

# The manifest that used to carry this requirement is gone, so the check lives
# here. Guarded by the regex: an unusual version string just skips it rather
# than failing a build that would have worked.
if(idf_ver MATCHES "^v([0-9]+)\\.([0-9]+)")
    if("${CMAKE_MATCH_1}.${CMAKE_MATCH_2}" VERSION_LESS "5.1")
        message(FATAL_ERROR "ESP-IDF 5.1 or newer is required, found ${idf_ver}.")
    endif()
endif()

message(STATUS "esp32s3_cam_gateway: offline build, components/ vendored, IDF ${idf_ver}")
