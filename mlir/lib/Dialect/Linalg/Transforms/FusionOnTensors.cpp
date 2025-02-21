//===- Fusion.cpp - Implementation of linalg Fusion -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the linalg dialect Fusion on tensors operations pass.
//
//===----------------------------------------------------------------------===//
#include "PassDetail.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace mlir;
using namespace mlir::linalg;

/// Implementation of fusion of generic ops and indexed_generic ops.
static bool areElementwiseOpsFusable(LinalgOp producer, LinalgOp consumer,
                                     unsigned consumerIdx) {
  // Producer and consumer must have tensor semantics.
  if (!producer.hasTensorSemantics() || !consumer.hasTensorSemantics())
    return false;

  // Verify that
  // - the producer has all "parallel" iterator type.
  if (producer.getNumParallelLoops() != producer.getNumLoops())
    return false;

  // Only allow fusing the producer of an input operand for now.
  // TODO: allow fusing the producer of an output operand.
  if (consumerIdx >= consumer.getNumInputs())
    return false;

  // Get the consumer index map. The number of results of the consumer index
  // map must match the number of loops of the producer.
  AffineMap consumerIndexMap = consumer.getIndexingMap(consumerIdx);
  if (consumerIndexMap.getNumResults() != producer.getNumLoops())
    return false;

  // Currently support only operations with single result.
  if (producer.getNumOutputs() != 1)
    return false;

  // Finally the index_map for the result must be invertible. For now just
  // verify it is a permutation.
  AffineMap producerResultIndexMap = producer.getOutputIndexingMap(0);
  return producerResultIndexMap.isPermutation();
}

/// Append to `fusedOpIndexingMapAttrs` the indexing maps for the operands of
/// the `producer` to use in the fused operation given the indexing map of the
/// result of the producer in the consumer.
static AffineMap getIndexingMapOfProducerOperandsInCoordinatesOfFusedOp(
    OpOperand &producerOpOperand, AffineMap producerResultIndexMap,
    AffineMap fusedConsumerArgIndexMap) {
  // The indexing map in the consumer op (fusedConsumerArgIndexMap) is a map
  // from consumer loop -> consumer arg tensor index/producer result tensor
  // index. The fused loop is same as the consumer loop. For each producer arg
  // the indexing map to be computed is a map from consumer loop -> producer
  // arg tensor index.
  // producerResultIndexMap is a map from producer loop -> tensor index.
  // Compute the inverse to get map from tensor index -> producer loop.
  // The inverse is a map from producer result tensor index -> producer loop.
  AffineMap invProducerResultIndexMap =
      inversePermutation(producerResultIndexMap);
  assert(invProducerResultIndexMap &&
         "expected producer result indexig map to be invertible");

  LinalgOp producer = cast<LinalgOp>(producerOpOperand.getOwner());
  // argMap is a map from producer loop -> producer arg tensor index.
  AffineMap argMap =
      producer.getIndexingMap(producerOpOperand.getOperandNumber());

  // Compose argMap with invProducerResultIndexMap to get a map from
  // producer result tensor index -> producer arg tensor index.
  AffineMap t1 = argMap.compose(invProducerResultIndexMap);

  // Compose t1 with fusedConsumerArgIndexMap gives an indexing map from
  // consumer loop/ fused loop -> producer arg tensor index.
  return t1.compose(fusedConsumerArgIndexMap);
}

/// Generate the region of the fused tensor operation. The region of the fused
/// op must be empty.
static void
generateFusedElementwiseOpRegion(PatternRewriter &rewriter, Operation *fusedOp,
                                 LinalgOp producer, LinalgOp consumer,
                                 AffineMap consumerToProducerLoopsMap,
                                 unsigned consumerIdx, unsigned nloops) {
  // Build the region of the fused op.
  Block &producerBlock = producer->getRegion(0).front();
  Block &consumerBlock = consumer->getRegion(0).front();
  Block *fusedBlock = new Block();
  fusedOp->getRegion(0).push_back(fusedBlock);
  BlockAndValueMapping mapper;
  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointToStart(fusedBlock);

  // The block arguments are
  // [index_0, index_1, ... ,
  //   consumer_operand_0, ... , consumer_operand_(`consumerIdx`-1),
  //   producer_operand_0, ... , producer_operand_(n-1)],
  //   consumer_operand_(`consumerIdx`), .. consumer_operand_(m-1)]
  // , where n is the number of producer's operand and m is the number
  // consumer's operand.
  // If both `numProducerIndices` and `numConsumerIndices` are zero, this is a
  // generic op. In this case, there are no indices in block arguments.
  unsigned numProducerIndices = isa<IndexedGenericOp>(producer.getOperation())
                                    ? producer.getNumLoops()
                                    : 0;
  unsigned numConsumerIndices = isa<IndexedGenericOp>(consumer.getOperation())
                                    ? consumer.getNumLoops()
                                    : 0;
  unsigned numFusedOpIndices =
      (isa<IndexedGenericOp>(producer.getOperation()) ||
       isa<IndexedGenericOp>(consumer.getOperation()))
          ? std::max(producer.getNumLoops(), consumer.getNumLoops())
          : 0;

  // 0. Firstly, add all the indices to the block arguments.
  for (unsigned i = 0, e = numFusedOpIndices; i < e; ++i)
    fusedBlock->addArgument(rewriter.getIndexType());
  // 1. Map consumer indices to fusedBlock indices 1-1.
  mapper.map(consumerBlock.getArguments().take_front(numConsumerIndices),
             fusedBlock->getArguments().take_front(numConsumerIndices));
  // 2a. Embed producer indices into fusedBlock index space 1-1.
  for (auto it :
       llvm::zip(producerBlock.getArguments().take_front(numProducerIndices),
                 fusedBlock->getArguments().take_front(numProducerIndices))) {
    auto newIndex = rewriter.create<mlir::AffineApplyOp>(
        producer.getLoc(),
        consumerToProducerLoopsMap.getSubMap(std::get<0>(it).getArgNumber()),
        fusedBlock->getArguments().take_front(numFusedOpIndices));
    mapper.map(std::get<0>(it), newIndex);
  }
  // 2b. Add an index operation for every fused loop dimension and use the
  // `consumerToProducerLoopsMap` to map the producer indices.
  if (producer.hasIndexSemantics()) {
    // Add an index operation for every fused loop dimension.
    unsigned numFusedOpLoops =
        std::max(producer.getNumLoops(), consumer.getNumLoops());
    SmallVector<Value> fusedIndices;
    fusedIndices.reserve(numFusedOpLoops);
    llvm::transform(llvm::seq<uint64_t>(0, numFusedOpLoops),
                    std::back_inserter(fusedIndices), [&](uint64_t dim) {
                      return rewriter.create<IndexOp>(producer.getLoc(), dim);
                    });
    for (IndexOp indexOp :
         llvm::make_early_inc_range(producerBlock.getOps<IndexOp>())) {
      Value newIndex = rewriter.create<mlir::AffineApplyOp>(
          producer.getLoc(),
          consumerToProducerLoopsMap.getSubMap(indexOp.dim()), fusedIndices);
      mapper.map(indexOp.getResult(), newIndex);
    }
  }
  // TODO: allow fusing the producer of an output operand.
  assert(consumerIdx < consumer.getNumInputs() &&
         "expected producer of input operand");
  // 3. Consumer input operands up to consumerIdx (exclusive).
  for (BlockArgument bbArg : consumerBlock.getArguments()
                                 .drop_front(numConsumerIndices)
                                 .take_front(consumerIdx)) // input assumption.
    mapper.map(bbArg, fusedBlock->addArgument(bbArg.getType()));

  // Replacing consumerIdx requires getting the cloned, yielded, value from
  // the (cloned) producer block. This happens in step 9.

  // 4. Splice in producer's input operands.
  for (BlockArgument bbArg : producerBlock.getArguments()
                                 .drop_front(numProducerIndices)
                                 .take_front(producer.getNumInputs()))
    mapper.map(bbArg, fusedBlock->addArgument(bbArg.getType()));

  // 4.b. Producer output operand/map that is fused needs to be mapped to the
  // producer bbArg if it is an "initTensor" (i.e. its value is actually read).
  assert(producer->getNumResults() == 1 && "expected single result producer");
  if (producer.isInitTensor(&producer.getOutputOpOperands()[0])) {
    BlockArgument bbArg =
        producerBlock.getArguments()
            .drop_front(numConsumerIndices + producer.getNumInputs())
            // TODO: bbArg index of
            .front();
    mapper.map(bbArg, fusedBlock->addArgument(bbArg.getType()));
  }
  // 5. Remaining consumer's input operands (drop past index `consumerIdx`).
  for (BlockArgument bbArg : consumerBlock.getArguments()
                                 .drop_front(numConsumerIndices)
                                 .take_front(consumer.getNumInputs())
                                 .drop_front(consumerIdx + 1))
    mapper.map(bbArg, fusedBlock->addArgument(bbArg.getType()));
  // 6. All of consumer's output operands.
  for (BlockArgument bbArg :
       consumerBlock.getArguments().take_back(consumer.getNumOutputs()))
    mapper.map(bbArg, fusedBlock->addArgument(bbArg.getType()));
  // 7. All of producer's output operands except the one fused.
  // TODO: allow fusion of multi-result producers.
  assert(producer->getNumResults() == 1 && "expected single result producer");

  // 8. Clone all producer operations except for the yield and index operations
  // to the fused operation.
  for (auto &op : producerBlock.without_terminator()) {
    if (!isa<IndexOp>(op))
      rewriter.clone(op, mapper);
  }
  // 9. Now we can map the consumerBlock's `consumerIdx` block argument. Just
  // forward the yield operand.
  auto yieldOp = cast<linalg::YieldOp>(producerBlock.getTerminator());
  // TODO: allow fusion of multi-result producers.
  assert(producer->getNumResults() == 1 && "expected single result producer");
  unsigned producerResultNumber = 0;
  Value replacement =
      mapper.lookupOrDefault(yieldOp.getOperand(producerResultNumber));
  // Sanity checks, if replacement is not already in the mapper then it must be
  // produced outside.
  if (replacement == yieldOp.getOperand(producerResultNumber)) {
    if (auto bb = replacement.dyn_cast<BlockArgument>())
      assert(bb.getOwner() != &producerBlock &&
             "yielded block argument must have been mapped");
    else
      assert(!producer->isAncestor(replacement.getDefiningOp()) &&
             "yielded value must have been mapped");
  }
  mapper.map(consumerBlock.getArgument(consumerIdx + numConsumerIndices),
             replacement);
  // 10. Clone operations from the consumer to the fused op.
  for (auto &op : consumerBlock.getOperations())
    rewriter.clone(op, mapper);

  // Sanity checks.
  assert(fusedBlock->getNumArguments() ==
             fusedOp->getNumOperands() + numFusedOpIndices &&
         "Ill-formed LinalgOp region");
}

