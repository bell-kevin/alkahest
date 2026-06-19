// SPDX-License-Identifier: AGPL-3.0-only
// Part of Alkahest. See LICENSE for terms.
//
// A deliberately tiny, dependency-free HTTP/1.1 server exposing Alkahest over
// the network: a single-page web UI plus a JSON API. This is the feature that
// makes the AGPL meaningful for this project — if you run this for others, you
// must share your modified source with them.
#include "alkahest/server.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "alkahest/core.hpp"
#include "alkahest/util.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace alk {
namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }

// ---- minimal JSON helpers (just enough for {input, recipe:[...]}) ----

// Decode a JSON string literal body (without surrounding quotes already split).
std::string json_unescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c != '\\') { out.push_back(c); continue; }
    if (++i >= s.size()) break;
    switch (s[i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case '/': out.push_back('/'); break;
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case 'u': {
        if (i + 4 < s.size()) {
          unsigned cp = std::stoul(s.substr(i + 1, 4), nullptr, 16);
          i += 4;
          if (cp < 0x80) out.push_back((char)cp);
          else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
          } else {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
          }
        }
        break;
      }
      default: out.push_back(s[i]);
    }
  }
  return out;
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b;
        } else out.push_back((char)c);
    }
  }
  return out;
}

// Extract the value of a top-level string field: "key":"...."
// Returns false if not present. Honors escaped quotes inside the value.
bool json_get_string(const std::string& body, const std::string& key, std::string& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = body.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = body.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t q = body.find('"', colon + 1);
  if (q == std::string::npos) return false;
  size_t i = q + 1;
  std::string raw;
  for (; i < body.size(); ++i) {
    if (body[i] == '\\') { raw.push_back(body[i]); if (i + 1 < body.size()) raw.push_back(body[++i]); continue; }
    if (body[i] == '"') break;
    raw.push_back(body[i]);
  }
  out = json_unescape(raw);
  return true;
}

// Extract a string array "recipe":["a","b \"c\""] into its elements.
bool json_get_string_array(const std::string& body, const std::string& key,
                           std::vector<std::string>& out) {
  std::string needle = "\"" + key + "\"";
  size_t k = body.find(needle);
  if (k == std::string::npos) return false;
  size_t lb = body.find('[', k);
  if (lb == std::string::npos) return false;
  size_t i = lb + 1;
  while (i < body.size()) {
    while (i < body.size() && (body[i] == ' ' || body[i] == ',' || body[i] == '\n' ||
                               body[i] == '\r' || body[i] == '\t')) ++i;
    if (i < body.size() && body[i] == ']') break;
    if (i >= body.size() || body[i] != '"') break;
    ++i;
    std::string raw;
    for (; i < body.size(); ++i) {
      if (body[i] == '\\') { raw.push_back(body[i]); if (i + 1 < body.size()) raw.push_back(body[++i]); continue; }
      if (body[i] == '"') { ++i; break; }
      raw.push_back(body[i]);
    }
    out.push_back(json_unescape(raw));
  }
  return true;
}

