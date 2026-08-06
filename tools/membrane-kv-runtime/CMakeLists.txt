add_executable(membrane-kv-runtime main.cpp)
set_target_properties(membrane-kv-runtime PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-runtime PRIVATE membrane_core llama)
target_include_directories(membrane-kv-runtime PRIVATE
  ${CMAKE_BINARY_DIR}/generated)
