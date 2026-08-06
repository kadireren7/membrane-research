add_executable(membrane-kv-variance main.cpp)
set_target_properties(membrane-kv-variance PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-variance PRIVATE membrane_core
  membrane_ggml_quant llama)
target_include_directories(membrane-kv-variance PRIVATE
  ${CMAKE_BINARY_DIR}/generated)
