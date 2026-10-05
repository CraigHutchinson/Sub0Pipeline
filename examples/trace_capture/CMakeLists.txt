add_executable(Sub0Pipeline_TraceCapture main.cpp)
target_link_libraries(Sub0Pipeline_TraceCapture
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop
)
target_compile_features(Sub0Pipeline_TraceCapture PRIVATE cxx_std_23)