static Optional<SmallVector<Value, 1>>
fuseElementwiseOpsImpl(LinalgOp producer, OpOperand &consumerOpOperand,
                       const ControlElementwiseOpsFusionFn &controlFn,
                       PatternRewriter &rewriter) {
  LinalgOp consumer = cast<LinalgOp>(consumerOpOperand.getOwner());
  unsigned consumerIdx = consumerOpOperand.getOperandNumber();
  if (!areElementwiseOpsFusable(producer, consumer, consumerIdx) ||
      !controlFn(producer->getResult(0), consumerOpOperand))
    return llvm::None;

  // TODO: allow fusing the producer of an output operand.
  assert(consumerIdx < consumer.getNumInputs() &&
         "expected producer of input operand");

  // Compute the fused operands list and indexing maps.
  SmallVector<Value> fusedOperands;
  SmallVector<AffineMap> fusedIndexMaps;
  fusedOperands.reserve(producer->getNumOperands() +
                        consumer->getNumOperands());
  fusedIndexMaps.reserve(producer->getNumOperands() +
                         consumer->getNumOperands());
  // In the following, numbering matches that of `generateFusedTensorOpRegion`.
  // 3. Consumer input operands/maps up to consumerIdx (exclusive).
  llvm::append_range(fusedOperands,
                     consumer.getInputs().take_front(consumerIdx));
  llvm::append_range(
      fusedIndexMaps,
      ArrayRef<AffineMap>{consumer.getInputIndexingMaps()}.take_front(
          consumerIdx));
  // 4. Splice in producer's input operands/maps.
  llvm::append_range(fusedOperands, producer.getInputs());
  assert(producer->getNumResults() == 1 && "expected single result producer");
  AffineMap producerResultIndexMap = producer.getOutputIndexingMap(0);
  for (auto &inputOpOperand : producer.getInputOpOperands()) {
    // Compute indexing maps for the producer args in the fused operation.
    AffineMap map = getIndexingMapOfProducerOperandsInCoordinatesOfFusedOp(
        inputOpOperand, producerResultIndexMap,
        consumer.getInputIndexingMap(consumerIdx));
    fusedIndexMaps.push_back(map);
  }
  // 4.b. Producer output operand/map that is fused needs to be passed if it is
  // an "initTensor" (i.e. its value is actually read).
  assert(producer->getNumResults() == 1 && "expected single result producer");
  if (producer.isInitTensor(&producer.getOutputOpOperands()[0])) {
    llvm::append_range(fusedOperands, producer.getOutputs().take_front());
    // Compute indexing maps for the producer args in the fused operation.
    AffineMap map = getIndexingMapOfProducerOperandsInCoordinatesOfFusedOp(
        producer.getOutputOpOperands().front(), producerResultIndexMap,
        consumer.getOutputIndexingMap(0));
    fusedIndexMaps.push_back(map);
  }
  // 5. Remaining consumer's input operands/maps (drop past index
  // `consumerIdx`).
  llvm::append_range(fusedOperands,
                     consumer.getInputs().drop_front(consumerIdx + 1));
  llvm::append_range(
      fusedIndexMaps,
      ArrayRef<AffineMap>{consumer.getInputIndexingMaps()}.drop_front(
          consumerIdx + 1));
  // 6. All of consumer's output operands (skip operands: added by the builder).
  // llvm::append_range(fusedOperands, consumer.getOutputs());
  llvm::append_range(fusedIndexMaps, consumer.getOutputIndexingMaps());
  // 7. All of producer's output operands/maps except the one fused.
  // TODO: allow fusion of multi-result producers.
  assert(producer->getNumResults() == 1 && "expected single result producer");

  // Generate the fused op.
  Operation *fusedOp;
  if (isa<GenericOp>(producer.getOperation()) &&
      isa<GenericOp>(consumer.getOperation())) {
    fusedOp = rewriter.create<GenericOp>(
        consumer.getLoc(), consumer->getResultTypes(),
        /*inputs=*/fusedOperands,
        // TODO: handle outputs.
        consumer.getOutputs(), rewriter.getAffineMapArrayAttr(fusedIndexMaps),
        consumer.iterator_types(),
        /*doc=*/nullptr,
        /*library_call=*/nullptr,
        /*sparse=*/nullptr);
  } else {
    fusedOp = rewriter.create<IndexedGenericOp>(
        consumer.getLoc(), consumer->getResultTypes(),
        /*inputs=*/fusedOperands,
        // TODO: handle outputs.
        consumer.getOutputs(), rewriter.getAffineMapArrayAttr(fusedIndexMaps),
        consumer.iterator_types(),
        /*doc=*/nullptr,
        /*library_call=*/nullptr,
        /*sparse=*/nullptr);
  }

  // Construct an AffineMap from consumer loops to producer loops.
  // consumer loop -> tensor index
  AffineMap consumerResultIndexMap = consumer.getInputIndexingMap(consumerIdx);
  // tensor index -> producer loop
  AffineMap invProducerResultIndexMap =
      inversePermutation(producerResultIndexMap);
  assert(invProducerResultIndexMap &&
         "expected producer result indexig map to be invertible");
  // consumer loop -> producer loop
  AffineMap consumerToProducerLoopsMap =
      invProducerResultIndexMap.compose(consumerResultIndexMap);

  generateFusedElementwiseOpRegion(rewriter, fusedOp, producer, consumer,
                                   consumerToProducerLoopsMap, consumerIdx,
                                   consumer.getNumLoops());
  return SmallVector<Value, 1>(fusedOp->getResults());
}

