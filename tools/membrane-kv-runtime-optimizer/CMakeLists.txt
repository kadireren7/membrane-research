add_executable(membrane-kv-runtime-optimizer main.cpp)
set_target_properties(membrane-kv-runtime-optimizer PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-runtime-optimizer PRIVATE membrane_core
  membrane_ggml_quant llama)
target_include_directories(membrane-kv-runtime-optimizer PRIVATE
  ${CMAKE_BINARY_DIR}/generated)
