add_executable(Sub0Pipeline_ErrorHandling main.cpp)
target_link_libraries(Sub0Pipeline_ErrorHandling
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Headless
)
target_compile_features(Sub0Pipeline_ErrorHandling PRIVATE cxx_std_23)
