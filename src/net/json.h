// A small JSON reader and writer, for the lobby's requests and answers (objects of strings, numbers, lists of
// strings): no more than that needs. A reply that isn't what's expected reads as missing fields, never a crash.
#pragma once
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace vfnet::json {

struct Value
{
	enum Kind { null, boolean, number, string, list, object } kind = null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Value> items;
	std::map<std::string, Value> fields;

	const Value &operator[](const std::string &k) const
	{
		static const Value none;
		auto it = fields.find(k);
		return kind == object && it != fields.end() ? it->second : none;
	}
	std::string str() const { return kind == string ? s : std::string(); }
	std::vector<std::string> strings() const
	{
		std::vector<std::string> out;
		if (kind == list)
			for (const Value &v : items)
				if (v.kind == string) out.push_back(v.s);
		return out;
	}
};

namespace detail {

struct Parser
{
	const char *p, *e;
	int depth = 0;
	void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
	bool lit(const char *w)
	{
		const size_t n = std::char_traits<char>::length(w);
		if (size_t(e - p) < n || std::string(p, n) != w) return false;
		p += n;
		return true;
	}
	bool str(std::string &out)
	{
		if (p >= e || *p != '"') return false;
		p++;
		while (p < e && *p != '"') {
			if (*p == '\\') {
				if (++p >= e) return false;
				switch (*p) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {   // (as '?' outside ASCII: nothing here needs more)
					if (e - p < 5) return false;
					unsigned c = 0;
					if (sscanf(std::string(p + 1, 4).c_str(), "%4x", &c) != 1) return false;
					out += c < 0x80 ? char(c) : '?';
					p += 4;
					break;
				}
				default: out += *p;
				}
				p++;
			} else
				out += *p++;
		}
		if (p >= e) return false;
		p++;
		return true;
	}
	bool value(Value &v)
	{
		if (++depth > 32) return false;
		ws();
		if (p >= e) return false;
		bool ok = true;
		if (*p == '{') {
			v.kind = Value::object;
			p++;
			ws();
			if (p < e && *p == '}') p++;
			else
				for (;;) {
					std::string k;
					ws();
					if (!str(k)) { ok = false; break; }
					ws();
					if (p >= e || *p++ != ':') { ok = false; break; }
					if (!value(v.fields[k])) { ok = false; break; }
					ws();
					if (p < e && *p == ',') { p++; continue; }
					if (p < e && *p == '}') { p++; break; }
					ok = false;
					break;
				}
		} else if (*p == '[') {
			v.kind = Value::list;
			p++;
			ws();
			if (p < e && *p == ']') p++;
			else
				for (;;) {
					v.items.emplace_back();
					if (!value(v.items.back())) { ok = false; break; }
					ws();
					if (p < e && *p == ',') { p++; continue; }
					if (p < e && *p == ']') { p++; break; }
					ok = false;
					break;
				}
		} else if (*p == '"') {
			v.kind = Value::string;
			ok = str(v.s);
		} else if (lit("true")) { v.kind = Value::boolean; v.b = true; }
		else if (lit("false")) { v.kind = Value::boolean; v.b = false; }
		else if (lit("null")) v.kind = Value::null;
		else {
			// (JSON's numbers only: a sign, digits, a point, an exponent; never nan, inf or hex, which strtod takes;
			// and nothing past what a double holds)
			size_t k = 0;
			const size_t most = size_t(std::min<ptrdiff_t>(e - p, 32));
			while (k < most && (isdigit(uint8_t(p[k])) || p[k] == '-' || p[k] == '+' || p[k] == '.' || p[k] == 'e' || p[k] == 'E')) k++;
			const std::string num(p, k);
			char *end = nullptr;
			v.n = k ? strtod(num.c_str(), &end) : 0;
			if (!k || end == num.c_str() || !std::isfinite(v.n) || std::fabs(v.n) > 1e15) ok = false;
			else { v.kind = Value::number; p += end - num.c_str(); }
		}
		depth--;
		return ok;
	}
};

} // namespace detail

inline bool parse(const std::string &text, Value &out)
{
	detail::Parser ps{ text.data(), text.data() + text.size() };
	out = Value();
	return ps.value(out);
}

// "..." with JSON's escapes
inline std::string quote(const std::string &s)
{
	std::string o = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
		else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
		else o += char(c);
	}
	return o + "\"";
}

inline std::string list(const std::vector<std::string> &v)
{
	std::string o = "[";
	for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + quote(v[i]);
	return o + "]";
}

} // namespace vfnet::json
