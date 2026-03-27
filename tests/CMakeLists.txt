set(VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# ── Unit tests ────────────────────────────────────────────────────────────────

add_executable(Sub0Pipeline_Tests
    test_main.cpp
    test_pipeline.cpp
    test_failure.cpp
    test_validation.cpp
    test_observer.cpp
    test_concurrent.cpp
)

target_link_libraries(Sub0Pipeline_Tests
    PRIVATE
        Sub0Pipeline::Sub0Pipeline
        Sub0Pipeline::Desktop
        Sub0Pipeline::Headless
)

target_include_directories(Sub0Pipeline_Tests
    PRIVATE "${VENDOR_DIR}"
)

target_compile_features(Sub0Pipeline_Tests PRIVATE cxx_std_23)

add_test(NAME Sub0Pipeline_Tests COMMAND Sub0Pipeline_Tests)

# ── DSL tests ─────────────────────────────────────────────────────────────────

add_executable(Sub0Pipeline_DslTests test_dsl.cpp)
target_link_libraries(Sub0Pipeline_DslTests
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Headless
)
target_include_directories(Sub0Pipeline_DslTests
    PRIVATE "${VENDOR_DIR}"
)
target_compile_features(Sub0Pipeline_DslTests PRIVATE cxx_std_23)
add_test(NAME Sub0Pipeline_DslTests COMMAND Sub0Pipeline_DslTests)

# ── Benchmarks (not run by ctest — run manually) ──────────────────────────────

if(SUB0PIPELINE_BUILD_BENCHMARKS)
    add_executable(Sub0Pipeline_Bench bench_pipeline.cpp)
    target_link_libraries(Sub0Pipeline_Bench
        PRIVATE
            Sub0Pipeline::Sub0Pipeline
            Sub0Pipeline::Desktop
            Sub0Pipeline::Headless
    )
    target_include_directories(Sub0Pipeline_Bench
        PRIVATE "${VENDOR_DIR}"
    )
    target_compile_features(Sub0Pipeline_Bench PRIVATE cxx_std_23)
endif()
