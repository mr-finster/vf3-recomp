// The Real3D drawn on the CPU. See r3d_raster.h.
//
// Shading, per pixel (what is known of the Real3D, as the reference renders it):
//   colour = texel * polygon colour (alpha: the polygon's translucency times the nodes' LOD alpha); pixels under
//   1/32 alpha are dropped. Lit polygons: times min(1, sun * max(0, N.L) + ambient) + spotlight, where N.L is the
//   fixed shade for fixed-shaded polygons; specular adds (smooth: N.L to a power by shininess; flat: the
//   reflection's Z) times its amount and the sun. Then clamped to 1 and fogged: mixed towards fog colour *
//   ambient by fog intensity * clamp(start + distance * density, 0, 1).
// Textures: 2 sheets of 2048x1024 16-bit texels, mipmaps beside them (level n at fixed places); the level by
// -log2(LOD base / distance^2), filtered bilinearly within and between levels. Formats: 0 RGB5 with a
// transparent bit, 1-4 4-bit grey + 4-bit alpha in one of the four nibble pairs, 5-6 8-bit grey (low or high
// byte; 255 transparent with contour), 7 RGBA4, 8-11 a 4-bit grey nibble (15 transparent; formats 1-4 with
// contour).
#include "r3d_raster.h"
#include "parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace video {

namespace {

const int mip_x[] = { 0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047 };
const int mip_y[] = { 0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023 };
const float NEAR = 1e-3f;

struct C4 { float r, g, b, a; };

inline C4 decode(int type, uint32_t v)
{
	C4 c = { 0, 0, 0, 0 };
	auto grey = [&](float g, float a) { c = { g, g, g, a }; };
	switch (type) {
	case 0:
		c.r = ((v >> 10) & 31) / 31.0f; c.g = ((v >> 5) & 31) / 31.0f; c.b = (v & 31) / 31.0f;
		c.a = 1.0f - float((v >> 15) & 1);
		break;
	case 1: grey((v & 15) / 15.0f, ((v >> 4) & 15) / 15.0f); break;
	case 2: grey(((v >> 4) & 15) / 15.0f, (v & 15) / 15.0f); break;
	case 3: grey(((v >> 8) & 15) / 15.0f, ((v >> 12) & 15) / 15.0f); break;
	case 4: grey(((v >> 12) & 15) / 15.0f, ((v >> 8) & 15) / 15.0f); break;
	case 5: { unsigned b = v & 0xFF; grey(b / 255.0f, b == 255 ? 0.0f : 1.0f); break; }
	case 6: { unsigned b = (v >> 8) & 0xFF; grey(b / 255.0f, b == 255 ? 0.0f : 1.0f); break; }
	case 7:
		c = { ((v >> 12) & 15) / 15.0f, ((v >> 8) & 15) / 15.0f, ((v >> 4) & 15) / 15.0f, (v & 15) / 15.0f };
		break;
	case 8: case 9: case 10: case 11: {
		unsigned n = (v >> ((type - 8) * 4)) & 15;
		grey(n / 15.0f, n == 15 ? 0.0f : 1.0f);
		break;
	}
	default: break;
	}
	return c;
}

// The two texels' places along one axis (in 0-1 of the texture) and the weight between them
inline float tex_locations(int wrap, float size, float u, float &u0, float &u1)
{
	const float texel = 1.0f / size, half = 0.5f / size;
	auto fract = [](float x) { return x - std::floor(x); };
	if (wrap == 0) {
		u = u * size - 0.5f;
		u0 = fract((std::floor(u) + 0.5f) / size);
		u1 = fract(u0 + texel);
		return fract(u);
	}
	if (wrap == 1) {
		u = fract(u) * size - 0.5f;
	} else {
		float odd = std::floor(std::fmod(u, 2.0f) + (u < 0 ? 2.0f : 0.0f));
		u = odd > 0 ? 1.0f - fract(u) : fract(u);
		u = u * size - 0.5f;
	}
	u0 = (std::floor(u) + 0.5f) / size;
	u1 = u0 + texel;
	if (u0 < 0) u0 = 0;
	if (u1 >= 1) u1 = 1 - half;
	return fract(u);
}

} // namespace

