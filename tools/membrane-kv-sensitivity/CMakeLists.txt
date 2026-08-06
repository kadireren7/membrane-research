add_executable(membrane-kv-sensitivity main.cpp)
set_target_properties(membrane-kv-sensitivity PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-sensitivity PRIVATE membrane_core
  membrane_ggml_quant llama)
