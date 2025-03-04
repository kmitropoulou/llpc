/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  InOutBuilder.cpp
 * @brief LLPC source file: implementation of Builder methods for shader input and output
 ***********************************************************************************************************************
 */
#include "BuilderImpl.h"
#include "lgc/LgcContext.h"
#include "lgc/patch/ShaderInputs.h"
#include "lgc/state/AbiUnlinked.h"
#include "lgc/state/PipelineState.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "lgc-builder-impl-inout"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create a read of (part of) a generic (user) input value, passed from the previous shader stage.
// The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
// A "location" contains four "components", each of which can contain a 16- or 32-bit scalar value. A
// 64-bit scalar value takes two components.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index; for FS custom interpolated input: auxiliary
// interpolation value; else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::CreateReadGenericInput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                            unsigned locationCount, InOutInfo inputInfo, Value *vertexIndex,
                                            const Twine &instName) {
  return readGenericInputOutput(false, resultTy, location, locationOffset, elemIdx, locationCount, inputInfo,
                                vertexIndex, instName);
}

// =====================================================================================================================
// Create a read of (part of) a perVertex input value, passed from the previous shader stage.
// The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
// A "location" contains four "components", each of which can contain a 16- or 32-bit scalar value. A
// 64-bit scalar value takes two components.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inputInfo : Extra input info (FS interp info)
// @param vertexIndex : Vertex index, for each vertex, it is unique. the range is 0-2, up to three vertices per
// primitive; for FS custom interpolated input: auxiliary interpolation value;
// @param instName : Name to give instruction(s)
// @returns Value of input
Value *InOutBuilder::CreateReadPerVertexInput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                              unsigned locationCount, InOutInfo inputInfo, Value *vertexIndex,
                                              const Twine &instName) {
  assert(!resultTy->isAggregateType());
  assert(inputInfo.getInterpMode() == InOutInfo::InterpModeCustom);
  assert(m_shaderStage == ShaderStageFragment);

  // Fold constant pLocationOffset into location.
  if (auto constLocOffset = dyn_cast<ConstantInt>(locationOffset)) {
    location += constLocOffset->getZExtValue();
    locationOffset = getInt32(0);
    locationCount = divideCeil(resultTy->getPrimitiveSizeInBits(), 128);
  }

  // Mark the usage of the input/output.
  markGenericInputOutputUsage(false, location, locationCount, inputInfo, vertexIndex);

  // Fixing the order
  bool parity = false;
  Value *odd = nullptr;
  Value *even = nullptr;

  auto primType = getPipelineState()->getPrimitiveType();
  switch (primType) {
  case PrimitiveType::Point:
    break;
  case PrimitiveType::Line_List:
  case PrimitiveType::Line_Strip:
    break;
  case PrimitiveType::Triangle_List: {
    switch (cast<ConstantInt>(vertexIndex)->getZExtValue()) {
    case 0: {
      vertexIndex = getInt32(2);
      break;
    }
    case 1: {
      vertexIndex = getInt32(0);
      break;
    }
    case 2: {
      vertexIndex = getInt32(1);
      break;
    }
    }
    break;
  }
  case PrimitiveType::Triangle_Strip:
  case PrimitiveType::Triangle_Fan:
  case PrimitiveType::Triangle_Strip_Adjacency: {
    switch (cast<ConstantInt>(vertexIndex)->getZExtValue()) {
    case 0: {
      odd = getInt32(2);
      even = vertexIndex;
      break;
    }
    case 1: {
      odd = getInt32(0);
      even = vertexIndex;
      break;
    }
    case 2: {
      odd = getInt32(1);
      even = vertexIndex;
      break;
    }
    }
    parity = true;
    break;
  }
  case PrimitiveType::Triangle_List_Adjacency: {
    switch (cast<ConstantInt>(vertexIndex)->getZExtValue()) {
    case 0: {
      odd = getInt32(1);
      even = vertexIndex;
      break;
    }
    case 1: {
      odd = getInt32(2);
      even = vertexIndex;
      break;
    }
    case 2: {
      odd = getInt32(0);
      even = vertexIndex;
      break;
    }
    }
    parity = true;
    break;
  }
  default:
    llvm_unreachable("Unable to get vertices per primitive!");
    break;
  }

  auto readInput = [&](Value *vertexIndex) {
    std::string callName = lgcName::InputImportInterpolant;
    SmallVector<Value *, 5> args({
        getInt32(location),
        locationOffset,
        elemIdx,
        getInt32(inputInfo.getInterpMode()),
        vertexIndex,
    });
    addTypeMangling(resultTy, args, callName);
    return emitCall(callName, resultTy, args, {Attribute::ReadOnly, Attribute::WillReturn}, &*GetInsertPoint());
  };

  Value *result = nullptr;
  if (parity) {
    // Odd
    auto oddRes = readInput(odd);
    // Even
    auto evenRes = readInput(even);

    auto primtiveId = readBuiltIn(false, BuiltInPrimitiveId, {}, nullptr, nullptr, "");
    auto evenV = CreateSRem(primtiveId, getInt32(2));
    Value *con = CreateICmpEQ(evenV, getInt32(0));
    result = CreateSelect(con, evenRes, oddRes);
  } else {
    result = readInput(vertexIndex);
  }

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create a read of (part of) a generic (user) output value, returning the value last written in this shader stage.
// The result type is as specified by pResultTy, a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// This operation is only supported for TCS.
//
// @param resultTy : Type of value to read
// @param location : Base location (row) of output
// @param locationOffset : Location offset; must be within locationCount if variable
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the output. Ignored if pLocationOffset is const
// @param outputInfo : Extra output info
// @param vertexIndex : For TCS per-vertex output: vertex index; else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::CreateReadGenericOutput(Type *resultTy, unsigned location, Value *locationOffset, Value *elemIdx,
                                             unsigned locationCount, InOutInfo outputInfo, Value *vertexIndex,
                                             const Twine &instName) {
  return readGenericInputOutput(true, resultTy, location, locationOffset, elemIdx, locationCount, outputInfo,
                                vertexIndex, instName);
}

