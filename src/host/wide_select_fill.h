// VF3_WIDE_SELECT's option 2 (wide_select.h): the sides as a content-aware fill of the screen's own background.
// Each side is the background's colour at the edge, line by line (the edge's own columns averaged: its gradient, and
// the lines and bands that reach the edge, carried straight on), plus its texture: the background's detail (each
// pixel less its line's local mean, 17 pixels across), quilted from the plate's background anywhere on the screen
// (8 x 8 blocks of solid tiles whose colour is near the edge's: the photo under the letters and the 3D, not the
// portraits or the stage's stills). Quilting (Efros and Freeman's): 32 x 32 patches overlapping by 8, out from the
// edge's own 12 columns mirrored (no seam at the 4:3 edge), each patch the source window whose detail best matches
// what's made where they overlap, near its own lines (the bands line up), joined along the overlap's least-error path
// (a cut, not a blend). Made once a screen, on a thread of its own, once the screen has settled (its fade in); then
// each picture is the edge's colours of that picture plus the detail made (scaled with the screen's brightness: its
// fades), so the cost each picture is a few adds a pixel.
// On the stage screen (both programs: the same layout) the preview of the stage, lines ARENA_Y0 to ARENA_Y1, isn't
// carried on: beside it the panels' metal goes on down as a frame round it (option 2), or the sides are dark there
// (option 6).
#pragma once
#include <chrono>
#include <atomic>
#include <memory>
#include <thread>

