// SPDX-License-Identifier: AGPL-3.0-only
// Part of Alkahest. See LICENSE for terms.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "alkahest/util.hpp"
#include "ops_internal.hpp"

namespace alk::ops {
namespace {

std::string stats(const std::string& in) {
  size_t bytes = in.size();
  size_t chars = util::utf8_count(in);
  auto lines = util::split_lines(in);
  size_t line_count = in.empty() ? 0 : lines.size();
  // Word count: runs of non-whitespace.
  size_t words = 0;
  bool in_word = false;
  for (char c : in) {
    if (std::isspace((unsigned char)c)) in_word = false;
    else if (!in_word) { in_word = true; ++words; }
  }
  double H = util::shannon_entropy(in);
  std::ostringstream o;
  o << "lines       " << line_count << "\n"
    << "words       " << words << "\n"
    << "characters  " << chars << "\n"
    << "bytes       " << bytes << "\n"
    << "entropy     " << util::fmt_double(std::round(H * 1000) / 1000) << " bits/byte";
  return o.str();
}

std::string char_frequency(const std::string& in) {
  std::map<unsigned char, size_t> freq;
  for (unsigned char c : in) ++freq[c];
  // Sort by descending count.
  std::vector<std::pair<unsigned char, size_t>> v(freq.begin(), freq.end());
  std::stable_sort(v.begin(), v.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  size_t max = v.empty() ? 1 : v.front().second;
  std::ostringstream o;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) o << "\n";
    unsigned char c = v[i].first;
    std::string label;
    if (c == ' ') label = "' ' (space)";
    else if (c == '\n') label = "\\n";
    else if (c == '\t') label = "\\t";
    else if (c == '\r') label = "\\r";
    else if (std::isprint(c)) label = std::string("'") + (char)c + "'";
    else { char b[8]; std::snprintf(b, sizeof b, "0x%02x", c); label = b; }
    int bar = (int)std::lround(40.0 * v[i].second / max);
    std::ostringstream lab;
    lab.width(12);
    lab << std::left << label;
    o << lab.str() << std::string(bar, '#') << " " << v[i].second;
  }
  return o.str();
}

// A printable preview, with non-printable bytes shown as dots and length capped.
std::string preview(const std::string& s, size_t cap = 64) {
  std::string out;
  for (size_t i = 0; i < s.size() && i < cap; ++i) {
    unsigned char c = s[i];
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\r') out += "\\r";
    else if (std::isprint(c)) out.push_back((char)c);
    else out.push_back('.');
  }
  if (s.size() > cap) out += "...";
  return out;
}

// ---- the "detect" showcase ----
// Try each candidate decoding, keep the ones that succeed and yield mostly
// printable output, and rank them so the most plausible interpretation is first.

struct Candidate {
  std::string label;
  std::string decoded;
  double score;
};

// Strict printability: fraction of bytes that are ordinary ASCII text. Unlike
// util::looks_printable, high bytes (>=0x80) do NOT count, so random binary —
// which is roughly half high bytes — scores low. This is what lets `detect`
// reject a base64 string that merely happens to use the base64 alphabet.
double ascii_ratio(const std::string& s) {
  if (s.empty()) return 0.0;
  size_t ok = 0;
  for (unsigned char c : s)
    if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F)) ++ok;
  return (double)ok / (double)s.size();
}

bool all_chars(const std::string& s, bool (*pred)(int)) {
  for (unsigned char c : s)
    if (!pred(c) && !std::isspace(c)) return false;
  return !s.empty();
}