// =====================================================================================================================
// Read (a part of) a user input/output value.
//
// @param isOutput : True if reading an output (currently only supported with TCS)
// @param resultTy : Type of value to read
// @param location : Base location (row) of input
// @param locationOffset : Variable location offset; must be within locationCount
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the input
// @param inOutInfo : Extra input/output info (FS interp info)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index; for FS custom interpolated input: auxiliary
// interpolation value; else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::readGenericInputOutput(bool isOutput, Type *resultTy, unsigned location, Value *locationOffset,
                                            Value *elemIdx, unsigned locationCount, InOutInfo inOutInfo,
                                            Value *vertexIndex, const Twine &instName) {
  assert(resultTy->isAggregateType() == false);
  assert(isOutput == false || m_shaderStage == ShaderStageTessControl);

  // Fold constant pLocationOffset into location. (Currently a variable pLocationOffset is only supported in
  // TCS, TES, and FS custom interpolation.)
  if (auto constLocOffset = dyn_cast<ConstantInt>(locationOffset)) {
    location += constLocOffset->getZExtValue();
    locationOffset = getInt32(0);
    locationCount = (resultTy->getPrimitiveSizeInBits() + 127U) / 128U;
  }

  // Mark the usage of the input/output.
  markGenericInputOutputUsage(isOutput, location, locationCount, inOutInfo, vertexIndex);

  // Generate LLPC call for reading the input/output.
  StringRef baseCallName = lgcName::InputImportGeneric;
  SmallVector<Value *, 6> args;
  switch (m_shaderStage) {
  case ShaderStageVertex: {
    // VS:  @lgc.input.import.vertex.%Type%(i32 location, i32 elemIdx)
    assert(locationOffset == getInt32(0));
    baseCallName = lgcName::InputImportVertex;
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    break;
  }

  case ShaderStageTessControl:
  case ShaderStageTessEval: {
    // TCS: @lgc.{input|output}.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
    // TES: @lgc.input.import.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx)
    args.push_back(getInt32(location));
    args.push_back(locationOffset);
    args.push_back(elemIdx);
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    if (isOutput)
      baseCallName = lgcName::OutputImportGeneric;
    break;
  }

  case ShaderStageGeometry: {
    // GS:  @lgc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 vertexIdx)
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  }

  case ShaderStageFragment: {
    // FS:  @lgc.input.import.generic.%Type%(i32 location, i32 elemIdx, i32 interpMode, i32 interpLoc)
    //      @lgc.input.import.interpolant.%Type%(i32 location, i32 locOffset, i32 elemIdx,
    //                                           i32 interpMode, <2 x float> | i32 auxInterpValue)
    if (inOutInfo.hasInterpAux()) {
      // Prepare arguments for import interpolant call
      Value *auxInterpValue = modifyAuxInterpValue(vertexIndex, inOutInfo);
      baseCallName = lgcName::InputImportInterpolant;
      args.push_back(getInt32(location));
      args.push_back(locationOffset);
      args.push_back(elemIdx);
      args.push_back(getInt32(inOutInfo.getInterpMode()));
      args.push_back(auxInterpValue);
    } else {
      assert(locationOffset == getInt32(0));
      args.push_back(getInt32(location));
      args.push_back(elemIdx);
      args.push_back(getInt32(inOutInfo.getInterpMode()));
      args.push_back(getInt32(inOutInfo.getInterpLoc()));
    }
    break;
  }

  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  std::string callName(baseCallName);
  addTypeMangling(resultTy, args, callName);
  Value *result = emitCall(callName, resultTy, args, {Attribute::ReadOnly, Attribute::WillReturn}, &*GetInsertPoint());

  result->setName(instName);
  return result;
}

// =====================================================================================================================
// Create a write of (part of) a generic (user) output value, setting the value to pass to the next shader stage.
// The value to write must be a scalar or vector type with no more than four elements.
// A "location" can contain up to a 4-vector of 16- or 32-bit components, or up to a 2-vector of
// 64-bit components. Two locations together can contain up to a 4-vector of 64-bit components.
// A non-constant pLocationOffset is currently only supported for TCS.
//
// @param valueToWrite : Value to write
// @param location : Base location (row) of output
// @param locationOffset : Location offset; must be within locationCount if variable
// @param elemIdx : Element index in vector. (This is the SPIR-V "component", except that it is half the component for
// 64-bit elements.)
// @param locationCount : Count of locations taken by the output. Ignored if pLocationOffset is const
// @param outputInfo : Extra output info (GS stream ID, FS integer signedness)
// @param vertexIndex : For TCS per-vertex output: vertex index; else nullptr
Instruction *InOutBuilder::CreateWriteGenericOutput(Value *valueToWrite, unsigned location, Value *locationOffset,
                                                    Value *elemIdx, unsigned locationCount, InOutInfo outputInfo,
                                                    Value *vertexIndex) {
  assert(valueToWrite->getType()->isAggregateType() == false);

  // Fold constant pLocationOffset into location. (Currently a variable pLocationOffset is only supported in
  // TCS.)
  if (auto constLocOffset = dyn_cast<ConstantInt>(locationOffset)) {
    location += constLocOffset->getZExtValue();
    locationOffset = getInt32(0);
    locationCount = (valueToWrite->getType()->getPrimitiveSizeInBits() + 127U) / 128U;
  }

  // Mark the usage of the output.
  markGenericInputOutputUsage(/*isOutput=*/true, location, locationCount, outputInfo, vertexIndex);

  // Set up the args for the llpc call.
  SmallVector<Value *, 6> args;
  switch (m_shaderStage) {
  case ShaderStageVertex:
  case ShaderStageTessEval: {
    // VS:  @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    // TES: @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    break;
  }

  case ShaderStageTessControl: {
    // TCS: @lgc.output.export.generic.%Type%(i32 location, i32 locOffset, i32 elemIdx, i32 vertexIdx,
    //                                        %Type% outputValue)
    args.push_back(getInt32(location));
    args.push_back(locationOffset);
    args.push_back(elemIdx);
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  }

  case ShaderStageGeometry: {
    // GS:  @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, i32 streamId, %Type% outputValue)
    unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : InvalidValue;
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    args.push_back(getInt32(streamId));
    break;
  }

  case ShaderStageFragment: {
    // Mark fragment output type.
    markFsOutputType(valueToWrite->getType(), location, outputInfo);

    // FS:  @lgc.output.export.generic.%Type%(i32 location, i32 elemIdx, %Type% outputValue)
    assert(locationOffset == getInt32(0));
    args.push_back(getInt32(location));
    args.push_back(elemIdx);
    break;
  }

  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  args.push_back(valueToWrite);

  std::string llpcCallName = lgcName::OutputExportGeneric;
  addTypeMangling(nullptr, args, llpcCallName);
  return emitCall(llpcCallName, getVoidTy(), args, {}, &*GetInsertPoint());
}

// =====================================================================================================================
// Mark usage for a generic (user) input or output
//
// @param isOutput : False for input, true for output
// @param location : Input/output base location
// @param locationCount : Count of locations taken by the input
// @param inOutInfo : Extra input/output information
// @param vertexIndex : For TCS/TES/GS per-vertex input/output: vertex index; for FS custom-interpolated input:
// auxiliary value; else nullptr. (This is just used to tell whether an input/output is per-vertex.)
void InOutBuilder::markGenericInputOutputUsage(bool isOutput, unsigned location, unsigned locationCount,
                                               InOutInfo &inOutInfo, Value *vertexIndex) {
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);

  // Mark the input or output locations as in use.
  std::map<InOutLocationInfo, InOutLocationInfo> *inOutLocInfoMap = nullptr;
  std::map<unsigned, unsigned> *perPatchInOutLocMap = nullptr;
  if (!isOutput) {
    if (m_shaderStage != ShaderStageTessEval || vertexIndex) {
      // Normal input
      inOutLocInfoMap = &resUsage->inOutUsage.inputLocInfoMap;
    } else {
      // TES per-patch input
      perPatchInOutLocMap = &resUsage->inOutUsage.perPatchInputLocMap;
    }
  } else {
    if (m_shaderStage != ShaderStageTessControl || vertexIndex) {
      // Normal output
      inOutLocInfoMap = &resUsage->inOutUsage.outputLocInfoMap;
    } else {
      // TCS per-patch output
      perPatchInOutLocMap = &resUsage->inOutUsage.perPatchOutputLocMap;
    }
  }

  if (!isOutput || m_shaderStage != ShaderStageGeometry) {
    bool keepAllLocations = false;
    if (getPipelineState()->isUnlinked()) {
      if (isOutput && m_pipelineState->getNextShaderStage(m_shaderStage, true) == ShaderStageFragment)
        keepAllLocations = true;
      if (m_shaderStage == ShaderStageFragment && !isOutput)
        keepAllLocations = true;
    }
    unsigned startLocation = (keepAllLocations ? 0 : location);
    // Non-GS-output case.
    if (inOutLocInfoMap) {
      for (unsigned i = startLocation; i < location + locationCount; ++i) {
        InOutLocationInfo origLocationInfo;
        origLocationInfo.setLocation(i);
        auto &newLocationInfo = (*inOutLocInfoMap)[origLocationInfo];
        newLocationInfo.setData(InvalidValue);
      }
    }
    if (perPatchInOutLocMap) {
      for (unsigned i = startLocation; i < location + locationCount; ++i)
        (*perPatchInOutLocMap)[i] = InvalidValue;
    }
  } else {
    // GS output. We include the stream ID with the location in the map key.
    for (unsigned i = 0; i < locationCount; ++i) {
      InOutLocationInfo outLocationInfo;
      outLocationInfo.setLocation(location + i);
      outLocationInfo.setStreamId(inOutInfo.getStreamId());
      auto &newLocationInfo = (*inOutLocInfoMap)[outLocationInfo];
      newLocationInfo.setData(InvalidValue);
    }
  }

  if (!isOutput && m_shaderStage == ShaderStageFragment) {
    // Mark usage for interpolation info.
    markInterpolationInfo(inOutInfo);
  }
}

// =====================================================================================================================
// Mark interpolation info for FS input.
//
// @param interpInfo : Interpolation info (location and mode)
void InOutBuilder::markInterpolationInfo(InOutInfo &interpInfo) {
  assert(m_shaderStage == ShaderStageFragment);

  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
  switch (interpInfo.getInterpMode()) {
  case InOutInfo::InterpModeCustom:
    return;
  case InOutInfo::InterpModeSmooth:
    resUsage->builtInUsage.fs.smooth = true;
    break;
  case InOutInfo::InterpModeFlat:
    resUsage->builtInUsage.fs.flat = true;
    break;
  case InOutInfo::InterpModeNoPersp:
    resUsage->builtInUsage.fs.noperspective = true;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  // When per-sample shading is enabled, force nonperspective and smooth input with center-based interpolation to do
  // per-sample interpolation.
  // NOTE: if the input is used by interpolation functions (has auxiliary value), we should not modify its interpLoc
  // because it is used for modifyAuxInterpValue.
  if (getPipelineState()->getOptions().enableInterpModePatch &&
      interpInfo.getInterpLoc() == InOutInfo::InterpLocCenter && !interpInfo.hasInterpAux() &&
      (resUsage->builtInUsage.fs.smooth || resUsage->builtInUsage.fs.noperspective))
    interpInfo.setInterpLoc(InOutInfo::InterpLocSample);

  switch (interpInfo.getInterpLoc()) {
  case InOutInfo::InterpLocCenter:
    resUsage->builtInUsage.fs.center = true;
    break;
  case InOutInfo::InterpLocCentroid:
    resUsage->builtInUsage.fs.center = true;
    resUsage->builtInUsage.fs.centroid = true;
    break;
  case InOutInfo::InterpLocSample:
    resUsage->builtInUsage.fs.sample = true;
    resUsage->builtInUsage.fs.runAtSampleRate = true;
    break;
  default:
    break;
  }
}

// =====================================================================================================================
// Mark fragment output type
//
// @param outputTy : Output type
// @param location : Output location
// @param outputInfo : Extra output info (whether the output is signed)
void InOutBuilder::markFsOutputType(Type *outputTy, unsigned location, InOutInfo outputInfo) {
  assert(m_shaderStage == ShaderStageFragment);

  // Collect basic types of fragment outputs
  BasicType basicTy = BasicType::Unknown;

  Type *compTy = outputTy->getScalarType();
  const unsigned bitWidth = compTy->getScalarSizeInBits();
  const bool signedness = outputInfo.isSigned();

  if (compTy->isIntegerTy()) {
    // Integer type
    if (bitWidth == 8)
      basicTy = signedness ? BasicType::Int8 : BasicType::Uint8;
    else if (bitWidth == 16)
      basicTy = signedness ? BasicType::Int16 : BasicType::Uint16;
    else {
      assert(bitWidth == 32);
      basicTy = signedness ? BasicType::Int : BasicType::Uint;
    }
  } else if (compTy->isFloatingPointTy()) {
    // Floating-point type
    if (bitWidth == 16)
      basicTy = BasicType::Float16;
    else {
      assert(bitWidth == 32);
      basicTy = BasicType::Float;
    }
  } else
    llvm_unreachable("Should never be called!");

  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
  resUsage->inOutUsage.fs.outputTypes[location] = basicTy;
}

// =====================================================================================================================
// Modify auxiliary interp value according to custom interp mode
//
// @param auxInterpValue : Aux interp value from CreateReadInput (ignored for centroid location)
// @param inputInfo : InOutInfo containing interp mode and location
Value *InOutBuilder::modifyAuxInterpValue(Value *auxInterpValue, InOutInfo inputInfo) {
  if (inputInfo.getInterpLoc() != InOutInfo::InterpLocExplicit) {
    // Add intrinsic to calculate I/J for interpolation function
    std::string evalInstName;
    auto resUsage = getPipelineState()->getShaderResourceUsage(ShaderStageFragment);

    if (inputInfo.getInterpLoc() == InOutInfo::InterpLocCentroid) {
      Value *evalArg = nullptr;

      evalInstName = lgcName::InputImportBuiltIn;
      if (inputInfo.getInterpMode() == InOutInfo::InterpModeNoPersp) {
        evalInstName += "InterpLinearCentroid";
        evalArg = getInt32(BuiltInInterpLinearCentroid);
        resUsage->builtInUsage.fs.noperspective = true;
        resUsage->builtInUsage.fs.centroid = true;
      } else {
        evalInstName += "InterpPerspCentroid";
        evalArg = getInt32(BuiltInInterpPerspCentroid);
        resUsage->builtInUsage.fs.smooth = true;
        resUsage->builtInUsage.fs.centroid = true;
      }

      auxInterpValue = emitCall(evalInstName, FixedVectorType::get(getFloatTy(), 2), {evalArg}, Attribute::ReadOnly,
                                &*GetInsertPoint());
    } else {
      // Generate code to evaluate the I,J coordinates.
      if (inputInfo.getInterpLoc() == InOutInfo::InterpLocSample)
        auxInterpValue = readBuiltIn(false, BuiltInSamplePosOffset, {}, auxInterpValue, nullptr, "");
      if (inputInfo.getInterpMode() == InOutInfo::InterpModeNoPersp)
        auxInterpValue = evalIjOffsetNoPersp(auxInterpValue);
      else
        auxInterpValue = evalIjOffsetSmooth(auxInterpValue);
    }
  } else {
    assert(inputInfo.getInterpMode() == InOutInfo::InterpModeCustom);
  }
  return auxInterpValue;
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, linear (no perspective) version
//
// @param offset : Offset value, <2 x float> or <2 x half>
Value *InOutBuilder::evalIjOffsetNoPersp(Value *offset) {
  Value *center = readBuiltIn(false, BuiltInInterpLinearCenter, {}, nullptr, nullptr, "");
  return adjustIj(center, offset);
}

// =====================================================================================================================
// Evaluate I,J for interpolation: center offset, smooth (perspective) version
//
// @param offset : Offset value, <2 x float> or <2 x half>
Value *InOutBuilder::evalIjOffsetSmooth(Value *offset) {
  // Get <I/W, J/W, 1/W>
  Value *pullModel = readBuiltIn(false, BuiltInInterpPullMode, {}, nullptr, nullptr, "");
  // Adjust each coefficient by offset.
  Value *adjusted = adjustIj(pullModel, offset);
  // Extract <I/W, J/W, 1/W> part of that
  Value *ijDivW = CreateShuffleVector(adjusted, adjusted, ArrayRef<int>{0, 1});
  Value *rcpW = CreateExtractElement(adjusted, 2);
  // Get W by making a reciprocal of 1/W
  Value *w = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), rcpW);
  w = CreateVectorSplat(2, w);
  return CreateFMul(ijDivW, w);
}

// =====================================================================================================================
// Adjust I,J values by offset.
// This adjusts pValue by its X and Y derivatives times the X and Y components of pOffset.
// If pValue is a vector, this is done component-wise.
//
// @param value : Value to adjust, float or vector of float
// @param offset : Offset to adjust by, <2 x float> or <2 x half>
Value *InOutBuilder::adjustIj(Value *value, Value *offset) {
  offset = CreateFPExt(offset, FixedVectorType::get(getFloatTy(), 2));
  Value *offsetX = CreateExtractElement(offset, uint64_t(0));
  Value *offsetY = CreateExtractElement(offset, 1);
  if (auto vecTy = dyn_cast<FixedVectorType>(value->getType())) {
    offsetX = CreateVectorSplat(vecTy->getNumElements(), offsetX);
    offsetY = CreateVectorSplat(vecTy->getNumElements(), offsetY);
  }
  Value *derivX = CreateDerivative(value, /*isY=*/false, /*isFine=*/true);
  Value *derivY = CreateDerivative(value, /*isY=*/true, /*isFine=*/true);
  Value *adjustX = CreateFAdd(value, CreateFMul(derivX, offsetX));
  Value *adjustY = CreateFAdd(adjustX, CreateFMul(derivY, offsetY));
  return adjustY;
}

// =====================================================================================================================
// Create a write to an XFB (transform feedback / streamout) buffer.
// The value to write must be a scalar or vector type with no more than four elements.
// A non-constant pXfbOffset is not currently supported.
// The value is written to the XFB only if this is in the last-vertex-stage shader, i.e. VS (if no TCS/TES/GS),
// TES (if no GS) or GS.
//
// For GS, there is assumed to be an _output correspondence_, that is, for a particular stream ID, the
// value written to the XFB offset is the same value that is written to a particular
// built-in or user output location. CreateWriteOutput or CreateWriteBuiltIn (as applicable) must be used to
// actually write the same value to that location/built-in, then the value written to XFB for each affected
// vertex is undefined.
// If calls to CreateWriteXfbOutput for multiple vertices in a primitive, or in
// different primitives in the same stream, have different output correspondence, then it is undefined which
// of those correspondences is actually used when writing to XFB for each affected vertex.
//
// @param valueToWrite : Value to write
// @param isBuiltIn : True for built-in, false for user output (ignored if not GS)
// @param location : Location (row) or built-in kind of output (ignored if not GS)
// @param xfbBuffer : XFB buffer ID
// @param xfbStride : XFB stride
// @param xfbOffset : XFB byte offset
// @param outputInfo : Extra output info (GS stream ID)
Instruction *InOutBuilder::CreateWriteXfbOutput(Value *valueToWrite, bool isBuiltIn, unsigned location,
                                                unsigned xfbBuffer, unsigned xfbStride, Value *xfbOffset,
                                                InOutInfo outputInfo) {
  // xfbStride must be a non-zero value
  assert(xfbStride > 0);
  // Can currently only cope with constant xfbOffset.
  assert(isa<ConstantInt>(xfbOffset));

  // Ignore if not in last-vertex-stage shader (excluding copy shader).
  auto stagesAfterThisOneMask = -shaderStageToMask(static_cast<ShaderStage>(m_shaderStage + 1));
  if ((getPipelineState()->getShaderStageMask() & ~shaderStageToMask(ShaderStageFragment) &
       ~shaderStageToMask(ShaderStageCopyShader) & stagesAfterThisOneMask) != 0)
    return nullptr;

  // Mark the usage of the XFB buffer.
  auto resUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage);
  unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : 0;
  assert(xfbBuffer < MaxTransformFeedbackBuffers);
  assert(streamId < MaxGsStreams);
  resUsage->inOutUsage.xfbStrides[xfbBuffer] = xfbStride;
  resUsage->inOutUsage.enableXfb = true;
  resUsage->inOutUsage.streamXfbBuffers[streamId] |= 1 << xfbBuffer;

  if (m_shaderStage == ShaderStageGeometry) {

    // Mark the XFB output for copy shader generation.
    XfbOutInfo xfbOutInfo = {};
    xfbOutInfo.streamId = streamId;
    xfbOutInfo.xfbBuffer = xfbBuffer;
    xfbOutInfo.xfbOffset = cast<ConstantInt>(xfbOffset)->getZExtValue();
    xfbOutInfo.is16bit = valueToWrite->getType()->getScalarSizeInBits() == 16;

    // For packed generic GS output, the XFB output should be scalarized to align with the scalarized GS output
    if (getPipelineState()->canPackOutput(m_shaderStage) && !isBuiltIn) {
      Type *elementTy = valueToWrite->getType();
      unsigned scalarizeBy = 1;
      if (auto vectorTy = dyn_cast<FixedVectorType>(elementTy)) {
        scalarizeBy = vectorTy->getNumElements();
        elementTy = vectorTy->getElementType();
      }
      if (elementTy->getPrimitiveSizeInBits() == 64)
        scalarizeBy *= 2;
      unsigned xfbOffset = xfbOutInfo.xfbOffset;
      for (unsigned i = 0; i < scalarizeBy; ++i) {
        InOutLocationInfo outLocInfo;
        outLocInfo.setLocation(location);
        outLocInfo.setStreamId(streamId);
        outLocInfo.setComponent(i);
        outLocInfo.setBuiltIn(isBuiltIn);
        if (i >= 4) {
          outLocInfo.setLocation(location + 1);
          outLocInfo.setComponent(i - 4);
          xfbOutInfo.xfbOffset = xfbOffset + 16;
        }
        resUsage->inOutUsage.gs.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;
      }
    } else {
      InOutLocationInfo outLocInfo;
      outLocInfo.setLocation(location);
      outLocInfo.setBuiltIn(isBuiltIn);
      outLocInfo.setStreamId(streamId);
      resUsage->inOutUsage.gs.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;

      if (valueToWrite->getType()->getPrimitiveSizeInBits() > 128) {
        outLocInfo.setLocation(location + 1);
        xfbOutInfo.xfbOffset += 32;
        resUsage->inOutUsage.gs.locInfoXfbOutInfoMap[outLocInfo] = xfbOutInfo;
      }
    }
  }

  // XFB: @lgc.output.export.xfb.%Type%(i32 xfbBuffer, i32 xfbOffset, i32 streamId, %Type% outputValue)
  SmallVector<Value *, 4> args;
  std::string instName = lgcName::OutputExportXfb;
  args.push_back(getInt32(xfbBuffer));
  args.push_back(xfbOffset);
  args.push_back(getInt32(streamId));
  args.push_back(valueToWrite);
  addTypeMangling(nullptr, args, instName);
  return emitCall(instName, getVoidTy(), args, {}, &*GetInsertPoint());
}