/// Linearize the expressions in `sourceMap` based on the `reassociationMaps`
/// provided, given the shape of the source tensor that corresponds to the
/// `sourceMap`. Note that this implicitly assumes that the tensors dimensions
/// are "row-major" ordered logically.
///
/// For example:
///
/// %0 = op ... : tensor<?x?x4x5xf32>
/// with output index_map `affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>`
///
/// and reshape:
/// %1 = linalg.tensor_reshape %0 [affine_map<(i, j, k, l) -> (i)>,
///                                affine_map<(i, j, k, l) -> (j, k, l)>] :
///        tensor<?x?x4x5xf32> into tensor<?x?xf32>
///
/// would be rewritten into:
/// %0 = op ... : tensor<?x?x4x5xf32>
/// with output index_map
///   `affine_map<(d0, d1, d2, d3) -> (d0, d1 * 20 + d2 * 5 + d3)>`
static AffineMap linearizeCollapsedDims(AffineMap sourceMap,
                                        ArrayRef<int64_t> sourceShape,
                                        ArrayRef<AffineMap> reassociationMaps) {
  SmallVector<AffineExpr, 4> resultExprs;
  resultExprs.reserve(reassociationMaps.size());
  ArrayRef<AffineExpr> sourceExprs = sourceMap.getResults();
  MLIRContext *context = sourceMap.getContext();

  // Compute the result exprs based on the reassociation maps.
  for (AffineMap map : reassociationMaps) {
    ArrayRef<AffineExpr> collapsedDims = map.getResults();
    // Assume that they are in-order and contiguous (already checked in
    // verifier).
    assert(!collapsedDims.empty());
    unsigned startDim =
        collapsedDims.front().cast<AffineDimExpr>().getPosition();
    SmallVector<int64_t, 4> sizes;
    SmallVector<AffineExpr, 4> dimExprs;
    for (auto en :
         llvm::zip(sourceShape.slice(startDim, collapsedDims.size()),
                   sourceExprs.slice(startDim, collapsedDims.size()))) {
      if (std::get<0>(en) == 1)
        continue;
      sizes.push_back(std::get<0>(en));
      dimExprs.push_back(std::get<1>(en));
    }
    AffineExpr linearizedExpr =
        makeCanonicalStridedLayoutExpr(sizes, dimExprs, context);
    resultExprs.push_back(linearizedExpr);
  }
  return AffineMap::get(sourceMap.getNumDims(), sourceMap.getNumSymbols(),
                        resultExprs, context);
}

/// Checks if the `reshapeOp` can be fused with it consumer (if `asProducer` is
/// true) or its producer (if `asProducer` is false) given the indexing map at
/// its use.
static bool isTensorReshapeOpFoldableByLinearization(TensorReshapeOp reshapeOp,
                                                     AffineMap useIndexMap,
                                                     bool asProducer) {
  RankedTensorType returnType = reshapeOp.getResultType();
  RankedTensorType operandType = reshapeOp.getSrcType();
  // Reshape is fusable with its consumer (i.e. reshape as a producer) when its
  // operand is of lesser rank than the result. Fusing when operand has higher
  // rank will require use of mods and divs in the indexing maps of the fused op
  // which would make it non-invertible. Similarly reshape is fused with its
  // producer (i.e. reshape as consumer) only if the return type has lesser
  // rank.
  if ((asProducer && reshapeOp.getSrcType().hasStaticShape() &&
       returnType.getRank() < operandType.getRank()) ||
      (!asProducer && reshapeOp.getResultType().hasStaticShape() &&
       operandType.getRank() < returnType.getRank()))
    return false;
  return useIndexMap.isPermutation();
}

/// Based on the type of `op` create a linalg op of the same type, i.e. if `op`
/// is a linalg.generic operation, the create a `linalg.generic` operation with
/// the given `args`. Expects `op` to be `linalg.generic` or
/// `linalg.indexed_generic`.
template <typename... Args>
static LinalgOp createLinalgOpOfSameType(LinalgOp op, PatternRewriter &rewriter,
                                         Args... args) {
  if (isa<GenericOp>(op.getOperation()))
    return rewriter.create<GenericOp>(args...);
  if (isa<IndexedGenericOp>(op.getOperation()))
    return rewriter.create<IndexedGenericOp>(args...);
  llvm_unreachable(
      "expected only linalg.generic or linalg.indexed_generic ops");
  return nullptr;
}

/// Check if the reshape operation is only expansion into/collapsing of
/// unit-dimension.
static bool isUnitDimExpansionOnly(ArrayRef<int64_t> expandedShape,
                                   ArrayRef<AffineMap> reassociation) {
  for (auto &map : reassociation) {
    unsigned numUnitDims = 0;
    for (AffineExpr expr : map.getResults()) {
      unsigned position = expr.cast<AffineDimExpr>().getPosition();
      if (expandedShape[position] == 1)
        numUnitDims++;
    }
    if (numUnitDims != map.getNumResults() - 1)
      return false;
  }
  return true;
}

/// Conditions for folding a generic/indexed-generic operation with a reshape op
/// by expanding the iteration space dimensionality for tensor operations. These
/// are preconditions assumed by `foldReshapeByDimExpansion` which implements
/// the following fusion pattern.
///
///  Consider
///
///  %c = linalg.generic ins(%a, %b : memref<?x?x?xf32>, memref<?x?xf32>)
///         indexing_maps = [affine_map<(d0, d1, d2) -> (d1, d0, d2)>,
///                          affine_map<(d0, d1, d2) -> (d1, d2)>,
///                          affine_map<(d0, d1, d2) -> (d0, d2, d1)>]
///  %d = linalg.tensor_reshape %c
///         [affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1)>,
///          affine_map<(d0, d1, d2, d3, d4, d5) -> (d2)>,
///          affine_map<(d0, d1, d2, d3, d4, d5) -> (d3, d4, d5)>]
///       : tensor<?x?x?xf32> into tensor<?x?x?x?x?x?xf32>
///
///  The reshape can be folded into the `linalgOp` if the
///  generic/indexed-generic op loop dimensionality is increased to match the
///  result (operand) of the tensor_reshape when the reshape is expanding
///  (folding). The indexing_map of the fused tensor in the `linalgOp` and the
///  reassociation map helps compute the indexing maps of the modified op. For
///  the above example, based on the reassociation map it can be concluded that
///
///  - The loop used to access the first dimension of the fused tensor is split
///    into two.
///  - The loop used to access the second dimension of the fused tensor is kept
///    as is.
///  - The loop used to access the third dimension of the fused tensor is split
///    into three.
///
///  i.e. (e0, e1, e2, e3, e4) is the domain of the indexing map of the modified
///  op, then
///
///   d0 -> e0, e1
///   d1 -> e2, e3, e4
///   d2 -> e5
///
///  substituting this, the generic op can be rewritten as
///
///  %d = linalg.generic ins(%0, %1 : )
///        indexing_maps =
///         [affine_map<(e0, e1, e2, e3, e4, e5) -> (e2, e3, e4, e0, e1, e5)>,
///          affine_map<(e0, e1, e2, e3, e4, e5) -> (e2, e3, e4, e5)>,
///          affine_map<(e0, e1, e2, e3, e4, e5) -> (e0, e1, e5, e2, e3, e4)>]
///
///  Since operands to the linalg generic are now 5D, reshapes can be introduced
///  to make it consistent
///
///  %0 = linalg.tensor_reshape %a
///         [affine_map<(e0, e1, e2, e3, e4, e5) -> (e0, e1, e2),
///          affine_map<(e0, e1, e2, e3, e4, e5) -> (e3, e4),
///          affine_map<(e0, e1, e2, e3, e4, e5) -> (e5)]
///       : tensor<?x?x?xf32> into tensor<?x?x?x?x?x?xf32>
///  %1 = linalg.tensor_reshape %b
///         [affine_map<(e0, e1, e2, e3) -> (e0, e1, e2),
///          affine_map<(e0, e1, e2, e3) -> (e3)]
///       : tensor<?x?x?xf32> into tensor<?x?x?x?xf32>
///
///  The added reshapes are again expanding patterns, so they will get fused
///  with its producers if possible.
static bool isFusableWithReshapeByDimExpansion(LinalgOp linalgOp,
                                               unsigned fusedTensorIndex) {
  // Is fusable only if:
  // - The linalgOp is a generic op, or an indexed_generic.
  // - All the indexing maps for operands and results in linalgOp are projected
  //   permutations.
  // - The fused tensor is not a scalar.
  // - All the loops in linalgOp are parallel loops.
  return isa<GenericOp, IndexedGenericOp>(linalgOp.getOperation()) &&
         linalgOp.hasTensorSemantics() &&
         llvm::all_of(linalgOp.indexing_maps().getValue(),
                      [](Attribute attr) {
                        return attr.cast<AffineMapAttr>()
                            .getValue()
                            .isProjectedPermutation();
                      }) &&
         linalgOp.getIndexingMap(fusedTensorIndex).getNumResults() > 0 &&
         llvm::all_of(linalgOp.iterator_types(), [](Attribute attr) {
           return attr.cast<StringAttr>().getValue() ==
                  getParallelIteratorTypeName();
         });
}

namespace {
/// Information needed to expand a generic/indexed_generic operation to fold the
/// reshape with it.
class ExpansionInfo {
public:
  // Computes the mapping from original dimensions of the op to the dimensions
  // of the expanded op given the `indexingMap` of the fused operand/result of
  // the generic/indexed_generic op, the `reassocationMaps` of the reshape op
  // and the shape of the expanded op.
  LogicalResult compute(LinalgOp linalgOp, unsigned fusedTensorIndex,
                        ArrayRef<AffineMap> reassociationMaps,
                        ArrayRef<int64_t> expandedShape);
  unsigned getOrigOpNumDims() const { return reassociation.size(); }
  unsigned getExpandedOpNumDims() const { return expandedOpNumDims; }
  ReassociationIndicesRef getExpandedDims(unsigned i) const {
    return reassociation[i];
  }
  ArrayRef<int64_t> getExpandedShapeOfDim(unsigned i) const {
    return expandedShapeMap[i];
  }

private:
  /// Reassociation from the dimensions in the original operation to the
  /// dimension of the expanded operation.
  SmallVector<ReassociationIndices, 4> reassociation;
  /// Mapping from extent of loops in the original operation, to the extent of
  /// loops in the expanded operation.
  SmallVector<SmallVector<int64_t, 4>, 4> expandedShapeMap;
  unsigned expandedOpNumDims;
};
} // namespace

