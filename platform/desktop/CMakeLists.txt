# platform/desktop/CMakeLists.txt
# std::thread-based executor for Sub0Pipeline on desktop platforms.

add_library(Sub0Pipeline_Desktop STATIC "desktop_executor.cpp")
add_library(Sub0Pipeline::Desktop ALIAS Sub0Pipeline_Desktop)

target_link_libraries(Sub0Pipeline_Desktop
    PUBLIC Sub0Pipeline::Sub0Pipeline
)

target_compile_features(Sub0Pipeline_Desktop PUBLIC cxx_std_23)

target_compile_options(Sub0Pipeline_Desktop PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /Zc:preprocessor>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
