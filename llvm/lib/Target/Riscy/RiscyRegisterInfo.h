//===-- RiscyRegisterInfo.h - Define TargetMachine for Riscy ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the Riscy specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_RISCY_RISCYREGISTERINFO_H
#define LLVM_LIB_TARGET_RISCY_RISCYREGISTERINFO_H

#include "llvm/CodeGen/TargetRegisterInfo.h"

#define GET_REGINFO_HEADER
#include "RiscyGenRegisterInfo.h.inc"

namespace llvm {

struct RiscyRegisterInfo : public RiscyGenRegisterInfo {
	RiscyRegisterInfo(unsigned HwMode);

	const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const override;
	const uint32_t *getCallPreservedMask(const MachineFunction &MF, CallingConv::ID) const override;
	BitVector getReservedRegs(const MachineFunction &MF) const override;
	bool hasFP(const MachineFunction &MF) const override;
	Register getFrameRegister(const MachineFunction &MF) const override;
	void eliminateFrameIndex(MachineBasicBlock::iterator MI, int SPAdj, unsigned FIOperandNum, RegScavenger *RS = nullptr) const override;
	bool isConstantPhysReg(MCRegister PhysReg) const override;
};

}

#endif