void Raster::resize(int width, int height, int game_width)
{
	if (game_width <= 0) game_width = int(std::lround(496.0 * height / 384.0));
	game_width = std::min(game_width, width);
	if (width == w && height == h && game_width == gw)
		return;
	w = width;
	h = height;
	gw = game_width;
	scale = h / 384.0f;
	xscale = gw / 496.0f;
	gx = (w - gw) / 2;
	alloc();
}

void Raster::alloc()
{
	size_t n = size_t(w) * h;
	base.assign(n, {});
	t1.assign(n, {});
	t2.assign(n, {});
	depth.assign(n, 0);
	depth_saved.assign(n, 0);
	stencil.assign(n, 0);
	stencil_saved.assign(n, 0);
}

void Raster::drop_buffers()
{
	for (auto *v : { &base, &t1, &t2 }) { v->clear(); v->shrink_to_fit(); }
	for (auto *v : { &depth, &depth_saved }) { v->clear(); v->shrink_to_fit(); }
	for (auto *v : { &stencil, &stencil_saved }) { v->clear(); v->shrink_to_fit(); }
}

// The viewport's rectangle on the output
static void vp_rect(const Viewport &vp, int w, int gx, float xscale, float scale, int &x0, int &y0, int &x1, int &y1)
{
	if (vp.full_screen) {
		x0 = 0;
		x1 = w;
	} else {
		x0 = gx + int(vp.vp_x * xscale);
		x1 = x0 + int(vp.vp_w * xscale);
	}
	y0 = int(vp.vp_y * scale);
	y1 = y0 + int(vp.vp_h * scale);
}

void Raster::render(const Scene &scene, const uint16_t *tex_ram)
{
	if (base.size() != size_t(w) * h) alloc();   // (let go while the GPU drew)
	tex = tex_ram;
	prepare(scene);
	parallel_rows(h, [&](int y0, int y1) { draw_band(scene, y0, y1); }, 2);
}

// Every polygon to draw, transformed, clipped by the near plane and placed on the output, in the order the passes
// draw them (priority, then the opaque layer, then the two translucent ones; high priority polygons after)
void Raster::prepare(const Scene &scene)
{
	draws.clear();
	tris.clear();
	for (auto &b : batches) b = {};
	std::vector<int> items;
	for (int pri = 0; pri < 4; pri++) {
		Priority &P = prio[pri];
		P.any = false;
		P.has_overlay = false;
		for (auto &v : scene.viewports)
			if (v.priority == pri) {
				if (!v.models.empty()) P.any = true;
				for (auto &m : v.models)
					for (auto &mesh : *m.meshes)
						if (mesh.high_priority) P.has_overlay = true;
			}
		for (int pass = 0; pass < 2; pass++)
			for (int layer = 0; layer < 3; layer++) {
				Batch &B = batches[(pri * 2 + pass) * 3 + layer];
				B.begin = tris.size();
				items.clear();
				if (P.any && (pass == 0 || P.has_overlay))
					for (auto &v : scene.viewports)
						if (v.priority == pri) gather(v, layer, pass == 1, items);
				// the meshes' polygons on every core, in parts kept in order
				const int parts = int(std::min<size_t>(items.size(), 64));
				if (parts_out.size() < size_t(parts)) parts_out.resize(size_t(parts));
				parallel_tasks(parts, [&](int k) {
					std::vector<Tri> &out = parts_out[size_t(k)];
					out.clear();
					const size_t a = items.size() * size_t(k) / size_t(parts), e = items.size() * size_t(k + 1) / size_t(parts);
					for (size_t i = a; i < e; i++) prepare_mesh(items[i], out);
				});
				for (int k = 0; k < parts; k++) tris.insert(tris.end(), parts_out[size_t(k)].begin(), parts_out[size_t(k)].end());
				B.end = tris.size();
			}
	}
}

// The meshes of a viewport drawn in a layer and pass, as draws (in order)
void Raster::gather(const Viewport &vp, int layer, bool overlay, std::vector<int> &items)
{
	int x0, y0, x1, y1;
	vp_rect(vp, w, gx, xscale, scale, x0, y0, x1, y1);
	const float sx0 = float(x0), sy0 = float(y0), sw = float(x1 - x0), sh = float(y1 - y0);
	const float corr = vp.full_screen ? float(w) / gw : 1.0f;
	x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w, x1); y1 = std::min(h, y1);
	if (x0 >= x1 || y0 >= y1)
		return;
	for (auto &m : vp.models)
		for (auto &mesh : *m.meshes) {
			if (!mesh.in_layer(layer, m.alpha) || mesh.high_priority != overlay)
				continue;
			items.push_back(int(draws.size()));
			draws.push_back({ &vp, &m, &mesh, x0, y0, x1, y1, sx0, sy0, sw, sh, vp.left * corr, vp.right * corr, vp.bottom, vp.top });
		}
}

