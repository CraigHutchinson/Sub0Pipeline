# platform/headless/CMakeLists.txt
# Inline sequential executor — no threading, deterministic, for unit tests.

add_library(Sub0Pipeline_Headless STATIC "sequential_executor.cpp")
add_library(Sub0Pipeline::Headless ALIAS Sub0Pipeline_Headless)

target_link_libraries(Sub0Pipeline_Headless
    PUBLIC Sub0Pipeline::Sub0Pipeline
)

target_compile_features(Sub0Pipeline_Headless PUBLIC cxx_std_23)

target_compile_options(Sub0Pipeline_Headless PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /Zc:preprocessor>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
