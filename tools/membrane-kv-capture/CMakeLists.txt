add_executable(membrane-kv-capture main.cpp)
set_target_properties(membrane-kv-capture PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-capture PRIVATE membrane_core llama)
