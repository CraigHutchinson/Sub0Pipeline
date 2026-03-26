# platform/esp32p4/CMakeLists.txt
# FreeRTOS executor for Sub0Pipeline on ESP32-P4 dual-core RISC-V.
#
# This component is intended for ESP-IDF builds only.
# For standalone/desktop builds, use platform/desktop instead.

if(DEFINED COMPONENT_TARGET)
    # ESP-IDF component build path
    idf_component_include(sub0pipeline)
    idf_component_include(freertos)

    add_library(${COMPONENT_TARGET} STATIC "freertos_executor.cpp")
    target_include_directories(${COMPONENT_TARGET}
        PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include"
    )
    target_link_libraries(${COMPONENT_TARGET}
        PUBLIC  idf::sub0pipeline
        PRIVATE idf::freertos
                idf::log
    )
else()
    # Standalone build: skip (no FreeRTOS headers available)
    message(STATUS "Sub0Pipeline: esp32p4 executor skipped (no IDF environment)")
endif()
