// SPDX-License-Identifier: MIT
// Minimal JSON reader/writer for the HLE config and input remapping tables (no external dependency).
// Accepts // line comments in input.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace hle {

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  const Json* get(const std::string& k) const {
    if (type != Obj) return nullptr;
    for (auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
  }
  std::string str(const std::string& def = "") const { return type == Str ? s : def; }
  double num(double def = 0) const { return type == Num ? n : def; }
  bool boolean(bool def = false) const { return type == Bool ? b : def; }
  Json& set(const std::string& k, Json v) {
    type = Obj;
    for (auto& kv : o) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o.emplace_back(k, std::move(v));
    return o.back().second;
  }
  static Json string(std::string v) { Json j; j.type = Str; j.s = std::move(v); return j; }
  static Json number(double v) { Json j; j.type = Num; j.n = v; return j; }
  static Json boolean_(bool v) { Json j; j.type = Bool; j.b = v; return j; }
  static Json array() { Json j; j.type = Arr; return j; }
  static Json object() { Json j; j.type = Obj; return j; }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& t) : begin_(t.c_str()), p_(t.c_str()), end_(t.c_str() + t.size()) {}
  bool parse(Json& out, std::string& err) {
    ws();
    if (!value(out)) { err = err_ + " at offset " + std::to_string(p_ - begin_); return false; }
    ws();
    if (p_ != end_) { err = "trailing data at offset " + std::to_string(p_ - begin_); return false; }
    return true;
  }

 private:
  const char* begin_;
  const char* p_;
  const char* end_;
  std::string err_;
  void ws() {
    while (p_ < end_) {
      if (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r') ++p_;
      else if (*p_ == '/' && p_ + 1 < end_ && p_[1] == '/') { while (p_ < end_ && *p_ != '\n') ++p_; }
      else break;
    }
  }
  bool fail(const char* m) { err_ = m; return false; }
  bool lit(const char* w) {
    size_t n = std::char_traits<char>::length(w);
    if ((size_t)(end_ - p_) < n || std::string(p_, n) != w) return false;
    p_ += n;
    return true;
  }
  bool value(Json& j) {
    if (p_ >= end_) return fail("unexpected end");
    char c = *p_;
    if (c == '{') {
      ++p_; j.type = Json::Obj; ws();
      if (p_ < end_ && *p_ == '}') { ++p_; return true; }
      for (;;) {
        ws(); std::string k;
        if (p_ >= end_ || *p_ != '"' || !str(k)) return fail("expected key");
        ws();
        if (p_ >= end_ || *p_ != ':') return fail("expected ':'");
        ++p_; ws();
        Json v;
        if (!value(v)) return false;
        j.o.emplace_back(std::move(k), std::move(v)); ws();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++p_; j.type = Json::Arr; ws();
      if (p_ < end_ && *p_ == ']') { ++p_; return true; }
      for (;;) {
        ws(); Json v;
        if (!value(v)) return false;
        j.a.push_back(std::move(v)); ws();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { j.type = Json::Str; return str(j.s); }
    if (lit("true")) { j.type = Json::Bool; j.b = true; return true; }
    if (lit("false")) { j.type = Json::Bool; j.b = false; return true; }
    if (lit("null")) { j.type = Json::Null; return true; }
    char* e = nullptr;
    double d = std::strtod(p_, &e);
    if (e == p_) return fail("bad value");
    p_ = e; j.type = Json::Num; j.n = d;
    return true;
  }
  bool str(std::string& out) {
    ++p_;  // opening quote
    while (p_ < end_ && *p_ != '"') {
      char c = *p_++;
      if (c == '\\') {
        if (p_ >= end_) return fail("bad escape");
        char e = *p_++;
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (end_ - p_ < 4) return fail("bad \\u");
            unsigned cp = (unsigned)std::strtoul(std::string(p_, 4).c_str(), nullptr, 16);
            p_ += 4;
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
            else { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
            break;
          }
          default: out += e;
        }
      } else out += c;
    }
    if (p_ >= end_) return fail("unterminated string");
    ++p_;
    return true;
  }
};

inline bool json_parse(const std::string& text, Json& out, std::string& err) {
  JsonParser p(text);
  return p.parse(out, err);
}

inline void json_escape(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += (char)c;
  }
  out += '"';
}

inline void json_dump(const Json& j, std::string& out, int indent = 0) {
  auto pad = [&](int n) { out.append((size_t)n * 2, ' '); };
  switch (j.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
      char b[40];
      if (j.n == (double)(long long)j.n) std::snprintf(b, sizeof b, "%lld", (long long)j.n);
      else std::snprintf(b, sizeof b, "%g", j.n);
      out += b;
      break;
    }
    case Json::Str: json_escape(j.s, out); break;
    case Json::Arr: {
      out += '[';
      for (size_t i = 0; i < j.a.size(); ++i) {
        if (i) out += ", ";
        json_dump(j.a[i], out, indent + 1);
      }
      out += ']';
      break;
    }
    case Json::Obj: {
      out += '{';
      for (size_t i = 0; i < j.o.size(); ++i) {
        out += '\n'; pad(indent + 1);
        json_escape(j.o[i].first, out); out += ": ";
        json_dump(j.o[i].second, out, indent + 1);
        if (i + 1 < j.o.size()) out += ',';
      }
      out += '\n'; pad(indent); out += '}';
      break;
    }
  }
}

}  // namespace hle