LogicalResult ExpansionInfo::compute(LinalgOp linalgOp,
                                     unsigned fusedTensorIndex,
                                     ArrayRef<AffineMap> reassociationMaps,
                                     ArrayRef<int64_t> expandedShape) {
  if (reassociationMaps.empty())
    return failure();
  AffineMap fusedIndexMap = linalgOp.getIndexingMap(fusedTensorIndex);

  Optional<SmallVector<int64_t, 4>> originalLoopRange =
      linalgOp.getStaticLoopRanges();
  if (!originalLoopRange)
    return linalgOp.emitError("unable to find loop range for operation");

  reassociation.clear();
  expandedShapeMap.clear();
  // Compute the number of dimension in the expanded op that correspond to each
  // dimension of the original op.
  SmallVector<unsigned, 4> numExpandedDims(fusedIndexMap.getNumDims(), 1);
  expandedShapeMap.resize(fusedIndexMap.getNumDims());
  for (auto resultExpr : llvm::enumerate(fusedIndexMap.getResults())) {
    unsigned pos = resultExpr.value().cast<AffineDimExpr>().getPosition();
    AffineMap foldedDims = reassociationMaps[resultExpr.index()];
    numExpandedDims[pos] = foldedDims.getNumResults();
    ArrayRef<int64_t> shape =
        expandedShape.slice(foldedDims.getDimPosition(0), numExpandedDims[pos]);
    expandedShapeMap[pos].assign(shape.begin(), shape.end());
  }
  // The remaining dimensions remain the same.
  for (unsigned i : llvm::seq<unsigned>(0, fusedIndexMap.getNumDims()))
    if (expandedShapeMap[i].empty())
      expandedShapeMap[i] = {(*originalLoopRange)[i]};

  // Compute reassociation map from the original op to the expanded op.
  unsigned sum = 0;
  reassociation.reserve(fusedIndexMap.getNumDims());
  for (auto numFoldedDim : llvm::enumerate(numExpandedDims)) {
    auto seq = llvm::seq<int64_t>(sum, sum + numFoldedDim.value());
    reassociation.emplace_back(seq.begin(), seq.end());
    sum += numFoldedDim.value();
  }
  expandedOpNumDims = sum;
  return success();
}

/// Epanding the body of a linalg operation requires adaptations of the accessed
/// loop indices. Specifically, access of indices in the original operation need
/// to be replaced with linearizations of indices in the expanded op. That
/// requires the shape of the expanded dimensions to be static (at least all but
/// the most significant). For now check that these are all statically sized.
/// Note that this could be extended to handle dynamic case, but the
/// implementation below uses `affine.apply` which seems to have issues when the
/// shapes are not static.
LogicalResult isIndexedOpExpandable(LinalgOp linalgOp,
                                    const ExpansionInfo &expansionInfo) {
  for (unsigned i : llvm::seq<unsigned>(0, expansionInfo.getOrigOpNumDims())) {
    ArrayRef<int64_t> expandedShape = expansionInfo.getExpandedShapeOfDim(i);
    if (expandedShape.size() == 1)
      continue;
    for (int64_t shape : expandedShape.drop_front()) {
      if (ShapedType::isDynamic(shape)) {
        return linalgOp.emitError(
            "unable to fuse indexed generic op where the expanded dim is "
            "dynamic");
      }
    }
  }
  return success();
}

/// Return the indexing map to use in the expanded op for a given the
/// `indexingMap` of the original operation.
static AffineMap
getIndexingMapInExpandedOp(OpBuilder &builder, AffineMap indexingMap,
                           const ExpansionInfo &expansionInfo) {
  SmallVector<AffineExpr, 4> newExprs;
  for (AffineExpr expr : indexingMap.getResults()) {
    unsigned pos = expr.cast<AffineDimExpr>().getPosition();
    SmallVector<AffineExpr, 4> expandedExprs = llvm::to_vector<4>(
        llvm::map_range(expansionInfo.getExpandedDims(pos), [&](int64_t v) {
          return builder.getAffineDimExpr(static_cast<unsigned>(v));
        }));
    newExprs.append(expandedExprs.begin(), expandedExprs.end());
  }
  return AffineMap::get(expansionInfo.getExpandedOpNumDims(),
                        indexingMap.getNumSymbols(), newExprs,
                        builder.getContext());
}

/// Return the type of the operand/result to use in the expanded op given the
/// type in the original op.
static RankedTensorType getExpandedType(RankedTensorType originalType,
                                        AffineMap indexingMap,
                                        const ExpansionInfo &expansionInfo) {
  SmallVector<int64_t, 4> expandedShape;
  for (AffineExpr expr : indexingMap.getResults()) {
    unsigned dim = expr.cast<AffineDimExpr>().getPosition();
    auto dimExpansion = expansionInfo.getExpandedShapeOfDim(dim);
    expandedShape.append(dimExpansion.begin(), dimExpansion.end());
  }
  return RankedTensorType::get(expandedShape, originalType.getElementType());
}

/// Returns the reassociation maps to use in the `linalg.tensor_reshape`
/// operation to convert the operands of the origial operation to operands of
/// the expanded operation. The same method is used to compute the
/// `linalg.tensor_reshape` used to collapse the result of the expanded op to
/// get the value that can replace all uses of the results of the original op.
static SmallVector<ReassociationIndices, 4>
getReassociationForExpansion(AffineMap indexingMap,
                             const ExpansionInfo &expansionInfo) {
  SmallVector<ReassociationIndices, 4> reassociation;
  unsigned numReshapeDims = 0;
  for (AffineExpr expr : indexingMap.getResults()) {
    unsigned dim = expr.cast<AffineDimExpr>().getPosition();
    auto numExpandedDims = expansionInfo.getExpandedDims(dim).size();
    auto indices = llvm::to_vector<2>(
        llvm::seq<int64_t>(numReshapeDims, numReshapeDims + numExpandedDims));
    reassociation.emplace_back(std::move(indices));
    numReshapeDims += numExpandedDims;
  }
  return reassociation;
}

/// Build the body of the expanded IndexedGenericOp. The arguments for the
/// induction variables of the original operation need to be recovered by
/// linearizing the arguments of the corresponding dimensions of the expanded
/// op. For now it is assumed that the shapes of the expanded op needed for
/// linearization are static.
static void buildExpandedIndexedGenericOpRegion(
    PatternRewriter &rewriter, Location loc, Region &originalOpRegion,
    Region &fusedOpRegion, const ExpansionInfo &expansionInfo) {
  assert(fusedOpRegion.empty() && "expected fused op to have empty region");
  // Create an entry block in the fused region with same number of arguments
  // as the fused op
  Block *fusedEntryBlock = new Block;
  fusedOpRegion.push_back(fusedEntryBlock);
  rewriter.cloneRegionBefore(originalOpRegion, fusedOpRegion,
                             fusedOpRegion.end());

  // Merge the entry block of the fused op with the cloned blocks. For this
  // compute the value for arguments of the region in the original operation
  // in terms of the arguments of the fused op. Since the original operation
  // is expanded, the expanded dimensions need to be folded back to get the
  // replacement value for the arguments corresponding to interation index.
  // For now this expects that all the loop ranges are constants, which is
  // true if the shapes are all static. This has already been checked in the
  // precondition.
  using namespace edsc::op;
  using namespace edsc::intrinsics;
  OpBuilder::InsertionGuard guard(rewriter);
  SmallVector<Value, 4> argReplacements(originalOpRegion.getNumArguments());
  rewriter.setInsertionPointToStart(fusedEntryBlock);
  edsc::ScopedContext scopedContext(rewriter, loc);
  IndexType indexType = rewriter.getIndexType();
  for (auto i : llvm::seq<unsigned>(0, expansionInfo.getOrigOpNumDims())) {
    Value linearizedIndex = fusedEntryBlock->addArgument(indexType);
    ArrayRef<int64_t> expandedDimsShape =
        expansionInfo.getExpandedShapeOfDim(i).drop_front();
    for (unsigned shape : expandedDimsShape) {
      assert(!ShapedType::isDynamic(shape));
      linearizedIndex = linearizedIndex * std_constant_index(shape);
      linearizedIndex =
          linearizedIndex + fusedEntryBlock->addArgument(indexType);
    }
    argReplacements[i] = linearizedIndex;
  }
  for (auto i : llvm::seq<unsigned>(expansionInfo.getOrigOpNumDims(),
                                    argReplacements.size())) {
    argReplacements[i] =
        fusedEntryBlock->addArgument(originalOpRegion.getArgument(i).getType());
  }
  rewriter.mergeBlocks(fusedEntryBlock->getNextNode(), fusedEntryBlock,
                       argReplacements);
}

