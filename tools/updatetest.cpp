// updatetest: src/host/update_check.h's versions and GitHub's answers read (make test). No network: the answers are
// made here. Exits 1 if any check fails.
#include "../src/host/update_check.h"
#include <cstdio>

static int failed = 0;
static void check(bool ok, const std::string &what)
{
	if (!ok) { fprintf(stderr, "FAIL: %s\n", what.c_str()); failed++; }
}

static std::string rel(const std::string &tag, bool pre = false, bool draft = false, const std::string &url = "")
{
	return "{\"tag_name\":\"" + tag + "\",\"prerelease\":" + (pre ? "true" : "false") + ",\"draft\":" + (draft ? "true" : "false") +
	       ",\"html_url\":\"" + (url.empty() ? "https://github.com/mr-finster/vf3-recomp/releases/tag/" + tag : url) +
	       "\",\"assets\":[{\"name\":\"a.zip\",\"uploader\":{\"login\":\"x\"}}],\"body\":\"notes \\u00e9\"}";
}

int main()
{
	using namespace update_check;
	// versions
	struct { const char *a, *b; int want; } cmp[] = {
		{ "v0.1.9", "v0.1.10", -1 }, { "v0.1.10", "v0.1.9", 1 }, { "v1.8.0", "v1.8.0", 0 }, { "v1.8", "v1.8.0", 0 },
		{ "v1.10.0", "v1.9.9", 1 }, { "v2.0.0", "v1.99.99", 1 }, { "v0.1.22-alpha", "v0.1.22-alpha", 0 },
		{ "v0.1.23-alpha", "v0.1.22-alpha", 1 }, { "v0.1.9-alpha", "v0.1.10-alpha", -1 }, { "v0.1.22-alpha", "v0.1.22", -1 },
		{ "v0.1.22", "v0.1.22-alpha", 1 }, { "v0.1.22-alpha", "v0.1.22-beta", -1 }, { "v0.1.22-alpha.2", "v0.1.22-alpha.10", -1 },
		{ "v0.1.22-alpha", "v0.1.22-alpha.1", -1 }, { "v0.1.7-online-preview", "v0.1.7-alpha", 1 }, { "0.1.8", "v0.1.8", 0 },
		{ "v0.1.8-alpha", "v0.1.7-online-preview", 1 },
	};
	for (const auto &c : cmp) {
		version a, b;
		check(parse(c.a, a) && parse(c.b, b), std::string("parse ") + c.a + " " + c.b);
		const int got = compare(a, b);
		check((got > 0) - (got < 0) == c.want, std::string("compare ") + c.a + " " + c.b + " = " + std::to_string(got));
	}
	for (const char *bad : { "", "dev", "v", "8613e4c", "v0.1.22-alpha-15-g8613e4c", "v1.8.0-3-g16b7c12", "v1.8.0-dirty", "v0.1.22-alpha-dirty",
	                         "v1..0", "v1.8.0-", "v1.8.0 ", "1.2.3.4.5", "v1.8.0+meta", "va.b", "v1.8.0-α", "v1234567890.0" }) {
		version v;
		check(!parse(bad, v), std::string("not a version: '") + bad + "'");
	}
	check(newer("v1.9.0", "v1.8.0") && !newer("v1.8.0", "v1.8.0") && !newer("v1.7.0", "v1.8.0"), "newer");
	check(!newer("v9.0.0", "v1.8.0-3-g16b7c12") && !newer("v9.0.0", "dev") && !newer("garbage", "v1.0.0"), "newer: dev builds and garbage never");
	check(shown("v0.1.23-alpha") == "0.1.23-alpha" && shown("1.0") == "1.0", "shown");
	check(page_ok("https://github.com/mr-finster/vf1-recomp/releases/tag/v1.8.0"), "page_ok");
	for (const char *bad : { "http://github.com/mr-finster/x", "https://github.com/someone/x", "https://github.com.evil/mr-finster/",
	                         "https://github.com/mr-finster/x y", "https://github.com/mr-finster/\"x", "javascript:alert(1)" })
		check(!page_ok(bad), std::string("page not ok: ") + bad);
	// answers
	auto is = [](const result &r, result::what w, const std::string &tag = "") { return r.is == w && (tag.empty() || r.tag == tag); };
	check(is(read_reply(200, rel("v0.1.23-alpha"), "v0.1.22-alpha"), result::what::newer, "v0.1.23-alpha"), "newer (an object)");
	check(is(read_reply(200, rel("v0.1.22-alpha"), "v0.1.22-alpha"), result::what::none), "the same (a hotfix's tag again)");
	check(is(read_reply(200, rel("v0.1.21-alpha"), "v0.1.22-alpha"), result::what::none), "older");
	check(is(read_reply(200, rel("v0.1.23-alpha"), "v0.1.22-alpha-4-g8613e4c"), result::what::none), "a dev build never");
	check(is(read_reply(200, rel("v0.1.23-alpha"), "dev"), result::what::none), "dev never");
	check(is(read_reply(200, "[" + rel("v0.1.24-alpha", false, true) + "," + rel("v0.1.23-alpha") + "," + rel("v0.1.22-alpha") + "]", "v0.1.22-alpha"),
	         result::what::newer, "v0.1.23-alpha"), "a list: the draft skipped");
	check(is(read_reply(200, "[" + rel("v0.1.24-alpha", true) + "," + rel("v0.1.22-alpha") + "]", "v0.1.22-alpha"), result::what::newer, "v0.1.24-alpha"),
	      "a pre-release for a pre-release build");
	check(is(read_reply(200, "[" + rel("v1.9.0-beta", true) + "," + rel("v1.8.0") + "]", "v1.8.0"), result::what::none), "no pre-release for a release");
	check(is(read_reply(200, rel("v1.9.0-beta", true), "v1.8.0"), result::what::failed), "only a pre-release, a release build: none fit");
	check(is(read_reply(200, rel("v0.1.23", false, false, "https://evil.example/x"), "v0.1.22"), result::what::failed), "a page elsewhere: not taken");
	check(is(read_reply(403, "{\"message\":\"API rate limit exceeded\"}", "v0.1.22"), result::what::failed), "403");
	check(is(read_reply(404, "", "v0.1.22"), result::what::failed), "404");
	check(is(read_reply(0, "", "v0.1.22"), result::what::failed), "no answer");
	for (const char *g : { "", "not json", "{", "[1,2", "{\"tag_name\":5}", "[]", "{}", "null", "\"v9.9.9\"", "<html>rate limited</html>" })
		check(is(read_reply(200, g, "v0.1.22"), result::what::failed), std::string("garbage: ") + g);
	// once a day
	check(due(0, 1000) && !due(1000, 1000 + DAY - 1) && due(1000, 1000 + DAY) && due(1000 + 3 * DAY, 1000), "due");
	if (failed) { fprintf(stderr, "updatetest: %d failed\n", failed); return 1; }
	printf("updatetest: all passed\n");
	return 0;
}
