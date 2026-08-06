add_library(membrane_exactsim_core STATIC
  calibrate.cpp
  exact_engine.cpp
  bounded_quantile.cpp
  mem_guard.cpp
)
set_target_properties(membrane_exactsim_core PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_include_directories(membrane_exactsim_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(membrane_exactsim_core PUBLIC
  membrane_wssim_core
  membrane_cxlsim_core)

add_executable(membrane-kv-exact-sim main.cpp)
set_target_properties(membrane-kv-exact-sim PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-kv-exact-sim PRIVATE membrane_exactsim_core)

# Phase 6.5 item 11: standalone CSV/checkpoint integrity checker +
# repair tool -- deliberately only depends on membrane_core (sha256,
# CRC32) + checkpoint.h, not the simulator itself, so it stays usable
# even against artifacts from an older/newer binary.
add_executable(membrane-kv-exact-sim-verify integrity_tool.cpp)
set_target_properties(membrane-kv-exact-sim-verify PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_include_directories(membrane-kv-exact-sim-verify PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/membrane-kv-workingset-sim)
target_link_libraries(membrane-kv-exact-sim-verify PRIVATE membrane_core)
