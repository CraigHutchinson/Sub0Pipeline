set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# ── Unit tests ────────────────────────────────────────────────────────────────

add_executable(Sub0Pipeline_Tests
    test_main.cpp
    test_pipeline.cpp
    test_failure.cpp
    test_rerun.cpp
    test_ondemand.cpp
    test_cancel.cpp
    test_deadline.cpp
    test_pool.cpp
    test_validation.cpp
    test_observer.cpp
    test_concurrent.cpp
    test_priority_executor.cpp
    test_executors.cpp
)

target_link_libraries(Sub0Pipeline_Tests
    PRIVATE
        Sub0Pipeline::Sub0Pipeline
        Sub0Pipeline::Default
        Sub0Pipeline::Desktop
        Sub0Pipeline::Priority
)

target_include_directories(Sub0Pipeline_Tests
    PRIVATE "${VENDOR_DIR}"
)

target_compile_features(Sub0Pipeline_Tests PRIVATE cxx_std_23)

add_test(NAME Sub0Pipeline_Tests COMMAND Sub0Pipeline_Tests)

if(SUB0PIPELINE_EXCEPTIONS)
    add_executable(Sub0Pipeline_SubmissionTests test_main.cpp test_submission.cpp)
    target_link_libraries(Sub0Pipeline_SubmissionTests PRIVATE
        Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop Sub0Pipeline::Priority)
    target_include_directories(Sub0Pipeline_SubmissionTests PRIVATE "${VENDOR_DIR}")
    target_compile_features(Sub0Pipeline_SubmissionTests PRIVATE cxx_std_23)
    add_test(NAME Sub0Pipeline_SubmissionTests COMMAND Sub0Pipeline_SubmissionTests)
    set_tests_properties(Sub0Pipeline_SubmissionTests PROPERTIES TIMEOUT 30)

    # Replacement allocation operators are isolated from all other suites.
    add_executable(Sub0Pipeline_SubmissionAllocationTests
        test_main.cpp test_submission_allocations.cpp)
    target_link_libraries(Sub0Pipeline_SubmissionAllocationTests PRIVATE
        Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Priority)
    target_include_directories(Sub0Pipeline_SubmissionAllocationTests PRIVATE "${VENDOR_DIR}")
    target_compile_features(Sub0Pipeline_SubmissionAllocationTests PRIVATE cxx_std_23)
    add_test(NAME Sub0Pipeline_SubmissionAllocationTests COMMAND Sub0Pipeline_SubmissionAllocationTests)
    set_tests_properties(Sub0Pipeline_SubmissionAllocationTests PROPERTIES TIMEOUT 30)

    add_executable(Sub0Pipeline_FreeRtosSubmissionTests test_main.cpp test_freertos_submission.cpp
        "${PROJECT_SOURCE_DIR}/platform/esp32p4/freertos_executor.cpp")
    target_include_directories(Sub0Pipeline_FreeRtosSubmissionTests PRIVATE
        "${VENDOR_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/host_mocks")
    target_link_libraries(Sub0Pipeline_FreeRtosSubmissionTests PRIVATE Sub0Pipeline::Sub0Pipeline)
    add_test(NAME Sub0Pipeline_FreeRtosSubmissionTests COMMAND Sub0Pipeline_FreeRtosSubmissionTests)
    set_tests_properties(Sub0Pipeline_FreeRtosSubmissionTests PROPERTIES TIMEOUT 30)

    add_executable(Sub0Pipeline_QtSubmissionHostTests test_main.cpp test_qt_submission.cpp)
    target_include_directories(Sub0Pipeline_QtSubmissionHostTests PRIVATE "${VENDOR_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/host_mocks/qt" "${PROJECT_SOURCE_DIR}/examples/qt_bounded")
    target_link_libraries(Sub0Pipeline_QtSubmissionHostTests PRIVATE Sub0Pipeline::Sub0Pipeline)
    add_test(NAME Sub0Pipeline_QtSubmissionHostTests COMMAND Sub0Pipeline_QtSubmissionHostTests)
    set_tests_properties(Sub0Pipeline_QtSubmissionHostTests PROPERTIES TIMEOUT 30)
endif()

# ── DSL tests ─────────────────────────────────────────────────────────────────

add_executable(Sub0Pipeline_DslTests test_dsl.cpp)
target_link_libraries(Sub0Pipeline_DslTests
    PRIVATE Sub0Pipeline::Sub0Pipeline
)
target_include_directories(Sub0Pipeline_DslTests
    PRIVATE "${VENDOR_DIR}"
)
target_compile_features(Sub0Pipeline_DslTests PRIVATE cxx_std_23)
add_test(NAME Sub0Pipeline_DslTests COMMAND Sub0Pipeline_DslTests)

# ── Header self-containment ───────────────────────────────────────────────────
# One translation unit per public header, each including only that header (twice,
# to exercise its include guard). A header that leans on another's transitive
# includes fails this build.

file(GLOB_RECURSE SUB0PIPELINE_PUBLIC_HEADERS CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include/sub0pipeline/*.hpp")
set(SUB0PIPELINE_HEADER_CHECK_SOURCES "")
foreach(header IN LISTS SUB0PIPELINE_PUBLIC_HEADERS)
    string(REPLACE ".hpp" ".cpp" check_name "${header}")
    set(check_source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${check_name}")
    file(CONFIGURE OUTPUT "${check_source}" CONTENT
        "#include <${header}>
#include <${header}>
")
    list(APPEND SUB0PIPELINE_HEADER_CHECK_SOURCES "${check_source}")
endforeach()

add_library(Sub0Pipeline_HeaderCheck OBJECT ${SUB0PIPELINE_HEADER_CHECK_SOURCES})
target_link_libraries(Sub0Pipeline_HeaderCheck PRIVATE Sub0Pipeline::Sub0Pipeline)

# ── Benchmarks (not run by ctest — run manually) ──────────────────────────────

if(SUB0PIPELINE_BUILD_BENCHMARKS)
    # Replaces global new/delete in this standalone executable only.
    if(SUB0PIPELINE_EXCEPTIONS)
        add_executable(Sub0Pipeline_AllocationAudit audit_allocations.cpp)
        target_link_libraries(Sub0Pipeline_AllocationAudit PRIVATE Sub0Pipeline::Sub0Pipeline)
    endif()

    add_executable(Sub0Pipeline_Bench bench_pipeline.cpp)
    target_link_libraries(Sub0Pipeline_Bench
        PRIVATE
            Sub0Pipeline::Sub0Pipeline
            Sub0Pipeline::Desktop
            Sub0Pipeline::Priority
    )
    target_include_directories(Sub0Pipeline_Bench
        PRIVATE "${VENDOR_DIR}"
    )
    target_compile_features(Sub0Pipeline_Bench PRIVATE cxx_std_23)
endif()
