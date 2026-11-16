#pragma once
// SPDX-License-Identifier: MIT
// Minimal JSON reader for the relift oracle tools.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using Str = std::string;
struct JV {
  enum T { Null, Num, SStr, Arr, Obj, Bool } t = Null;
  double n = 0; uint64_t u = 0; bool isInt = false; Str s; bool b = false;
  std::vector<JV> a; std::vector<Str> ok; std::vector<JV> ov;
  const JV* get(const Str& k) const { for(size_t i = 0; i < ok.size(); i++) if(ok[i] == k) return &ov[i]; return nullptr; }
  uint64_t asU64() const {
    if(t == SStr) return strtoull(s.c_str(), nullptr, 0);
    if(t == Num) return isInt ? u : (uint64_t)(int64_t)n;
    if(t == Bool) return b; return 0;
  }
};
struct JP {
  const char* p; const char* e;
  void ws() { while(p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++; }
  bool parse(JV& v) {
    ws(); if(p >= e) return false;
    if(*p == '{') { p++; v.t = JV::Obj; ws(); if(*p == '}') { p++; return true; }
      for(;;) { JV k; ws(); if(!parse(k) || k.t != JV::SStr) return false; ws(); if(*p++ != ':') return false;
        JV x; if(!parse(x)) return false; v.ok.push_back(k.s); v.ov.push_back(x); ws(); if(*p == ',') { p++; continue; } if(*p == '}') { p++; return true; } return false; } }
    if(*p == '[') { p++; v.t = JV::Arr; ws(); if(*p == ']') { p++; return true; }
      for(;;) { JV x; if(!parse(x)) return false; v.a.push_back(x); ws(); if(*p == ',') { p++; continue; } if(*p == ']') { p++; return true; } return false; } }
    if(*p == '"') { p++; v.t = JV::SStr; while(p < e && *p != '"') { if(*p == '\\') { p++; char c = *p++; v.s += c == 'n' ? '\n' : c == 't' ? '\t' : c; } else v.s += *p++; } p++; return true; }
    if(!strncmp(p, "true", 4)) { p += 4; v.t = JV::Bool; v.b = true; return true; }
    if(!strncmp(p, "false", 5)) { p += 5; v.t = JV::Bool; v.b = false; return true; }
    if(!strncmp(p, "null", 4)) { p += 4; return true; }
    char* end; v.t = JV::Num; v.n = strtod(p, &end);
    if(end == p) return false;
    bool isi = true; for(const char* q = p; q < end; q++) if(*q == '.' || *q == 'e' || *q == 'E') isi = false;
    if(isi) { v.isInt = true; v.u = (*p == '-') ? (uint64_t)strtoll(p, nullptr, 10) : strtoull(p, nullptr, 10); }
    p = end; return true;
  }
};
