add_executable(membrane-kv-trace-capture main.cpp)
set_target_properties(membrane-kv-trace-capture PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-trace-capture PRIVATE membrane_core llama)