// =====================================================================================================================
// Create a read of (part of) a built-in input value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if pIndex is not nullptr. For ClipDistance or CullDistance when pIndex is nullptr,
// the array size is determined by inputInfo.GetArraySize().
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param inputInfo : Extra input info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::CreateReadBuiltInInput(BuiltInKind builtIn, InOutInfo inputInfo, Value *vertexIndex, Value *index,
                                            const Twine &instName) {
  assert(isBuiltInInput(builtIn));
  return readBuiltIn(false, builtIn, inputInfo, vertexIndex, index, instName);
}

// =====================================================================================================================
// Create a read of (part of) a built-in output value.
// The type of the returned value is the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if pIndex is not nullptr.
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::CreateReadBuiltInOutput(BuiltInKind builtIn, InOutInfo outputInfo, Value *vertexIndex,
                                             Value *index, const Twine &instName) {
  // Currently this only copes with reading an output in TCS.
  assert(m_shaderStage == ShaderStageTessControl);
  assert(isBuiltInOutput(builtIn));
  return readBuiltIn(true, builtIn, outputInfo, vertexIndex, index, instName);
}

// =====================================================================================================================
// Read (part of) a built-in value
//
// @param isOutput : True to read built-in output, false to read built-in input
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param inOutInfo : Extra input/output info (shader-defined array size)
// @param vertexIndex : For TCS/TES/GS per-vertex input: vertex index, else nullptr Special case for FS
// BuiltInSamplePosOffset: sample number. That special case only happens when ReadBuiltIn is called from
// ModifyAuxInterpValue.
// @param index : Array or vector index to access part of an input, else nullptr
// @param instName : Name to give instruction(s)
Value *InOutBuilder::readBuiltIn(bool isOutput, BuiltInKind builtIn, InOutInfo inOutInfo, Value *vertexIndex,
                                 Value *index, const Twine &instName) {
  // Mark usage.
  unsigned arraySize = inOutInfo.getArraySize();
  if (auto constIndex = dyn_cast_or_null<ConstantInt>(index))
    arraySize = constIndex->getZExtValue() + 1;

  if (!isOutput)
    markBuiltInInputUsage(builtIn, arraySize);
  else
    markBuiltInOutputUsage(builtIn, arraySize, InvalidValue);

  // Get the built-in type.
  Type *resultTy = getBuiltInTy(builtIn, inOutInfo);
  if (index) {
    if (isa<ArrayType>(resultTy))
      resultTy = resultTy->getArrayElementType();
    else
      resultTy = cast<VectorType>(resultTy)->getElementType();
  }

  // Handle certain common built-ins directly.
  if (Value *result = readCommonBuiltIn(builtIn, resultTy, instName))
    return result;

  if (m_shaderStage == ShaderStageCompute && !isOutput) {
    // We handle compute shader inputs directly.
    return readCsBuiltIn(builtIn, instName);
  }

  if (m_shaderStage == ShaderStageVertex && !isOutput) {
    // We can handle some vertex shader inputs directly.
    Value *result = readVsBuiltIn(builtIn, instName);
    if (result)
      return result;
  }

  // For now, this just generates a call to lgc.input.import.builtin. A future commit will
  // change it to generate IR more directly here.
  // A vertex index is valid only in TCS, TES, GS.
  // Currently we can only cope with an array/vector index in TCS/TES.
  SmallVector<Value *, 4> args;
  args.push_back(getInt32(builtIn));
  switch (m_shaderStage) {
  case ShaderStageTessControl:
  case ShaderStageTessEval:
    args.push_back(index ? index : getInt32(InvalidValue));
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  case ShaderStageGeometry:
    assert(!index);
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  case ShaderStageFragment:
    if (builtIn == BuiltInSamplePosOffset) {
      // Special case for BuiltInSamplePosOffset: pVertexIndex is the sample number.
      // That special case only happens when ReadBuiltIn is called from ModifyAuxInterpValue.
      Value *sampleNum = vertexIndex;
      vertexIndex = nullptr;
      args.push_back(sampleNum);
    }
    assert(!index && !vertexIndex);
    break;
  default:
    assert(!index && !vertexIndex);
    break;
  }

  std::string callName = isOutput ? lgcName::OutputImportBuiltIn : lgcName::InputImportBuiltIn;
  callName += PipelineState::getBuiltInName(builtIn);
  addTypeMangling(resultTy, args, callName);
  Value *result = emitCall(callName, resultTy, args, {Attribute::ReadOnly, Attribute::WillReturn}, &*GetInsertPoint());

  if (instName.isTriviallyEmpty())
    result->setName(PipelineState::getBuiltInName(builtIn));
  else
    result->setName(instName);

  return result;
}