bool Raster::arcade_lod = false;

void Raster::prepare_mesh(int di, std::vector<Tri> &result) const
{
	const Draw &d = draws[size_t(di)];
	const Viewport &vp = *d.vp;
	const Model &m = *d.model;
	const Mesh &mesh = *d.mesh;
	const int x0 = d.x0, y0 = d.y0, x1 = d.x1, y1 = d.y1;
	const float sx0 = d.vx, sy0 = d.vy, sw = d.vw, sh = d.vh, l = d.l, r = d.r, b = d.b, t = d.t;
	const Mat4 &M = m.matrix;
	// (the LOD base scaled by the lines drawn to the arcade's: each doubling a mipmap level finer, see arcade_lod)
	const float lod_k = arcade_lod ? 1.0f : std::max(1.0f, float(h) / 384.0f);   // (the arcade's 384 lines)
	float nmat[9];
	for (int c = 0; c < 3; c++) {
		for (int rr = 0; rr < 3; rr++) nmat[c * 3 + rr] = M.m[c * 4 + rr] / m.scale;
	}
		for (size_t i = 0; i + 2 < mesh.verts.size(); i += 3) {
			float v[3][11];
			for (int k = 0; k < 3; k++) {
				const Vertex &s = mesh.verts[i + k];
				float *o = v[k];
				for (int q = 0; q < 3; q++)
					o[q] = M.m[q] * s.pos[0] + M.m[4 + q] * s.pos[1] + M.m[8 + q] * s.pos[2] + M.m[12 + q];
				for (int q = 0; q < 3; q++)
					o[3 + q] = nmat[q] * s.normal[0] + nmat[3 + q] * s.normal[1] + nmat[6 + q] * s.normal[2];
				o[6] = s.uv[0];
				o[7] = s.uv[1];
				o[8] = s.fixed_shade;
				float fn[3];
				for (int q = 0; q < 3; q++)
					fn[q] = M.m[q] * s.face_normal[0] + M.m[4 + q] * s.face_normal[1] + M.m[8 + q] * s.face_normal[2];
				o[9] = o[0] * fn[0] + o[1] * fn[1] + o[2] * fn[2];
				o[10] = o[9] * -vp.cota * s.texture_np * lod_k;
			}
			if (v[0][9] > 0 && v[1][9] > 0 && v[2][9] > 0)
				continue;   // facing away
			// clipped by the near plane (view Z <= -NEAR) and the view's sides, with a guard band of half the view
			// each way (the sides only so that the positions stay small enough to rasterise exactly; the
			// viewport's rectangle cuts the pixels)
			float bufa[8][11], bufb[8][11];
			for (int k = 0; k < 3; k++) memcpy(bufa[k], v[k], sizeof(float) * 11);
			int n = 3;
			float (*in)[11] = bufa, (*out)[11] = bufb;
			const float gl = l - 0.5f * (r - l), gr = r + 0.5f * (r - l), gb = b - 0.5f * (t - b), gt = t + 0.5f * (t - b);
			auto plane = [&](int pl, const float *q) {
				switch (pl) {
				case 0: return -q[2] - NEAR;
				case 1: return q[0] + gl * q[2];    // x >= gl * w (w = -z)
				case 2: return -gr * q[2] - q[0];   // x <= gr * w
				case 3: return q[1] + gb * q[2];
				default: return -gt * q[2] - q[1];
				}
			};
			for (int pl = 0; pl < 5 && n >= 3; pl++) {
				int m = 0;
				for (int k = 0; k < n; k++) {
					const float *pa = in[k], *pb = in[(k + 1) % n];
					const float da = plane(pl, pa), db = plane(pl, pb);
					if (da >= 0) memcpy(out[m++], pa, sizeof(float) * 11);
					if ((da >= 0) != (db >= 0) && m < 8) {
						const float tt = da / (da - db);
						for (int q = 0; q < 11; q++) out[m][q] = pa[q] + (pb[q] - pa[q]) * tt;
						m++;
					}
					if (m >= 8) break;
				}
				n = m;
				std::swap(in, out);
			}
			if (n < 3)
				continue;
			float (*poly)[11] = in;
			Tri T;
			T.draw = di;
			T.n = n;
			float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
			for (int k = 0; k < n; k++) {
				float iw = 1.0f / -poly[k][2];
				float nx = (2.0f * (poly[k][0] * iw) - (r + l)) / (r - l);
				float ny = (2.0f * (poly[k][1] * iw) - (t + b)) / (t - b);
				T.sx[k] = sx0 + (nx + 1.0f) * 0.5f * sw;
				T.sy[k] = sy0 + (1.0f - ny) * 0.5f * sh;
				T.iw[k] = iw;
				for (int q = 0; q < 11; q++) T.a[k][q] = poly[k][q] * iw;
				minx = std::min(minx, T.sx[k]); maxx = std::max(maxx, T.sx[k]);
				miny = std::min(miny, T.sy[k]); maxy = std::max(maxy, T.sy[k]);
			}
			if (!(maxx >= x0 && minx < x1 && maxy >= y0 && miny < y1))
				continue;   // off the viewport (or not a number)
			result.push_back(T);
		}
}