const char* kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Alkahest</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; font: 15px/1.5 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
         background:#0e1116; color:#e6edf3; }
  header { padding:18px 24px; border-bottom:1px solid #21262d; }
  header h1 { margin:0; font-size:20px; letter-spacing:.5px; }
  header span { color:#7d8590; font-size:13px; }
  main { display:grid; grid-template-columns:1fr 320px 1fr; gap:0; height:calc(100vh - 64px); }
  @media (max-width: 900px){ main { grid-template-columns:1fr; height:auto; } }
  .pane { padding:16px 18px; overflow:auto; border-right:1px solid #21262d; }
  .pane h2 { margin:0 0 10px; font-size:12px; text-transform:uppercase; color:#7d8590; letter-spacing:1px; }
  textarea { width:100%; height:calc(100% - 30px); min-height:240px; resize:none; background:#0b0e13;
             color:#e6edf3; border:1px solid #21262d; border-radius:8px; padding:10px;
             font:inherit; }
  #out { white-space:pre-wrap; word-break:break-word; background:#0b0e13; border:1px solid #21262d;
         border-radius:8px; padding:10px; height:calc(100% - 30px); min-height:240px; overflow:auto; margin:0;}
  .recipe { list-style:none; margin:0; padding:0; }
  .recipe li { display:flex; align-items:center; gap:6px; background:#161b22; border:1px solid #21262d;
               border-radius:6px; padding:6px 8px; margin-bottom:6px; }
  .recipe li code { flex:1; color:#79c0ff; }
  .recipe button { background:none; border:none; color:#7d8590; cursor:pointer; font:inherit; }
  .recipe button:hover { color:#f85149; }
  .add { display:flex; gap:6px; margin-top:8px; }
  .add input { flex:1; background:#0b0e13; color:#e6edf3; border:1px solid #21262d; border-radius:6px;
               padding:6px 8px; font:inherit; }
  .add button, .palette button { background:#238636; color:#fff; border:none; border-radius:6px;
               padding:6px 10px; cursor:pointer; font:inherit; }
  .hint { color:#7d8590; font-size:12px; margin-top:10px; }
  datalist { display:none; }
  a { color:#79c0ff; }
</style>
</head>
<body>
<header>
  <h1>⚗ alkahest <span>— the universal solvent for text</span></h1>
</header>
<main>
  <section class="pane">
    <h2>Input</h2>
    <textarea id="in" placeholder="Type or paste text here..." autofocus></textarea>
  </section>
  <section class="pane">
    <h2>Recipe</h2>
    <ul class="recipe" id="steps"></ul>
    <div class="add">
      <input id="op" list="ops" placeholder="add operation (e.g. to-base64)">
      <button onclick="addStep()">+</button>
    </div>
    <datalist id="ops"></datalist>
    <p class="hint">Click an op name to remove it. Drag not needed — order is add order.
       Arguments: type them after the name, e.g. <code>caesar 3</code>.</p>
  </section>
  <section class="pane" style="border-right:none">
    <h2>Output</h2>
    <pre id="out"></pre>
  </section>
</main>
<script>
let steps = [];
const $ = s => document.querySelector(s);
async function loadOps(){
  try {
    const r = await fetch('/api/ops'); const j = await r.json();
    const dl = $('#ops'); dl.innerHTML='';
    j.ops.forEach(o => { const opt=document.createElement('option'); opt.value=o.name; opt.label=o.summary; dl.appendChild(opt); });
  } catch(e){}
}
function render(){
  const ul = $('#steps'); ul.innerHTML='';
  steps.forEach((s,i)=>{
    const li=document.createElement('li');
    const c=document.createElement('code'); c.textContent=s;
    const b=document.createElement('button'); b.textContent='✕'; b.title='remove';
    b.onclick=()=>{ steps.splice(i,1); render(); run(); };
    li.appendChild(c); li.appendChild(b); ul.appendChild(li);
  });
}
function addStep(){
  const v=$('#op').value.trim(); if(!v) return;
  steps.push(v); $('#op').value=''; render(); run();
}
$('#op')?.addEventListener('keydown', e=>{ if(e.key==='Enter') addStep(); });
let timer=null;
function schedule(){ clearTimeout(timer); timer=setTimeout(run, 150); }
async function run(){
  try {
    const r = await fetch('/api/run', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({input: $('#in').value, recipe: steps})});
    const j = await r.json();
    $('#out').textContent = j.ok ? j.output : ('error: ' + j.error);
    $('#out').style.color = j.ok ? '#e6edf3' : '#f85149';
  } catch(e){ $('#out').textContent='(server error)'; }
}
$('#in').addEventListener('input', schedule);
loadOps(); render();
</script>
</body>
</html>)HTML";

struct Request {
  std::string method, path, body;
};

std::string http_response(int code, const std::string& status,
                          const std::string& ctype, const std::string& body) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << " " << status << "\r\n"
    << "Content-Type: " << ctype << "\r\n"
    << "Content-Length: " << body.size() << "\r\n"
    << "Connection: close\r\n"
    << "X-Content-Type-Options: nosniff\r\n"
    << "\r\n"
    << body;
  return o.str();
}

// Build the JSON response for POST /api/run.
std::string handle_run(const std::string& body) {
  std::string input;
  std::vector<std::string> recipe_lines;
  json_get_string(body, "input", input);
  json_get_string_array(body, "recipe", recipe_lines);

  Recipe recipe;
  for (const auto& line : recipe_lines) {
    Step s = parse_step_line(line);
    if (!s.op.empty()) recipe.push_back(s);
  }
  std::ostringstream o;
  try {
    std::string out = run_recipe(registry(), recipe, input);
    o << "{\"ok\":true,\"output\":\"" << json_escape(out) << "\"}";
  } catch (const std::exception& e) {
    o << "{\"ok\":false,\"error\":\"" << json_escape(e.what()) << "\"}";
  }
  return o.str();
}

std::string handle_ops() {
  std::ostringstream o;
  o << "{\"ops\":[";
  bool first = true;
  for (const auto* op : registry().all()) {
    if (!first) o << ",";
    first = false;
    o << "{\"name\":\"" << json_escape(op->name) << "\",\"summary\":\""
      << json_escape(op->summary) << "\",\"category\":\"" << json_escape(op->category)
      << "\",\"usage\":\"" << json_escape(op->usage) << "\"}";
  }
  o << "]}";
  return o.str();
}

#ifndef _WIN32
// Read a full HTTP request (headers + body per Content-Length) from a socket.
bool read_request(int fd, Request& req) {
  std::string data;
  char buf[4096];
  size_t header_end = std::string::npos;
  // Read until we have the headers.
  while (header_end == std::string::npos) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    data.append(buf, n);
    header_end = data.find("\r\n\r\n");
    if (data.size() > (1u << 20)) return false;  // 1 MiB header cap
  }
  std::istringstream rs(data.substr(0, header_end));
  std::string line;
  std::getline(rs, line);
  {
    std::istringstream ls(line);
    ls >> req.method >> req.path;
  }
  size_t content_length = 0;
  while (std::getline(rs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = util::to_lower(util::trim(line.substr(0, colon)));
    std::string val = util::trim(line.substr(colon + 1));
    if (name == "content-length") { try { content_length = std::stoul(val); } catch (...) {} }
  }
  std::string body = data.substr(header_end + 4);
  while (body.size() < content_length) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    body.append(buf, n);
    if (body.size() > (8u << 20)) break;  // 8 MiB body cap
  }
  req.body = body;
  return true;
}

void send_all(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
    if (n <= 0) break;
    off += (size_t)n;
  }
}
#endif

}  // namespace

int run_server(const std::string& host, int port) {
#ifdef _WIN32
  std::cerr << "alk serve: the built-in server is only available on POSIX systems "
               "in this build.\n";
  (void)host; (void)port;
  return 1;
#else
  int srv = ::socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { std::perror("socket"); return 1; }
  int yes = 1;
  ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "alk serve: invalid host address: " << host << "\n";
    ::close(srv);
    return 2;
  }
  if (::bind(srv, (sockaddr*)&addr, sizeof addr) < 0) {
    std::perror("bind");
    ::close(srv);
    return 1;
  }
  if (::listen(srv, 16) < 0) {
    std::perror("listen");
    ::close(srv);
    return 1;
  }

  std::signal(SIGINT, on_sigint);
  std::signal(SIGPIPE, SIG_IGN);
  std::cout << "alkahest serving on http://" << host << ":" << port
            << "  (Ctrl-C to stop)\n";

  while (!g_stop) {
    sockaddr_in cli{};
    socklen_t clilen = sizeof cli;
    int fd = ::accept(srv, (sockaddr*)&cli, &clilen);
    if (fd < 0) {
      if (g_stop) break;
      continue;
    }
    Request req;
    if (read_request(fd, req)) {
      std::string resp;
      if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        resp = http_response(200, "OK", "text/html; charset=utf-8", kIndexHtml);
      } else if (req.method == "GET" && req.path == "/api/ops") {
        resp = http_response(200, "OK", "application/json", handle_ops());
      } else if (req.method == "POST" && req.path == "/api/run") {
        resp = http_response(200, "OK", "application/json", handle_run(req.body));
      } else if (req.method == "GET" && req.path == "/healthz") {
        resp = http_response(200, "OK", "text/plain", "ok");
      } else {
        resp = http_response(404, "Not Found", "text/plain", "not found\n");
      }
      send_all(fd, resp);
    }
    ::close(fd);
  }
  ::close(srv);
  std::cout << "\nalkahest server stopped.\n";
  return 0;
#endif
}

}  // namespace alk
