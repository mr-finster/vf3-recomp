// VF3_WIDE_SELECT's option 2 (wide_select.h): the sides as a content-aware fill of the screen's own background, made
// so the 4:3 picture's edge can't be told.
// - Quilted (Efros and Freeman's) from the background's own pixels, whole (their full detail): 40 x 24 patches (20 x
//   12 where the background is in small pieces, as the stage screen's panels), overlapping by 10 across and 8 down,
//   from the plate's background anywhere on the screen (8 x 8 blocks of solid tiles, colour near the edge's, no line
//   down them: the photo under the letters and the 3D, not the portraits, stills or frames). Each patch is the source
//   window that best matches what's there where they overlap (each one's mean taken off: the shape, not the shade),
//   near its own lines, and as strong a texture as the edge's own (its steps across and down: matching alone favours
//   the faintest); joined along the overlap's least-error path. No block with a speck far brighter than its texture.
// - The first patches overlap the 4:3 picture's own last 16 columns: the streaks there carry on into the side, and the
//   cut can go up to 8 columns into the picture where those are background (the 2D's own pixels the plate's: never
//   letters, frames, portraits or the 3D), so the edge isn't a straight line.
// - Each pixel made is the edge's colour on its line, carried out with the picture's own large-scale slope there
//   (eased out over 96 pixels; on the stage panels, whose shading runs steeply toward the 4:3 edges, over 200), plus
//   its source pixel's detail (it less its line's mean over 97 columns round it, 49 on the stage panels, in the
//   plate): the texture whole and as sharp as the photo's, its shading across with it, the shade the edge's, so no
//   seam in colour where patches meet or at the 4:3 edge. (A gradient-domain solve, Poisson's, the side's steps the
//   patches' own with the picture's pixels fixed, was tried: told apart from the photo more easily, its streaks
//   shorter.)
// - Beside the stage screen's preview the frame is quilted the same way from the panels' blocks, on the panels'
//   colour and slope above and below it (between them), the detail less its mean over 17 lines too.
// Made once a screen, on a thread of its own, once the screen has settled (its fade in); each picture then is that,
// moved by how much the edge's colour has moved since (a fade: the game's colour offsets, added).
// On the stage screen (both programs: the same layout) the preview of the stage, lines ARENA_Y0 to ARENA_Y1, isn't
// carried on: beside it the panels' metal goes on down as a frame round it (option 2), or the sides are dark there
// (option 6).
#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace wide_select {

constexpr float DETMAX = 40, LINEK = 3, LINEC = 48, SDK = 12, SPECK = 150;
constexpr int OVS = 3, CXS = 3;
constexpr int FMY = 8;
constexpr int KR = 16, KIN = 8;               // (the picture's own columns the first patches overlap; of those, how many may be replaced)
constexpr int ARENA_Y0 = 68, ARENA_Y1 = 262;   // (the stage screen's preview, aligned lines: both programs)

struct FillResult
{
	int reach = 0;
	std::vector<int16_t> px[2];       // ((KIN + reach) x H x 3: each side's colours, from d = -KIN (inside the picture) out)
	std::vector<uint8_t> mine[2];     // ((KIN + reach) x H: made here (inside the picture: replaced))
	std::vector<uint8_t> zone[2];     // (each line's columns averaged for its colour at the edge)
	std::vector<int16_t> edge[2];     // (H x 3: that colour when made)
	std::vector<uint8_t> arena;       // (H: the stage screen's preview lines)
	// (each side's colour at the edge and its slope outwards, line by line, as the side was made from them; LE: the
	// slope's easing: the vignette takes the slope off and darkens instead)
	std::vector<float> base[2], slope[2];
	float LE = 96;
	bool stage = false;
	double lum = 0, ms = 0;
	int patches = 0, candidates = 0;
	bool ok = false;
};

inline double plate_lum(const std::vector<uint32_t> &p)
{
	double s = 0; long n = 0;
	for (size_t i = 0; i < p.size(); i += 7) if (p[i] >> 24) { s += ch(p[i], 0) + ch(p[i], 1) + ch(p[i], 2); n++; }
	return n ? s / n : 0;
}

// the 2D as shown (the layers above over those below; 0 where neither draws)
inline std::vector<uint32_t> visible_2d(const std::vector<uint32_t> &above, const std::vector<uint32_t> &below)
{
	std::vector<uint32_t> v(size_t(W) * H, 0);
	if (above.size() != v.size() || below.size() != v.size()) return v;
	for (size_t i = 0; i < v.size(); i++) v[i] = (above[i] >> 24) ? above[i] : below[i];
	return v;
}

// the edge's colour on line y of a side: the mean of the plate's background pixels in its first z columns
inline bool edge_mean(const std::vector<uint32_t> &plate, int s, int y, int z, int out[3])
{
	long m[3] = {}; int n = 0;
	for (int i = 0; i < z; i++) {
		const uint32_t v = plate[size_t(y) * W + (s ? W - 1 - RCUT - i : i)];
		if (v >> 24) { for (int c = 0; c < 3; c++) m[c] += ch(v, c); n++; }
	}
	if (n) for (int c = 0; c < 3; c++) out[c] = int(m[c] / n);
	return n > 0;
}

