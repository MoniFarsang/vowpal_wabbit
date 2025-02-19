// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "cb_continuous_label.h"

#include "cb_label_parser.h"
#include "debug_print.h"
#include "example.h"
#include "io/logger.h"
#include "model_utils.h"
#include "parse_primitives.h"
#include "text_utils.h"
#include "vw.h"
#include "vw_exception.h"

#include <cfloat>
#include <iomanip>

using namespace LEARNER;

namespace CB
{
template <>
void default_label_additional_fields<VW::cb_continuous::continuous_label>(VW::cb_continuous::continuous_label&)
{
}
}  // namespace CB

void parse_pdf(const std::vector<VW::string_view>& words, size_t words_index, VW::label_parser_reuse_mem& reuse_mem,
    VW::reduction_features& red_features, VW::io::logger& logger)
{
  auto& cats_reduction_features = red_features.template get<VW::continuous_actions::reduction_features>();
  for (size_t i = words_index; i < words.size(); i++)
  {
    if (words[i] == CHOSEN_ACTION) { break; /* no more pdf to parse*/ }
    tokenize(':', words[i], reuse_mem.tokens);
    if (reuse_mem.tokens.empty() || reuse_mem.tokens.size() < 3) { continue; }
    VW::continuous_actions::pdf_segment seg;
    seg.left = float_of_string(reuse_mem.tokens[0], logger);
    seg.right = float_of_string(reuse_mem.tokens[1], logger);
    seg.pdf_value = float_of_string(reuse_mem.tokens[2], logger);
    cats_reduction_features.pdf.push_back(seg);
  }
  if (!VW::continuous_actions::is_valid_pdf(cats_reduction_features.pdf)) { cats_reduction_features.pdf.clear(); }
}

void parse_chosen_action(const std::vector<VW::string_view>& words, size_t words_index,
    VW::label_parser_reuse_mem& reuse_mem, VW::reduction_features& red_features, VW::io::logger& logger)
{
  auto& cats_reduction_features = red_features.template get<VW::continuous_actions::reduction_features>();
  for (size_t i = words_index; i < words.size(); i++)
  {
    tokenize(':', words[i], reuse_mem.tokens);
    if (reuse_mem.tokens.empty() || reuse_mem.tokens.size() < 1) { continue; }
    cats_reduction_features.chosen_action = float_of_string(reuse_mem.tokens[0], logger);
    break;  // there can only be one chosen action
  }
}