// =====================================================================================================================
// Read and directly handle certain built-ins that are common between shader stages
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param resultTy : Expected result type
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *InOutBuilder::readCommonBuiltIn(BuiltInKind builtIn, llvm::Type *resultTy, const Twine &instName) {
  switch (builtIn) {

  case BuiltInSubgroupEqMask:
  case BuiltInSubgroupGeMask:
  case BuiltInSubgroupGtMask:
  case BuiltInSubgroupLeMask:
  case BuiltInSubgroupLtMask: {
    // Handle the subgroup mask built-ins directly.
    Value *result = nullptr;
    Value *localInvocationId = readBuiltIn(false, BuiltInSubgroupLocalInvocationId, {}, nullptr, nullptr, "");
    if (getPipelineState()->getShaderSubgroupSize(m_shaderStage) == 64)
      localInvocationId = CreateZExt(localInvocationId, getInt64Ty());

    switch (builtIn) {
    case BuiltInSubgroupEqMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), 1), localInvocationId);
      break;
    case BuiltInSubgroupGeMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), -1), localInvocationId);
      break;
    case BuiltInSubgroupGtMask:
      result = CreateShl(ConstantInt::get(localInvocationId->getType(), -2), localInvocationId);
      break;
    case BuiltInSubgroupLeMask:
      result = CreateSub(CreateShl(ConstantInt::get(localInvocationId->getType(), 2), localInvocationId),
                         ConstantInt::get(localInvocationId->getType(), 1));
      break;
    case BuiltInSubgroupLtMask:
      result = CreateSub(CreateShl(ConstantInt::get(localInvocationId->getType(), 1), localInvocationId),
                         ConstantInt::get(localInvocationId->getType(), 1));
      break;
    default:
      llvm_unreachable("Should never be called!");
    }
    if (getPipelineState()->getShaderSubgroupSize(m_shaderStage) == 64) {
      result = CreateInsertElement(Constant::getNullValue(FixedVectorType::get(getInt64Ty(), 2)), result, uint64_t(0));
      result = CreateBitCast(result, resultTy);
    } else
      result = CreateInsertElement(ConstantInt::getNullValue(resultTy), result, uint64_t(0));
    result->setName(instName);
    return result;
  }

  case BuiltInSubgroupSize:
    // SubgroupSize is a constant.
    return getInt32(getPipelineState()->getShaderSubgroupSize(m_shaderStage));

  case BuiltInSubgroupLocalInvocationId:
    // SubgroupLocalInvocationId is the lane number within the wave.
    return CreateGetLaneNumber();

  case BuiltInDeviceIndex:
    // DeviceId is a constant from the pipeline state normally, or a reloc to get that constant
    // at link time for an unlinked shader.
    if (m_pipelineState->isUnlinked())
      return CreateRelocationConstant(reloc::DeviceIdx);
    return getInt32(getPipelineState()->getDeviceIndex());

  default:
    // Built-in not handled here.
    return nullptr;
  }
}

