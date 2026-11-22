# Workaround for upstream espressif__esp-modbus under GCC 14 + -O2:
# managed_components/espressif__esp-modbus/CMakeLists.txt appends private
# -Werror, causing port_tcp_utils.c:685 to fail with -Werror=stringop-truncation
# when CONFIG_COMPILER_OPTIMIZATION_PERF=y (-O2) is enabled.
# Loaded via -DCMAKE_PROJECT_INCLUDE from platformio.ini so fresh clones work
# without modifying the gitignored auto-generated CMakeLists.txt.
function(_tesla_fsd_relax_esp_modbus)
    if(TARGET __idf_espressif__esp-modbus)
        target_compile_options(__idf_espressif__esp-modbus PRIVATE -Wno-stringop-truncation)
    endif()
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _tesla_fsd_relax_esp_modbus)