void Raster::draw_band(const Scene &scene, int y0, int y1)
{
	const size_t a = size_t(y0) * w, e = size_t(y1) * w;
	const Px zero = { 0, 0, 0, 0 };
	std::fill(base.begin() + a, base.begin() + e, zero);
	std::fill(t1.begin() + a, t1.begin() + e, zero);
	std::fill(t2.begin() + a, t2.begin() + e, zero);
	if (scene.blank) {
		std::fill(base.begin() + a, base.begin() + e, Px{ 1, 1, 1, 1 });
		return;
	}
	fog_layers(scene, y0, y1);
	for (int pri = 0; pri < 4; pri++) {
		if (!prio[pri].any)
			continue;
		for (int pass = 0; pass < 2; pass++) {
			std::fill(depth.begin() + a, depth.begin() + e, 0.0f);
			std::fill(stencil.begin() + a, stencil.begin() + e, 0);
			const Batch *B = &batches[(pri * 2 + pass) * 3];
			for (size_t i = B[0].begin; i < B[0].end; i++) draw_tri(tris[i], 0, y0, y1);
			std::copy(depth.begin() + a, depth.begin() + e, depth_saved.begin() + a);
			std::copy(stencil.begin() + a, stencil.begin() + e, stencil_saved.begin() + a);
			for (size_t i = B[1].begin; i < B[1].end; i++) draw_tri(tris[i], 1, y0, y1);
			std::copy(depth_saved.begin() + a, depth_saved.begin() + e, depth.begin() + a);
			std::copy(stencil_saved.begin() + a, stencil_saved.begin() + e, stencil.begin() + a);
			for (size_t i = B[2].begin; i < B[2].end; i++) draw_tri(tris[i], 2, y0, y1);
			if (!prio[pri].has_overlay)
				break;
		}
	}
}