std::string detect(const std::string& in) {
  std::string t = util::trim(in);
  std::vector<Candidate> cands;

  auto consider = [&](const std::string& label, const std::string& decoded) {
    if (decoded.empty()) return;
    double ratio = ascii_ratio(decoded);
    if (ratio < 0.85) return;  // gibberish — discard
    // Prefer results that are highly printable and differ from the input.
    double score = ratio * 100.0;
    if (decoded != t) score += 5;
    // Mild penalty for very short results (less certain).
    if (decoded.size() < 3) score -= 20;
    cands.push_back({label, decoded, score});
  };

  // Hex: only even-length strings of hex digits (and whitespace).
  {
    std::string compact;
    for (char c : t) if (!std::isspace((unsigned char)c)) compact.push_back(c);
    if (!compact.empty() && compact.size() % 2 == 0 &&
        all_chars(compact, [](int c) -> bool { return std::isxdigit(c); })) {
      try { consider("hex", util::hex_decode(t)); } catch (...) {}
    }
  }

  // Decimal byte codes: space-separated integers in 0..255.
  {
    std::istringstream iss(t);
    std::string tok;
    bool ok = true, any = false;
    while (iss >> tok) {
      any = true;
      for (char c : tok) if (!std::isdigit((unsigned char)c)) { ok = false; break; }
      if (!ok) break;
      try { if (std::stoi(tok) > 255) ok = false; } catch (...) { ok = false; }
      if (!ok) break;
    }
    if (ok && any && t.find(' ') != std::string::npos) {
      std::string out;
      std::istringstream i2(t);
      int v;
      while (i2 >> v) out.push_back((char)v);
      consider("decimal bytes", out);
    }
  }

  // Binary: only 0/1 and whitespace, multiple of 8 bits.
  {
    std::string bits;
    for (char c : t) if (c == '0' || c == '1') bits.push_back(c);
    bool only01 = all_chars(t, [](int c) -> bool { return c == 0x30 || c == 0x31; });
    if (only01 && !bits.empty() && bits.size() % 8 == 0) {
      std::string out;
      for (size_t i = 0; i < bits.size(); i += 8) {
        unsigned char v = 0;
        for (int b = 0; b < 8; ++b) v = (v << 1) | (bits[i + b] - '0');
        out.push_back((char)v);
      }
      consider("binary", out);
    }
  }

  // URL percent-encoding: contains % followed by hex, or '+'.
  if (t.find('%') != std::string::npos) {
    try {
      std::string d = util::url_decode(t);
      if (d != t) consider("url-encoded", d);
    } catch (...) {}
  }

  // Base64 / Base64url: plausible alphabet and length.
  {
    auto is_b64 = [](const std::string& s, bool url) {
      if (s.size() < 4) return false;
      for (char c : s) {
        if (std::isalnum((unsigned char)c)) continue;
        if (url) { if (c == '-' || c == '_' || c == '=') continue; }
        else { if (c == '+' || c == '/' || c == '=') continue; }
        return false;
      }
      return true;
    };
    std::string compact;
    for (char c : t) if (!std::isspace((unsigned char)c)) compact.push_back(c);
    if (is_b64(compact, false)) {
      try { consider("base64", util::b64_decode(compact)); } catch (...) {}
    }
    if (is_b64(compact, true) &&
        (compact.find('-') != std::string::npos || compact.find('_') != std::string::npos)) {
      try { consider("base64url", util::b64_decode(compact)); } catch (...) {}
    }
  }

  // ROT13 is intentionally not auto-detected: it is not structurally
  // distinguishable from plain text, so guessing it would fire on every
  // ordinary sentence. Run the `rot13` operation directly if you need it.

  // Morse: only dots, dashes, slashes, spaces.
  {
    bool morse = !t.empty();
    for (char c : t)
      if (c != '.' && c != '-' && c != '/' && c != ' ' && !std::isspace((unsigned char)c)) {
        morse = false; break;
      }
    if (morse && (t.find('.') != std::string::npos || t.find('-') != std::string::npos)) {
      cands.push_back({"morse", "(looks like Morse code — try 'from-morse')", 50.0});
    }
  }

  if (cands.empty())
    return "No known encoding detected. The input looks like plain text.";

  std::stable_sort(cands.begin(), cands.end(),
                   [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

  std::ostringstream o;
  o << "Detected " << cands.size() << " candidate"
    << (cands.size() == 1 ? "" : "s") << " (most likely first):\n";
  for (size_t i = 0; i < cands.size(); ++i) {
    o << "\n" << (i + 1) << ". " << cands[i].label << "\n   "
      << preview(cands[i].decoded);
  }
  return o.str();
}

const std::vector<std::string>& lorem_words() {
  static const std::vector<std::string> w = {
      "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing",
      "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore",
      "et", "dolore", "magna", "aliqua", "enim", "ad", "minim", "veniam",
      "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi", "aliquip",
      "ex", "ea", "commodo", "consequat", "duis", "aute", "irure", "in",
      "reprehenderit", "voluptate", "velit", "esse", "cillum", "fugiat",
      "nulla", "pariatur", "excepteur", "sint", "occaecat", "cupidatat",
      "non", "proident", "sunt", "culpa", "qui", "officia", "deserunt",
      "mollit", "anim", "id", "est", "laborum"};
  return w;
}

}  // namespace

void register_analysis(Registry& r) {
  const char* C = "analysis";

  def(r, "stats").cat(C).sum("Report line/word/character/byte counts and entropy")
      .fn([](const std::string& in, const Args&) { return stats(in); });
  def(r, "char-frequency").cat(C).sum("Histogram of byte frequencies").alias("freq")
      .fn([](const std::string& in, const Args&) { return char_frequency(in); });
  def(r, "entropy").cat(C).sum("Shannon entropy in bits per byte")
      .fn([](const std::string& in, const Args&) {
        return util::fmt_double(std::round(util::shannon_entropy(in) * 1000) / 1000);
      });
  def(r, "count-lines").cat(C).sum("Print the number of lines")
      .fn([](const std::string& in, const Args&) {
        return std::to_string(in.empty() ? 0 : util::split_lines(in).size());
      });
  def(r, "count-words").cat(C).sum("Print the number of whitespace-delimited words")
      .fn([](const std::string& in, const Args&) {
        size_t n = 0; bool w = false;
        for (char c : in) {
          if (std::isspace((unsigned char)c)) w = false;
          else if (!w) { w = true; ++n; }
        }
        return std::to_string(n);
      });
  def(r, "count-chars").cat(C).sum("Print the number of characters (codepoints)")
      .fn([](const std::string& in, const Args&) {
        return std::to_string(util::utf8_count(in));
      });

  def(r, "detect").cat(C).sum("Guess the input's encoding and show decoded previews")
      .alias("magic")
      .fn([](const std::string& in, const Args&) { return detect(in); });

  def(r, "lorem").cat(C).sum("Generate N words of lorem ipsum").use("lorem [words]").args(0, 1)
      .fn([](const std::string&, const Args& a) {
        long n = 24;
        if (!a.empty()) { try { n = std::stol(a[0]); } catch (...) {} }
        const auto& w = lorem_words();
        std::vector<std::string> out;
        for (long i = 0; i < n; ++i) {
          std::string word = w[i % w.size()];
          if (i == 0) word[0] = (char)std::toupper((unsigned char)word[0]);
          out.push_back(word);
        }
        std::string s = util::join(out, " ");
        if (!s.empty()) s.push_back('.');
        return s;
      });
  def(r, "repeat").cat(C).sum("Repeat the input N times").use("repeat <n>").args(1, 1)
      .fn([](const std::string& in, const Args& a) {
        long n = std::stol(a[0]);
        if (n < 0) throw std::runtime_error("count must be non-negative");
        std::string out;
        out.reserve(in.size() * n);
        for (long i = 0; i < n; ++i) out += in;
        return out;
      });
}

}  // namespace alk::ops
