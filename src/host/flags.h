// The countries with a flag in assets/ui/flags.png (made with it by tools/flags/make_flags.py), in its order: the
// i-th's at (i % 20 * 50 + 1, i / 20 * 38 + 1), 48 x 36.
#pragma once
#include <string>

namespace flags {

constexpr int COLS = 20, CELL_W = 50, CELL_H = 38, W = 48, H = 36;
inline const char *const CODES =
	"ad ae af ag ai al am ao aq ar as at au aw ax az ba bb bd be "
	"bf bg bh bi bj bl bm bn bo bq br bs bt bv bw by bz ca cc cd "
	"cf cg ch ci ck cl cm cn co cp cr cu cv cw cx cy cz de dg dj "
	"dk dm do dz ec ee eg eh er es et eu fi fj fk fm fo fr ga gb "
	"gd ge gf gg gh gi gl gm gn gp gq gr gs gt gu gw gy hk hm hn "
	"hr ht hu ic id ie il im in io iq ir is it je jm jo jp ke kg "
	"kh ki km kn kp kr kw ky kz la lb lc li lk lr ls lt lu lv ly "
	"ma mc md me mf mg mh mk ml mm mn mo mp mq mr ms mt mu mv mw "
	"mx my mz na nc ne nf ng ni nl no np nr nu nz om pa pc pe pf "
	"pg ph pk pl pm pn pr ps pt pw py qa re ro rs ru rw sa sb sc "
	"sd se sg sh si sj sk sl sm sn so sr ss st sv sx sy sz tc td "
	"tf tg th tj tk tl tm tn to tr tt tv tw tz ua ug um un us uy "
	"uz va vc ve vg vi vn vu wf ws xk xx ye yt za zm zw";

// A country's place in the list (two letters, either case), or -1 if it has no flag
inline int index_of(const std::string &code)
{
	if (code.size() != 2) return -1;
	const std::string c = { char(code[0] | 0x20), char(code[1] | 0x20) };
	const std::string all = CODES;
	for (size_t i = 0, n = 0; i + 1 < all.size(); i += 3, n++)
		if (all[i] == c[0] && all[i + 1] == c[1]) return int(n);
	return -1;
}

} // namespace flags