void Raster::fog_layers(const Scene &scene, int band0, int band1)
{
	// The ambient fog: black over the viewport of the lowest priority with fog, by 1 - its fog ambient
	{
		float amb = 1.0f;
		const Viewport *pick = nullptr;
		for (int i = 0; i < 4; i++) {
			bool has = false;
			for (auto &v : scene.viewports) {
				if (v.priority != i) continue;
				has = true;
				if (v.scroll_fog > 0) continue;
				if (v.fog_density <= 0 && v.fog_start <= 0) continue;
				if (v.fog_ambient < amb) { pick = &v; amb = v.fog_ambient; }
			}
			if (pick || has) break;
		}
		if (pick) {
			int x0, y0, x1, y1;
			vp_rect(*pick, w, gx, xscale, scale, x0, y0, x1, y1);
			for (int y = std::max(band0, y0); y < std::min(band1, y1); y++)
				for (int x = std::max(0, x0); x < std::min(w, x1); x++)
					base[size_t(y) * w + x] = { 0, 0, 0, 1.0f - amb };
		}
	}
	// The scroll fog: the fog colour over the viewport, by the scroll fog amount
	const Viewport *pick = nullptr;
	for (int i = 0; i < 4 && !pick; i++)
		for (auto &v : scene.viewports)
			if (v.priority == i && v.scroll_fog > 0 && (!pick || pick->scroll_fog < v.scroll_fog))
				pick = &v;
	if (!pick)
		return;
	for (auto &n : scene.viewports) {
		if (memcmp(n.fog_colour, pick->fog_colour, sizeof(n.fog_colour)))
			continue;
		bool spot_on_fog = n.spot_fog_colour[0] > 0 || n.spot_fog_colour[1] > 0 || n.spot_fog_colour[2] > 0;
		bool fog = n.fog_density > 0 || n.fog_start > 0 || n.scroll_att > 0;
		bool spot_fog = n.fog_attenuation > 0 && spot_on_fog;
		if (!fog && !spot_fog)
			continue;
		float att = (fog && spot_fog) ? 0.0f : n.fog_attenuation;
		int x0, y0, x1, y1;
		vp_rect(*pick, w, gx, xscale, scale, x0, y0, x1, y1);
		for (int y = std::max(band0, y0); y < std::min(band1, y1); y++)
			for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
				float px = (x - gx + 0.5f) / xscale, py = (y + 0.5f) / scale;
				float ex = (px - n.spot_ellipse[0]) / n.spot_ellipse[2], ey = (py - n.spot_ellipse[1]) / n.spot_ellipse[3];
				float el = std::max(0.0f, 1.0f - (ex * ex + ey * ey));
				Px &o = base[size_t(y) * w + x];
				for (int k = 0; k < 3; k++)
					(&o.r)[k] = pick->fog_colour[k] * n.fog_ambient + att * n.spot_fog_colour[k] * el * pick->fog_colour[k];
				o.a = pick->scroll_fog;
			}
		break;
	}
}

namespace {

struct Sampler
{
	const uint16_t *tex;
	const Mesh *mesh;
	int page;
	int bx, by;   // the texture's place (the mesh's; a flipbook's step before: flip_x, flip_y)
	C4 fetch(int level, int x, int y) const
	{
		x &= 2047 >> level;
		y &= 1023 >> level;
		return decode(mesh->format, tex[size_t(page * 1024 + mip_y[level] + y) * 2048 + mip_x[level] + x]);
	}
	C4 bilinear(int level, float u, float v) const
	{
		const float sw = float(mesh->width >> level), sh = float(mesh->height >> level);
		const int px = bx >> level, py = by >> level;
		float u0, u1, v0, v1;
		float a = tex_locations(mesh->wrap_u, sw, u, u0, u1);
		float b = tex_locations(mesh->wrap_v, sh, v, v0, v1);
		int x0 = int(u0 * sw + px), x1 = int(u1 * sw + px), y0 = int(v0 * sh + py), y1 = int(v1 * sh + py);
		C4 p00 = fetch(level, x0, y0), p10 = fetch(level, x1, y0), p01 = fetch(level, x0, y1), p11 = fetch(level, x1, y1);
		if (mesh->alpha_test) {
			// contour: the colour of the more opaque neighbour spreads into the transparent texels
			auto rgb = [](C4 &d, const C4 &s) { d.r = s.r; d.g = s.g; d.b = s.b; };
			if (p00.a > p10.a) rgb(p10, p00);
			if (p00.a > p01.a) rgb(p01, p00);
			if (p10.a > p00.a) rgb(p00, p10);
			if (p10.a > p11.a) rgb(p11, p10);
			if (p01.a > p00.a) rgb(p00, p01);
			if (p01.a > p11.a) rgb(p11, p01);
			if (p11.a > p01.a) rgb(p01, p11);
			if (p11.a > p10.a) rgb(p10, p11);
		}
		auto mix = [](const C4 &x, const C4 &y, float t) {
			return C4{ x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t };
		};
		return mix(mix(p00, p10, a), mix(p01, p11, a), b);
	}
};

} // namespace

