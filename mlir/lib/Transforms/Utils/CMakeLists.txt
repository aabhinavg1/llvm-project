add_mlir_library(MLIRTransformUtils
  CFGToSCF.cpp
  CommutativityUtils.cpp
  ControlFlowSinkUtils.cpp
  DialectConversion.cpp
  FoldUtils.cpp
  GreedyPatternRewriteDriver.cpp
  Inliner.cpp
  InliningUtils.cpp
  LoopInvariantCodeMotionUtils.cpp
  RegionUtils.cpp
  WalkPatternRewriteDriver.cpp

  ADDITIONAL_HEADER_DIRS
  ${MLIR_MAIN_INCLUDE_DIR}/mlir/Transforms

  LINK_LIBS PUBLIC
  MLIRAnalysis
  MLIRCallInterfaces
  MLIRControlFlowInterfaces
  MLIRFunctionInterfaces
  MLIRLoopLikeInterface
  MLIRSideEffectInterfaces
  MLIRSubsetOpInterface
  MLIRRewrite
  )
