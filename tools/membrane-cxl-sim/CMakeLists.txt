add_library(membrane_cxlsim_core STATIC
  sim_engine.cpp
  workload.cpp
)
set_target_properties(membrane_cxlsim_core PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_include_directories(membrane_cxlsim_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(membrane_cxlsim_core PUBLIC membrane_core)

add_executable(membrane-cxl-sim main.cpp)
set_target_properties(membrane-cxl-sim PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_link_libraries(membrane-cxl-sim PRIVATE membrane_cxlsim_core)