void Raster::draw_tri(const Tri &T, int layer, int band0, int band1)
{
	const Draw &d = draws[T.draw];
	const Mesh &mesh = *d.mesh;
	const Viewport &vp = *d.vp;
	const Model &model = *d.model;
	const int cy0 = std::max(d.y0, band0), cy1 = std::min(d.y1, band1);
	if (cy0 >= cy1)
		return;
	// the polygon's colour (the same at every vertex) and the state shared by its pixels
	const Vertex &pv = mesh.verts[0];
	C4 poly = { pv.colour[0] / 255.0f, pv.colour[1] / 255.0f, pv.colour[2] / 255.0f, pv.colour[3] / 255.0f * model.alpha };
	if (mesh.translator_map) { poly.r *= 16; poly.g *= 16; poly.b *= 16; }
	const Sampler smp = { tex, &mesh, mesh.page ^ model.page, mesh.x, mesh.y };
	const Sampler smp_flip = { tex, &mesh, mesh.page ^ model.page, mesh.flip_x, mesh.flip_y };
	const float num_levels = mesh.textured ? std::floor(std::log2(float(std::min(mesh.width, mesh.height)))) - 1.0f : 0.0f;
	static const float exp_index[4] = { 8, 16, 32, 64 }, mult_index[4] = { 1.6f, 1.6f, 2.4f, 3.2f };
	const int shin = std::clamp(int(mesh.shininess), 0, 3);

	for (int f = 1; f + 1 < T.n; f++) {
		const int idx[3] = { 0, f, f + 1 };
		const float x0 = T.sx[0], y0 = T.sy[0], x1 = T.sx[f], y1 = T.sy[f], x2 = T.sx[f + 1], y2 = T.sy[f + 1];
		const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
		if (area == 0 || !std::isfinite(area))
			continue;
		const int bx0 = std::max(d.x0, int(std::floor(std::min({ x0, x1, x2 }))));
		const int bx1 = std::min(d.x1 - 1, int(std::ceil(std::max({ x0, x1, x2 }))));
		const int by0 = std::max(cy0, int(std::floor(std::min({ y0, y1, y2 }))));
		const int by1 = std::min(cy1 - 1, int(std::ceil(std::max({ y0, y1, y2 }))));
		if (bx0 > bx1 || by0 > by1)
			continue;
		const float inv_area = 1.0f / area;
		const float *aw[3] = { T.a[idx[0]], T.a[idx[1]], T.a[idx[2]] };
		const float w0i = T.iw[idx[0]], w1i = T.iw[idx[1]], w2i = T.iw[idx[2]];
		// edge functions, stepped along the row
		const float A0 = -(y2 - y1), B0 = (x2 - x1), A1 = -(y0 - y2), B1 = (x0 - x2), A2 = -(y1 - y0), B2 = (x1 - x0);
		const bool ccw = area > 0;
		for (int y = by0; y <= by1; y++) {
			const float py = y + 0.5f;
			const float pxs = bx0 + 0.5f;
			float e0 = (pxs - x1) * A0 + (py - y1) * B0;
			float e1 = (pxs - x2) * A1 + (py - y2) * B1;
			float e2 = (pxs - x0) * A2 + (py - y0) * B2;
			for (int x = bx0; x <= bx1; x++, e0 += A0, e1 += A1, e2 += A2) {
				if (ccw ? (e0 < 0 || e1 < 0 || e2 < 0) : (e0 > 0 || e1 > 0 || e2 > 0))
					continue;
				const float px = x + 0.5f;
				const float b0 = e0 * inv_area, b1 = e1 * inv_area, b2 = e2 * inv_area;
				const float wi = b0 * w0i + b1 * w1i + b2 * w2i;
				if (!(wi > 0))
					continue;
				const float wv = 1.0f / wi;
				float a[11];
				for (int q = 0; q < 11; q++) a[q] = (b0 * aw[0][q] + b1 * aw[1][q] + b2 * aw[2][q]) * wv;
				if (a[9] > 0)
					continue;   // facing away
				const size_t at = size_t(y) * w + x;
				if (mesh.layered && (stencil[at] & 0x7F))
					continue;
				// depth: 1/w, nearer is greater; the opaque layer passes equal, the translucent ones only nearer
				if (layer == 0 ? wi < depth[at] : wi <= depth[at]) {
					if (mesh.layered) stencil[at]++;
					continue;
				}
				// ---- texture ----
				C4 t = { 1, 1, 1, 1 };
				if (mesh.textured) {
					float lod = -std::log2(wi * wi * a[10]);
					float fl = std::clamp(lod, 0.0f, num_levels);
					if (!(fl == fl)) fl = 0;
					int il = int(fl);
					float fr = fl - il;
					auto sample = [&](const Sampler &sm) {
						C4 t1 = sm.bilinear(il, a[6], a[7]);
						if (fr > 0) {
							C4 t2 = sm.bilinear(il + 1, a[6], a[7]);
							t1 = { t1.r + (t2.r - t1.r) * fr, t1.g + (t2.g - t1.g) * fr, t1.b + (t2.b - t1.b) * fr, t1.a + (t2.a - t1.a) * fr };
						}
						return t1;
					};
					t = sample(smp);
					if (mesh.flip > 0) {   // a flipbook's step faded in: the step before's picture mixed in
						const C4 o = sample(smp_flip);
						const float k = mesh.flip;
						t = { t.r + (o.r - t.r) * k, t.g + (o.g - t.g) * k, t.b + (o.b - t.b) * k, t.a + (o.a - t.a) * k };
					}
					if (mesh.inverted) { t.r = 1 - t.r; t.g = 1 - t.g; t.b = 1 - t.b; }
					if (mesh.alpha_test && t.a < 32.0f / 255.0f)
						continue;
					if (mesh.texture_alpha) {
						if (layer == 0 ? t.a < 1.0f : t.a * poly.a >= 1.0f)
							continue;
					} else if (!mesh.alpha_test) {
						t.a = 1;
					}
				}
				C4 c = { t.r * poly.r, t.g * poly.g, t.b * poly.b, t.a * poly.a };
				if (c.a < 1.0f / 32.0f)
					continue;
				// ---- spotlight ----
				float lobe = 0, lobe_fog = 0;
				{
					float gxp = (px - gx) / xscale, gyp = py / scale;
					float ex = (gxp - vp.spot_ellipse[0]) / vp.spot_ellipse[2], ey = (gyp - vp.spot_ellipse[1]) / vp.spot_ellipse[3];
					float ell = std::max(0.0f, 1.0f - (ex * ex + ey * ey));
					// (the spotlight's distances in Step 1.0's view space / 2048, as the reference's: VF3's lava cave is
					// lit red across the arena, 9,000 units from the camera, by a spotlight 2,304 deep)
					const float sz = a[2] * (1.0f / 2048.0f);
					float enable = -sz >= vp.spot_range[0] ? 1.0f : 0.0f;
					float range = 0;
					if (vp.spot_range[1] != 0) {
						float ext = std::fabs(vp.spot_range[1]);
						float dd = std::min(vp.spot_range[0] + ext + sz, 0.0f);
						float q = dd / (1.0f + ext) - 1.0f;
						range = enable / (q * q);
					}
					lobe = range * ell;
					lobe_fog = enable * ell;
				}
				// ---- lighting ----
				if (mesh.lighting) {
					float sun;
					if (mesh.fixed_shading) sun = a[8];
					else sun = vp.sun[0] * a[3] + vp.sun[1] * a[4] + vp.sun[2] * a[5];
					sun = std::clamp(sun, -1.0f, 1.0f);
					if (sun_clamp || c.a < 0.999f) sun = std::max(sun, 0.0f);
					// the sun and ambient light up to full, the spotlight's colour on top
					float li = std::min(sun * vp.sun_intensity + vp.ambient, 1.0f);
					float lr = li + vp.spot_colour[0] * lobe;
					float lg = li + vp.spot_colour[1] * lobe;
					float lb = li + vp.spot_colour[2] * lobe;
					c.r *= lr; c.g *= lg; c.b *= lb;
					if (mesh.specular) {
						float s;
						if (mesh.smooth_shading) {
							s = std::pow(std::max(0.0f, sun), exp_index[shin]) * mult_index[shin];
						} else {
							float nl = vp.sun[0] * a[3] + vp.sun[1] * a[4] + vp.sun[2] * a[5];
							float rz = -vp.sun[2] + 2.0f * nl * a[5];
							s = std::max(0.0f, rz);
						}
						s *= mesh.specular_value * vp.sun_intensity;
						if (poly.a < 1.0f) c.a = std::max(c.a, s);
						c.r += s; c.g += s; c.b += s;
					}
				}
				c.r = std::min(c.r, 1.0f); c.g = std::min(c.g, 1.0f); c.b = std::min(c.b, 1.0f);
				// ---- fog ----
				float fog = mesh.fog_intensity * std::clamp(vp.fog_start + -a[2] * vp.fog_density, 0.0f, 1.0f);
				if (fog > 0) {
					for (int k = 0; k < 3; k++) {
						float target = vp.fog_colour[k] * vp.fog_ambient + vp.spot_fog_colour[k] * vp.fog_attenuation * vp.fog_colour[k] * lobe_fog;
						float &ch = (&c.r)[k];
						ch += (target - ch) * fog;
					}
				}
				depth[at] = wi;
				if (mesh.layered) stencil[at]++;
				if (layer == 0) {
					base[at] = { c.r, c.g, c.b, 1.0f };
					t1[at] = t2[at] = { 0, 0, 0, 0 };
				} else if (layer == 1) {
					t1[at] = { c.r, c.g, c.b, std::min(c.a, 1.0f) };
				} else {
					t2[at] = { c.r, c.g, c.b, std::min(c.a, 1.0f) };
				}
			}
		}
	}
}

