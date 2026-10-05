add_executable(Sub0Pipeline_TraceCapture main.cpp)
target_link_libraries(Sub0Pipeline_TraceCapture
    PRIVATE Sub0Pipeline::Sub0Pipeline Sub0Pipeline::Desktop
)
target_compile_features(Sub0Pipeline_TraceCapture PRIVATE cxx_std_23)

if(SUB0PIPELINE_BUILD_TESTING AND NOT IS_SUBPROJECT)
    add_test(NAME Sub0Pipeline_TraceCapture COMMAND Sub0Pipeline_TraceCapture)
    add_test(NAME Sub0Pipeline_TraceCaptureBoot COMMAND Sub0Pipeline_TraceCapture boot)
    add_test(NAME Sub0Pipeline_TraceCaptureFailure COMMAND Sub0Pipeline_TraceCapture failure)
endif()
