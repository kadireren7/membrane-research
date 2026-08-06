add_executable(membrane-kv-quality main.cpp)
set_target_properties(membrane-kv-quality PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-quality PRIVATE membrane_core llama)
