add_executable(Sub0Pipeline_JobOptions main.cpp)
target_link_libraries(Sub0Pipeline_JobOptions
    PRIVATE Sub0Pipeline::Sub0Pipeline
)
target_compile_features(Sub0Pipeline_JobOptions PRIVATE cxx_std_23)