namespace wide_select {

constexpr int PMAX = 32, OMAX = 8, MIRROR = 12, HB = 8;
constexpr int ARENA_Y0 = 68, ARENA_Y1 = 262;   // (the stage screen's preview, aligned lines: both programs)

struct FillResult
{
	int reach = 0;
	std::vector<int16_t> det[2];      // (reach x H x 3: each side's detail, d out from its edge)
	std::vector<uint8_t> zone[2];     // (each line's columns averaged for its colour at the edge, 1-16)
	std::vector<uint8_t> arena;       // (H: the stage screen's preview lines)
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

// the edge's colour on line y of side s (side view f): its first z columns' mean
inline void edge_colour(const Filled &f, int y, int z, int out[3])
{
	int m[3] = {};
	for (int i = 0; i < z; i++) for (int c = 0; c < 3; c++) m[c] += ch(f.at(y, i), c);
	for (int c = 0; c < 3; c++) out[c] = m[c] / z;
}

inline FillResult synth(std::vector<uint32_t> plate, int kind, int reach)
{
	const auto t0 = std::chrono::steady_clock::now();
	FillResult r;
	r.reach = reach;
	r.lum = plate_lum(plate);
	r.arena.assign(H, 0);
	if (kind == 19 || kind == 20) std::fill(r.arena.begin() + ARENA_Y0, r.arena.begin() + ARENA_Y1, 1);
	Filled f[2];
	std::vector<int> bands[2];
	std::vector<int> E[2];   // (H x 3)
	for (int s = 0; s < 2; s++) {
		f[s].make(plate.data(), s);
		clean_zones(f[s], plate.data(), s, bands[s]);
		r.zone[s].assign(H, 8);
		E[s].assign(size_t(H) * 3, 0);
		for (int y = 0; y < H; y++) {
			const int z = std::clamp(bands[s][size_t(y / 8)], 1, 16);
			r.zone[s][size_t(y)] = uint8_t(z);
			edge_colour(f[s], y, z, &E[s][size_t(y) * 3]);
		}
	}
	// the detail, in the plate's own places (side 0's view is the plate as it is): each pixel less its line's mean
	// over 2 HB + 1 columns
	const Filled &F = f[0];
	std::vector<int16_t> D(size_t(W) * H * 3);
	for (int y = 0; y < H; y++) {
		int sum[3] = {};
		auto px = [&](int x) { return F.at(y, std::clamp(x, 0, FZ - 1)); };
		for (int x = -HB; x <= HB; x++) for (int c = 0; c < 3; c++) sum[c] += ch(px(x), c);
		for (int x = 0; x < W; x++) {
			for (int c = 0; c < 3; c++) D[(size_t(y) * W + x) * 3 + c] = int16_t(ch(px(x), c) - sum[c] / (2 * HB + 1));
			for (int c = 0; c < 3; c++) sum[c] += ch(px(x + HB + 1), c) - ch(px(x - HB), c);
		}
	}
	// the background: 8 x 8 blocks all solid, not the preview, their colour near the edge's on their lines (either side)
	// and their detail no more than a photo's
	const int BW = W / 8, BH = H / 8;
	std::vector<uint8_t> ok(size_t(W) * H, 0);
	for (int by = 0; by < BH; by++)
		for (int bx = 0; bx < BW; bx++) {
			bool all = true, prev = false;
			long m[3] = {}, e[2][3] = {}, det = 0;
			for (int y = by * 8; y < by * 8 + 8; y++) {
				prev |= r.arena[size_t(y)] != 0;
				for (int x = bx * 8; x < bx * 8 + 8; x++) {
					const uint32_t v = plate[size_t(y) * W + x];
					all &= (v >> 24) != 0;
					for (int c = 0; c < 3; c++) { m[c] += ch(v, c); det += std::abs(D[(size_t(y) * W + x) * 3 + c]); }
				}
				for (int s = 0; s < 2; s++) for (int c = 0; c < 3; c++) e[s][c] += E[s][size_t(y) * 3 + c];
			}
			if (!all || prev) continue;
			long dist = 1 << 30;
			for (int s = 0; s < 2; s++) {
				long d = 0;
				for (int c = 0; c < 3; c++) d += std::abs(m[c] / 64 - e[s][c] / 8);
				dist = std::min(dist, d);
			}
			if (dist > 70 || det / 192 > 40) continue;
			// (and no line down it: a frame's or a still's edge, which copied out would be a line down the side) the
			// block's columns' steps to the next, the largest against the middle one
			{
				int st[8];
				for (int i = 0; i < 8; i++) {
					const int x = std::min(bx * 8 + i, FZ - 2);
					int sum = 0;
					for (int y = by * 8; y < by * 8 + 8; y++) for (int c = 0; c < 3; c++) sum += std::abs(ch(F.at(y, x + 1), c) - ch(F.at(y, x), c));
					st[i] = sum;
				}
				int so[8];
				std::copy(st, st + 8, so);
				std::sort(so, so + 8);
				if (so[7] > 3 * so[3] + 48) continue;
			}
			for (int y = by * 8; y < by * 8 + 8; y++) std::fill_n(&ok[size_t(y) * W + bx * 8], 8, 1);
		}
	// the source windows (pw x ph, every 2 pixels) all background
	std::vector<int> I(size_t(W + 1) * (H + 1), 0);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			I[size_t(y + 1) * (W + 1) + x + 1] = ok[size_t(y) * W + x] + I[size_t(y) * (W + 1) + x + 1] + I[size_t(y + 1) * (W + 1) + x] - I[size_t(y) * (W + 1) + x];
	auto area = [&](int x0, int y0, int x1, int y1) { return I[size_t(y1) * (W + 1) + x1] - I[size_t(y0) * (W + 1) + x1] - I[size_t(y1) * (W + 1) + x0] + I[size_t(y0) * (W + 1) + x0]; };
	struct Cand { int16_t x, y; };
	std::vector<Cand> cands;
	// (32 x 32 patches overlapping by 8; where the background is in small pieces (the stage screen's panels, between
	// their stills and letters) 16 x 16 by 4)
	int pw = PMAX, ph = PMAX, ov = OMAX;
	for (int pass = 0; pass < 2; pass++) {
		cands.clear();
		for (int y = 0; y + ph <= H; y += 3)
			for (int x = 0; x + pw <= FZ; x += 3)
				if (area(x, y, x + pw, y + ph) == pw * ph) cands.push_back({ int16_t(x), int16_t(y) });
		if (cands.size() >= 2000 || pass) break;
		pw = ph = PMAX / 2; ov = OMAX / 2;
	}
	r.candidates = int(cands.size());
	if (cands.empty()) return r;
	uint32_t seed = 0x9E3779B9u;
	for (int s = 0; s < 2; s++) {
		const int RC = reach + pw;
		std::vector<int16_t> C(size_t(RC) * H * 3, 0);
		std::vector<uint8_t> have(size_t(RC) * H, 0);
		// the edge's own columns mirrored first (lines beside the preview: none)
		for (int y = 0; y < H; y++) {
			if (r.arena[size_t(y)] || !f[s].row_ok[y]) continue;
			const int z = std::max(bands[s][size_t(y / 8)], 1);
			for (int d = 0; d < std::min(MIRROR, z); d++) {   // (only the edge's background: a band's whose is short, that far)
				const int m = d % (2 * z), i = m < z ? m : 2 * z - 1 - m;
				const uint32_t v = f[s].at(y, i);
				for (int c = 0; c < 3; c++) C[(size_t(y) * RC + d) * 3 + c] = int16_t(ch(v, c) - E[s][size_t(y) * 3 + c]);
				have[size_t(y) * RC + d] = 1;
			}
		}
		std::vector<Cand> placed;   // (each patch's source, by column then line)
		const int rows = (H - ov + (ph - ov) - 1) / (ph - ov);
		for (int k = 0, x0 = MIRROR - ov; x0 < reach; k++, x0 += pw - ov)
			for (int j = 0; j < rows; j++) {
				const int y0 = std::min(j * (ph - ov), H - ph);
				const bool beside = r.arena[size_t(std::min(y0 + ph / 2, H - 1))] != 0;
				long best = -1;
				Cand bc = cands[0];
				// the overlap's pixels (every other one each way), and each candidate's error there (stopped once past the best)
				std::vector<std::pair<int16_t, int16_t>> at;
				for (int v = 0; v < ph; v += 2)
					for (int u = 0; u < pw; u += 2) if (have[size_t(y0 + v) * RC + x0 + u]) at.push_back({ int16_t(v), int16_t(u) });
				const long n = std::max<long>(1, long(at.size()));
				for (const Cand &c : cands) {
					long pen = 0;
					if (!beside) { const long dy = c.y - y0; pen = dy * dy / 2; }   // (near its own lines)
					seed = seed * 1664525u + 1013904223u;
					pen += long(seed >> 28);
					if (best >= 0 && pen >= best) continue;
					long e = 0;
					for (const auto &[v, u] : at) {
						const int16_t *a = &C[(size_t(y0 + v) * RC + x0 + u) * 3], *b = &D[(size_t(c.y + v) * W + c.x + u) * 3];
						for (int q = 0; q < 3; q++) { const int d = a[q] - b[q]; e += d * d; }
						if (best >= 0 && e * 16 / n + pen >= best) break;
					}
					e = e * 16 / n + pen;
					if (best < 0 || e < best) { best = e; bc = c; }
				}
				placed.push_back(bc);
				r.patches++;
				// the cuts: down the left overlap (a column a line), across the top one (a line a column), the least error
				long err[PMAX][PMAX];
				for (int v = 0; v < ph; v++)
					for (int u = 0; u < pw; u++) {
						long e = 0;
						if (have[size_t(y0 + v) * RC + x0 + u]) {
							const int16_t *a = &C[(size_t(y0 + v) * RC + x0 + u) * 3], *b = &D[(size_t(bc.y + v) * W + bc.x + u) * 3];
							for (int q = 0; q < 3; q++) { const int d = a[q] - b[q]; e += d * d; }
						}
						err[v][u] = e;
					}
				int cut_v[PMAX], cut_h[PMAX];   // (the left cut's column on each line; the top cut's line in each column)
				{
					long acc[PMAX][OMAX]; int from[PMAX][OMAX];
					for (int v = 0; v < ph; v++)
						for (int u = 0; u < ov; u++) {
							long b = 0; int bu = u;
							if (v > 0) {
								b = acc[v - 1][u];
								for (int du = -1; du <= 1; du += 2) if (u + du >= 0 && u + du < ov && acc[v - 1][u + du] < b) { b = acc[v - 1][u + du]; bu = u + du; }
							}
							acc[v][u] = err[v][u] + b; from[v][u] = bu;
						}
					int u = 0;
					for (int q = 1; q < ov; q++) if (acc[ph - 1][q] < acc[ph - 1][u]) u = q;
					for (int v = ph - 1; v >= 0; v--) { cut_v[v] = u; u = from[v][u]; }
				}
				{
					long acc[PMAX][OMAX]; int from[PMAX][OMAX];
					for (int u = 0; u < pw; u++)
						for (int v = 0; v < ov; v++) {
							long b = 0; int bv = v;
							if (u > 0) {
								b = acc[u - 1][v];
								for (int dv = -1; dv <= 1; dv += 2) if (v + dv >= 0 && v + dv < ov && acc[u - 1][v + dv] < b) { b = acc[u - 1][v + dv]; bv = v + dv; }
							}
							acc[u][v] = err[v][u] + b; from[u][v] = bv;
						}
					int v = 0;
					for (int q = 1; q < ov; q++) if (acc[pw - 1][q] < acc[pw - 1][v]) v = q;
					for (int u = pw - 1; u >= 0; u--) { cut_h[u] = v; v = from[u][v]; }
				}
				for (int v = 0; v < ph; v++)
					for (int u = 0; u < pw; u++) {
						uint8_t &hv = have[size_t(y0 + v) * RC + x0 + u];
						const bool left_ok = u >= ov || u > cut_v[v], top_ok = v >= ov || v > cut_h[u] || j == 0;
						if (hv && !(left_ok && top_ok)) continue;
						const int16_t *b = &D[(size_t(bc.y + v) * W + bc.x + u) * 3];
						std::copy(b, b + 3, &C[(size_t(y0 + v) * RC + x0 + u) * 3]);
						hv = 1;
					}
			}
		// the texture's strength matched to the edge's, band by band (8 lines): the detail's spread in the edge's own
		// columns (its first 24) against the made one's past the mirrored columns
		for (int b = 0; b < H / 8; b++) {
			double st = 0, sc = 0; long nt = 0, nc = 0;
			for (int y = b * 8; y < b * 8 + 8; y++) {
				if (r.arena[size_t(y)]) continue;
				for (int i = 0; i < 24; i++) {
					const int x = s ? FZ - 1 - i : i;
					for (int c = 0; c < 3; c++) { const double v = D[(size_t(y) * W + x) * 3 + c]; st += v * v; nt++; }
				}
				for (int d = MIRROR; d < reach; d++) for (int c = 0; c < 3; c++) { const double v = C[(size_t(y) * RC + d) * 3 + c]; sc += v * v; nc++; }
			}
			if (!nt || !nc || sc <= 0) continue;
			const double g = std::clamp(std::sqrt((st / nt) / (sc / nc)), 0.5, 2.0);
			for (int y = b * 8; y < b * 8 + 8; y++)
				for (int d = MIRROR; d < RC; d++) for (int c = 0; c < 3; c++) { int16_t &v = C[(size_t(y) * RC + d) * 3 + c]; v = int16_t(std::lround(v * g)); }
		}
		r.det[s].assign(size_t(reach) * H * 3, 0);
		for (int y = 0; y < H; y++) std::copy(&C[size_t(y) * RC * 3], &C[size_t(y) * RC * 3] + size_t(reach) * 3, &r.det[s][size_t(y) * reach * 3]);
	}
	r.ok = true;
	r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return r;
}

// The fill's state: the screen it's for, its frames so far and how settled, the job making it, what it made
struct FillJob { std::atomic<bool> done{ false }; FillResult res; };
struct FillState
{
	int key = -1, frames = 0, settled = 0, shown = 0;
	double last_lum = -1;
	std::shared_ptr<FillJob> job;   // (made on a thread of its own, let go of if the screen changes first)
	std::shared_ptr<FillResult> res;
};

// Option 2 (dark: 6) into out's sides, as far as reach. False while the fill's still being made (the sides left empty).
inline bool fill(int key, const std::vector<uint32_t> &plate, std::vector<uint32_t> &out, FillState &st, int reach, bool dark)
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
		if (getenv("VF3_WIDE_SELECT_SYNC")) { st.res = std::make_shared<FillResult>(synth(plate, kind, r)); st.shown = 8; }   // (tests: at once)
		else {
			st.job = std::make_shared<FillJob>();
			std::thread([j = st.job, p = plate, kind, r] { j->res = synth(p, kind, r); j->done.store(true, std::memory_order_release); }).detach();
		}
	}
	if (!st.res || !st.res->ok) return false;
	const FillResult &R = *st.res;
	const float k = float(std::clamp(lum / std::max(R.lum, 1.0), 0.0, 1.5));
	const float in = std::min(1.0f, ++st.shown / 8.0f);   // (faded in over 8 pictures once made)
	for (int s = 0; s < 2; s++) {
		// the edge's colour now, each line (its first columns' background pixels; a line with none there, the line
		// before's); beside the preview, the panels' above and below it, between them
		std::vector<int> E(size_t(H) * 3, 0);
		std::vector<uint8_t> has(H, 0);
		for (int y = 0; y < H; y++) {
			if (R.arena[size_t(y)]) continue;
			long m[3] = {}; int n = 0;
			for (int i = 0; i < R.zone[s][size_t(y)]; i++) {
				const uint32_t v = plate[size_t(y) * W + (s ? W - 1 - RCUT - i : i)];
				if (v >> 24) { for (int c = 0; c < 3; c++) m[c] += ch(v, c); n++; }
			}
			if (n) { for (int c = 0; c < 3; c++) E[size_t(y) * 3 + c] = int(m[c] / n); has[y] = 1; }
		}
		// (lines with none: between the nearest with, above and below)
		for (int y = 0; y < H; y++) {
			if (has[y] || R.arena[size_t(y)]) continue;
			int a = y - 1, b = y + 1;
			while (a >= 0 && (!has[a] || R.arena[size_t(a)])) a--;
			while (b < H && (!has[b] || R.arena[size_t(b)])) b++;
			if (a < 0 && b >= H) break;
			for (int yy = y; yy < std::min(b, H); yy++) {
				if (R.arena[size_t(yy)]) continue;
				const int *ea = &E[size_t(a >= 0 ? a : b) * 3], *eb = &E[size_t(b < H ? b : a) * 3];
				const float t = a < 0 || b >= H ? 0.0f : float(yy - a) / float(b - a);
				for (int c = 0; c < 3; c++) E[size_t(yy) * 3 + c] = int(ea[c] + (eb[c] - ea[c]) * t);
				has[yy] = 2;
			}
			y = b - 1;
		}
		if (!dark) {
			// beside the preview: the panels' colour above it and below it (their lines 8 to 24 off it, not their
			// edges' bevels), between them
			for (int y = 0; y < H; y++) {
				if (!R.arena[size_t(y)]) continue;
				int b = y;
				while (b < H && R.arena[size_t(b)]) b++;
				auto mean = [&](int y0, int y1, int m[3]) {
					long t[3] = {}; int n = 0;
					for (int yy = std::max(y0, 0); yy < std::min(y1, H); yy++)
						if (has[yy] && !R.arena[size_t(yy)]) { for (int c = 0; c < 3; c++) t[c] += E[size_t(yy) * 3 + c]; n++; }
					for (int c = 0; c < 3; c++) m[c] = n ? int(t[c] / n) : -1;
					return n > 0;
				};
				int ea[3], eb[3];
				const bool ha = mean(y - 24, y - 8, ea), hb = mean(b + 8, b + 24, eb);
				if (ha || hb)
					for (int yy = y; yy < b; yy++) {
						const float t = float(yy - y + 1) / float(b - y + 1);
						for (int c = 0; c < 3; c++) E[size_t(yy) * 3 + c] = !hb ? ea[c] : !ha ? eb[c] : int(ea[c] + (eb[c] - ea[c]) * t);
						has[yy] = 1;
					}
				y = b - 1;
			}
		}
		for (int y = 0; y < H; y++) {
			if (!has[y]) continue;
			const int *e = &E[size_t(y) * 3];
			const int16_t *dt = &R.det[s][size_t(y) * R.reach * 3];
			const bool frame = R.arena[size_t(y)];
			for (int d = 0; d < std::min(reach + (s ? RCUT : 0), R.reach); d++) {
				float c3[3];
				for (int c = 0; c < 3; c++) c3[c] = (e[c] + dt[d * 3 + c] * k) * in;
				if (frame && d < 2) for (float &c : c3) c *= 0.55f;   // (the frame's inner edge: a bevel's shadow)
				out[size_t(y) * WW + (s ? EXT + W - RCUT + d : EXT - 1 - d)] = rgb(int(c3[0]), int(c3[1]), int(c3[2]));
			}
		}
	}
	return true;
}

} // namespace wide_select
