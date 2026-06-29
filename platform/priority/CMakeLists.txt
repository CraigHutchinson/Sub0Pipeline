add_library(Sub0Pipeline_Priority STATIC priority_executor.cpp)
add_library(Sub0Pipeline::Priority ALIAS Sub0Pipeline_Priority)

target_compile_features(Sub0Pipeline_Priority PUBLIC cxx_std_23)
target_link_libraries(Sub0Pipeline_Priority PUBLIC Sub0Pipeline::Sub0Pipeline)

target_compile_options(Sub0Pipeline_Priority PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /Zc:preprocessor>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