/// Update the body of an expanded linalg operation having index semantics. The
/// indices of the original operation need to be recovered by linearizing the
/// indices of the correspoding dimensions of the expanded operation. For now it
/// is assumed that the shapes of the expanded operation needed for
/// linearization are static.
static void updateExpandedIndexOpRegion(PatternRewriter &rewriter, Location loc,
                                        Region &fusedRegion,
                                        const ExpansionInfo &expansionInfo) {
  // Replace the original indices by the linearization of the expanded indices.
  for (IndexOp indexOp :
       llvm::make_early_inc_range(fusedRegion.front().getOps<IndexOp>())) {
    ArrayRef<int64_t> expandedDims =
        expansionInfo.getExpandedDims(indexOp.dim());
    assert(!expandedDims.empty() && "expected valid expansion info");

    // Skip index operations that are not affected by the expansion.
    if (expandedDims.size() == 1 &&
        expandedDims.front() == (int64_t)indexOp.dim())
      continue;

    // Linearize the expanded indices of the original index dimension.
    OpBuilder::InsertionGuard guard(rewriter);
    rewriter.setInsertionPointAfter(indexOp);
    ArrayRef<int64_t> expandedDimsShape =
        expansionInfo.getExpandedShapeOfDim(indexOp.dim()).drop_front();
    SmallVector<Value> expandedIndices;
    expandedIndices.reserve(expandedDims.size() - 1);
    llvm::transform(
        expandedDims.drop_front(), std::back_inserter(expandedIndices),
        [&](int64_t dim) { return rewriter.create<IndexOp>(loc, dim); });
    Value newIndex = rewriter.create<IndexOp>(loc, expandedDims.front());
    for (auto it : llvm::zip(expandedDimsShape, expandedIndices)) {
      assert(!ShapedType::isDynamic(std::get<0>(it)));
      AffineExpr idx, acc;
      bindDims(rewriter.getContext(), idx, acc);
      newIndex = rewriter.create<AffineApplyOp>(
          indexOp.getLoc(), idx + acc * std::get<0>(it),
          ValueRange{std::get<1>(it), newIndex});
    }
    rewriter.replaceOp(indexOp, newIndex);
  }
}

/// Implements the fusion of a tensor_reshape op and a generic/indexed_generic
/// op as explained in `isFusableWithReshapeByExpansion`. Assumes that those
/// conditions have been satisfied.
static Optional<SmallVector<Value, 1>>
fuseWithReshapeByExpansion(LinalgOp linalgOp, TensorReshapeOp reshapeOp,
                           unsigned fusedTensorIndex,
                           PatternRewriter &rewriter) {
  assert(isFusableWithReshapeByDimExpansion(linalgOp, fusedTensorIndex) &&
         "preconditions for fuse operation failed");
  // Check if reshape is expanding or collapsing.
  bool isExpanding =
      reshapeOp.getSrcType().getRank() < reshapeOp.getResultType().getRank();
  RankedTensorType expandedType =
      isExpanding ? reshapeOp.getResultType() : reshapeOp.getSrcType();
  bool hasIndexSemantics = linalgOp.hasIndexSemantics() ||
                           isa<IndexedGenericOp>(linalgOp.getOperation());

  ExpansionInfo expansionInfo;
  if (failed(expansionInfo.compute(linalgOp, fusedTensorIndex,
                                   reshapeOp.getReassociationMaps(),
                                   expandedType.getShape())))
    return llvm::None;

  if (hasIndexSemantics &&
      failed(isIndexedOpExpandable(linalgOp, expansionInfo)))
    return llvm::None;

  SmallVector<AffineMap, 4> expandedOpIndexingMaps = llvm::to_vector<4>(
      llvm::map_range(linalgOp.getIndexingMaps(), [&](AffineMap m) {
        return getIndexingMapInExpandedOp(rewriter, m, expansionInfo);
      }));

  SmallVector<Value, 4> expandedOpOperands;
  for (auto operand : llvm::enumerate(linalgOp.getInputs())) {
    if (operand.index() == fusedTensorIndex) {
      expandedOpOperands.push_back(reshapeOp.src());
      continue;
    }
    AffineMap indexingMap = linalgOp.getInputIndexingMap(operand.index());
    RankedTensorType expandedOperandType =
        getExpandedType(operand.value().getType().cast<RankedTensorType>(),
                        indexingMap, expansionInfo);
    if (expandedOperandType != operand.value().getType()) {
      // Reshape the operand to get the right type.
      SmallVector<ReassociationIndices, 4> reassociation =
          getReassociationForExpansion(indexingMap, expansionInfo);
      expandedOpOperands.push_back(rewriter.create<TensorReshapeOp>(
          linalgOp.getLoc(), expandedOperandType, operand.value(),
          reassociation));
      continue;
    }
    expandedOpOperands.push_back(operand.value());
  }

  Location loc = linalgOp.getLoc();
  SmallVector<Value, 1> outputs;
  for (auto result : llvm::enumerate(linalgOp.getOutputs())) {
    AffineMap indexingMap = linalgOp.getOutputIndexingMap(result.index());
    RankedTensorType expandedOutputType =
        getExpandedType(result.value().getType().cast<RankedTensorType>(),
                        indexingMap, expansionInfo);
    if (expandedOutputType != result.value().getType()) {
      SmallVector<ReassociationIndices, 4> reassociation =
          getReassociationForExpansion(indexingMap, expansionInfo);
      outputs.push_back(rewriter.create<TensorReshapeOp>(
          linalgOp.getLoc(), expandedOutputType, result.value(),
          reassociation));
    }
  }

  // The iterator types of the expanded op are all parallel.
  SmallVector<StringRef, 4> iteratorTypes(expansionInfo.getExpandedOpNumDims(),
                                          getParallelIteratorTypeName());

  TypeRange resultTypes = ValueRange(outputs).getTypes();
  LinalgOp fusedOp = createLinalgOpOfSameType(
      linalgOp, rewriter, linalgOp.getLoc(), resultTypes,
      /*inputs=*/expandedOpOperands, outputs, expandedOpIndexingMaps,
      iteratorTypes);
  Region &fusedRegion = fusedOp->getRegion(0);
  Region &originalRegion = linalgOp->getRegion(0);

  if (isa<GenericOp>(linalgOp.getOperation())) {
    rewriter.cloneRegionBefore(originalRegion, fusedRegion,
                               fusedRegion.begin());
  } else {
    assert(isa<IndexedGenericOp>(linalgOp.getOperation()));
    buildExpandedIndexedGenericOpRegion(rewriter, loc, originalRegion,
                                        fusedRegion, expansionInfo);
  }

  // Update the index accesses after the expansion.
  if (linalgOp.hasIndexSemantics())
    updateExpandedIndexOpRegion(rewriter, loc, fusedRegion, expansionInfo);

  // Reshape the result values to their original shape if this is a collapsing
  // reshape folded into its consumer.
  SmallVector<Value, 1> resultVals;
  for (auto result : llvm::enumerate(linalgOp->getResults())) {
    if (!isExpanding &&
        resultTypes[result.index()] != result.value().getType()) {
      SmallVector<ReassociationIndices, 4> reassociation =
          getReassociationForExpansion(
              linalgOp.getOutputIndexingMap(result.index()), expansionInfo);
      resultVals.push_back(rewriter.create<TensorReshapeOp>(
          linalgOp.getLoc(), result.value().getType(),
          fusedOp->getResult(result.index()), reassociation));
    } else {
      resultVals.push_back(fusedOp->getResult(result.index()));
    }
  }
  // Assuming a single result.
  return resultVals;
}

