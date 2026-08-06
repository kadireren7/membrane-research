add_executable(membrane-hw-sim main.c)
target_link_libraries(membrane-hw-sim PRIVATE membrane_core)

if(MEMBRANE_ENABLE_LLAMA)
  # Same source, built a second time with the ggml oracle cross-check
  # compiled in (item 2's "bit-exact vs ggml reference" requirement) --
  # a separate target rather than a compile option on the portable one,
  # since only this variant needs to link membrane_ggml_quant/ggml.
  add_executable(membrane-hw-sim-oracle main.c)
  target_compile_definitions(membrane-hw-sim-oracle PRIVATE
    MEMBRANE_HWSIM_WITH_GGML_ORACLE)
  target_link_libraries(membrane-hw-sim-oracle PRIVATE membrane_core
    membrane_ggml_quant)
endif()
