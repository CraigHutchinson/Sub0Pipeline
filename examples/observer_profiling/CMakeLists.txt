add_executable(Sub0Pipeline_ObserverProfiling main.cpp)
target_link_libraries(Sub0Pipeline_ObserverProfiling
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop
)
target_compile_features(Sub0Pipeline_ObserverProfiling PRIVATE cxx_std_23)
