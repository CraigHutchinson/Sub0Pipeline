add_executable(Sub0Pipeline_OnDemandJobs main.cpp)
target_link_libraries(Sub0Pipeline_OnDemandJobs
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Headless
)
target_compile_features(Sub0Pipeline_OnDemandJobs PRIVATE cxx_std_23)
