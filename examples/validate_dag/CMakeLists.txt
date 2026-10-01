add_executable(Sub0Pipeline_ValidateDag main.cpp)
target_link_libraries(Sub0Pipeline_ValidateDag
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Headless
)
target_compile_features(Sub0Pipeline_ValidateDag PRIVATE cxx_std_23)