// =====================================================================================================================
// Read compute shader input
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *InOutBuilder::readCsBuiltIn(BuiltInKind builtIn, const Twine &instName) {
  auto &shaderMode = m_pipelineState->getShaderModes()->getComputeShaderMode();
  switch (builtIn) {

  case BuiltInWorkgroupSize:
    // WorkgroupSize is a constant vector supplied by shader mode.
    return ConstantVector::get({getInt32(shaderMode.workgroupSizeX), getInt32(shaderMode.workgroupSizeY),
                                getInt32(shaderMode.workgroupSizeZ)});

  case BuiltInNumWorkgroups: {
    // NumWorkgroups is a v3i32 loaded from an address pointed to by a special user data item.
    Value *numWorkgroupPtr = ShaderInputs::getSpecialUserData(UserDataMapping::Workgroup, BuilderBase::get(*this));
    LoadInst *load = CreateLoad(FixedVectorType::get(getInt32Ty(), 3), numWorkgroupPtr);
    load->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(getContext(), {}));
    return load;
  }

  case BuiltInWorkgroupId:
    // WorkgroupId is a v3i32 shader input (three SGPRs set up by hardware).
    return ShaderInputs::getInput(ShaderInput::WorkgroupId, BuilderBase::get(*this), *getLgcContext());

  case BuiltInLocalInvocationId: {
    // LocalInvocationId is a v3i32 shader input (three VGPRs set up in hardware).
    Value *localInvocationId =
        ShaderInputs::getInput(ShaderInput::LocalInvocationId, BuilderBase::get(*this), *getLgcContext());

    // Unused dimensions need zero-initializing.
    if (shaderMode.workgroupSizeZ <= 1) {
      if (shaderMode.workgroupSizeY <= 1)
        localInvocationId = CreateInsertElement(localInvocationId, getInt32(0), 1);
      localInvocationId = CreateInsertElement(localInvocationId, getInt32(0), 2);
    }

    // If the option is enabled, we might want to reconfigure the workgroup layout later, which
    // means the value of LocalInvocationId needs modifying. We can't do that now, as we need to
    // know whether the shader uses images. Detect here that we can't do that optimization in
    // a compute library. We don't detect here that we can't do the optimization in a compute shader
    // with calls, as that detection is slightly more expensive.
    if (m_pipelineState->getOptions().reconfigWorkgroupLayout && !getPipelineState()->isComputeLibrary()) {
      // Insert a call that later on might get lowered to code to reconfigure the workgroup.
      localInvocationId = CreateNamedCall(lgcName::ReconfigureLocalInvocationId, localInvocationId->getType(),
                                          localInvocationId, Attribute::ReadNone);
    }
    return localInvocationId;
  }

  case BuiltInNumSubgroups: {
    // workgroupSize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z
    // gl_NumSubgroups = (workgroupSize + gl_SubGroupSize - 1) / gl_SubgroupSize
    auto &mode = m_pipelineState->getShaderModes()->getComputeShaderMode();
    unsigned workgroupSize = mode.workgroupSizeX * mode.workgroupSizeY * mode.workgroupSizeZ;
    unsigned subgroupSize = m_pipelineState->getShaderWaveSize(m_shaderStage);
    unsigned numSubgroups = (workgroupSize + subgroupSize - 1) / subgroupSize;
    return getInt32(numSubgroups);
  }

  case BuiltInGlobalInvocationId: {
    // GlobalInvocationId is WorkgroupId * WorkgroupSize + LocalInvocationId
    Value *result = CreateMul(readCsBuiltIn(BuiltInWorkgroupId), readCsBuiltIn(BuiltInWorkgroupSize));
    return CreateAdd(result, readCsBuiltIn(BuiltInLocalInvocationId));
  }

  case BuiltInLocalInvocationIndex: {
    // LocalInvocationIndex is
    // (WorkgroupSize.Y * LocalInvocationId.Z + LocalInvocationId.Y) * WorkGroupSize.X + LocalInvocationId.X
    Value *workgroupSize = readCsBuiltIn(BuiltInWorkgroupSize);
    Value *localInvocationId = readCsBuiltIn(BuiltInLocalInvocationId);
    Value *input = CreateMul(CreateExtractElement(workgroupSize, 1), CreateExtractElement(localInvocationId, 2));
    input = CreateAdd(input, CreateExtractElement(localInvocationId, 1));
    input = CreateMul(CreateExtractElement(workgroupSize, uint64_t(0)), input);
    input = CreateAdd(input, CreateExtractElement(localInvocationId, uint64_t(0)));
    return input;
  }

  case BuiltInSubgroupId: {
    // gl_SubgroupID = gl_LocationInvocationIndex / gl_SubgroupSize
    Value *localInvocationIndex = readCsBuiltIn(BuiltInLocalInvocationIndex);
    unsigned subgroupSize = getPipelineState()->getShaderWaveSize(m_shaderStage);
    return CreateLShr(localInvocationIndex, getInt32(Log2_32(subgroupSize)));
  }

  default:
    // Not handled. This should never happen; we need to handle all CS built-ins here because the old way of
    // handling them (caller will handle with lgc.input.import.builtin, which is then lowered in
    // PatchInOutImportExport) does not work with compute-with-calls.
    llvm_unreachable("Unhandled CS built-in");
    return nullptr;
  }
}

