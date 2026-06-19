// SPDX-License-Identifier: AGPL-3.0-only
// Part of Alkahest. See LICENSE for terms.
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "alkahest/core.hpp"
#include "alkahest/server.hpp"
#include "alkahest/tui.hpp"
#include "alkahest/util.hpp"

namespace alk {

namespace {

const char* kVersion = "0.1.0";

void print_logo(std::ostream& os) {
  os << "alkahest " << kVersion << " — the universal solvent for text\n";
}

std::string read_all(std::istream& is) {
  std::ostringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open file: " + path);
  return read_all(f);
}

// Build a recipe from already-tokenized CLI arguments, where a standalone "+"
// separates one operation from the next. The first token of each group is the
// operation name; the remainder are its arguments (taken verbatim — the shell
// has already done the quoting).
Recipe recipe_from_args(const std::vector<std::string>& toks) {
  Recipe recipe;
  Step cur;
  bool have = false;
  for (const auto& t : toks) {
    if (t == "+") {
      if (have) recipe.push_back(cur);
      cur = Step{};
      have = false;
      continue;
    }
    if (!have) { cur.op = t; have = true; }
    else cur.args.push_back(t);
  }
  if (have) recipe.push_back(cur);
  return recipe;
}

void print_general_help(std::ostream& os) {
  print_logo(os);
  os << "\n"
        "Pipe text through composable operations, or chain them into recipes.\n"
        "\n"
        "USAGE\n"
        "  alk <op> [args] [+ <op> [args] ...]   transform stdin and print result\n"
        "  alk run <recipe.alk> [more.alk ...]   run operations from a file\n"
        "  alk list [category]                   list available operations\n"
        "  alk help <op>                         show details for one operation\n"
        "  alk serve [--host H] [--port N]       start the local web UI / API\n"
        "  alk tui                               interactive recipe builder\n"
        "  alk version                           print version\n"
        "\n"
        "OPTIONS\n"
        "  -i, --in FILE     read input from FILE instead of stdin\n"
        "  -o, --out FILE    write output to FILE instead of stdout\n"
        "  -n, --no-newline  do not append a trailing newline\n"
        "\n"
        "EXAMPLES\n"
        "  echo -n hello | alk to-hex\n"
        "  printf 'b,a,c' | alk \"split ,\" + sort + \"join ,\"\n"
        "  alk to-base64 + sha256 <<< \"data\"\n"
        "  echo 68656c6c6f | alk detect\n"
        "  alk run clean.alk < messy.csv\n"
        "\n"
        "Run 'alk list' to see all " << registry().all().size()
     << " operations. Alkahest is free software (AGPL-3.0).\n";
}

void list_ops(std::ostream& os, const std::string& only_cat) {
  const auto& reg = registry();
  for (const auto& cat : reg.categories()) {
    if (!only_cat.empty() && cat != only_cat) continue;
    os << "\n" << cat << "\n";
    for (const auto* op : reg.all()) {
      if (op->category != cat) continue;
      std::string name = op->name;
      if (!op->aliases.empty()) {
        name += " (";
        for (size_t i = 0; i < op->aliases.size(); ++i) {
          if (i) name += ", ";
          name += op->aliases[i];
        }
        name += ")";
      }
      std::ostringstream left;
      left << "  " << name;
      std::string l = left.str();
      if (l.size() < 28) l += std::string(28 - l.size(), ' ');
      os << l << op->summary << "\n";
    }
  }
  if (!only_cat.empty()) {
    bool found = false;
    for (const auto& c : reg.categories()) if (c == only_cat) found = true;
    if (!found) os << "No such category: " << only_cat << "\n";
  }
}

int help_for_op(std::ostream& os, const std::string& name) {
  const Operation* op = registry().find(name);
  if (!op) {
    os << "Unknown operation: " << name << "\n";
    // Offer suggestions.
    auto hits = registry().search(name);
    if (!hits.empty()) {
      os << "Did you mean: ";
      for (size_t i = 0; i < hits.size() && i < 5; ++i) {
        if (i) os << ", ";
        os << hits[i]->name;
      }
      os << "?\n";
    }
    return 1;
  }
  os << op->name;
  if (!op->aliases.empty()) {
    os << "  (aka ";
    for (size_t i = 0; i < op->aliases.size(); ++i) {
      if (i) os << ", ";
      os << op->aliases[i];
    }
    os << ")";
  }
  os << "\n  " << op->summary << "\n";
  os << "  category: " << op->category << "\n";
  os << "  usage:    " << op->usage << "\n";
  if (op->max_args == 0) os << "  (takes no arguments)\n";
  else if (op->max_args < 0) os << "  (takes " << op->min_args << " or more arguments)\n";
  else if (op->min_args == op->max_args) os << "  (takes exactly " << op->min_args << ")\n";
  else os << "  (takes " << op->min_args << "-" << op->max_args << " arguments)\n";
  return 0;
}

}  // namespace

int cli_main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);

  // --- pull out global options (only those appearing before the first op) ---
  std::string in_file, out_file;
  bool no_newline = false;
  size_t i = 0;
  for (; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-i" || a == "--in") {
      if (i + 1 >= args.size()) { std::cerr << "alk: " << a << " needs a file\n"; return 2; }
      in_file = args[++i];
    } else if (a == "-o" || a == "--out") {
      if (i + 1 >= args.size()) { std::cerr << "alk: " << a << " needs a file\n"; return 2; }
      out_file = args[++i];
    } else if (a == "-n" || a == "--no-newline") {
      no_newline = true;
    } else {
      break;  // first non-option token: subcommand or operation
    }
  }
  std::vector<std::string> rest(args.begin() + i, args.end());

  // --- no arguments: help if interactive, else act as a pass-through (cat) ---
  if (rest.empty()) {
    if (util::is_tty(0)) { print_general_help(std::cout); return 0; }
    // Reading from a pipe with no ops: just copy input through.
    std::cout << read_all(std::cin);
    return 0;
  }

  const std::string& cmd = rest[0];

  // --- subcommands ---
  if (cmd == "version" || cmd == "--version" || cmd == "-v") {
    print_logo(std::cout);
    return 0;
  }
  if (cmd == "help" || cmd == "--help" || cmd == "-h") {
    if (rest.size() >= 2) return help_for_op(std::cout, rest[1]);
    print_general_help(std::cout);
    return 0;
  }
  if (cmd == "list") {
    list_ops(std::cout, rest.size() >= 2 ? rest[1] : std::string());
    return 0;
  }
  if (cmd == "serve") {
    std::string host = "127.0.0.1";
    int port = 8744;
    for (size_t k = 1; k < rest.size(); ++k) {
      if ((rest[k] == "--host" || rest[k] == "-H") && k + 1 < rest.size()) host = rest[++k];
      else if ((rest[k] == "--port" || rest[k] == "-p") && k + 1 < rest.size())
        port = std::stoi(rest[++k]);
    }
    return run_server(host, port);
  }
  if (cmd == "tui") {
    std::string seed;
    if (!in_file.empty()) seed = read_file(in_file);
    else if (!util::is_tty(0)) seed = read_all(std::cin);
    return run_tui(seed);
  }

  // --- otherwise: build and run a recipe ---
  Recipe recipe;
  try {
    if (cmd == "run") {
      if (rest.size() < 2) { std::cerr << "alk: run needs a recipe file\n"; return 2; }
      std::string text;
      for (size_t k = 1; k < rest.size(); ++k) {
        text += read_file(rest[k]);
        if (!text.empty() && text.back() != '\n') text.push_back('\n');
      }
      recipe = parse_recipe_text(text);
    } else {
      recipe = recipe_from_args(rest);
    }
  } catch (const std::exception& e) {
    std::cerr << "alk: " << e.what() << "\n";
    return 2;
  }

  if (recipe.empty()) { std::cerr << "alk: no operations to run\n"; return 2; }

  // Validate operation names up front for a friendly error.
  for (const auto& step : recipe) {
    if (!registry().find(step.op)) {
      std::cerr << "alk: unknown operation '" << step.op << "'";
      auto hits = registry().search(step.op);
      if (!hits.empty()) std::cerr << " (did you mean '" << hits[0]->name << "'?)";
      std::cerr << "\n";
      return 2;
    }
  }

  // Read input.
  std::string input;
  try {
    if (!in_file.empty()) input = read_file(in_file);
    else if (!util::is_tty(0)) input = read_all(std::cin);
    // else: leave empty (some ops like `lorem`/`eval <expr>` need no input)
  } catch (const std::exception& e) {
    std::cerr << "alk: " << e.what() << "\n";
    return 2;
  }

  // Run.
  std::string output;
  try {
    output = run_recipe(registry(), recipe, input);
  } catch (const OpError& e) {
    std::cerr << "alk: " << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "alk: " << e.what() << "\n";
    return 1;
  }

  // Write output.
  if (!no_newline && (output.empty() || output.back() != '\n')) output.push_back('\n');
  if (!out_file.empty()) {
    std::ofstream f(out_file, std::ios::binary);
    if (!f) { std::cerr << "alk: cannot write file: " << out_file << "\n"; return 2; }
    f << output;
  } else {
    std::cout << output;
  }
  return 0;
}

}  // namespace alk
