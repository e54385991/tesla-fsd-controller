# Workaround for upstream component warnings under GCC 15 + -O2 / C23:
# 1) managed_components/espressif__esp-modbus/CMakeLists.txt appends private
#    -Werror, causing port_tcp_utils.c:685 to fail with -Werror=stringop-truncation
#    when CONFIG_COMPILER_OPTIMIZATION_PERF=y (-O2) is enabled.
# 2) managed_components/espressif__cbor (tinycbor) defines unreachable() in
#    compilersupport_p.h, colliding with GCC 15 C23 <stddef.h>; its own
#    SHELL:-include unreachable_fix.h does not propagate through PlatformIO's
#    SCons bridge. Compiling only this unused component as gnu17 avoids the
#    C23 stddef.h macro cleanly.
# 3) framework-arduinoespressif32 compiles unused ESP_SR / NetworkClientSecure
#    #warning directives (-Wcpp) and Esp.cpp includes C99 HAL designated
#    initializers under C++ (-Wmissing-field-initializers).
# Loaded via -DCMAKE_PROJECT_tesla-fsd-controller_INCLUDE from platformio.ini so
# fresh clones work without modifying the gitignored auto-generated CMakeLists.txt.
# Project sources in src/ keep strict -Wall -Wextra -Werror via platformio.ini.
function(_tesla_fsd_relax_upstream_warnings)
    if(TARGET __idf_espressif__esp-modbus)
        target_compile_options(__idf_espressif__esp-modbus PRIVATE -Wno-stringop-truncation)
    endif()
    if(TARGET __idf_espressif__cbor)
        target_compile_options(__idf_espressif__cbor PRIVATE -std=gnu17)
    endif()
    idf_build_get_property(_components BUILD_COMPONENTS)
    foreach(_comp IN LISTS _components)
        if(_comp MATCHES "^framework-arduinoespressif32" AND TARGET "__idf_${_comp}")
            target_compile_options("__idf_${_comp}" PRIVATE
                -Wno-missing-field-initializers
                -Wno-deprecated-declarations
                -Wno-cpp)
        endif()
    endforeach()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _tesla_fsd_relax_upstream_warnings)