// =====================================================================================================================
// Read vertex shader input
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param instName : Name to give instruction(s)
// @returns : Value of input; nullptr if not handled here
Value *InOutBuilder::readVsBuiltIn(BuiltInKind builtIn, const Twine &instName) {
  BuilderBase &builder = BuilderBase::get(*this);
  switch (builtIn) {
  case BuiltInBaseVertex:
    return ShaderInputs::getSpecialUserData(UserDataMapping::BaseVertex, builder);
  case BuiltInBaseInstance:
    return ShaderInputs::getSpecialUserData(UserDataMapping::BaseInstance, builder);
  case BuiltInDrawIndex:
    return ShaderInputs::getSpecialUserData(UserDataMapping::DrawIndex, builder);
  case BuiltInVertexIndex:
    return ShaderInputs::getVertexIndex(builder, *getLgcContext());
  case BuiltInInstanceIndex:
    return ShaderInputs::getInstanceIndex(builder, *getLgcContext());
  default:
    // Not handled; caller will handle with lgc.input.import.builtin, which is then lowered in PatchInOutImportExport.
    return nullptr;
  }
}

// =====================================================================================================================
// Create a write of (part of) a built-in output value.
// The type of the value to write must be the fixed type of the specified built-in (see BuiltInDefs.h),
// or the element type if pIndex is not nullptr.
//
// @param valueToWrite : Value to write
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param outputInfo : Extra output info (shader-defined array size; GS stream id)
// @param vertexIndex : For TCS per-vertex output: vertex index, else nullptr
// @param index : Array or vector index to access part of an input, else nullptr
Instruction *InOutBuilder::CreateWriteBuiltInOutput(Value *valueToWrite, BuiltInKind builtIn, InOutInfo outputInfo,
                                                    Value *vertexIndex, Value *index) {
  // Mark usage.
  unsigned streamId = outputInfo.hasStreamId() ? outputInfo.getStreamId() : InvalidValue;
  unsigned arraySize = outputInfo.getArraySize();
  if (auto constIndex = dyn_cast_or_null<ConstantInt>(index))
    arraySize = constIndex->getZExtValue() + 1;
  markBuiltInOutputUsage(builtIn, arraySize, streamId);

#ifndef NDEBUG
  // Assert we have the right type. Allow for ClipDistance/CullDistance being a different array size.
  Type *expectedTy = getBuiltInTy(builtIn, outputInfo);
  if (index) {
    if (isa<ArrayType>(expectedTy))
      expectedTy = cast<ArrayType>(expectedTy)->getElementType();
    else
      expectedTy = cast<VectorType>(expectedTy)->getElementType();
  }
  assert(expectedTy == valueToWrite->getType() ||
         ((builtIn == BuiltInClipDistance || builtIn == BuiltInCullDistance) &&
          valueToWrite->getType()->getArrayElementType() == expectedTy->getArrayElementType()));
#endif

  // For now, this just generates a call to lgc.output.export.builtin. A future commit will
  // change it to generate IR more directly here.
  // A vertex index is valid only in TCS, TES, GS.
  // Currently we can only cope with an array/vector index in TCS.
  //
  // VS:  @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
  // TCS: @lgc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, i32 elemIdx, i32 vertexIdx,
  //                                                  %Type% outputValue)
  // TES: @lgc.output.export.builtin.%BuiltIn%.%Type%(i32 builtInId, %Type% outputValue)
  // GS:  @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, i32 streamId, %Type% outputValue)
  // FS:  @lgc.output.export.builtin.%BuiltIn%(i32 builtInId, %Type% outputValue)
  SmallVector<Value *, 4> args;
  args.push_back(getInt32(builtIn));
  switch (m_shaderStage) {
  case ShaderStageTessControl:
    args.push_back(index ? index : getInt32(InvalidValue));
    args.push_back(vertexIndex ? vertexIndex : getInt32(InvalidValue));
    break;
  case ShaderStageGeometry:
    assert(!index && !vertexIndex);
    args.push_back(getInt32(streamId));
    break;
  default:
    assert(!index && !vertexIndex);
    break;
  }
  args.push_back(valueToWrite);

  std::string callName = lgcName::OutputExportBuiltIn;
  callName += PipelineState::getBuiltInName(builtIn);
  addTypeMangling(nullptr, args, callName);
  return cast<Instruction>(emitCall(callName, getVoidTy(), args, {}, &*GetInsertPoint()));
}

