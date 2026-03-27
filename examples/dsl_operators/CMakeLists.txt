add_executable(Sub0Pipeline_DslOperators main.cpp)
target_link_libraries(Sub0Pipeline_DslOperators
    PRIVATE Sub0Pipeline::DSL Sub0Pipeline::Desktop
)
target_compile_features(Sub0Pipeline_DslOperators PRIVATE cxx_std_23)