inline FillResult synth(std::vector<uint32_t> plate, std::vector<uint32_t> vis, int kind, int reach)
{
	const auto t0 = std::chrono::steady_clock::now();
	FillResult r;
	r.reach = reach;
	r.lum = plate_lum(plate);
	r.arena.assign(H, 0);
	if (kind == 19 || kind == 20) std::fill(r.arena.begin() + ARENA_Y0, r.arena.begin() + ARENA_Y1, 1);
	Filled F;   // (side 0's view: the plate as it is, its holes filled)
	F.make(plate.data(), 0);
	std::vector<float> FP(size_t(FZ) * H * 3);
	for (int y = 0; y < H; y++) for (int x = 0; x < FZ; x++) for (int c = 0; c < 3; c++) FP[(size_t(y) * FZ + x) * 3 + c] = float(ch(F.at(y, x), c));
	auto fp = [&](int x, int y, int c) { return FP[(size_t(std::clamp(y, 0, H - 1)) * FZ + size_t(std::clamp(x, 0, FZ - 1))) * 3 + c]; };
	// (each plate pixel's mean over the 2 FMX + 1 columns and 2 FMY + 1 lines round it: a source pixel's detail is it less
	// this, so a patch brings its texture, its streaks' lines too, but not its shade)
	// (the line's mean over 2 FMX + 1 columns: wide, so a patch keeps its own shading across, as the photo has it;
	// the stage panels' narrower, their shading near the edges steep)
	const int FMX = kind == 19 || kind == 20 ? 24 : 48;
	std::vector<float> FM(FP.size()), FR(FP.size());   // (FR: the line's mean alone)
	{
		std::vector<float> rowm(FP.size());
		for (int y = 0; y < H; y++)
			for (int c = 0; c < 3; c++) {
				float sum = 0;
				for (int x = -FMX; x <= FMX; x++) sum += fp(x, y, c);
				for (int x = 0; x < FZ; x++) {
					rowm[(size_t(y) * FZ + x) * 3 + c] = sum / float(2 * FMX + 1);
					sum += fp(x + FMX + 1, y, c) - fp(x - FMX, y, c);
				}
			}
		for (int y = 0; y < H; y++)
			for (int x = 0; x < FZ; x++)
				for (int c = 0; c < 3; c++) {
					float t = 0;
					for (int k = -FMY; k <= FMY; k++) t += rowm[(size_t(std::clamp(y + k, 0, H - 1)) * FZ + x) * 3 + c];
					FM[(size_t(y) * FZ + x) * 3 + c] = t / float(2 * FMY + 1);
					FR[(size_t(y) * FZ + x) * 3 + c] = rowm[(size_t(y) * FZ + x) * 3 + c];
				}
	}
	auto fr = [&](int x, int y, int c) { return FR[(size_t(std::clamp(y, 0, H - 1)) * FZ + size_t(std::clamp(x, 0, FZ - 1))) * 3 + c]; };
	auto fm = [&](int x, int y, int c) { return FM[(size_t(std::clamp(y, 0, H - 1)) * FZ + size_t(std::clamp(x, 0, FZ - 1))) * 3 + c]; };
	// each side's zones (how far in the background goes), its colour at the edge line by line, its slope across
	std::vector<int> bands[2];
	std::vector<float> E[2], slope[2];
	const float slc = kind == 19 || kind == 20 ? 1.0f : 0.4f;
	const bool stage = kind == 19 || kind == 20;
	// (the stage panels' blocks: nearer the edge's colour, and fewer bright specks, than the photo's: their bluish
	// parts and the white dashes of their lettering showed as marks)
	const float speck = stage ? 90.0f : SPECK, cdist = stage ? 40.0f : 70.0f;   // (the slope's most, a channel's step a pixel: the stage panels' shading toward the edges is steep)
	for (int s = 0; s < 2; s++) {
		Filled fs;
		fs.make(plate.data(), s);
		clean_zones(fs, plate.data(), s, bands[s]);
		r.zone[s].assign(H, 1);
		E[s].assign(size_t(H) * 3, 0);
		r.edge[s].assign(size_t(H) * 3, 0);
		std::vector<float> sl(size_t(H) * 3, 0);
		std::vector<uint8_t> has(H, 0);
		for (int y = 0; y < H; y++) {
			const int zb = std::max(bands[s][size_t(y / 8)], 1);
			const int z = std::clamp(zb, 1, 16);
			r.zone[s][size_t(y)] = uint8_t(z);
			int e[3];
			if (!edge_mean(plate, s, y, z, e)) for (int c = 0; c < 3; c++) e[c] = ch(fs.at(y, 0), c);
			for (int c = 0; c < 3; c++) { E[s][size_t(y) * 3 + c] = float(e[c]); r.edge[s][size_t(y) * 3 + c] = int16_t(e[c]); }
			// (the slope across, outwards: the zone's inner half against its outer half, where it's 8 or more)
			if (zb >= 8 && !r.arena[size_t(y)]) {
				const int zz = std::min(zb, 48), h = zz / 2;
				for (int c = 0; c < 3; c++) {
					float a = 0, b = 0;
					for (int i = 0; i < h; i++) { a += ch(fs.at(y, i), c); b += ch(fs.at(y, h + i), c); }
					sl[size_t(y) * 3 + c] = (a - b) / float(h * h);
				}
				has[y] = 1;
			}
		}
		// (smoothed over 25 lines, clamped: a large-scale slope, not a line's)
		slope[s].assign(size_t(H) * 3, 0);
		for (int y = 0; y < H; y++)
			for (int c = 0; c < 3; c++) {
				float t = 0; int n = 0;
				for (int k = -12; k <= 12; k++) { const int yy = y + k; if (yy >= 0 && yy < H && has[yy]) { t += sl[size_t(yy) * 3 + c]; n++; } }
				slope[s][size_t(y) * 3 + c] = n && !r.arena[size_t(y)] ? std::clamp(t / float(n), -slc, slc) : 0.0f;
			}
	}
	// the background to take patches from: 8 x 8 blocks all solid, not the preview, their colour near the edge's on
	// their lines (either side), no more detail than a photo's, no line down them
	const int BW = W / 8, BH = H / 8;
	std::vector<uint8_t> ok(size_t(W) * H, 0);
	std::vector<float> blk_det(size_t(BW) * BH, -1);
	for (int by = 0; by < BH; by++)
		for (int bx = 0; bx < BW; bx++) {
			bool all = true, prev = false;
			float m[3] = {}, det = 0;
			float e[2][3] = {};
			for (int y = by * 8; y < by * 8 + 8; y++) {
				prev |= r.arena[size_t(y)] != 0;
				for (int x = bx * 8; x < bx * 8 + 8; x++) {
					const uint32_t v = plate[size_t(y) * W + x];
					all &= (v >> 24) != 0 && x < FZ;
					for (int c = 0; c < 3; c++) { m[c] += ch(v, c); det += std::fabs(ch(v, c) - fp(x + 1, y, c)); }
				}
				for (int s = 0; s < 2; s++) for (int c = 0; c < 3; c++) e[s][c] += E[s][size_t(y) * 3 + c];
			}
			if (!all || prev) continue;
			float dist = 1e9f;
			for (int s = 0; s < 2; s++) {
				float d = 0;
				for (int c = 0; c < 3; c++) d += std::fabs(m[c] / 64.0f - e[s][c] / 8);
				dist = std::min(dist, d);
			}
			if (dist > cdist || det / 192 > DETMAX) continue;
			{   // (no speck far brighter than the rest, for its texture: a highlight copied out shows as a mark)
				float lum[64];
				int k = 0;
				for (int y = by * 8; y < by * 8 + 8; y++) for (int x = bx * 8; x < bx * 8 + 8; x++) { const uint32_t v = plate[size_t(y) * W + x]; lum[k++] = float(ch(v, 0) + ch(v, 1) + ch(v, 2)); }
				float sorted[64];
				std::copy(lum, lum + 64, sorted);
				std::nth_element(sorted, sorted + 32, sorted + 64);
				const float med = sorted[32];
				float dev = 0;
				for (float l : lum) dev += std::fabs(l - med);
				dev /= 64;
				if (*std::max_element(lum, lum + 64) > med + std::max(speck, 6 * dev)) continue;
			}
			blk_det[size_t(by) * BW + bx] = det / 192;
			float so[8];
			for (int i = 0; i < 8; i++) {
				const int x = bx * 8 + i;
				float sum = 0;
				for (int y = by * 8; y < by * 8 + 8; y++) for (int c = 0; c < 3; c++) sum += std::fabs(fp(x + 1, y, c) - fp(x, y, c));
				so[i] = sum;
			}
			std::sort(so, so + 8);
			if (so[7] > LINEK * so[3] + LINEC) continue;
			for (int y = by * 8; y < by * 8 + 8; y++) std::fill_n(&ok[size_t(y) * W + bx * 8], 8, 1);
		}
	// (and no stronger than the background at the edges (their 3 blocks in), by more than 1.8 times: a panel's
	// letters drawn into its picture, not its brushing)
	{
		std::vector<float> ed;
		for (int by = 0; by < BH; by++) for (int bx : { 0, 1, 2, BW - 3, BW - 2, BW - 1 }) if (blk_det[size_t(by) * BW + bx] >= 0) ed.push_back(blk_det[size_t(by) * BW + bx]);
		if (ed.size() >= 4) {
			std::nth_element(ed.begin(), ed.begin() + ed.size() / 2, ed.end());
			const float lim = ed[ed.size() / 2] * 1.8f + 4;
			for (int by = 0; by < BH; by++)
				for (int bx = 0; bx < BW; bx++)
					if (blk_det[size_t(by) * BW + bx] > lim)
						for (int y = by * 8; y < by * 8 + 8; y++) std::fill_n(&ok[size_t(y) * W + bx * 8], 8, 0);
		}
	}
	std::vector<int> I(size_t(W + 1) * (H + 1), 0);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			I[size_t(y + 1) * (W + 1) + x + 1] = ok[size_t(y) * W + x] + I[size_t(y) * (W + 1) + x + 1] + I[size_t(y + 1) * (W + 1) + x] - I[size_t(y) * (W + 1) + x];
	auto area = [&](int x0, int y0, int x1, int y1) { return I[size_t(y1) * (W + 1) + x1] - I[size_t(y0) * (W + 1) + x1] - I[size_t(y1) * (W + 1) + x0] + I[size_t(y0) * (W + 1) + x0]; };
	struct Cand { int16_t x, y; float sd; };
	std::vector<Cand> cands;
	int pw = 40, ph = 24, ox = 10, oy = 8;
	for (int pass = 0; pass < 2; pass++) {
		cands.clear();
		for (int y = 0; y + ph <= H; y += 2)
			for (int x = 0; x + pw <= FZ; x += CXS)
				if (area(x, y, x + pw, y + ph) == pw * ph) cands.push_back({ int16_t(x), int16_t(y), 0 });
		if (cands.size() >= 1000 || pass) break;
		pw = 20; ph = 12; ox = 6; oy = 4;
	}
	// (each window's texture strength: the mean step across, over its pixels)
	for (Cand &c : cands) {
		float t = 0;
		for (int v = 0; v + 1 < ph; v += 2) for (int u = 0; u + 1 < pw; u += 2) for (int q = 0; q < 3; q++) t += std::fabs(fp(c.x + u + 1, c.y + v, q) - fp(c.x + u, c.y + v, q)) + std::fabs(fp(c.x + u, c.y + v + 1, q) - fp(c.x + u, c.y + v, q));
		c.sd = t / float(((ph - 1) / 2 + 1) * ((pw - 1) / 2 + 1));
	}
	r.candidates = int(cands.size());
	if (cands.empty()) return r;
	uint32_t seed = 0x9E3779B9u;
	for (int s = 0; s < 2; s++) {
		const int D0 = -KR, RC = reach + pw;   // (the canvas: d from D0 to RC)
		const int CW = RC - D0;
		auto at = [&](int d, int y) { return size_t(y) * CW + size_t(d - D0); };
		std::vector<float> Q(size_t(CW) * H * 3, 0);
		std::vector<int16_t> sx(size_t(CW) * H, -1), sy(size_t(CW) * H, -1);
		std::vector<uint8_t> have(size_t(CW) * H, 0), real(size_t(CW) * H, 0), free_(size_t(CW) * H, 0);
		auto plate_x = [&](int d) { return s ? W - RCUT + d : -1 - d; };   // (d < 0: the picture's own)
		// the picture's own last KR columns: placed; those that are background as shown (the plate's pixel shows),
		// and no further in than KIN, may be replaced
		for (int y = 0; y < H; y++) {
			if (r.arena[size_t(y)]) continue;
			for (int d = D0; d < 0; d++) {
				const int x = plate_x(d);
				const uint32_t v = vis[size_t(y) * W + x], p = plate[size_t(y) * W + x];
				if (!(v >> 24)) continue;
				for (int c = 0; c < 3; c++) Q[at(d, y) * 3 + c] = float(ch(v, c));
				have[at(d, y)] = real[at(d, y)] = 1;
				sx[at(d, y)] = int16_t(x); sy[at(d, y)] = int16_t(y);
				free_[at(d, y)] = d >= -KIN && (p >> 24) && (v & 0xFFFFFF) == (p & 0xFFFFFF);
			}
		}
		// (the texture strength to match, at the patch's lines: the picture's own edge's, background there)
		auto target_sd = [&](int y0, int h) {
			float t = 0; int n = 0;
			for (int y = y0; y < y0 + h; y += 2)
				for (int d = D0; d < -1; d++) {
					const size_t i = at(d, y), k = at(d + 1, y), j = at(d, std::min(y + 1, H - 1));
					if (!free_[i] && !(real[i] && d < -KIN)) continue;
					if (!have[i] || !have[k] || !have[j]) continue;
					for (int q = 0; q < 3; q++) t += std::fabs(Q[k * 3 + q] - Q[i * 3 + q]) + std::fabs(Q[j * 3 + q] - Q[i * 3 + q]);
					n++;
				}
			return n > 16 ? t / float(n) : -1.0f;
		};
		auto src = [&](const Cand &c, int u, int v, int q) { return fp(s ? c.x + u : c.x + pw - 1 - u, c.y + v, q); };   // (the left side reads its source mirrored: as the picture runs outwards)
		const int rows = (H - oy + (ph - oy) - 1) / (ph - oy);
		static thread_local float err[48][48], accv[48][16], acch[48][16];
		static thread_local int fromv[48][16], fromh[48][16];
		for (int x0 = -KIN - 2; x0 < reach; x0 += pw - ox)
			for (int j = 0; j < rows; j++) {
				const int y0 = std::min(j * (ph - oy), H - ph);
				const bool beside = r.arena[size_t(std::min(y0 + ph / 2, H - 1))] != 0;
				const float tsd = beside ? -1.0f : target_sd(y0, ph);
				// the overlap's pixels (every other one), each channel's mean there taken off both sides
				std::vector<std::pair<int16_t, int16_t>> ov;
				float cm[3] = {};
				for (int v = 0; v < ph; v += OVS)
					for (int u = (v / OVS) % 2; u < pw; u += OVS)
						if (have[at(x0 + u, y0 + v)]) { ov.push_back({ int16_t(v), int16_t(u) }); for (int q = 0; q < 3; q++) cm[q] += Q[at(x0 + u, y0 + v) * 3 + q]; }
				const float n = std::max(1.0f, float(ov.size()));
				for (float &q : cm) q /= n;
				float best = -1;
				Cand bc = cands[0];
				for (int win : { 48, 128, H }) {   // (near its own lines; further if there's nothing near)
				if (best >= 0) break;
				for (const Cand &c : cands) {
					if (!beside && std::abs(c.y - y0) > win) continue;
					if (!beside && win > 48 && std::abs(c.y - y0) <= 48) continue;
					float pen = 0, mul = 1;
					if (!beside) { const float dy = float(c.y - y0); pen += dy * dy * 0.6f; }
					if (tsd > 0) { const float k = (c.sd - tsd) / std::max(tsd, 4.0f); mul += SDK * k * k; }   // (a texture as strong as the edge's: matching alone favours the faintest)
					seed = seed * 1664525u + 1013904223u;
					pen += float(seed >> 28);
					if (best >= 0 && pen >= best) continue;
					float sm[3] = {};
					for (const auto &[v, u] : ov) for (int q = 0; q < 3; q++) sm[q] += src(c, u, v, q);
					for (float &q : sm) q /= n;
					float e = 0;
					bool stop = false;
					for (const auto &[v, u] : ov) {
						for (int q = 0; q < 3; q++) { const float d = (Q[at(x0 + u, y0 + v) * 3 + q] - cm[q]) - (src(c, u, v, q) - sm[q]); e += d * d; }
						if (best >= 0 && e * 16 / n * mul + pen >= best) { stop = true; break; }
					}
					if (stop) continue;
					e = e * 16 / n * mul + pen;
					if (best < 0 || e < best) { best = e; bc = c; }
				}
				}
				r.patches++;
				// the cuts, on the overlap's error with each side's mean off (as matched): down the left, across the top
				float pm[3] = {};
				for (const auto &[v, u] : ov) for (int q = 0; q < 3; q++) pm[q] += src(bc, u, v, q);
				for (float &q : pm) q /= n;
				for (int v = 0; v < ph; v++)
					for (int u = 0; u < pw; u++) {
						float e = 0;
						const size_t i = at(x0 + u, y0 + v);
						if (have[i]) for (int q = 0; q < 3; q++) { const float d = (Q[i * 3 + q] - cm[q]) - (src(bc, u, v, q) - pm[q]); e += d * d; }
						if (real[i] && !free_[i]) e = 1e12f;   // (a picture's pixel that isn't background stays)
						err[v][u] = e;
					}
				int cut_v[48], cut_h[48];
				// down the left overlap: each line's cut column (the line's pixels left of it stay; never left of the
				// last that must stay)
				for (int v = 0; v < ph; v++) {
					int lim = -1;
					for (int u = 0; u < ox; u++) if (err[v][u] >= 1e11f) lim = u;
					for (int u = 0; u < ox; u++) {
						float b = 0; int bu = u;
						if (v > 0) {
							b = accv[v - 1][u];
							for (int du = -1; du <= 1; du += 2) if (u + du >= 0 && u + du < ox && accv[v - 1][u + du] < b) { b = accv[v - 1][u + du]; bu = u + du; }
						}
						accv[v][u] = (u < lim ? 1e12f : std::min(err[v][u], 1e10f)) + b; fromv[v][u] = bu;
					}
				}
				{
					int u = 0;
					for (int q = 1; q < ox; q++) if (accv[ph - 1][q] < accv[ph - 1][u]) u = q;
					for (int v = ph - 1; v >= 0; v--) { cut_v[v] = u; u = fromv[v][u]; }
				}
				for (int u = 0; u < pw; u++)
					for (int v = 0; v < oy; v++) {
						float b = 0; int bv = v;
						if (u > 0) {
							b = acch[u - 1][v];
							for (int dv = -1; dv <= 1; dv += 2) if (v + dv >= 0 && v + dv < oy && acch[u - 1][v + dv] < b) { b = acch[u - 1][v + dv]; bv = v + dv; }
						}
						acch[u][v] = std::min(err[v][u], 1e10f) + b; fromh[u][v] = bv;
					}
				{
					int v = 0;
					for (int q = 1; q < oy; q++) if (acch[pw - 1][q] < acch[pw - 1][v]) v = q;
					for (int u = pw - 1; u >= 0; u--) { cut_h[u] = v; v = fromh[u][v]; }
				}
				for (int v = 0; v < ph; v++)
					for (int u = 0; u < pw; u++) {
						const size_t i = at(x0 + u, y0 + v);
						if (real[i] && !free_[i]) continue;
						const bool left_ok = u >= ox || u > cut_v[v], top_ok = v >= oy || v > cut_h[u] || j == 0;
						if (have[i] && !(left_ok && top_ok)) continue;
						for (int q = 0; q < 3; q++) Q[i * 3 + q] = src(bc, u, v, q);
						sx[i] = int16_t(s ? bc.x + u : bc.x + pw - 1 - u); sy[i] = int16_t(bc.y + v);
						have[i] = 1; real[i] = 0;
					}
			}
		// Each pixel made: the edge's colour on its line, carried out with the picture's slope there (eased over 96),
		// plus its source's detail (the source pixel less its line's mean over 2 FMX + 1 columns round it, in the plate: the
		// patch's texture whole, not its shade, so no seam in colour where patches meet). Beside the preview: the
		// panels' colour and slope above and below it (lines 8 to 24 off it), between them, plus the detail less the
		// mean over 2 FMY + 1 lines too (the panels' brushing, line to line, comes with it)
		std::vector<float> T(size_t(CW) * H * 3, 0);
		const float LE = stage ? 200.0f : 96.0f;   // (the stage panels' shading toward the edges carried further: nearly straight on)
		r.LE = LE;
		r.stage = stage;
		auto L96 = [LE](int d) { return d > 0 ? LE * (1 - std::exp(-float(d) / LE)) : float(d); };
		{
			std::vector<float> Es(size_t(H) * 3, 0), Sl(slope[s]);
			for (int y = 0; y < H; y++) {
				if (r.arena[size_t(y)]) continue;
				for (int q = 0; q < 3; q++) {
					float t = 0; int n = 0;
					for (int k = -FMY; k <= FMY; k++) { const int z = y + k; if (z >= 0 && z < H && !r.arena[size_t(z)]) { t += E[s][size_t(z) * 3 + q]; n++; } }
					Es[size_t(y) * 3 + q] = t / float(std::max(n, 1));
				}
			}
			std::vector<float> Ey(Es);
			for (int y = 0; y < H; y++) {
				if (!r.arena[size_t(y)]) continue;
				int b = y;
				while (b < H && r.arena[size_t(b)]) b++;
				float ea[3] = {}, eb[3] = {}, sa[3] = {}, sb[3] = {}; int na = 0, nb = 0;
				for (int yy = std::max(y - 24, 0); yy < y - 8; yy++) { for (int c = 0; c < 3; c++) { ea[c] += Es[size_t(yy) * 3 + c]; sa[c] += slope[s][size_t(yy) * 3 + c]; } na++; }
				for (int yy = b + 8; yy < std::min(b + 24, H); yy++) { for (int c = 0; c < 3; c++) { eb[c] += Es[size_t(yy) * 3 + c]; sb[c] += slope[s][size_t(yy) * 3 + c]; } nb++; }
				for (int yy = y; yy < b; yy++) {
					const float t = float(yy - y + 1) / float(b - y + 1);
					for (int c = 0; c < 3; c++) {
						const float a = na ? ea[c] / float(na) : eb[c] / float(std::max(nb, 1)), bb = nb ? eb[c] / float(nb) : a;
						const float p = na ? sa[c] / float(na) : sb[c] / float(std::max(nb, 1)), pb = nb ? sb[c] / float(nb) : p;
						Ey[size_t(yy) * 3 + c] = a + (bb - a) * t;
						Sl[size_t(yy) * 3 + c] = p + (pb - p) * t;
					}
				}
				y = b - 1;
			}
			r.slope[s] = Sl;
			r.base[s].assign(size_t(H) * 3, 0);
			for (int y = 0; y < H; y++) for (int q = 0; q < 3; q++) r.base[s][size_t(y) * 3 + q] = r.arena[size_t(y)] ? Ey[size_t(y) * 3 + q] : E[s][size_t(y) * 3 + q];
			for (int y = 0; y < H; y++)
				for (int d = D0; d < RC; d++) {
					const size_t i = at(d, y);
					if (!have[i]) continue;
					const float ease = L96(d);
					for (int q = 0; q < 3; q++)
						T[i * 3 + q] = (r.arena[size_t(y)] ? Ey[size_t(y) * 3 + q] + Q[i * 3 + q] - fm(sx[i], sy[i], q) : E[s][size_t(y) * 3 + q] + Q[i * 3 + q] - fr(sx[i], sy[i], q)) + Sl[size_t(y) * 3 + q] * ease;
				}
		}
		// (the picture's own pixels stay; the rest is what it's held to)
		std::vector<float> S(Q);
		for (size_t i = 0; i < have.size(); i++) if (have[i] && !real[i]) for (int q = 0; q < 3; q++) S[i * 3 + q] = T[i * 3 + q];
		const int OW = KIN + reach;
		r.px[s].assign(size_t(OW) * H * 3, 0);
		r.mine[s].assign(size_t(OW) * H, 0);
		for (int y = 0; y < H; y++)
			for (int d = -KIN; d < reach; d++) {
				const size_t i = at(d, y), o = size_t(y) * OW + size_t(d + KIN);
				if (!have[i] || real[i] || (d < 0 && !free_[i])) continue;   // (in the picture: only its background replaced)
				r.mine[s][o] = 1;
				for (int q = 0; q < 3; q++) r.px[s][o * 3 + q] = int16_t(std::clamp(std::lround(S[i * 3 + q]), 0L, 255L));
			}
	}
	r.ok = true;
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return r;
}

