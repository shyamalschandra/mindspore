/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "pre_activate/ascend/ascend_backend_optimization.h"
#include <memory>
#include <string>
#include "pre_activate/common/optimizer.h"
#include "pre_activate/ascend/ir_fission/bn_split.h"
#include "pre_activate/ascend/ir_fission/bn_grad_split.h"
#include "pre_activate/ascend/ir_fusion/fused_batch_norm_fusion.h"
#include "pre_activate/ascend/ir_fission/layer_norm_grad_split.h"
#include "pre_activate/ascend/ir_fusion/allreduce_fusion.h"
#include "pre_activate/ascend/ir_fusion/square_sum_fusion.h"
#include "pre_activate/ascend/ir_fusion/clip_by_norm_no_div_square_sum_fusion.h"
#include "pre_activate/ascend/ir_fusion/lamb_update_with_lr_rule_fusion.h"
#include "pre_activate/ascend/ir_fusion/clip_by_value_fusion.h"
#include "pre_activate/ascend/ir_fusion/confusion_softmax_grad_rule.h"
#include "pre_activate/ascend/ir_fusion/lamb_next_mv_rule.h"
#include "pre_activate/ascend/ir_fusion/lamb_next_mv_with_decay_rule.h"
#include "pre_activate/ascend/ir_fusion/lamb_next_mv_with_decay_v1_rule.h"
#include "pre_activate/ascend/ir_fusion/lamb_next_right_rule.h"
#include "pre_activate/ascend/ir_fusion/lamb_update_with_lr_v2.h"
#include "pre_activate/ascend/ir_fusion/layer_norm_beta_gamma_backprop_fusion.h"
#include "pre_activate/ascend/ir_fusion/reshape_transpose_fusion.h"
#include "pre_activate/ascend/ir_fusion/transpose_reshape_fusion.h"
#include "pre_activate/ascend/ir_fusion/adam_apply_one_fusion.h"
#include "pre_activate/ascend/ir_fusion/adam_apply_one_with_decay_rule.h"
#include "pre_activate/ascend/ir_fusion/transpose_transdata_fusion.h"
#include "pre_activate/ascend/ir_fusion/transdata_split.h"
#include "pre_activate/ascend/ir_fission/topk_split.h"
#include "pre_activate/ascend/ir_fusion/momentum_lossscale_fusion.h"
#include "pre_activate/ascend/ir_fusion/mul_add_fusion.h"
#include "pre_activate/ascend/ir_fusion/mul_addn_fusion.h"
#include "pre_activate/ascend/ir_fusion/matmul_biasadd_fusion.h"
#include "pre_activate/ascend/format_type/insert_trans_op.h"
#include "pre_activate/pass/getitem_tuple.h"
#include "pre_activate/pass/optimize_dependence.h"
#include "pre_activate/pass/erase_visit_attr.h"
#include "pre_activate/ascend/format_type/insert_cast.h"
#include "pre_activate/pass/eliminate_redundant_op.h"
#include "pre_activate/pass/common_subexpression_elimination.h"
#include "pre_activate/ascend/format_type/merge_cast_to_op.h"
#include "pre_activate/ascend/format_type/check_consistency.h"
#include "pre_activate/ascend/buffer_fusion/buffer_fusion.h"
#include "pre_activate/ascend/format_type/deal_ref_trans_and_cast.h"
#include "pre_activate/ascend/ir_fission/add_memcpy_async.h"
#include "pre_activate/ascend/format_type/insert_cast_for_runop.h"
#include "pre_activate/ascend/format_type/insert_transdata_for_runop.h"
#include "utils/context/ms_context.h"
#include "debug/anf_ir_dump.h"
#include "debug/anf_ir_utils.h"