// =====================================================================================================================
// Get the type of a built-in. This overrides the one in Builder to additionally recognize the internal built-ins.
//
// @param builtIn : Built-in kind
// @param inOutInfo : Extra input/output info (shader-defined array size)
Type *InOutBuilder::getBuiltInTy(BuiltInKind builtIn, InOutInfo inOutInfo) {
  switch (static_cast<unsigned>(builtIn)) {
  case BuiltInSamplePosOffset:
  case BuiltInInterpLinearCenter:
    return FixedVectorType::get(getFloatTy(), 2);
  case BuiltInInterpPullMode:
    return FixedVectorType::get(getFloatTy(), 3);
  default:
    return Builder::getBuiltInTy(builtIn, inOutInfo);
  }
}

// =====================================================================================================================
// Mark usage of a built-in input. This is only needed where a built-in is handled by generating lgc.import.input
// to be lowered in PatchInOutImportExport, and not when it is directly generated here using
// ShaderInputs::getInput() and/or ShaderInputs::getSpecialUserData().
//
// @param builtIn : Built-in ID
// @param arraySize : Number of array elements for ClipDistance and CullDistance. (Multiple calls to this function for
// this built-in might have different array sizes; we take the max)
void InOutBuilder::markBuiltInInputUsage(BuiltInKind &builtIn, unsigned arraySize) {
  auto &usage = getPipelineState()->getShaderResourceUsage(m_shaderStage)->builtInUsage;
  assert((builtIn != BuiltInClipDistance && builtIn != BuiltInCullDistance) || arraySize != 0);
  switch (m_shaderStage) {
  case ShaderStageVertex: {
    switch (builtIn) {
    case BuiltInPrimitiveId:
      usage.vs.primitiveId = true;
      break;
    case BuiltInViewIndex:
      usage.vs.viewIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageTessControl: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tcs.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.tcs.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.tcs.clipDistanceIn = std::max(usage.tcs.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tcs.cullDistanceIn = std::max(usage.tcs.cullDistanceIn, arraySize);
      break;
    case BuiltInPatchVertices:
      usage.tcs.patchVertices = true;
      break;
    case BuiltInPrimitiveId:
      usage.tcs.primitiveId = true;
      break;
    case BuiltInInvocationId:
      usage.tcs.invocationId = true;
      break;
    case BuiltInViewIndex:
      usage.tcs.viewIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageTessEval: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tes.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.tes.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.tes.clipDistanceIn = std::max(usage.tes.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tes.cullDistanceIn = std::max(usage.tes.cullDistanceIn, arraySize);
      break;
    case BuiltInPatchVertices:
      usage.tes.patchVertices = true;
      break;
    case BuiltInPrimitiveId:
      usage.tes.primitiveId = true;
      break;
    case BuiltInTessCoord:
      usage.tes.tessCoord = true;
      break;
    case BuiltInTessLevelOuter:
      usage.tes.tessLevelOuter = true;
      break;
    case BuiltInTessLevelInner:
      usage.tes.tessLevelInner = true;
      break;
    case BuiltInViewIndex:
      usage.tes.viewIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageGeometry: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.gs.pointSizeIn = true;
      break;
    case BuiltInPosition:
      usage.gs.positionIn = true;
      break;
    case BuiltInClipDistance:
      usage.gs.clipDistanceIn = std::max(usage.gs.clipDistanceIn, arraySize);
      break;
    case BuiltInCullDistance:
      usage.gs.cullDistanceIn = std::max(usage.gs.cullDistanceIn, arraySize);
      break;
    case BuiltInPrimitiveId:
      usage.gs.primitiveIdIn = true;
      break;
    case BuiltInInvocationId:
      usage.gs.invocationId = true;
      break;
    case BuiltInViewIndex:
      usage.gs.viewIndex = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageFragment: {
    switch (static_cast<unsigned>(builtIn)) {
    case BuiltInFragCoord:
      usage.fs.fragCoord = true;
      break;
    case BuiltInFrontFacing:
      usage.fs.frontFacing = true;
      break;
    case BuiltInClipDistance:
      usage.fs.clipDistance = std::max(usage.fs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.fs.cullDistance = std::max(usage.fs.cullDistance, arraySize);
      break;
    case BuiltInPointCoord:
      usage.fs.pointCoord = true;
      // NOTE: gl_PointCoord is emulated via a general input. Those qualifiers therefore have to
      // be marked as used.
      usage.fs.smooth = true;
      if (getPipelineState()->getRasterizerState().perSampleShading)
        usage.fs.sample = true;
      else
        usage.fs.center = true;
      break;
    case BuiltInPrimitiveId:
      usage.fs.primitiveId = true;
      break;
    case BuiltInSampleId:
      usage.fs.sampleId = true;
      usage.fs.runAtSampleRate = true;
      break;
    case BuiltInSamplePosition:
      usage.fs.samplePosition = true;
      // NOTE: gl_SamplePostion is derived from gl_SampleID
      usage.fs.sampleId = true;
      usage.fs.runAtSampleRate = true;
      break;
    case BuiltInSampleMask:
      usage.fs.sampleMaskIn = true;
      break;
    case BuiltInLayer:
      usage.fs.layer = true;
      break;
    case BuiltInViewportIndex:
      usage.fs.viewportIndex = true;
      break;
    case BuiltInShadingRate:
      usage.fs.shadingRate = true;
      break;
    case BuiltInHelperInvocation:
      usage.fs.helperInvocation = true;
      break;
    case BuiltInViewIndex:
      usage.fs.viewIndex = true;
      break;
    case BuiltInBaryCoordNoPersp:
      usage.fs.baryCoordNoPersp = true;
      if (getPipelineState()->getOptions().enableInterpModePatch) {
        usage.fs.baryCoordNoPerspSample = true;
        builtIn = BuiltInBaryCoordNoPerspSample;
      }
      break;
    case BuiltInBaryCoordNoPerspCentroid:
      usage.fs.baryCoordNoPerspCentroid = true;
      break;
    case BuiltInBaryCoordNoPerspSample:
      usage.fs.baryCoordNoPerspSample = true;
      break;
    case BuiltInBaryCoordSmooth:
      usage.fs.baryCoordSmooth = true;
      if (getPipelineState()->getOptions().enableInterpModePatch) {
        usage.fs.baryCoordSmoothSample = true;
        builtIn = BuiltInBaryCoordSmoothSample;
      }
      break;
    case BuiltInBaryCoordSmoothCentroid:
      usage.fs.baryCoordSmoothCentroid = true;
      break;
    case BuiltInBaryCoordSmoothSample:
      usage.fs.baryCoordSmoothSample = true;
      break;
    case BuiltInBaryCoordPullModel:
      usage.fs.baryCoordPullModel = true;
      break;
    case BuiltInBaryCoord:
      usage.fs.baryCoord = true;
      usage.fs.primitiveId = true;
      break;
    case BuiltInBaryCoordNoPerspKHR:
      usage.fs.baryCoordNoPerspKHR = true;
      usage.fs.primitiveId = true;
      break;

    // Internal built-ins.
    case BuiltInInterpLinearCenter:
      usage.fs.noperspective = true;
      usage.fs.center = true;
      break;
    case BuiltInInterpPullMode:
      usage.fs.smooth = true;
      usage.fs.pullMode = true;
      break;
    case BuiltInSamplePosOffset:
      usage.fs.runAtSampleRate = true;
      break;

    default:
      break;
    }
    break;
  }

  default:
    break;
  }
}

// =====================================================================================================================
// Mark usage of a built-in output
//
// @param builtIn : Built-in ID
// @param arraySize : Number of array elements for ClipDistance and CullDistance. (Multiple calls to this function for
// this built-in might have different array sizes; we take the max)
// @param streamId : GS stream ID, or InvalidValue if not known
void InOutBuilder::markBuiltInOutputUsage(BuiltInKind builtIn, unsigned arraySize, unsigned streamId) {
  auto &usage = getPipelineState()->getShaderResourceUsage(m_shaderStage)->builtInUsage;
  assert((builtIn != BuiltInClipDistance && builtIn != BuiltInCullDistance) || arraySize != 0);
  switch (m_shaderStage) {
  case ShaderStageVertex: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.vs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.vs.position = true;
      break;
    case BuiltInClipDistance:
      usage.vs.clipDistance = std::max(usage.vs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.vs.cullDistance = std::max(usage.vs.cullDistance, arraySize);
      break;
    case BuiltInViewportIndex:
      usage.vs.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.vs.layer = true;
      break;
    case BuiltInPrimitiveShadingRate:
      usage.vs.primitiveShadingRate = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageTessControl: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tcs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.tcs.position = true;
      break;
    case BuiltInClipDistance:
      usage.tcs.clipDistance = std::max(usage.tcs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tcs.cullDistance = std::max(usage.tcs.cullDistance, arraySize);
      break;
    case BuiltInTessLevelOuter:
      usage.tcs.tessLevelOuter = true;
      break;
    case BuiltInTessLevelInner:
      usage.tcs.tessLevelInner = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageTessEval: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.tes.pointSize = true;
      break;
    case BuiltInPosition:
      usage.tes.position = true;
      break;
    case BuiltInClipDistance:
      usage.tes.clipDistance = std::max(usage.tes.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.tes.cullDistance = std::max(usage.tes.cullDistance, arraySize);
      break;
    case BuiltInViewportIndex:
      usage.tes.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.tes.layer = true;
      break;
    default:
      break;
    }
    break;
  }

  case ShaderStageGeometry: {
    switch (builtIn) {
    case BuiltInPointSize:
      usage.gs.pointSize = true;
      break;
    case BuiltInPosition:
      usage.gs.position = true;
      break;
    case BuiltInClipDistance:
      usage.gs.clipDistance = std::max(usage.gs.clipDistance, arraySize);
      break;
    case BuiltInCullDistance:
      usage.gs.cullDistance = std::max(usage.gs.cullDistance, arraySize);
      break;
    case BuiltInPrimitiveId:
      usage.gs.primitiveId = true;
      break;
    case BuiltInViewportIndex:
      usage.gs.viewportIndex = true;
      break;
    case BuiltInLayer:
      usage.gs.layer = true;
      break;
    case BuiltInPrimitiveShadingRate:
      usage.gs.primitiveShadingRate = true;
      break;
    default:
      break;
    }
    // Collect raster stream ID for the export of built-ins
    if (streamId != InvalidValue)
      getPipelineState()->getShaderResourceUsage(m_shaderStage)->inOutUsage.gs.rasterStream = streamId;
    break;
  }

  case ShaderStageFragment: {
    switch (builtIn) {
    case BuiltInFragDepth:
      usage.fs.fragDepth = true;
      break;
    case BuiltInSampleMask:
      usage.fs.sampleMask = true;
      break;
    case BuiltInFragStencilRef:
      usage.fs.fragStencilRef = true;
      break;
    default:
      break;
    }
    break;
  }

  default:
    break;
  }
}

#ifndef NDEBUG
// =====================================================================================================================
// Get a bitmask of which shader stages are valid for a built-in to be an input or output of
//
// @param builtIn : Built-in kind, one of the BuiltIn* constants
// @param isOutput : True to get the mask for output rather than input
unsigned InOutBuilder::getBuiltInValidMask(BuiltInKind builtIn, bool isOutput) {
  // See BuiltInDefs.h for an explanation of the letter codes.
  enum class StageValidMask : unsigned {
    C = (1 << ShaderStageCompute),
    D = (1 << ShaderStageTessEval),
    H = (1 << ShaderStageTessControl),
    HD = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval),
    HDG = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
    HDGP = (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry) |
           (1 << ShaderStageFragment),
    HG = (1 << ShaderStageTessControl) | (1 << ShaderStageGeometry),
    MG = (1 << ShaderStageGeometry),
    MVG = (1 << ShaderStageVertex) | (1 << ShaderStageGeometry),
    MVDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
    MVHDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) |
            (1 << ShaderStageGeometry),
    N = 0,
    P = (1 << ShaderStageFragment),
    TMC = (1 << ShaderStageCompute),
    TMV = (1 << ShaderStageVertex),
    TMVHDGPC = (1 << ShaderStageVertex) | (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) |
               (1 << ShaderStageGeometry) | (1 << ShaderStageFragment) | (1 << ShaderStageCompute),
    V = (1 << ShaderStageVertex),
    VDG = (1 << ShaderStageVertex) | (1 << ShaderStageTessEval) | (1 << ShaderStageGeometry),
    VHDGP = (1 << ShaderStageVertex) | (1 << ShaderStageTessControl) | (1 << ShaderStageTessEval) |
            (1 << ShaderStageGeometry) | (1 << ShaderStageFragment),
  };

  unsigned validMask = 0;
  switch (builtIn) {
#define BUILTIN(name, number, out, in, type)                                                                           \
  case BuiltIn##name:                                                                                                  \
    validMask = static_cast<unsigned>(StageValidMask::in) | (static_cast<unsigned>(StageValidMask::out) << 16);        \
    break;
#include "lgc/BuiltInDefs.h"
#undef BUILTIN
  default:
    llvm_unreachable("Should never be called!");
    break;
  }
  return isOutput ? (validMask >> 16) : (validMask & 0xFFFF);
}

// =====================================================================================================================
// Determine whether a built-in is an input for a particular shader stage.
//
// @param builtIn : Built-in type, one of the BuiltIn* constants
bool InOutBuilder::isBuiltInInput(BuiltInKind builtIn) {
  return (getBuiltInValidMask(builtIn, false) >> m_shaderStage) & 1;
}

// =====================================================================================================================
// Determine whether a built-in is an output for a particular shader stage.
//
// @param builtIn : Built-in type, one of the BuiltIn* constants
bool InOutBuilder::isBuiltInOutput(BuiltInKind builtIn) {
  return (getBuiltInValidMask(builtIn, true) >> m_shaderStage) & 1;
}
#endif