// The stage screen's sides darkened toward the picture's outer edges, down to this percent of the edge's brightness
// there (0: none, the panels' slope carried on straight; the host sets 55)
inline int vignette_floor = 0;

// The fill's state: the screen it's for, its frames so far and how settled, the job making it, what it made
struct FillJob { std::atomic<bool> done{ false }; FillResult res; };
struct FillState
{
	int key = -1, frames = 0, settled = 0, shown = 0;
	double last_lum = -1;
	std::shared_ptr<FillJob> job;   // (made on a thread of its own, let go of if the screen changes first)
	std::shared_ptr<FillResult> res;
	// (the vignette's lines' value and slope at the edge, each side's, and what they were made for)
	std::vector<float> vg0[2], vk[2];
	const FillResult *vig_for = nullptr;
	int vig_dn[2] = {}, vig_age = 0;
};

// Option 2 (dark: 6) into out: its sides as far as reach, and (where made) up to KIN columns into the 4:3 picture.
// False while the fill's still being made (the sides left empty).
inline bool fill(int key, const std::vector<uint32_t> &plate, const std::vector<uint32_t> &above, const std::vector<uint32_t> &below,
                 std::vector<uint32_t> &out, FillState &st, int reach, bool dark)
{
	if (st.key != key) { st = FillState{}; st.key = key; }
	st.frames++;
	const double lum = plate_lum(plate);
	st.settled = std::fabs(lum - st.last_lum) < std::max(1.0, lum * 0.01) ? st.settled + 1 : 0;
	st.last_lum = lum;
	if (st.job && st.job->done.load(std::memory_order_acquire)) {
		st.res = std::make_shared<FillResult>(std::move(st.job->res));
		st.job.reset();
		st.shown = 0;
		if (getenv("VF3_WIDE_SELECT_LOG"))
			fprintf(stderr, "(wide select) fill for screen %d: %.1f ms, %d candidates, %d patches%s\n", key, st.res->ms, st.res->candidates, st.res->patches, st.res->ok ? "" : " (no background to take it from)");
	}
	const bool want = !st.job && lum > 30 && (st.settled >= 3 || st.frames >= 45) &&
	                  (!st.res || st.res->reach < reach || lum > st.res->lum * 1.25 + 6);
	if (want) {
		const int r = std::max(reach, 96) + RCUT;
		const int kind = key;
		std::vector<uint32_t> vis = visible_2d(above, below);
		if (getenv("VF3_WIDE_SELECT_SYNC")) {   // (tests: at once)
			st.res = std::make_shared<FillResult>(synth(plate, vis, kind, r));
			st.shown = 8;
			if (getenv("VF3_WIDE_SELECT_LOG")) fprintf(stderr, "(wide select) fill for screen %d: %.1f ms, %d candidates, %d patches\n", key, st.res->ms, st.res->candidates, st.res->patches);
		}
		else {
			st.job = std::make_shared<FillJob>();
			std::thread([j = st.job, p = plate, v = std::move(vis), kind, r] { j->res = synth(p, v, kind, r); j->done.store(true, std::memory_order_release); }).detach();
		}
	}
	if (!st.res || !st.res->ok) return false;
	const FillResult &R = *st.res;
	const float in = std::min(1.0f, ++st.shown / 8.0f);   // (faded in over 8 pictures once made)
	const int OW = KIN + R.reach;
	for (int s = 0; s < 2; s++) {
		// how far the edge's colour has moved since it was made (a fade), line by line; beside the preview, the panels'
		std::vector<int> mv(size_t(H) * 3, 0);
		long tot[3] = {}; int nt = 0;
		for (int y = 0; y < H; y++) {
			if (R.arena[size_t(y)]) continue;
			int e[3];
			if (!edge_mean(plate, s, y, R.zone[s][size_t(y)], e)) continue;
			for (int c = 0; c < 3; c++) { mv[size_t(y) * 3 + c] = e[c] - R.edge[s][size_t(y) * 3 + c]; tot[c] += mv[size_t(y) * 3 + c]; }
			nt++;
		}
		for (int y = 0; y < H; y++) if (R.arena[size_t(y)] && nt) for (int c = 0; c < 3; c++) mv[size_t(y) * 3 + c] = int(tot[c] / nt);
		const int dn = std::min(reach + (s ? RCUT : 0), R.reach);
		// The vignette (the stage screen, vignette_floor > 0): the panels' own end shading carried on as a darkening
		// and eased (a cubic) to vignette_floor of the edge's brightness at the picture's outer edge (dn out), in place
		// of the slope carried on straight. Its value and slope at the 4:3 edge the panel's own there, line by line
		// (9 lines' mean): its last 3 columns' brightness against the side's colour there (g0), and their step from
		// the 9 columns before them (vk, a fraction a pixel); beside the preview, between the panels above and below
		// (made again every 30 pictures, for a new fill, or a new width: a ratio and a slope of the panels', which
		// a fade hardly moves)
		std::vector<float> &vg0 = st.vg0[s], &vk = st.vk[s];
		const bool vig_again = st.vig_for != st.res.get() || st.vig_dn[s] != dn || st.vig_age >= 30 || vg0.size() != size_t(H);
		st.vig_dn[s] = dn;
		// (the cubic's terms and the slope's easing, by d: the same for every line)
		std::vector<float> h00, h10, h01, lev;
		if (vignette_floor > 0 && R.stage) {
			h00.resize(size_t(dn) + 1); h10.resize(size_t(dn) + 1); h01.resize(size_t(dn) + 1); lev.resize(size_t(dn) + 1);
			for (int d = 0; d <= dn; d++) {
				const float t = float(d) / float(dn), t2 = t * t, t3 = t2 * t;
				h00[size_t(d)] = 2 * t3 - 3 * t2 + 1; h10[size_t(d)] = (t3 - 2 * t2 + t) * float(dn); h01[size_t(d)] = -2 * t3 + 3 * t2;
				lev[size_t(d)] = R.LE * (1 - std::exp(-float(d) / R.LE));
			}
		}
		if (vig_again) { vg0.assign(H, 1.0f); vk.assign(H, 0.0f); }
		if (vig_again && vignette_floor > 0 && R.stage && !R.base[s].empty()) {
			std::vector<float> le(H, -1), li(H, -1);
			auto lum_of = [&](int y, int i0, int i1) {
				float t = 0; int n = 0;
				for (int i = i0; i < i1; i++) {
					const uint32_t v = plate[size_t(y) * W + size_t(s ? W - 1 - RCUT - i : i)];
					if (v >> 24) { t += float(ch(v, 0) + ch(v, 1) + ch(v, 2)); n++; }
				}
				return n ? t / float(n) : -1.0f;
			};
			for (int y = 0; y < H; y++) if (!R.arena[size_t(y)]) { le[size_t(y)] = lum_of(y, 0, 3); li[size_t(y)] = lum_of(y, 3, 12); }
			std::vector<uint8_t> ok(H, 0);
			for (int y = 0; y < H; y++) {
				if (R.arena[size_t(y)]) continue;
				float a = 0, b = 0, c = 0; int n = 0;
				for (int k = -4; k <= 4; k++) {
					const int z = y + k;
					if (z < 0 || z >= H || R.arena[size_t(z)] || le[size_t(z)] < 0 || li[size_t(z)] < 0) continue;
					const float *bs = &R.base[s][size_t(z) * 3];
					a += le[size_t(z)]; b += li[size_t(z)]; c += bs[0] + bs[1] + bs[2]; n++;
				}
				if (!n || c <= 0) continue;
				vg0[size_t(y)] = std::clamp(a / c, 0.5f, 1.2f);
				vk[size_t(y)] = (a - b) / 7.5f / c;   // (the two groups' middles 7.5 columns apart; outwards)
				ok[y] = 1;
			}
			for (int y = 0; y < H; y++) {
				if (ok[y]) continue;
				int a = y - 1, b = y + 1;
				while (a >= 0 && !ok[a]) a--;
				while (b < H && !ok[b]) b++;
				if (a < 0 && b >= H) break;
				const float t = a < 0 ? 1.0f : b >= H ? 0.0f : float(y - a) / float(b - a);
				const int ia = a < 0 ? b : a, ib = b >= H ? a : b;
				vg0[size_t(y)] = vg0[size_t(ia)] + (vg0[size_t(ib)] - vg0[size_t(ia)]) * t;
				vk[size_t(y)] = vk[size_t(ia)] + (vk[size_t(ib)] - vk[size_t(ia)]) * t;
			}
		}
		for (int y = 0; y < H; y++) {
			if (dark && R.arena[size_t(y)]) continue;
			const int *m = &mv[size_t(y) * 3];
			float k0 = 0, fl = 0, g0 = 1;
			const bool vig = vignette_floor > 0 && R.stage && !R.base[s].empty();
			if (vig) {
				fl = float(vignette_floor) / 100.0f;
				g0 = vg0[size_t(y)];
				k0 = std::clamp(vk[size_t(y)], -3.0f * std::max(g0 - fl, 0.0f) / float(dn), 0.0f);
			}
			auto g_of = [&](int d) { return h00[size_t(d)] * g0 + h10[size_t(d)] * k0 + h01[size_t(d)] * fl; };
			for (int d = -KIN; d < std::min(reach + (s ? RCUT : 0), R.reach); d++) {
				const size_t o = size_t(y) * OW + size_t(d + KIN);
				if (!R.mine[s][o]) continue;
				const int16_t *p = &R.px[s][o * 3];
				float c3[3];
				for (int c = 0; c < 3; c++) c3[c] = float(p[c] + m[c]);
				if (vig && d > 0) {
					const float g = g_of(std::min(d, dn)), le = lev[size_t(std::min(d, dn))];
					for (int c = 0; c < 3; c++) c3[c] = (c3[c] - R.slope[s][size_t(y) * 3 + c] * le) * g;
				}
				if (in < 1) {   // (fading in: from what's there)
					uint32_t was = 0;
					if (d < 0) { const size_t k = size_t(y) * W + size_t(s ? W - RCUT + d : -1 - d); was = (above[k] >> 24) ? above[k] : below[k]; }
					for (int c = 0; c < 3; c++) c3[c] = float(ch(was, c)) + (c3[c] - float(ch(was, c))) * in;
				}
				out[size_t(y) * WW + size_t(s ? EXT + W - RCUT + d : EXT - 1 - d)] = rgb(int(c3[0] + 0.5f), int(c3[1] + 0.5f), int(c3[2] + 0.5f));
			}
		}
	}
	st.vig_age = st.vig_for != st.res.get() || st.vig_age >= 30 ? 0 : st.vig_age + 1;
	st.vig_for = st.res.get();
	return true;
}

} // namespace wide_select