namespace VW
{
namespace cb_continuous
{
////////////////////////////////////////////////////
// Begin: parse a,c,p label format
void parse_label(continuous_label& ld, reduction_features& red_features, VW::label_parser_reuse_mem& reuse_mem,
    const std::vector<VW::string_view>& words, VW::io::logger& logger)
{
  ld.costs.clear();

  if (words.empty()) { return; }

  if (!(words[0] == CA_LABEL)) { THROW("Continuous actions labels require the first word to be ca"); }

  for (size_t i = 1; i < words.size(); i++)
  {
    if (words[i] == PDF) { parse_pdf(words, i + 1, reuse_mem, red_features, logger); }
    else if (words[i] == CHOSEN_ACTION)
    {
      parse_chosen_action(words, i + 1, reuse_mem, red_features, logger);
    }
    else if (words[i - 1] == CA_LABEL)
    {
      continuous_label_elm f{0.f, FLT_MAX, 0.f};
      tokenize(':', words[i], reuse_mem.tokens);

      if (reuse_mem.tokens.empty() || reuse_mem.tokens.size() > 4)
        THROW("malformed cost specification: "
            << "reuse_mem.tokens");

      f.action = float_of_string(reuse_mem.tokens[0], logger);

      if (reuse_mem.tokens.size() > 1) { f.cost = float_of_string(reuse_mem.tokens[1], logger); }

      if (std::isnan(f.cost))
        THROW("error NaN cost (" << reuse_mem.tokens[1] << " for action: " << reuse_mem.tokens[0]);

      f.pdf_value = .0;
      if (reuse_mem.tokens.size() > 2) { f.pdf_value = float_of_string(reuse_mem.tokens[2], logger); }

      if (std::isnan(f.pdf_value))
        THROW("error NaN pdf_value (" << reuse_mem.tokens[2] << " for action: " << reuse_mem.tokens[0]);

      if (f.pdf_value < 0.0)
      {
        logger.err_warn("invalid pdf_value < 0 specified for an action, resetting to 0.");
        f.pdf_value = .0;
      }

      ld.costs.push_back(f);
    }
  }
}

label_parser the_label_parser = {
    // default_label
    [](polylabel& label) { CB::default_label<continuous_label>(label.cb_cont); },
    // parse_label
    [](polylabel& label, reduction_features& red_features, VW::label_parser_reuse_mem& reuse_mem,
        const VW::named_labels* /*ldict*/, const std::vector<VW::string_view>& words,
        VW::io::logger& logger) { parse_label(label.cb_cont, red_features, reuse_mem, words, logger); },
    // cache_label
    [](const polylabel& label, const reduction_features& /*red_features*/, io_buf& cache,
        const std::string& upstream_name,
        bool text) { return VW::model_utils::write_model_field(cache, label.cb_cont, upstream_name, text); },
    // read_cached_label
    [](polylabel& label, reduction_features& /*red_features*/, io_buf& cache) {
      return VW::model_utils::read_model_field(cache, label.cb_cont);
    },
    // get_weight
    // CB::weight just returns 1.f? This seems like it could be a bug...
    [](const polylabel& /*label*/, const reduction_features& /*red_features*/) { return 1.f; },
    // test_label
    [](const polylabel& label) { return CB::is_test_label<continuous_label, continuous_label_elm>(label.cb_cont); },
    // label type
    VW::label_type_t::continuous};

// End: parse a,c,p label format
////////////////////////////////////////////////////

}  // namespace cb_continuous

std::string to_string(const cb_continuous::continuous_label_elm& elm, int decimal_precision)
{
  return fmt::format("{{{},{},{}}}", VW::fmt_float(elm.action, decimal_precision),
      VW::fmt_float(elm.cost, decimal_precision), VW::fmt_float(elm.pdf_value, decimal_precision));
}

std::string to_string(const cb_continuous::continuous_label& lbl, int decimal_precision)
{
  std::ostringstream strstream;
  strstream << "[l.cb_cont={";
  for (const auto cost : lbl.costs) { strstream << to_string(cost, decimal_precision); }
  strstream << "}]";
  return strstream.str();
}

namespace model_utils
{
size_t read_model_field(io_buf& io, VW::cb_continuous::continuous_label_elm& cle)
{
  size_t bytes = 0;
  bytes += read_model_field(io, cle.action);
  bytes += read_model_field(io, cle.cost);
  bytes += read_model_field(io, cle.pdf_value);
  return bytes;
}

size_t write_model_field(
    io_buf& io, const VW::cb_continuous::continuous_label_elm& cle, const std::string& upstream_name, bool text)
{
  size_t bytes = 0;
  bytes += write_model_field(io, cle.action, upstream_name + "_action", text);
  bytes += write_model_field(io, cle.cost, upstream_name + "_cost", text);
  bytes += write_model_field(io, cle.pdf_value, upstream_name + "_pdf_value", text);
  return bytes;
}

size_t read_model_field(io_buf& io, VW::cb_continuous::continuous_label& cl)
{
  size_t bytes = 0;
  bytes += read_model_field(io, cl.costs);
  return bytes;
}

size_t write_model_field(
    io_buf& io, const VW::cb_continuous::continuous_label& cl, const std::string& upstream_name, bool text)
{
  size_t bytes = 0;
  bytes += write_model_field(io, cl.costs, upstream_name + "_costs", text);
  return bytes;
}
}  // namespace model_utils
}  // namespace VW
