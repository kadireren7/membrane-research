add_library(membrane_wssim_core STATIC
  attn_workload.cpp
  attn_trace_reader.cpp
  policy.cpp
  hotcache.cpp
  engine.cpp
)
set_target_properties(membrane_wssim_core PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_include_directories(membrane_wssim_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tools/membrane-cxl-sim)
target_link_libraries(membrane_wssim_core PUBLIC membrane_core)

add_executable(membrane-kv-workingset-sim main.cpp)
set_target_properties(membrane-kv-workingset-sim PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-workingset-sim PRIVATE membrane_wssim_core)
