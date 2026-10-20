add_executable(Sub0Pipeline_RemoteSubgraph main.cpp)
target_link_libraries(Sub0Pipeline_RemoteSubgraph
    PRIVATE Sub0Pipeline::Remote Sub0Pipeline::Priority
)
target_compile_features(Sub0Pipeline_RemoteSubgraph PRIVATE cxx_std_23)