namespace {

/// Pattern to fold tensor_reshape op with its consumer by using the source of
/// the reshape op as the operand in the consumer (instead of the result of the
/// tensor_reshapeop) when the tensor_reshape op is collapsing. The
/// corresponding index map in the consumer needs to be modified to linearize
/// the folded dimension.
///
/// For example,
///
/// #map0 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
/// %0 = linalg.tensor_reshape %arg0
///        [affine_map<(i, j, k, l) -> (i)>, affine_map<(i, j, k, l) -> (j, k)>,
///         affine_map<(i, j, k, l) -> (l)>]
///      tensor<?x?x?xf32> into tensor<?x?x4x?xf32>
/// %1 = linalg.generic { indexing_maps = [#map0, #map0, #map0], ... }
///        ins(%0, %arg1 : tensor<?x?x4x?xf32>, tensor<?x?x4x?xf32>) ...
///        -> tensor<?x?x4x?xf32>
///
/// can be folded into
///
/// #map0 = affine_map<(d0, d1, d2, d3) -> (d0, d1 * 4 + d2, d3)>
/// #map1 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
/// %0 = linalg.generic { indexing_maps = [#map0, #map1, #map1] ... }
///        ins(%arg0, %arg1 : tensor<?x?x?xf32>, tensor<?x?x4x?xf32>) ...
///        -> tensor<?x?x4x?xf32>
template <typename LinalgOpTy, bool foldUnitDimReshapesOnly>
struct FoldProducerReshapeOpByLinearization
    : public OpRewritePattern<LinalgOpTy> {
  using OpRewritePattern<LinalgOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(LinalgOpTy op,
                                PatternRewriter &rewriter) const override {
    if (!op.hasTensorSemantics())
      return failure();
    LinalgOp linalgOp = cast<LinalgOp>(op.getOperation());
    for (auto operand : llvm::enumerate(linalgOp.getInputs())) {
      TensorReshapeOp reshapeOp =
          operand.value().getDefiningOp<TensorReshapeOp>();
      if (!reshapeOp ||
          !isTensorReshapeOpFoldableByLinearization(
              reshapeOp, linalgOp.getInputIndexingMap(operand.index()),
              /*asProducer =*/true) ||
          (foldUnitDimReshapesOnly &&
           !isUnitDimExpansionOnly(reshapeOp.getResultType().getShape(),
                                   reshapeOp.getReassociationMaps())))
        continue;

      // Compute the fused operands list,
      SmallVector<Value, 2> fusedOperands(linalgOp.getInputs());
      fusedOperands[operand.index()] = reshapeOp.src();
      fusedOperands.append(linalgOp.getOutputs().begin(),
                           linalgOp.getOutputs().end());

      // Compute indexing_maps for the fused operation. The indexing_maps for
      // the operands of the consumers that arent fused are the same.
      SmallVector<AffineMap, 4> fusedIndexMaps = llvm::to_vector<4>(
          op.indexing_maps().template getAsValueRange<AffineMapAttr>());

      // Accepted consumer maps are either identity or permutation.
      auto invMap = inversePermutation(fusedIndexMaps[operand.index()]);

      // Compute the indexing map to use for the result of the producer.
      AffineMap modifiedMap =
          linearizeCollapsedDims(invMap, reshapeOp.getResultType().getShape(),
                                 reshapeOp.getReassociationMaps());
      for (AffineExpr expr : modifiedMap.getResults()) {
        if (!expr.isPureAffine())
          return failure();
      }
      fusedIndexMaps[operand.index()] = modifiedMap;

      // Further check that the resulting index maps can be fused and
      // inverted. Without this the resultant op is not legal.
      if (!inversePermutation(concatAffineMaps(fusedIndexMaps))) {
        return rewriter.notifyMatchFailure(
            op, "fused op loop bound computation failed");
      }

      rewriter.startRootUpdate(op);
      op->setOperands(fusedOperands);
      op.indexing_mapsAttr(rewriter.getAffineMapArrayAttr(fusedIndexMaps));
      rewriter.finalizeRootUpdate(op);
      return success();
    }
    return failure();
  }
};

static SmallVector<ReassociationIndices>
getReassociationIndices(ArrayRef<AffineMap> maps) {
  SmallVector<ReassociationIndices> reassociation;
  for (AffineMap map : maps) {
    ReassociationIndices indices;
    for (unsigned i = 0, e = map.getNumResults(); i < e; i++) {
      unsigned pos = map.getResult(i).cast<AffineDimExpr>().getPosition();
      indices.push_back(pos);
    }
    reassociation.push_back(indices);
  }
  return reassociation;
}

/// Pattern to move rank reducing reshape after an elementwise linalg generic
/// op. This is useful to expose more fusion opportunities between named ops and
/// generic op. This can only be done if there is no broadcast or permuation
/// within the dimensions we need to merge.
///
/// For example,
///
///  %0 = linalg.tensor_reshape %A [
///    affine_map<(d0, d1, d2) -> (d0, d1)>, affine_map<(d0, d1, d2) -> (d2)>]
///      : tensor<12544x16xf32> into tensor<112x112x16xf32>
///  %2 = linalg.generic {indexing_maps = [
///    affine_map<(d0, d1, d2) -> (d0, d1, d2)>,
///    affine_map<(d0, d1, d2) -> (d2)>,
///    affine_map<(d0, d1, d2) -> (d0, d1, d2)>], iterator_types =
///    ["parallel", "parallel", "parallel"]} {
///  } -> tensor<112x112x16xf32>
///
///  into
///
///  %2 = linalg.generic {indexing_maps = [
///    affine_map<(d0, d1) -> (d0, d1)>,
///    affine_map<(d0, d1) -> (d1)>,
///    affine_map<(d0, d1) -> (d0, d1)>],
///    iterator_types = ["parallel", "parallel"]} ins(%arg0, %arg1
///    : tensor<12544x16xf32>, tensor<16xf32>) outs(%1 : tensor<12544x16xf32>) {
///  } -> tensor<12544x16xf32>
///  %3 = linalg.tensor_reshape %2 [
///    #affine_map<(d0, d1, d2) -> (d0, d1)>, affine_map<(d0, d1, d2) -> (d2)>]
///    : tensor<12544x16xf32> into tensor<112x112x16xf32>
template <typename GenericOpTy>
struct PushExpandingReshape : public OpRewritePattern<GenericOpTy> {
  using OpRewritePattern<GenericOpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(GenericOpTy op,
                                PatternRewriter &rewriter) const override {
    // Only apply to elementwise linalg on tensor.
    if (!op.hasTensorSemantics() ||
        op.getNumParallelLoops() != op.getNumLoops())
      return failure();
    // Only support identity output maps. It could be extended to permuations if
    // needed.
    if (llvm::any_of(op.getOutputIndexingMaps(),
                     [](AffineMap map) { return !map.isIdentity(); }))
      return failure();
    int64_t destRank = op.getNumParallelLoops();
    SmallVector<Value, 4> newOperands = llvm::to_vector<4>(op.getInputs());
    TensorReshapeOp reshapeFound;
    // 1. Look for tensor_reshape operands and figure out save the dimensions
    // merged.
    for (auto operand : llvm::enumerate(op.getInputs())) {
      TensorReshapeOp reshapeOp =
          operand.value().template getDefiningOp<TensorReshapeOp>();
      if (!reshapeOp || reshapeOp.getSrcType().getRank() >
                            reshapeOp.getResultType().getRank()) {
        continue;
      }
      // TODO: We could support non-identity map as long as the merged
      // dimensions are still contiguous.
      if (!op.getIndexingMaps()[operand.index()].isIdentity())
        continue;
      if (reshapeFound) {
        // Only support a second reshape op if it has the same reassociate maps.
        if (reshapeFound.getReassociationMaps() ==
            reshapeOp.getReassociationMaps())
          newOperands[operand.index()] = reshapeOp.src();
        continue;
      }
      reshapeFound = reshapeOp;
      newOperands[operand.index()] = reshapeOp.src();
    }
    if (!reshapeFound)
      return failure();

    // Calculate the reassociation indices and rassociated reverse map.
    SmallVector<ReassociationIndices> reassociation =
        getReassociationIndices(reshapeFound.getReassociationMaps());
    SmallVector<unsigned, 4> remap(destRank);
    for (auto &indices : llvm::enumerate(reassociation)) {
      for (int64_t index : indices.value()) {
        remap[index] = indices.index();
      }
    }
    // 2. Verify that we can merge the dimensions in the linalg and that we
    // don't need to create new reshapes operands. Inserting new reshape
    // operands would defeat the purpose of the transformation.
    for (auto operand : llvm::enumerate(op.getInputs())) {
      if (operand.value() == newOperands[operand.index()]) {
        AffineMap map = op.getIndexingMaps()[operand.index()];
        for (unsigned i : llvm::seq(unsigned(0), map.getNumResults())) {
          if (reassociation[remap[map.getDimPosition(i)]].size() > 1)
            return failure();
        }
      }
    }

    // 3. Calculate the affine map remapping and the reassociation to apply to
    // output tensors.
    SmallVector<AffineMap, 4> newMaps;
    unsigned newRank = reassociation.size();
    for (auto map : op.getIndexingMaps()) {
      SmallVector<AffineExpr> newExprs;
      for (auto expr : map.getResults()) {
        unsigned position = expr.template cast<AffineDimExpr>().getPosition();
        // Skip dimension merged except for the last of the group.
        if (reassociation[remap[position]].back() == position) {
          newExprs.push_back(
              getAffineDimExpr(remap[position], op.getContext()));
        }
      }
      newMaps.push_back(AffineMap::get(newRank, 0, newExprs, op.getContext()));
    }

    // 4. Reshape the output tensors.
    SmallVector<Value> newOutputs;
    SmallVector<Type> newOutputTypes;
    for (auto output : op.outputs()) {
      auto newOutputType = RankedTensorType::get(
          reshapeFound.getSrcType().getShape(),
          output.getType().template cast<RankedTensorType>().getElementType());
      Value newOutput = rewriter.create<TensorReshapeOp>(
          op->getLoc(), newOutputType, output, reassociation);
      newOutputTypes.push_back(newOutputType);
      newOutputs.push_back(newOutput);
    }
    // 5. Create a new generic op with lowerer rank.
    SmallVector<StringRef, 4> iteratorTypes(newRank,
                                            getParallelIteratorTypeName());
    auto newOp =
        rewriter.create<GenericOpTy>(op->getLoc(), newOutputTypes, newOperands,
                                     newOutputs, newMaps, iteratorTypes);
    rewriter.inlineRegionBefore(op.region(), newOp.region(),
                                newOp.region().begin());
    // 6. Reshape the so that the type matches the uses.
    SmallVector<Value> newResults;
    for (auto result : llvm::enumerate(newOp->getResults())) {
      newResults.push_back(rewriter.create<TensorReshapeOp>(
          op->getLoc(), op.getOutputTensorTypes()[result.index()],
          result.value(), reassociation));
    }
    rewriter.replaceOp(op, newResults);
    return success();
  }
};

/// Pattern to fuse a tensor_reshape op with its consumer
/// generic/indexed_generic op, when the reshape op is collapsing
/// dimensions. The dimensionality of the loop in the consumer is expanded.
template <typename GenericOpTy>
class FoldWithProducerReshapeOpByExpansion
    : public OpRewritePattern<GenericOpTy> {
public:
  FoldWithProducerReshapeOpByExpansion(
      MLIRContext *context, ControlElementwiseOpsFusionFn foldReshapes,
      PatternBenefit benefit = 1)
      : OpRewritePattern<GenericOpTy>(context, benefit),
        controlFoldingReshapes(foldReshapes) {}

  LogicalResult matchAndRewrite(GenericOpTy genericOp,
                                PatternRewriter &rewriter) const override {
    LinalgOp linalgOp = cast<LinalgOp>(genericOp.getOperation());
    for (auto operand : llvm::enumerate(linalgOp.getInputs())) {
      TensorReshapeOp reshapeOp =
          operand.value().getDefiningOp<TensorReshapeOp>();
      if (!reshapeOp)
        continue;
      // Fold only if
      // - The tensor reshape op is folding.
      // - All constraints of fusing with reshape by expansion are met.
      if (reshapeOp.getSrcType().getRank() <
              reshapeOp.getResultType().getRank() ||
          !isFusableWithReshapeByDimExpansion(linalgOp, operand.index()) ||
          (!controlFoldingReshapes(
              reshapeOp->getResult(0),
              linalgOp.getInputOpOperands()[operand.index()])))
        continue;

      Optional<SmallVector<Value, 1>> replacementValues =
          fuseWithReshapeByExpansion(linalgOp, reshapeOp, operand.index(),
                                     rewriter);
      if (!replacementValues)
        return failure();
      rewriter.replaceOp(genericOp, replacementValues.getValue());
      return success();
    }
    return failure();
  }

private:
  ControlElementwiseOpsFusionFn controlFoldingReshapes;
};

/// Pattern to fold tensor_reshape op with its producer. The corresponding index
/// map in the consumer needs to be modified to linearize the folded dimension.
template <bool foldUnitDimReshapesOnly>
struct FoldConsumerReshapeOpByLinearization
    : public OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    LinalgOp producer = reshapeOp.src().getDefiningOp<LinalgOp>();
    if (!producer ||
        !isa<GenericOp, IndexedGenericOp>(producer.getOperation()) ||
        !producer.hasTensorSemantics() || producer.getNumOutputs() != 1 ||
        !isTensorReshapeOpFoldableByLinearization(
            reshapeOp, producer.getOutputIndexingMap(0),
            /*asProducer =*/false) ||
        (foldUnitDimReshapesOnly &&
         !isUnitDimExpansionOnly(reshapeOp.getSrcType().getShape(),
                                 reshapeOp.getReassociationMaps())))
      return failure();
    // The indexing_maps for the operands of the fused operation are same as
    // those for the operands of the producer.
    SmallVector<AffineMap, 4> fusedIndexMaps = llvm::to_vector<4>(
        producer.indexing_maps().getAsValueRange<AffineMapAttr>());

