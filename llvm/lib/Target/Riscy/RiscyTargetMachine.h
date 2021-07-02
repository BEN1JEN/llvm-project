//===-- RiscyTargetMachine.h - Define TargetMachine for Riscy ---*- C++ -*-===//
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

#ifndef LLVM_LIB_TARGET_RISCY_RISCYTARGETMACHINE_H
#define LLVM_LIB_TARGET_RISCY_RISCYTARGETMACHINE_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

class RiscyTargetMachine : public LLVMTargetMachine {
private:
	const DataLayout DataLayout;
	RiscySubtarget Subtarget;
	RiscyInstrInfo InstrInfo;
	TargetFrameInfo FrameInfo;

protected:
	virtual const TargetAsmInfo *createTargetAsmInfo() const;

public:
	RiscyTargetMachine(const Module &module, const std::string &fs);

	virtual const RiscyInstrInfo *getInstrInfo() const { return &InstrInfo; }
	virtual const TargetFrameInfo *getFrameInfo() const { return &FrameInfo; }
	virtual const TargetSubtarget *getSubtargetImpl() const { return &Subtarget; }
	virtual const TargetRegisterInfo *getRegisterInfo() const { return &InstrInfo.getRegisterInfo(); }
	virtual const DataLayout *getDataLayout() const { return &DataLayout; }
	static unsigned getModuleMatchQuality(const Module &module);
};

} // namespace llvm

#endif
