// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtDialect.h"
#include "iree/compiler/Codegen/Dialect/IREECodegenAttrs.h"
#include "iree/compiler/Codegen/Dialect/IREECodegenDialect.h"
#include "iree/compiler/Codegen/LLVMGPU/KernelConfig.h"
#include "iree/compiler/Codegen/LLVMGPU/PassDetail.h"
#include "iree/compiler/Codegen/LLVMGPU/Passes.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/NVGPU/IR/NVGPUDialect.h"
#include "mlir/Dialect/PDL/IR/PDL.h"
#include "mlir/Dialect/PDLInterp/IR/PDLInterp.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Transform/IR/TransformDialect.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/Passes.h"

namespace mlir {
namespace iree_compiler {

namespace {
/// Lowers an hal.executable.variant operation to scalar/native-vector
/// code. Invokes different compilation pipeline to
/// - first lower to scalar/native-vector code
/// - then convert to NVVM/ROCDL dialect.
/// This should be merged with the equivalent pass in LinalgToLLVM. Fo
/// simplicity it is currently a separate pass.
class LLVMGPULowerExecutableTargetPass
    : public LLVMGPULowerExecutableTargetBase<
          LLVMGPULowerExecutableTargetPass> {
public:
  void getDependentDialects(DialectRegistry &registry) const override {
    // clang-format off
    registry
        .insert<IREE::HAL::HALDialect,
                IREE::LinalgExt::IREELinalgExtDialect,
                linalg::LinalgDialect,
                gpu::GPUDialect,
                nvgpu::NVGPUDialect,
                pdl::PDLDialect,
                pdl_interp::PDLInterpDialect,
                scf::SCFDialect,
                tensor::TensorDialect,
                transform::TransformDialect,
                vector::VectorDialect>();
    // clang-format on
  }

  LLVMGPULowerExecutableTargetPass() = default;
  LLVMGPULowerExecutableTargetPass(
      const LLVMGPULowerExecutableTargetPass &pass){};

  void runOnOperation() override;
};
} // namespace

void LLVMGPULowerExecutableTargetPass::runOnOperation() {
  IREE::HAL::ExecutableVariantOp variantOp = getOperation();
  ModuleOp moduleOp = variantOp.getInnerModule();
  OpPassManager executableLoweringPipeline(
      IREE::HAL::ExecutableVariantOp::getOperationName());

  // There might be multiple entry points in the module. Currently, all of
  // them need to have the same pipeline. This should have been verified during
  // strategy selection, but we still need to retrieve the translation info
  // here.
  llvm::StringMap<IREE::HAL::ExecutableExportOp> exportOps =
      getAllEntryPoints(moduleOp);
  std::optional<IREE::Codegen::TranslationInfoAttr> translationInfo;
  for (auto &it : exportOps) {
    auto exportOp = it.second;
    if (IREE::Codegen::TranslationInfoAttr currTranslationInfo =
            getTranslationInfo(exportOp)) {
      if (translationInfo) {
        if (currTranslationInfo != translationInfo.value()) {
          moduleOp.emitOpError(
              "unhandled compilation of entry point functions with different "
              "translation info");
        }
      } else {
        translationInfo = currTranslationInfo;
      }
    }
  }

  if (translationInfo.has_value()) {
    switch (translationInfo.value().getDispatchLoweringPassPipeline()) {
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUDefault:
      addGPUDefaultPassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUDistribute:
      addGPUSimpleDistributePassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUVectorize:
      addGPUVectorizationPassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUMatmulSimt:
      addGPUMatmulSimtPassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUMatmulTensorCore:
      addGPUMatmulTensorCorePassPipeline(
          executableLoweringPipeline,
          translationInfo.value().getSoftwarePipelineDepth());
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::
        LLVMGPUMatmulTensorCoreMmaSync:
      addGPUMatmulTensorCoreMmaSyncPassPipeline(
          executableLoweringPipeline,
          translationInfo.value().getSoftwarePipelineDepth());
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUTransposeSharedMem:
      addGPUTransposePassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUWarpReduction:
      addGPUWarpReductionPassPipeline(executableLoweringPipeline);
      break;
    case IREE::Codegen::DispatchLoweringPassPipeline::LLVMGPUPackUnPack:
      addGPUPackUnPackPasses(executableLoweringPipeline);
      break;
    // Transform-dialect pipelines.
    case IREE::Codegen::DispatchLoweringPassPipeline::TransformDialectCodegen:
      addGPUTransformDialectPasses(executableLoweringPipeline);
      break;
    // no pipeline specified, nothing to do.
    case IREE::Codegen::DispatchLoweringPassPipeline::None:
      return;
    default:
      variantOp.emitOpError("Unsupported pipeline on GPU target.");
      return signalPassFailure();
    }
  }

  if (failed(runPipeline(executableLoweringPipeline, variantOp))) {
    return signalPassFailure();
  }
}

std::unique_ptr<OperationPass<IREE::HAL::ExecutableVariantOp>>
createLLVMGPULowerExecutableTargetPass() {
  return std::make_unique<LLVMGPULowerExecutableTargetPass>();
}

} // namespace iree_compiler
} // namespace mlir