    auto invMap = inversePermutation(producer.getOutputIndexingMap(0));

    // Compute the indexing map to use for the operand of the producer.
    AffineMap modifiedMap =
        linearizeCollapsedDims(invMap, reshapeOp.getSrcType().getShape(),
                               reshapeOp.getReassociationMaps());
    for (AffineExpr expr : modifiedMap.getResults()) {
      if (!expr.isPureAffine()) {
        return rewriter.notifyMatchFailure(
            producer, "fused op indexing map is not affine");
      }
    }
    fusedIndexMaps.back() = modifiedMap;

    // Further check that the resulting index maps can be fused and
    // inverted. Without this the resultant op is not legal.
    if (!inversePermutation(concatAffineMaps(fusedIndexMaps))) {
      return rewriter.notifyMatchFailure(
          producer, "fused op loop bound computation failed");
    }

    Location loc = producer.getLoc();
    Value output = rewriter.create<TensorReshapeOp>(
        loc, producer.getOutputs()[0], reshapeOp.getReassociationExprs());
    LinalgOp fusedOp = createLinalgOpOfSameType(
        producer, rewriter, loc, reshapeOp.getResultType(),
        /*inputs=*/producer.getInputs(),
        // TODO: handle outputs.
        /*outputs=*/output, rewriter.getAffineMapArrayAttr(fusedIndexMaps),
        producer.iterator_types(),
        /*doc=*/nullptr,
        /*library_call=*/nullptr,
        /*sparse=*/nullptr);
    auto &fusedRegion = fusedOp->getRegion(0);
    rewriter.cloneRegionBefore(producer->getRegion(0), fusedRegion,
                               fusedRegion.begin());
    rewriter.replaceOp(reshapeOp, fusedOp->getResults());
    return success();
  }
};

/// Pattern to fold a tensor_reshape op with its producer generic op if the
/// tensor_reshape op is expanding, by expanding the dimensionality of the loop
/// in the producer op.
struct FoldReshapeWithGenericOpByExpansion
    : public OpRewritePattern<TensorReshapeOp> {
  using OpRewritePattern<TensorReshapeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(TensorReshapeOp reshapeOp,
                                PatternRewriter &rewriter) const override {
    // Fold only if
    // - The tensor reshape op is a expanding case.
    // - All constraints of fusing with reshape by expansion are met.
    if (reshapeOp.getSrcType().getRank() > reshapeOp.getResultType().getRank())
      return failure();
    LinalgOp producer = reshapeOp.src().getDefiningOp<LinalgOp>();
    if (!producer || producer.getNumOutputs() != 1 ||
        !isFusableWithReshapeByDimExpansion(producer,
                                            producer.getNumInputs()) ||
        isUnitDimExpansionOnly(reshapeOp.getResultType().getShape(),
                               reshapeOp.getReassociationMaps()))
      return failure();
    Optional<SmallVector<Value, 1>> replacementValues =
        fuseWithReshapeByExpansion(producer, reshapeOp, producer.getNumInputs(),
                                   rewriter);
    if (!replacementValues)
      return failure();
    rewriter.replaceOp(reshapeOp, replacementValues.getValue());
    return success();
  }
};

/// Pattern to fold a GenericOp/IndexedGenericOp with a splat constant.
template <typename LinalgOpTy>
class FoldSplatConstants : public OpRewritePattern<LinalgOpTy> {
public:
  FoldSplatConstants(MLIRContext *context, ControlElementwiseOpsFusionFn &fun,
                     PatternBenefit benefit = 1)
      : OpRewritePattern<LinalgOpTy>(context, benefit), controlFn(fun) {}

