add_executable(membrane-kv-attn-quality main.cpp)
set_target_properties(membrane-kv-attn-quality PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_include_directories(membrane-kv-attn-quality PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/ggml/include)
target_link_libraries(membrane-kv-attn-quality PRIVATE llama ggml)
