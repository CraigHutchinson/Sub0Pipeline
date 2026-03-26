add_executable(Sub0Pipeline_MinimalPipeline main.cpp)
target_link_libraries(Sub0Pipeline_MinimalPipeline
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Headless
)
target_compile_features(Sub0Pipeline_MinimalPipeline PRIVATE cxx_std_23)