void Raster::compose(const uint32_t *under, uint32_t *out) const
{
	for (size_t i = 0, n = size_t(w) * h; i < n; i++) {
		uint32_t u = under[i];
		float r = (u & 0xFF) / 255.0f, g = ((u >> 8) & 0xFF) / 255.0f, b = ((u >> 16) & 0xFF) / 255.0f;
		const Px &p = base[i];
		if (p.a > 0) {
			r += (p.r - r) * p.a; g += (p.g - g) * p.a; b += (p.b - b) * p.a;
		}
		// the two translucent layers: where both have a pixel, mixed and opaque
		const Px &x = t1[i], &y = t2[i];
		Px tr;
		if (x.a * y.a > 0) {
			float k = (y.a + (1.0f - x.a)) / 2.0f;
			tr = { x.r + (y.r - x.r) * k, x.g + (y.g - x.g) * k, x.b + (y.b - x.b) * k, 1.0f };
		} else {
			tr = x.a > 0 ? x : y;
		}
		if (tr.a > 0) {
			r += (tr.r - r) * tr.a; g += (tr.g - g) * tr.a; b += (tr.b - b) * tr.a;
		}
		auto q = [](float c) { return uint32_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); };
		out[i] = 0xFF000000u | q(b) << 16 | q(g) << 8 | q(r);
	}
}