namespace mindspore {
namespace opt {
void RunOpAscendDataLayout(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto data_layout_pm = std::make_shared<PassManager>("pynative_transop_pm");
  data_layout_pm->AddPass(std::make_shared<LayerNormGradSplit>());
  data_layout_pm->AddPass(std::make_shared<RunOpInsertTransData>());
  data_layout_pm->AddPass(std::make_shared<GetitemTuple>());
  data_layout_pm->AddPass(std::make_shared<CommonSubexpressionElimination>());
  data_layout_pm->AddPass(std::make_shared<EliminateRedundantOp>());
  data_layout_pm->AddPass(std::make_shared<OptimizeDependence>());
  data_layout_pm->AddPass(std::make_shared<EraseVisitAttr>());
  data_layout_pm->AddPass(std::make_shared<TransDataSplit>());
  optimizer->AddPassManager(data_layout_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void RunOpAscendMixPrecision(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto mixed_precision_pm = std::make_shared<PassManager>("pynative_transop_pm");
  mixed_precision_pm->AddPass(std::make_shared<RunOpInsertCast>());
  mixed_precision_pm->AddPass(std::make_shared<GetitemTuple>());
  mixed_precision_pm->AddPass(std::make_shared<CommonSubexpressionElimination>());
  mixed_precision_pm->AddPass(std::make_shared<EliminateRedundantOp>());
  mixed_precision_pm->AddPass(std::make_shared<OptimizeDependence>());
  mixed_precision_pm->AddPass(std::make_shared<DealRefTransAndCast>());
  mixed_precision_pm->AddPass(std::make_shared<GetitemTuple>());
  mixed_precision_pm->AddPass(std::make_shared<MergeCastToOp>());
  mixed_precision_pm->AddPass(std::make_shared<LayerNormBetaGammaBackpropFusion>());
  mixed_precision_pm->AddPass(std::make_shared<EraseVisitAttr>());
  optimizer->AddPassManager(mixed_precision_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void AscendDataLayout(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto data_layout_pm = std::make_shared<PassManager>("transop_pm");
  data_layout_pm->AddPass(std::make_shared<LayerNormGradSplit>());
  data_layout_pm->AddPass(std::make_shared<InsertTransOp>());
  data_layout_pm->AddPass(std::make_shared<GetitemTuple>());
  data_layout_pm->AddPass(std::make_shared<CommonSubexpressionElimination>());
  data_layout_pm->AddPass(std::make_shared<EliminateRedundantOp>());
  data_layout_pm->AddPass(std::make_shared<OptimizeDependence>());
  data_layout_pm->AddPass(std::make_shared<EraseVisitAttr>());
  data_layout_pm->AddPass(std::make_shared<TransDataSplit>());
  optimizer->AddPassManager(data_layout_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void AscendMixPrecision(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto mixed_precision_pm = std::make_shared<PassManager>("cast_pm");
  mixed_precision_pm->AddPass(std::make_shared<InsertCast>());
  mixed_precision_pm->AddPass(std::make_shared<GetitemTuple>());
  mixed_precision_pm->AddPass(std::make_shared<CommonSubexpressionElimination>());
  mixed_precision_pm->AddPass(std::make_shared<EliminateRedundantOp>());
  mixed_precision_pm->AddPass(std::make_shared<OptimizeDependence>());
  mixed_precision_pm->AddPass(std::make_shared<DealRefTransAndCast>());
  mixed_precision_pm->AddPass(std::make_shared<GetitemTuple>());
  mixed_precision_pm->AddPass(std::make_shared<MergeCastToOp>());
  mixed_precision_pm->AddPass(std::make_shared<LayerNormBetaGammaBackpropFusion>());
  mixed_precision_pm->AddPass(std::make_shared<EraseVisitAttr>());
  optimizer->AddPassManager(mixed_precision_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
}

void AscendBackendIRFusionOptimization(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->save_graphs_flag();
  auto save_graphs_path = context_ptr->save_graphs_path();
  if (save_graphs_path.empty()) {
    save_graphs_path = ".";
  }
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_ir_fusion_before.ir";
    DumpIR(file_path, kernel_graph);
    DumpIRProto(kernel_graph, "before_hwopt");
  }
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto ir_fusion_pm = std::make_shared<PassManager>("ir_fusion_pm");
  ir_fusion_pm->AddPass(std::make_shared<BnSplit>());
  ir_fusion_pm->AddPass(std::make_shared<BnGradSplit>());
  ir_fusion_pm->AddPass(std::make_shared<AddMemcpyAsync>());
  if (context_ptr->ir_fusion_flag()) {
    ir_fusion_pm->AddPass(std::make_shared<SquareSumFusion>());
    ir_fusion_pm->AddPass(std::make_shared<ClipByNormNoDivSquareSumFusion>());
    ir_fusion_pm->AddPass(std::make_shared<LambUpdateWithLRRuleFusion>());
    ir_fusion_pm->AddPass(std::make_shared<ConfusionSoftmaxGradRule>());
    ir_fusion_pm->AddPass(std::make_shared<LambNextMVWithDecayV1Rule>());
    ir_fusion_pm->AddPass(std::make_shared<LambNextMVRule>());
    ir_fusion_pm->AddPass(std::make_shared<LambNextMVWithDecayRule>());
    ir_fusion_pm->AddPass(std::make_shared<LambNextRightRule>());
    ir_fusion_pm->AddPass(std::make_shared<LambUpdateWithLrV2>());
    ir_fusion_pm->AddPass(std::make_shared<ReshapeTransposeFusion>());
    ir_fusion_pm->AddPass(std::make_shared<TransposeReshapeFusion>());
    ir_fusion_pm->AddPass(std::make_shared<ClipByValueFusion>());
    ir_fusion_pm->AddPass(std::make_shared<FusedBatchNormFusion>());
    ir_fusion_pm->AddPass(std::make_shared<TopKSplit>());
    ir_fusion_pm->AddPass(std::make_shared<AdamApplyOneWithDecayRule>());
    ir_fusion_pm->AddPass(std::make_shared<AdamApplyOneFusion>());
    ir_fusion_pm->AddPass(std::make_shared<MomentumLossscaleFusion>());
    ir_fusion_pm->AddPass(std::make_shared<MulAddFusion>());
    ir_fusion_pm->AddPass(std::make_shared<MulAddNFusion>());
    ir_fusion_pm->AddPass(std::make_shared<MatmulBiasaddFusion>());
    ir_fusion_pm->AddPass(std::make_shared<GetitemTuple>());
    ir_fusion_pm->AddPass(std::make_shared<TransposeTransDataFusion>());
  }
  optimizer->AddPassManager(ir_fusion_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_ir_fusion_after.ir";
    DumpIR(file_path, kernel_graph);
  }
}

void RunOpAscendBackendIRFusionOptimization(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  if (!context_ptr->ir_fusion_flag()) {
    MS_LOG(INFO) << "IRFusion is not enable, skip";
    return;
  }
  bool save_graphs = context_ptr->save_graphs_flag();
  auto save_graphs_path = context_ptr->save_graphs_path();
  if (save_graphs_path.empty()) {
    save_graphs_path = ".";
  }
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_ir_fusion_before.ir";
    DumpIR(file_path, kernel_graph);
  }
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto ir_fusion_pm = std::make_shared<PassManager>("ir_fusion_pm");
  ir_fusion_pm->AddPass(std::make_shared<BnSplit>());

  optimizer->AddPassManager(ir_fusion_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_ir_fusion_after.ir";
    DumpIR(file_path, kernel_graph);
  }
}

void AscendBackendOptimization(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  auto context_ptr = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context_ptr);
  bool save_graphs = context_ptr->save_graphs_flag();
  auto save_graphs_path = context_ptr->save_graphs_path();
  if (save_graphs_path.empty()) {
    save_graphs_path = ".";
  }
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_before.ir";
    DumpIR(file_path, kernel_graph);
  }
  // data layout optimization
  AscendDataLayout(kernel_graph);
  // mixed precision optimization
  AscendMixPrecision(kernel_graph);
  // buffer fusion
  // other optimization
  auto optimizer = std::make_shared<GraphOptimizer>();
  auto other_pm = std::make_shared<PassManager>("other_pm");
  other_pm->AddPass(std::make_shared<AllReduceFusion>());
  other_pm->AddPass(std::make_shared<BufferFusion>());
  other_pm->AddPass(std::make_shared<GetitemTuple>());
  other_pm->AddPass(std::make_shared<CommonSubexpressionElimination>());
  other_pm->AddPass(std::make_shared<CheckConsistency>());
  optimizer->AddPassManager(other_pm);
  (void)optimizer->Optimize(kernel_graph);
  kernel_graph->SetExecOrderByDefault();
  if (save_graphs) {
    std::string file_path = save_graphs_path + "/" + "hwopt_d_end.ir";
    DumpIR(file_path, kernel_graph);
    DumpIRProto(kernel_graph, "after_hwopt");
  }
}
}  // namespace opt
}  // namespace mindspore
