// json_util.cpp - see json_util.hpp.

#include "json_util.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace zorm_mcp {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos
                                                           : pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

bool isAsciiIdentStart(unsigned char c) {
  return std::isalpha(c) || c == '_';
}
bool isAsciiIdentChar(unsigned char c) {
  return std::isalnum(c) || c == '_';
}

bool isValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    int extra = 0;
    unsigned int cp = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + static_cast<size_t>(extra) >= s.size()) return false;
    for (int k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10FFFF)
      return false;
    i += static_cast<size_t>(extra) + 1;
  }
  return true;
}

}  // namespace

mcp::Json textResult(const std::string& text) {
  mcp::Json result;
  mcp::Json content = mcp::Json("[]");
  mcp::Json item;
  item.add("type", mcp::Json::str("text"));
  item.add("text", mcp::Json::str(text));
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  return result;
}

mcp::Json toolResult(const mcp::Json& structured) {
  mcp::Json result = textResult(structured.toString());
  if (structured.isObject()) {
    for (const auto& k : structured.getAllKeys().toVector()) {
      const std::string kk = k.toString();
      result.add(kk, structured[kk]);
    }
  }
  return result;
}

mcp::Json rowVal(const mcp::Json& row, std::initializer_list<const char*> keys) {
  if (!row.isObject()) return mcp::Json(nullptr);
  for (const char* k : keys) {
    if (ZJSON::hasChild(row, k)) return row[k];
  }
  return mcp::Json(nullptr);
}

std::string rowStr(const mcp::Json& row, std::initializer_list<const char*> keys) {
  const mcp::Json v = rowVal(row, keys);
  return (v.isNull() || v.isError()) ? std::string() : v.toString();
}

bool validIdentifier(const std::string& name) {
  if (name.empty()) return false;
  if (!isAsciiIdentStart(static_cast<unsigned char>(name[0]))) return false;
  return std::all_of(name.begin() + 1, name.end(),
                     [](unsigned char c) { return isAsciiIdentChar(c); });
}

bool validIdentifierList(const std::string& csv) {
  if (trim(csv).empty()) return false;
  for (const auto& item : split(csv, ',')) {
    if (!validIdentifier(trim(item))) return false;
  }
  return true;
}

bool validSortClause(const std::string& spec) {
  if (trim(spec).empty()) return false;
  for (const auto& rawClause : split(spec, ',')) {
    const std::string clause = trim(rawClause);
    if (clause.empty()) return false;
    std::string word;
    std::vector<std::string> words;
    for (char c : clause) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!word.empty()) {
          words.push_back(word);
          word.clear();
        }
      } else {
        word.push_back(c);
      }
    }
    if (!word.empty()) words.push_back(word);
    if (words.empty() || !validIdentifier(words[0])) return false;
    for (size_t i = 1; i < words.size(); ++i) {
      std::string dir = words[i];
      for (auto& ch : dir)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (dir != "asc" && dir != "desc") return false;
    }
  }
  return true;
}

std::size_t rewritePgPlaceholders(std::string& sql) {
  std::string out;
  out.reserve(sql.size() + 8);
  std::size_t index = 0;
  std::size_t replaced = 0;
  size_t i = 0;
  const size_t n = sql.size();
  auto copyUntil = [&](const std::string& terminator) {
    const size_t at = sql.find(terminator, i);
    if (at == std::string::npos) {
      out.append(sql, i, std::string::npos);
      i = n;
    } else {
      out.append(sql, i, at + terminator.size() - i);
      i = at + terminator.size();
    }
  };
  while (i < n) {
    const char c = sql[i];
    if (c == '\'') {
      out.push_back(c);
      ++i;
      while (i < n) {
        out.push_back(sql[i]);
        if (sql[i] == '\'') {
          if (i + 1 < n && sql[i + 1] == '\'') {
            out.push_back(sql[i + 1]);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
    } else if (c == '"') {
      out.push_back(c);
      ++i;
      while (i < n) {
        out.push_back(sql[i]);
        if (sql[i] == '"') {
          if (i + 1 < n && sql[i + 1] == '"') {
            out.push_back(sql[i + 1]);
            i += 2;
            continue;
          }
          ++i;
          break;
        }
        ++i;
      }
    } else if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      copyUntil("\n");
    } else if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
      // PostgreSQL allows nested block comments.
      int depth = 1;
      out.append("/*");
      i += 2;
      while (i < n && depth > 0) {
        if (sql[i] == '/' && i + 1 < n && sql[i + 1] == '*') {
          ++depth;
          out.append("/*");
          i += 2;
        } else if (sql[i] == '*' && i + 1 < n && sql[i + 1] == '/') {
          --depth;
          out.append("*/");
          i += 2;
        } else {
          out.push_back(sql[i++]);
        }
      }
    } else if (c == '$') {
      // Dollar-quoted string: $tag$...$tag$ (tag starts with a letter/_ so a
      // $1 placeholder is not mistaken for a tag).
      size_t j = i + 1;
      if (j < n && isAsciiIdentStart(static_cast<unsigned char>(sql[j]))) {
        while (j < n && isAsciiIdentChar(static_cast<unsigned char>(sql[j]))) ++j;
        if (j < n && sql[j] == '$') {
          const std::string tag = sql.substr(i, j - i + 1);
          const size_t close = sql.find(tag, j + 1);
          if (close == std::string::npos) {
            out.append(sql, i, std::string::npos);
            i = n;
          } else {
            out.append(sql, i, close + tag.size() - i);
            i = close + tag.size();
          }
          continue;
        }
      }
      out.push_back(c);
      ++i;
    } else if (c == '?') {
      out += "$" + std::to_string(++index);
      ++replaced;
      ++i;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  if (replaced > 0) sql.swap(out);
  return replaced;
}

std::string toUtf8FromGbk(const std::string& text) {
  if (text.empty() || isValidUtf8(text)) return text;
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(936, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (wlen <= 0) return text;
  std::wstring wide(static_cast<size_t>(wlen), L'\0');
  if (MultiByteToWideChar(936, 0, text.data(), static_cast<int>(text.size()),
                          &wide[0], wlen) <= 0)
    return text;
  const int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0,
                                       nullptr, nullptr);
  if (ulen <= 0) return text;
  std::string utf8(static_cast<size_t>(ulen), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, &utf8[0], ulen, nullptr,
                          nullptr) <= 0)
    return text;
  return utf8;
#else
  return text;
#endif
}

}  // namespace zorm_mcp
