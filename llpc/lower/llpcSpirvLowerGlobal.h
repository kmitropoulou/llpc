/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcSpirvLowerGlobal.h
 * @brief LLPC header file: contains declaration of class Llpc::SpirvLowerGlobal.
 ***********************************************************************************************************************
 */
#pragma once

#include "SPIRVInternal.h"
#include "llpcSpirvLower.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace Llpc {

// =====================================================================================================================
// Represents the pass of SPIR-V lowering operations for globals (global variables, inputs, and outputs).
class SpirvLowerGlobal : public SpirvLower, public llvm::PassInfoMixin<SpirvLowerGlobal> {
public:
  SpirvLowerGlobal();
  llvm::PreservedAnalyses run(llvm::Module &module, llvm::ModuleAnalysisManager &analysisManager);
  bool runImpl(llvm::Module &module);

  void handleCallInst(bool checkEmitCall, bool checkInterpCall);
  void handleReturnInst();

  void handleLoadInst();
  void handleLoadInstGlobal(LoadInst &loadInst, const unsigned addrSpace);
  void handleLoadInstGEP(GetElementPtrInst *const getElemPtr, LoadInst &loadInst, const unsigned addrSpace);

  void handleStoreInst();
  void handleStoreInstGlobal(StoreInst &storeInst);
  void handleStoreInstGEP(GetElementPtrInst *const getElemPtr, StoreInst &storeInst);

  static llvm::StringRef name() { return "Lower SPIR-V globals (global variables, inputs, and outputs"; }

private:
  void mapGlobalVariableToProxy(llvm::GlobalVariable *globalVar);
  void mapInputToProxy(llvm::GlobalVariable *input);
  void mapOutputToProxy(llvm::GlobalVariable *input);

  void lowerGlobalVar();
  void lowerInput();
  void lowerOutput();
  void lowerInOutInPlace();
  void lowerBufferBlock();
  void lowerPushConsts();

  void cleanupReturnBlock();

  llvm::Value *addCallInstForInOutImport(llvm::Type *inOutTy, unsigned addrSpace, llvm::Constant *inOutMeta,
                                         llvm::Value *startLoc, unsigned maxLocOffset, llvm::Value *compIdx,
                                         llvm::Value *vertexIdx, unsigned interpLoc, llvm::Value *interpInfo,
                                         bool isPerVertexDimension, llvm::Instruction *insertPos);

  void addCallInstForOutputExport(llvm::Value *outputValue, llvm::Constant *outputMeta, llvm::Value *locOffset,
                                  unsigned maxLocOffset, unsigned xfbOffsetAdjust, unsigned xfbBufferAdjust,
                                  llvm::Value *elemIdx, llvm::Value *vertexIdx, unsigned emitStreamId,
                                  llvm::Instruction *insertPos);

  Value *loadDynamicIndexedMembers(Type *inOutTy, unsigned addrSpace, const std::vector<Value *> &indexOperands,
                                   unsigned operandIdx, Constant *inOutMetaVal, Value *locOffset, unsigned interpLoc,
                                   Value *auxInterpValue, Instruction *insertPos);

  llvm::Value *loadInOutMember(llvm::Type *inOutTy, unsigned addrSpace, const std::vector<llvm::Value *> &indexOperands,
                               unsigned operandIdx, unsigned maxLocOffset, llvm::Constant *inOutMeta,
                               llvm::Value *locOffset, llvm::Value *vertexIdx, unsigned interpLoc,
                               llvm::Value *interpInfo, llvm::Instruction *insertPos);

  void storeOutputMember(llvm::Type *outputTy, llvm::Value *storeValue, const std::vector<llvm::Value *> &indexOperands,
                         unsigned operandIdx, unsigned maxLocOffset, llvm::Constant *outputMeta, llvm::Value *locOffset,
                         llvm::Value *vertexIdx, llvm::Instruction *insertPos);

  void interpolateInputElement(unsigned interpLoc, llvm::Value *interpInfo, llvm::CallInst &callInst);

  llvm::Value *toInt32Value(llvm::Value *value, llvm::Instruction *insertPos);

  std::unordered_map<llvm::Value *, llvm::Value *> m_globalVarProxyMap; // Proxy map for lowering global variables
  std::unordered_map<llvm::Value *, llvm::Value *> m_inputProxyMap;     // Proxy map for lowering inputs

  // NOTE: Here we use list to store pairs of output proxy mappings. This is because we want output patching to be
  // "ordered" (resulting LLVM IR for the patching always be consistent).
  std::list<std::pair<llvm::Value *, llvm::Value *>> m_outputProxyMap; // Proxy list for lowering outputs

  llvm::BasicBlock *m_retBlock; // The return block of entry point

  bool m_lowerInputInPlace;  // Whether to lower input inplace
  bool m_lowerOutputInPlace; // Whether to lower output inplace

  std::unordered_set<llvm::ReturnInst *> m_retInsts;  // "Return" instructions to be removed
  std::unordered_set<llvm::CallInst *> m_emitCalls;   // "Call" instructions to emit vertex (geometry shader)
  std::unordered_set<llvm::LoadInst *> m_loadInsts;   // "Load" instructions to be removed
  std::unordered_set<llvm::StoreInst *> m_storeInsts; // "Store" instructions to be removed
  std::unordered_set<llvm::CallInst *> m_interpCalls; // "Call" instruction to do input interpolation
                                                      // (fragment shader)
};

// =====================================================================================================================
// Legacy pass manager wrapper class
class LegacySpirvLowerGlobal : public llvm::ModulePass {
public:
  LegacySpirvLowerGlobal();

  virtual bool runOnModule(llvm::Module &module);

  static char ID; // ID of this pass

private:
  LegacySpirvLowerGlobal(const LegacySpirvLowerGlobal &) = delete;
  LegacySpirvLowerGlobal &operator=(const LegacySpirvLowerGlobal &) = delete;

  SpirvLowerGlobal Impl;
};

} // namespace Llpc
