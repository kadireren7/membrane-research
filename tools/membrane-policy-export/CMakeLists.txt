add_executable(membrane-policy-export main.c)
target_link_libraries(membrane-policy-export PRIVATE membrane_core)
target_include_directories(membrane-policy-export PRIVATE
  ${CMAKE_BINARY_DIR}/generated)