  LogicalResult matchAndRewrite(LinalgOpTy op,
                                PatternRewriter &rewriter) const override {
    if (!op.hasTensorSemantics())
      return failure();
    LinalgOp linalgOp = cast<LinalgOp>(op.getOperation());
    for (auto operand : llvm::enumerate(linalgOp.getInputOpOperands())) {
      Operation *def = operand.value().get().getDefiningOp();
      DenseElementsAttr constantAttr;
      if (!def ||
          !matchPattern(def, m_Constant<DenseElementsAttr>(&constantAttr)) ||
          !constantAttr.isSplat() ||
          !controlFn(def->getResult(0), operand.value()))
        continue;

      // The indexing_maps for the operands of the fused operation are same as
      // those for the operands of the linalgOp without the indexing map at
      // operand.index()
      SmallVector<AffineMap, 4> fusedIndexMaps = llvm::to_vector<4>(
          linalgOp.indexing_maps().getAsValueRange<AffineMapAttr>());
      fusedIndexMaps.erase(std::next(fusedIndexMaps.begin(), operand.index()));

      // Check if the operation shapes to loops map is computable.
      if (!inversePermutation(concatAffineMaps(fusedIndexMaps))) {
        return rewriter.notifyMatchFailure(
            linalgOp, "fused op loop bound computation failed");
      }

      // The operands list is same as the linalgOp with the argument for
      // constant index dropped.
      SmallVector<Value, 4> fusedOperands(linalgOp.getInputs());
      fusedOperands.erase(std::next(fusedOperands.begin(), operand.index()));

      // Create a constant scalar value from the splat constant.
      Value scalarConstant = rewriter.create<ConstantOp>(
          def->getLoc(), constantAttr.getSplatValue());

      LinalgOp fusedOp = createLinalgOpOfSameType(
          linalgOp, rewriter, rewriter.getUnknownLoc(),
          linalgOp->getResultTypes(),
          /*inputs=*/fusedOperands,
          /*outputs=*/linalgOp.getOutputs(),
          rewriter.getAffineMapArrayAttr(fusedIndexMaps),
          linalgOp.iterator_types(),
          /*doc=*/nullptr,
          /*library_call=*/nullptr,
          /*sparse=*/nullptr);

      // Map the block argument corresponding to the replaced argument with the
      // scalar constant.
      Region &linalgOpRegion = linalgOp->getRegion(0);
      Block &entryBlock = *linalgOpRegion.begin();
      unsigned argIndex = entryBlock.getNumArguments() -
                          linalgOp.getNumShapedOperands() + operand.index();
      BlockAndValueMapping mapping;
      mapping.map(entryBlock.getArgument(argIndex), scalarConstant);
      Region &fusedRegion = fusedOp->getRegion(0);
      rewriter.cloneRegionBefore(linalgOpRegion, fusedRegion,
                                 fusedRegion.begin(), mapping);
      rewriter.replaceOp(linalgOp, fusedOp->getResults());
      return success();
    }
    return failure();
  }

private:
  ControlElementwiseOpsFusionFn controlFn;
};
} // namespace

static Optional<SmallVector<Value, 1>>
fuseElementwiseOps(PatternRewriter &rewriter, OpOperand &consumerOpOperand,
                   const ControlElementwiseOpsFusionFn &controlFn) {
  Operation *producer = consumerOpOperand.get().getDefiningOp();
  if (!producer || producer->getNumResults() != 1)
    return llvm::None;

  // Fuse when consumer is GenericOp or IndexedGenericOp.
  if (!isa<GenericOp, IndexedGenericOp>(consumerOpOperand.getOwner()) ||
      !isa<GenericOp, IndexedGenericOp>(producer))
    return llvm::None;

  return fuseElementwiseOpsImpl(cast<LinalgOp>(producer), consumerOpOperand,
                                controlFn, rewriter);
}

bool mlir::linalg::skipUnitDimReshape(const OpResult &producer,
                                      const OpOperand &consumer) {
  auto reshapeOp = producer.getDefiningOp<linalg::TensorReshapeOp>();
  return !isUnitDimExpansionOnly(reshapeOp.getSrcType().getShape(),
                                 reshapeOp.getReassociationMaps());
}

namespace {
/// Patterns to fuse a generic op, with the producer of its operands.
template <typename LinalgOpTy>
class FuseElementwiseOps : public OpRewritePattern<LinalgOpTy> {
public:
  FuseElementwiseOps(MLIRContext *context, ControlElementwiseOpsFusionFn &fun,
                     PatternBenefit benefit = 1)
      : OpRewritePattern<LinalgOpTy>(context, benefit), controlFn(fun) {}

  LogicalResult matchAndRewrite(LinalgOpTy op,
                                PatternRewriter &rewriter) const override {
    // Find the first operand that is defined by another generic op on tensors.
    for (OpOperand &opOperand : op.getShapedOpOperands()) {
      LinalgOp producerOp =
          dyn_cast_or_null<LinalgOp>(opOperand.get().getDefiningOp());
      if (!producerOp || !producerOp.hasTensorSemantics())
        continue;
      Optional<SmallVector<Value, 1>> fusedOpResults =
          fuseElementwiseOps(rewriter, opOperand, controlFn);
      if (fusedOpResults) {
        rewriter.replaceOp(op, *fusedOpResults);
        return success();
      }
    }
    return failure();
  }

private:
  ControlElementwiseOpsFusionFn controlFn;
};

/// Pass that fuses generic ops on tensors. Used only for testing.
struct FusionOfTensorOpsPass
    : public LinalgFusionOfTensorOpsBase<FusionOfTensorOpsPass> {
  void runOnOperation() override {
    Operation *op = getOperation();
    RewritePatternSet patterns(op->getContext());
    ControlElementwiseOpsFusionFn allowFoldingFn =
        [](const OpResult &producer, const OpOperand &consumer) {
          return true;
        };
    populateElementwiseOpsFusionPatterns(
        patterns,
        LinalgElementwiseFusionOptions().setControlFoldingReshapes(
            allowFoldingUnitDimReshapes ? allowFoldingFn : skipUnitDimReshape));
    (void)applyPatternsAndFoldGreedily(op->getRegions(), std::move(patterns));
  }
};

/// Pass to test folding of reshape op with generic/indexed_generic ops by
/// linearization.
struct FoldReshapeOpsByLinearizationPass
    : public LinalgFoldReshapeOpsByLinearizationBase<
          FoldReshapeOpsByLinearizationPass> {
  void runOnOperation() override {
    Operation *op = getOperation();
    RewritePatternSet patterns(op->getContext());
    populateFoldReshapeOpsByLinearizationPatterns(patterns);
    (void)applyPatternsAndFoldGreedily(op->getRegions(), std::move(patterns));
  }
};

} // namespace

void mlir::linalg::populateFoldReshapeOpsByLinearizationPatterns(
    RewritePatternSet &patterns) {
  patterns.add<FoldProducerReshapeOpByLinearization<GenericOp, false>,
               FoldProducerReshapeOpByLinearization<IndexedGenericOp, false>,
               FoldConsumerReshapeOpByLinearization<false>>(
      patterns.getContext());
}

void mlir::linalg::populateFoldUnitDimsReshapeOpsByLinearizationPatterns(
    RewritePatternSet &patterns) {
  patterns.add<FoldProducerReshapeOpByLinearization<GenericOp, true>,
               FoldProducerReshapeOpByLinearization<IndexedGenericOp, true>,
               FoldConsumerReshapeOpByLinearization<true>>(
      patterns.getContext());
}

void mlir::linalg::populateFoldReshapeOpsByExpansionPatterns(
    RewritePatternSet &patterns,
    ControlElementwiseOpsFusionFn controlFoldingReshapes) {
  patterns.add<FoldReshapeWithGenericOpByExpansion>(patterns.getContext());
  patterns.add<FoldWithProducerReshapeOpByExpansion<GenericOp>,
               FoldWithProducerReshapeOpByExpansion<IndexedGenericOp>>(
      patterns.getContext(), controlFoldingReshapes);
}

void mlir::linalg::populateElementwiseOpsFusionPatterns(
    RewritePatternSet &patterns, LinalgElementwiseFusionOptions options) {
  auto *context = patterns.getContext();
  patterns
      .add<FuseElementwiseOps<GenericOp>, FuseElementwiseOps<IndexedGenericOp>,
           FoldSplatConstants<GenericOp>, FoldSplatConstants<IndexedGenericOp>>(
          context, options.controlElementwiseOpsFusionFn);
  populateFoldReshapeOpsByExpansionPatterns(patterns,
                                            options.controlFoldingReshapesFn);
  AffineApplyOp::getCanonicalizationPatterns(patterns, context);
  GenericOp::getCanonicalizationPatterns(patterns, context);
  IndexedGenericOp::getCanonicalizationPatterns(patterns, context);
  TensorReshapeOp::getCanonicalizationPatterns(patterns, context);
}

void mlir::linalg::populatePushReshapeOpsPatterns(RewritePatternSet &patterns) {
  auto *context = patterns.getContext();
  patterns.add<PushExpandingReshape<GenericOp>,
               PushExpandingReshape<IndexedGenericOp>>(context);
}

std::unique_ptr<Pass> mlir::createLinalgFusionOfTensorOpsPass() {
  return std::make_unique<FusionOfTensorOpsPass>();
}

std::unique_ptr<Pass> mlir::createFoldReshapeOpsByLinearizationPass() {
  return std::make_unique<FoldReshapeOpsByLinearizationPass>();
}
