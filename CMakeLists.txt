cmake_minimum_required(VERSION 3.21)

# Detect if Sub0Pipeline is being included inside another project
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0Pipeline VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0PIPELINE_BUILD_TESTING  "Build unit-tests"   ON)
option(SUB0PIPELINE_BUILD_EXAMPLES "Build examples"     OFF)
option(SUB0PIPELINE_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(SUB0PIPELINE_PLATFORM_DESKTOP  "Build DesktopExecutor (std::thread)" ON)

if(SUB0PIPELINE_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SUB0PIPELINE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── Core library ─────────────────────────────────────────────────────────────

add_library(Sub0Pipeline STATIC
    src/sub0pipeline.cpp
)
add_library(Sub0Pipeline::Sub0Pipeline ALIAS Sub0Pipeline)

target_compile_features(Sub0Pipeline PUBLIC cxx_std_23)

target_compile_options(Sub0Pipeline PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /Zc:preprocessor>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

target_include_directories(Sub0Pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# ── Platform executors ────────────────────────────────────────────────────────

if(SUB0PIPELINE_PLATFORM_DESKTOP)
    add_subdirectory(platform/desktop)
endif()

add_subdirectory(platform/headless)

# ── Install ───────────────────────────────────────────────────────────────────

if(NOT IS_SUBPROJECT)

    include(CMakePackageConfigHelpers)

    set(SUB0PIPELINE_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Sub0Pipeline")

    configure_package_config_file(
        "${CMAKE_CURRENT_LIST_DIR}/cmake/Sub0PipelineConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Sub0PipelineConfig.cmake"
        INSTALL_DESTINATION
            "${SUB0PIPELINE_CMAKE_CONFIG_DESTINATION}"
    )

    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/Sub0PipelineConfigVersion.cmake"
        COMPATIBILITY
            SameMajorVersion
    )

    install(
        TARGETS
            Sub0Pipeline
        EXPORT
            Sub0PipelineTargets
        DESTINATION
            ${CMAKE_INSTALL_LIBDIR}
    )

    install(
        EXPORT
            Sub0PipelineTargets
        NAMESPACE
            Sub0Pipeline::
        DESTINATION
            ${SUB0PIPELINE_CMAKE_CONFIG_DESTINATION}
    )

    install(
        FILES
            "${PROJECT_BINARY_DIR}/Sub0PipelineConfigVersion.cmake"
            "${PROJECT_BINARY_DIR}/Sub0PipelineConfig.cmake"
        DESTINATION
            "${SUB0PIPELINE_CMAKE_CONFIG_DESTINATION}"
    )

    install(
        DIRECTORY
            "include/"
        DESTINATION
            "${CMAKE_INSTALL_INCLUDEDIR}"
    )

endif()
