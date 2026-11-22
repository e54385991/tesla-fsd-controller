# Workaround for upstream component warnings under GCC 14 + -O2:
# 1) managed_components/espressif__esp-modbus/CMakeLists.txt appends private
#    -Werror, causing port_tcp_utils.c:685 to fail with -Werror=stringop-truncation
#    when CONFIG_COMPILER_OPTIMIZATION_PERF=y (-O2) is enabled.
# 2) framework-arduinoespressif32 compiles unused ESP_SR / NetworkClientSecure
#    #warning directives (-Wcpp) and Esp.cpp includes C99 HAL designated
#    initializers under C++ (-Wmissing-field-initializers).
# Loaded via -DCMAKE_PROJECT_INCLUDE from platformio.ini so fresh clones work
# without modifying the gitignored auto-generated CMakeLists.txt.
# Project sources in src/ keep strict -Wall -Wextra -Werror via platformio.ini.
function(_tesla_fsd_relax_upstream_warnings)
    if(TARGET __idf_espressif__esp-modbus)
        target_compile_options(__idf_espressif__esp-modbus PRIVATE -Wno-stringop-truncation)
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