void Raster::to_premultiplied(uint8_t *dst, size_t pitch, int k) const
{
	k = std::max(k, 1);
	const float inv = 1.0f / float(k * k);
	// one drawn pixel's three layers put together (premultiplied)
	auto one = [&](size_t at, float o[4]) {
		const Px &p = base[at], &q = t1[at], &s = t2[at];
		float r = p.r * p.a, g = p.g * p.a, b = p.b * p.a, al = p.a;
		Px tr;
		if (q.a * s.a > 0) {
			float m = (s.a + (1.0f - q.a)) / 2.0f;
			tr = { q.r + (s.r - q.r) * m, q.g + (s.g - q.g) * m, q.b + (s.b - q.b) * m, 1.0f };
		} else {
			tr = q.a > 0 ? q : s;
		}
		if (tr.a > 0) {
			r = tr.r * tr.a + r * (1 - tr.a); g = tr.g * tr.a + g * (1 - tr.a); b = tr.b * tr.a + b * (1 - tr.a);
			al = tr.a + al * (1 - tr.a);
		}
		o[0] += std::clamp(r, 0.0f, 1.0f); o[1] += std::clamp(g, 0.0f, 1.0f); o[2] += std::clamp(b, 0.0f, 1.0f); o[3] += std::clamp(al, 0.0f, 1.0f);
	};
	parallel_rows(h / k, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			uint32_t *o = reinterpret_cast<uint32_t *>(dst + size_t(y) * pitch);
			for (int x = 0; x < w / k; x++) {
				float sum[4] = {};
				for (int j = 0; j < k; j++)
					for (int i = 0; i < k; i++) one(size_t(y * k + j) * w + size_t(x * k + i), sum);
				auto qz = [&](float c) { return uint32_t(std::clamp(c * inv, 0.0f, 1.0f) * 255.0f + 0.5f); };
				o[x] = qz(sum[3]) << 24 | qz(sum[2]) << 16 | qz(sum[1]) << 8 | qz(sum[0]);
			}
		}
	}, 2);
}

} // namespace video
