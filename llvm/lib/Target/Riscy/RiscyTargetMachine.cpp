//===-- RiscyTargetMachine.cpp - Define TargetMachine for Riscy -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the info about Riscy target spec.
//
//===----------------------------------------------------------------------===//

#include "RiscyTargetMachine.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeRiscyTarget() {
}

RiscyTargetMachine::RiscyTargetMachine(const Module &module, const std::string &fs)
	: DataLayout("e-p:32:32:32-i32:32"),
	  Subtarget(module, fs), InstrInfo(Subtarget), 
	  FrameInfo(TargetFrameInfo::StackGrowsDown, 8, 0) {}

virtual const TargetAsmInfo *RiscyTargetMachine::createTargetAsmInfo() const;
static unsigned RiscyTargetMachine::getModuleMatchQuality(const Module &module);
