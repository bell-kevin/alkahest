// SPDX-License-Identifier: AGPL-3.0-only
// Part of Alkahest. See LICENSE for terms.
#include <algorithm>
#include <cctype>

#include "alkahest/core.hpp"
#include "alkahest/util.hpp"

namespace alk {

void Registry::add(Operation op) {
  for (const auto& a : op.aliases) alias_[a] = op.name;
  ops_[op.name] = std::move(op);
}

const Operation* Registry::find(const std::string& name) const {
  auto it = ops_.find(name);
  if (it != ops_.end()) return &it->second;
  auto a = alias_.find(name);
  if (a != alias_.end()) {
    auto it2 = ops_.find(a->second);
    if (it2 != ops_.end()) return &it2->second;
  }
  return nullptr;
}

std::vector<const Operation*> Registry::all() const {
  std::vector<const Operation*> v;
  v.reserve(ops_.size());
  for (const auto& kv : ops_) v.push_back(&kv.second);
  std::sort(v.begin(), v.end(),
            [](const Operation* a, const Operation* b) { return a->name < b->name; });
  return v;
}

std::vector<std::string> Registry::categories() const {
  std::vector<std::string> cats;
  for (const auto& kv : ops_) {
    if (std::find(cats.begin(), cats.end(), kv.second.category) == cats.end())
      cats.push_back(kv.second.category);
  }
  std::sort(cats.begin(), cats.end());
  return cats;
}

namespace {
// Subsequence fuzzy match: returns a score (higher = better) or -1 if no match.
// Contiguous and early matches score higher; an exact prefix wins big.
int fuzzy_score(const std::string& hay_in, const std::string& needle_in) {
  std::string hay = util::to_lower(hay_in);
  std::string needle = util::to_lower(needle_in);
  if (needle.empty()) return 1;
  if (hay == needle) return 10000;
  if (hay.rfind(needle, 0) == 0) return 5000 - (int)hay.size();  // prefix
  size_t pos = hay.find(needle);
  if (pos != std::string::npos) return 2000 - (int)pos;  // substring
  // subsequence
  size_t h = 0, n = 0;
  int score = 0, streak = 0;
  while (h < hay.size() && n < needle.size()) {
    if (hay[h] == needle[n]) {
      ++n;
      streak += 1;
      score += 5 + streak;  // reward consecutive matches
    } else {
      streak = 0;
    }
    ++h;
  }
  if (n != needle.size()) return -1;
  return score - (int)pos;
}
}  // namespace

std::vector<const Operation*> Registry::search(const std::string& query) const {
  std::vector<std::pair<int, const Operation*>> scored;
  for (const auto& kv : ops_) {
    const Operation& op = kv.second;
    int best = fuzzy_score(op.name, query);
    for (const auto& a : op.aliases) best = std::max(best, fuzzy_score(a, query));
    // a weaker match against the summary still surfaces related ops
    int sumv = fuzzy_score(op.summary, query);
    if (sumv > 0) best = std::max(best, sumv / 4);
    if (best > 0) scored.emplace_back(best, &op);
  }
  std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second->name < b.second->name;
  });
  std::vector<const Operation*> out;
  out.reserve(scored.size());
  for (auto& p : scored) out.push_back(p.second);
  return out;
}

Registry& registry() {
  static Registry* r = [] {
    auto* reg = new Registry();
    register_all(*reg);
    return reg;
  }();
  return *r;
}

// ---- Recipes ----

// Truncate a recipe line at the first *unquoted* '#' that begins a token
// (i.e. at line start or preceded by whitespace), mirroring shell comments.
// A '#' inside quotes, backslash-escaped, or embedded mid-token stays literal.
static std::string strip_trailing_comment(const std::string& line) {
  enum { NONE, SINGLE, DOUBLE } q = NONE;
  bool prev_space = true;  // start-of-line counts as a token boundary
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (q == SINGLE) {
      if (c == '\'') q = NONE;
      prev_space = false;
    } else if (q == DOUBLE) {
      if (c == '"') q = NONE;
      else if (c == '\\' && i + 1 < line.size()) ++i;  // skip escaped char
      prev_space = false;
    } else {  // NONE (unquoted)
      if (c == '#' && prev_space) return line.substr(0, i);
      if (std::isspace((unsigned char)c)) {
        prev_space = true;
      } else {
        if (c == '\'') q = SINGLE;
        else if (c == '"') q = DOUBLE;
        else if (c == '\\' && i + 1 < line.size()) ++i;  // escaped char is literal
        prev_space = false;
      }
    }
  }
  return line;
}

Step parse_step_line(const std::string& line) {
  std::string t = util::trim(strip_trailing_comment(line));
  Step s;
  if (t.empty()) return s;  // empty / comment-only line signals "skip"
  auto toks = util::tokenize(t);
  if (toks.empty()) return s;
  s.op = toks[0];
  s.args.assign(toks.begin() + 1, toks.end());
  return s;
}

Recipe parse_recipe_text(const std::string& text) {
  Recipe r;
  for (const auto& line : util::split_lines(text)) {
    Step s = parse_step_line(line);
    if (!s.op.empty()) r.push_back(std::move(s));
  }
  return r;
}

std::string recipe_to_text(const Recipe& r) {
  std::string out;
  for (const auto& step : r) {
    out += step.op;
    for (const auto& a : step.args) {
      out.push_back(' ');
      // quote args containing whitespace or quotes
      bool need = a.empty();
      for (char c : a)
        if (std::isspace((unsigned char)c) || c == '"' || c == '\'') need = true;
      if (need) {
        out.push_back('"');
        for (char c : a) {
          if (c == '"' || c == '\\') out.push_back('\\');
          out.push_back(c);
        }
        out.push_back('"');
      } else {
        out += a;
      }
    }
    out.push_back('\n');
  }
  return out;
}

std::string run_recipe(const Registry& reg, const Recipe& recipe,
                       const std::string& input) {
  std::string cur = input;
  for (size_t idx = 0; idx < recipe.size(); ++idx) {
    const Step& step = recipe[idx];
    const Operation* op = reg.find(step.op);
    if (!op) {
      throw OpError("step " + std::to_string(idx + 1) + ": unknown operation '" +
                    step.op + "'");
    }
    int n = (int)step.args.size();
    if (n < op->min_args ||
        (op->max_args >= 0 && n > op->max_args)) {
      throw OpError("step " + std::to_string(idx + 1) + " (" + op->name +
                    "): wrong number of arguments; usage: " + op->usage);
    }
    try {
      cur = op->fn(cur, step.args);
    } catch (const std::exception& e) {
      throw OpError("step " + std::to_string(idx + 1) + " (" + op->name +
                    "): " + e.what());
    }
  }
  return cur;
}

}  // namespace alk
