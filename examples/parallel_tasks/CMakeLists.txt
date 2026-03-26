add_executable(Sub0Pipeline_ParallelTasks main.cpp)
target_link_libraries(Sub0Pipeline_ParallelTasks
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop
)
target_compile_features(Sub0Pipeline_ParallelTasks PRIVATE cxx_std_23)
