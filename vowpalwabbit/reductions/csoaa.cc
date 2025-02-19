// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.
#include "csoaa.h"

#include "config/options.h"
#include "io/logger.h"
#include "setup_base.h"
#include "vw.h"
#include "vw_exception.h"

#include <utility>

using namespace VW::LEARNER;
using namespace COST_SENSITIVE;
using namespace VW::config;

#undef VW_DEBUG_LOG
#define VW_DEBUG_LOG vw_dbg::csoaa

namespace CSOAA
{
struct csoaa
{
  uint32_t num_classes = 0;
  int indexing = -1;
  bool search = false;
  VW::polyprediction* pred = nullptr;
  VW::io::logger logger;
  csoaa(VW::io::logger logger) : logger(std::move(logger)) {}
  ~csoaa() { free(pred); }
};

template <bool is_learn>
inline void inner_loop(single_learner& base, VW::example& ec, uint32_t i, float cost, uint32_t& prediction,
    float& score, float& partial_prediction, int indexing)
{
  if (is_learn)
  {
    ec.weight = (cost == FLT_MAX) ? 0.f : 1.f;
    ec.l.simple.label = cost;
    if (indexing == 0) { base.learn(ec, i); }
    else
    {
      base.learn(ec, i - 1);
    }
  }
  else
  {
    if (indexing == 0) { base.predict(ec, i); }
    else
    {
      base.predict(ec, i - 1);
    }
  }

  partial_prediction = ec.partial_prediction;
  if (ec.partial_prediction < score || (ec.partial_prediction == score && i < prediction))
  {
    score = ec.partial_prediction;
    prediction = i;
  }
  add_passthrough_feature(ec, i, ec.partial_prediction);
}

#define DO_MULTIPREDICT true

template <bool is_learn>
void predict_or_learn(csoaa& c, single_learner& base, VW::example& ec)
{
  if (!c.search)
  {
    for (auto& cost : ec.l.cs.costs)
    {
      auto& lbl = cost.class_index;
      // Update indexing
      if (c.indexing == -1 && lbl == 0)
      {
        c.logger.out_info("label 0 found -- labels are now considered 0-indexed.");
        c.indexing = 0;
      }
      else if (c.indexing == -1 && lbl == c.num_classes)
      {
        c.logger.out_info("label {0} found -- labels are now considered 1-indexed.", c.num_classes);
        c.indexing = 1;
      }

      // Label validation
      if (c.indexing == 0 && lbl >= c.num_classes)
      {
        c.logger.out_warn(
            "label {0} is not in {{0,{1}}}. This won't work for 0-indexed actions.", lbl, c.num_classes - 1);
        lbl = 0;
      }
      else if (c.indexing == 1 && (lbl < 1 || lbl > c.num_classes))
      {
        c.logger.out_warn("label {0} is not in {{1,{1}}}. This won't work for 1-indexed actions.", lbl, c.num_classes);
        lbl = static_cast<uint32_t>(c.num_classes);
      }
    }
  }

  COST_SENSITIVE::label ld = std::move(ec.l.cs);

  // Guard VW::example state restore against throws
  auto restore_guard = VW::scope_exit([&ld, &ec] { ec.l.cs = std::move(ld); });

  uint32_t prediction = (c.indexing == 0) ? 0 : 1;
  float score = FLT_MAX;
  size_t pt_start = ec.passthrough ? ec.passthrough->size() : 0;
  ec.l.simple = {0.};
  ec._reduction_features.template get<simple_label_reduction_features>().reset_to_default();

  bool dont_learn = DO_MULTIPREDICT && !is_learn;

  if (!ld.costs.empty())
  {
    for (auto& cl : ld.costs)
    { inner_loop<is_learn>(base, ec, cl.class_index, cl.x, prediction, score, cl.partial_prediction, c.indexing); }
    ec.partial_prediction = score;
  }
  else if (dont_learn)
  {
    ec.l.simple = {FLT_MAX};
    ec._reduction_features.template get<simple_label_reduction_features>().reset_to_default();

    base.multipredict(ec, 0, c.num_classes, c.pred, false);
    if (c.indexing == 0)
    {
      for (uint32_t i = 0; i <= c.num_classes; i++)
      {
        add_passthrough_feature(ec, i, c.pred[i].scalar);
        if (c.pred[i].scalar < c.pred[prediction].scalar) { prediction = i; }
      }
      ec.partial_prediction = c.pred[prediction].scalar;
    }
    else
    {
      for (uint32_t i = 1; i <= c.num_classes; i++)
      {
        add_passthrough_feature(ec, i, c.pred[i - 1].scalar);
        if (c.pred[i - 1].scalar < c.pred[prediction - 1].scalar) { prediction = i; }
      }
      ec.partial_prediction = c.pred[prediction - 1].scalar;
    }
  }
  else
  {
    float temp;
    for (uint32_t i = 1; i <= c.num_classes; i++)
    { inner_loop<false>(base, ec, i, FLT_MAX, prediction, score, temp, c.indexing); }
  }

  if (ec.passthrough)
  {
    uint64_t second_best = 0;
    float second_best_cost = FLT_MAX;
    for (size_t i = 0; i < ec.passthrough->size() - pt_start; i++)
    {
      float val = ec.passthrough->values[pt_start + i];
      if ((val > ec.partial_prediction) && (val < second_best_cost))
      {
        second_best_cost = val;
        second_best = ec.passthrough->indices[pt_start + i];
      }
    }
    if (second_best_cost < FLT_MAX)
    {
      float margin = second_best_cost - ec.partial_prediction;
      add_passthrough_feature(ec, constant * 2, margin);
      add_passthrough_feature(ec, constant * 2 + 1 + second_best, 1.);
    }
    else
      add_passthrough_feature(ec, constant * 3, 1.);
  }

  ec.pred.multiclass = prediction;
}

void finish_example(VW::workspace& all, csoaa&, VW::example& ec) { COST_SENSITIVE::finish_example(all, ec); }

base_learner* csoaa_setup(VW::setup_base_i& stack_builder)
{
  options_i& options = *stack_builder.get_options();
  VW::workspace& all = *stack_builder.get_all_pointer();
  auto c = VW::make_unique<csoaa>(all.logger);
  option_group_definition new_options("[Reduction] Cost Sensitive One Against All");
  new_options
      .add(make_option("csoaa", c->num_classes).keep().necessary().help("One-against-all multiclass with <k> costs"))
      .add(make_option("indexing", c->indexing).one_of({0, 1}).keep().help("Choose between 0 or 1-indexing"));

  if (!options.add_parse_and_check_necessary(new_options)) { return nullptr; }

  if (options.was_supplied("probabilities"))
  { THROW("csoaa does not support probabilities flag, please use oaa or multilabel_oaa"); }
  c->search = options.was_supplied("search");

  c->pred = calloc_or_throw<VW::polyprediction>(c->num_classes);
  size_t ws = c->num_classes;
  auto* l = make_reduction_learner(std::move(c), as_singleline(stack_builder.setup_base_learner()),
      predict_or_learn<true>, predict_or_learn<false>, stack_builder.get_setupfn_name(csoaa_setup))
                .set_learn_returns_prediction(
                    true) /* csoaa.learn calls gd.learn. nothing to be gained by calling csoaa.predict first */
                .set_params_per_weight(ws)
                .set_output_prediction_type(VW::prediction_type_t::multiclass)
                .set_input_label_type(VW::label_type_t::cs)
                .set_finish_example(finish_example)
                .build();

  all.example_parser->lbl_parser = cs_label;
  all.cost_sensitive = make_base(*l);
  return all.cost_sensitive;
}
}  // namespace CSOAA
