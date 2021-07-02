add_llvm_component_group(Riscy)

set(LLVM_TARGET_DEFINITIONS Riscy.td)

tablegen(LLVM RiscyGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM RiscyGenInstrInfo.inc -gen-instr-info)

add_public_tablegen_target(RiscyCommonTableGen)

add_llvm_target(RiscyCodeGen
  RiscyTargetMachine.cpp

  LINK_COMPONENTS
  Target

  ADD_TO_COMPONENT
  Riscy
  )
