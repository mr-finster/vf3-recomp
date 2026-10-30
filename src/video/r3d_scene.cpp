// The Real3D's scene. See r3d_scene.h.
//
// Addresses are in words. Culling RAM: below 0x100000 the low culling RAM, 0x800000-0x83FFFF the high. Models:
// below 0x100000 polygon RAM, else VROM.
//
// Viewport node (Step 1.0), words:
//   0      bit 5 disabled, bits 3-4 priority (0-3)
//   1      the next viewport (0x01000000: none)
//   2      the first culling node
//   3      cota (float; the texture LOD's scale)
//   4-7    sun Z, X, Y (floats, in the Real3D's axes) and intensity
//   8-0xB  cv = 1/(right-left), cw = 1/(top-bottom), io, jo (floats: the frustum)
//   0xC-13 the four culling planes' factors (left u, v; top u, w; right u, v; bottom u, w)
//   0x14   height (high 16) and width (low 16), 14.2
//   0x16   the matrix table (a culling RAM address); 0x17 the LOD table
//   0x1A   Y (high 16) and X (low 16) of the top left, 12.4
//   0x1C   the line-of-sight test's position
//   0x1D   spotlight Y size (high 16) and Y centre (low 16, 13.3); 0x1E the same for X
//   0x1F   spotlight extent; 0x21 1/its start (floats)
//   0x20   bits 11-13 spotlight colour, 8-10 its colour on fog (RGB, a bit each), 0-7 scroll fog
//   0x22   fog colour (R 16-23, G 8-15, B 0-7); 0x23 fog density (float)
//   0x24   fog attenuation 16-23, ambient 8-15, scroll fog attenuation 0-7
//   0x25   fog ambient 16-23, fog start 0-15 (signed, /255)
//
// Culling node (Step 1.0: 8 words; the later steps' 10, less words 1 and 2):
//   0      bits 0-1 type (0 viewport), bit 2 has a colour table address, bit 3 the child is a 4-entry LOD
//          table, bit 4 a translation (words 2-4) in place of a matrix, bit 7 reset the matrix's rotation, bits
//          8-9 both set: not drawn (one: half alpha, on the polygons); (bits 0-2 == 6: no sibling)
//   1      bits 0-11 the matrix (index in the matrix table), 12-18 the LOD table entry, 19-31 colour table bits,
//          bit 29 the LOD table's entries are culling nodes (else models)
//   2-4    translation (floats)
//   5      the child (bits 0-26; bits 28-31 colour table bits)
//   6      the sibling (bits 0-24, bit 24 none; bits 25-31 colour table bits)
//   7      blend radius (high 16) and culling radius (low 16), 16-bit floats
// A child pointer's bits 24-26 say what it is: 0 a culling node, 1 a model, 4 a list of culling nodes (entries
// with bit 24: empty, bit 25: the last).
//
// Matrices: 12 floats, translation first then the 3x3 by rows. The table's first is the base (view) matrix; the
// Real3D's axes are taken to GL's by a fixed swap (x' = y, y' = -z, z' = -x).
//
// Polygon (7 words of header, then 4 words for each vertex it doesn't share with the previous polygon):
//   0      bits 0-3 vertices shared (vertex i of the previous polygon), bit 6 a quad, bit 7 specular, bits 8-9
//          discard (both: not drawn; bit 9 alone halves alpha), bits 26-31 specular amount
//   1-3    the face normal (high 24 bits each, 2.22); word 1 also: bit 1 colour in word 4 (else a colour table
//          index), bit 2 the last polygon, bit 3 smooth shading (vertex normals), bit 4 double-sided, bit 5 fixed
//          shading (the shade in the vertex's low X byte), bit 6 texture coordinates 1x (else 1/8)
//   2      bits 0-1 mirror V, U; bits 2-3 microtexture min LOD, 4 microtexture, 5-7 its number
//   3      bits 0-2 texture height, 3-5 width (32 << n), 6-7 smooth V, U
//   4      RGB (24-31, 16-23, 8-15) or colour index (8-19); bit 6 the texture sheet, bit 7 translator map, bits
//          0-4 with word 5's bit 7 the texture's X (in 32s)
//   5      bits 0-4 the texture's Y (in 32s), 8-31 texture LOD base (12.12)
//   6      bits 0-2 texture alpha, 3 layered, 4 high priority, 5-6 shininess, 7-9 texture format, 10 textured,
//          11-15 fog intensity (/16), 16 no lighting, 17 the second translucent layer, 18-22 translucency
//          (x/32), 23 opaque, 24-30 translator map offset (2: inverted texture), 31 contour (alpha test)
// Vertex: X, Y, Z (high 24 bits each, 17.7; the low bytes the normal, or the fixed shade), then U (high 16) and
// V (low 16).
#include "r3d_scene.h"
#include <atomic>
#include "../machine/model3.h"
#include <algorithm>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace video {

Mat4 Mat4::identity()
{
	Mat4 r = {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
	return r;
}

Mat4 Mat4::operator*(const Mat4 &b) const
{
	Mat4 r;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r.m[j * 4 + i] = m[i] * b.m[j * 4] + m[4 + i] * b.m[j * 4 + 1] + m[8 + i] * b.m[j * 4 + 2] + m[12 + i] * b.m[j * 4 + 3];
	return r;
}

namespace {

inline float as_float(uint32_t v)
{
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// The Real3D's 16-bit float (the radii)
float float16(uint16_t v)
{
	uint32_t a = uint32_t(v) << 15;
	uint32_t e = (a & 0x7E000000) >> 25;
	e += e <= 31 ? 127 : 127 - 64;
	return as_float((a & 0x80000000) | (e << 23) | ((a & 0x1FFFFFF) >> 2));
}

inline float byte_to_float(int8_t b) { return (2.0f * b + 1.0f) * (1.0f / 255.0f); }

const float spot_rgb[8][3] = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 0 }, { 1, 1, 1 } };

const int shared_count[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };

} // namespace

const uint32_t *SceneBuilder::culling(uint32_t addr) const
{
	addr &= 0xFFFFFF;
	if (addr >= 0x800000 && addr < 0x840000) return cull_hi + (addr & 0x3FFFF);
	if (addr < 0x100000) return cull_lo + addr;
	return nullptr;
}

const uint32_t *SceneBuilder::model_at(uint32_t addr) const
{
	addr &= 0xFFFFFF;
	return addr < 0x100000 ? poly + addr : vrom + addr;
}

void SceneBuilder::build(const m3::Model3 &board, Scene &scene)
{
	cull_lo = board.cull_lo.data();
	cull_hi = board.cull_hi.data();
	poly = board.poly_ram.data();
	vrom = reinterpret_cast<const uint32_t *>(board.vrom.data());
	scene.viewports.clear();
	scene.blank = false;
	static std::atomic<uint64_t> next_id{ 1 };
	scene.id = next_id++;
	visits = 0;
	if (rom_cache.size() > 20000)
		rom_cache.clear();
	if (dyn_cache.size() > 20000)
		dyn_cache.clear();
	viewport(0x800000, scene, 0);
}

void SceneBuilder::mult_matrix(uint32_t index)
{
	if (!matrix_base)
		return;
	const float *s = matrix_base + index * 12;
	Mat4 m = {};
	m.m[0] = s[3]; m.m[4] = s[4]; m.m[8] = s[5]; m.m[12] = s[0];
	m.m[1] = s[6]; m.m[5] = s[7]; m.m[9] = s[8]; m.m[13] = s[1];
	m.m[2] = s[9]; m.m[6] = s[10]; m.m[10] = s[11]; m.m[14] = s[2];
	m.m[15] = 1;
	cur = cur * m;
}

void SceneBuilder::viewport(uint32_t addr, Scene &scene, int guard)
{
	for (; guard < 64; guard++) {
		if (!(addr & 0xFFFFFF))
			return;
		const uint32_t *v = culling(addr);
		if (!v)
			return;
		scene.viewports.emplace_back();
		Viewport &p = scene.viewports.back();
		vp = &p;
		p.priority = (v[0] >> 3) & 3;
		p.vp_x = (v[0x1A] & 0xFFFF) / 16.0f;
		p.vp_y = (v[0x1A] >> 16) / 16.0f;
		p.vp_w = (v[0x14] & 0xFFFF) / 4.0f;
		p.vp_h = (v[0x14] >> 16) / 4.0f;
		p.full_screen = p.vp_x == 0 && p.vp_w >= 495 && p.vp_y == 0 && p.vp_h >= 383;
		matrix_base = reinterpret_cast<const float *>(culling(v[0x16] & 0xFFFFFF));
		lod_table = reinterpret_cast<const float *>(culling(v[0x17] & 0xFFFFFF));
		float cv = as_float(v[8]), cw = as_float(v[9]), io = as_float(v[10]), jo = as_float(v[11]);
		planes = { as_float(v[0xC]), as_float(v[0xD]), as_float(v[0xE]), as_float(v[0xF]),
			as_float(v[0x10]), as_float(v[0x11]), as_float(v[0x12]), as_float(v[0x13]) };
		if (p.full_screen && widen > 1) {
			// x / widen in the side planes' tests: their edges as far out as the picture goes (the planes' normals
			// shortened with it: a sphere's distance read short, never culled sooner)
			planes.lv /= widen;
			planes.rv /= widen;
		}
		p.left = (0.0f - jo) / cv;
		p.right = (1.0f - jo) / cv;
		p.bottom = -(1.0f - io) / cw;
		p.top = -(0.0f - io) / cw;
		p.cota = as_float(v[3]);
		p.sun[0] = as_float(v[5]);
		p.sun[1] = -as_float(v[6]);
		p.sun[2] = -as_float(v[4]);
		p.sun_intensity = std::clamp(as_float(v[7]), 0.0f, 1.0f);
		if (std::isnan(p.sun_intensity)) p.sun_intensity = 0;
		p.ambient = ((v[0x24] >> 8) & 0xFF) / 255.0f;
		int spot = (v[0x20] >> 11) & 7, spot_fog = (v[0x20] >> 8) & 7;
		p.spot_ellipse[0] = int16_t(v[0x1E] & 0xFFFF) / 8.0f;
		p.spot_ellipse[1] = int16_t(v[0x1D] & 0xFFFF) / 8.0f;
		p.spot_ellipse[2] = std::round(2047.0f / std::max(1.0f, float((v[0x1E] >> 16) & 0xFFFF)));
		p.spot_ellipse[3] = std::round(2047.0f / std::max(1.0f, float((v[0x1D] >> 16) & 0xFFFF)));
		p.spot_range[0] = 1.0f / as_float(v[0x21]);
		p.spot_range[1] = as_float(v[0x1F]);
		for (int i = 0; i < 3; i++) {
			p.spot_colour[i] = spot_rgb[spot][i];
			p.spot_fog_colour[i] = spot_rgb[spot_fog][i];
		}
		p.fog_colour[0] = ((v[0x22] >> 16) & 0xFF) / 255.0f;
		p.fog_colour[1] = ((v[0x22] >> 8) & 0xFF) / 255.0f;
		p.fog_colour[2] = (v[0x22] & 0xFF) / 255.0f;
		p.fog_density = std::fabs(as_float(v[0x23]));
		if (!std::isfinite(p.fog_density)) p.fog_density = 0;
		p.fog_start = int16_t(v[0x25] & 0xFFFF) / 255.0f;
		p.fog_attenuation = ((v[0x24] >> 16) & 0xFF) / 255.0f;
		p.fog_ambient = ((v[0x25] >> 16) & 0xFF) / 255.0f;
		p.scroll_fog = (v[0x20] & 0xFF) / 255.0f;
		p.scroll_att = (v[0x24] & 0xFF) / 255.0f;

		attr = Attribs();
		depth = 0;
		stack.clear();
		// The Real3D's axes to GL's, then the base matrix
		cur = {};
		cur.m[4] = 1;      // x' = y
		cur.m[9] = -1;     // y' = -z
		cur.m[2] = -1;     // z' = -x
		cur.m[15] = 1;
		mult_matrix(0);
		path = 0;
		if (!(v[0] & 0x20) && ((v[2] >> 24) & 5) == 0)
			node_ptr(v[2]);
		if (v[1] == 0x01000000)
			return;
		addr = v[1];
	}
}

void SceneBuilder::node_ptr(uint32_t addr)
{
	if (!(addr & 0xFFFFFF))
		return;
	switch ((addr >> 24) & 5) {
	case 0: node(addr & 0xFFFFFF); break;
	case 1: draw_model(addr & 0xFFFFFF); break;
	case 4: pointer_list(addr & 0xFFFFFF); break;
	default: break;
	}
}

void SceneBuilder::pointer_list(uint32_t addr)
{
	const uint32_t *list = culling(addr);
	if (!list)
		return;
	for (int i = 0; i < 0x10000; i++) {
		if (list[i] & 0x01000000)
			break;
		node(list[i] & 0xFFFFFF);
		if (list[i] & 0x02000000)
			break;
	}
}

// A node and the siblings after it: the chain walked here, each drawn last first (as one recursion a sibling did:
// a node naming itself as its sibling, or a long chain, ran the stack out), at most MAX_CHAIN of them
void SceneBuilder::node(uint32_t addr)
{
	constexpr size_t MAX_CHAIN = 4096;
	const size_t base = chain.size();
	for (uint32_t a = addr; chain.size() - base < MAX_CHAIN;) {
		const uint32_t *n = culling(a);
		if (!n)
			break;
		chain.push_back(a);
		const uint32_t w0 = n[0], sibling = n[6] & 0x1FFFFFF;
		if ((w0 & 3) == 0 || (w0 & 0x300) == 0x300 || (w0 & 7) == 6 || (sibling & 0x1000000) || !sibling)
			break;
		a = sibling;
	}
	for (size_t i = chain.size(); i-- > base;) {
		const uint32_t a = chain[i];   // (a copy: node_one adds to the chain)
		node_one(a);
	}
	chain.resize(base);
}

void SceneBuilder::node_one(uint32_t addr)
{
	// (and at most NODE_BUDGET a scene: nodes that lead back to themselves, a cycle through children or a LOD's two
	// choices, doubled at each level up to the depth limit)
	if (depth >= 1024 || ++visits > NODE_BUDGET)
		return;
	const uint32_t *n = culling(addr);
	if (!n)
		return;
	const uint32_t w0 = n[0], w1 = n[1], w5 = n[5], w6 = n[6], w7 = n[7];
	if ((w0 & 3) == 0)
		return;                 // a viewport: not drawn from here
	if ((w0 & 0x300) == 0x300)
		return;
	const uint32_t child = w5 & 0x7FFFFFF;
	if (w0 & 4)
		colour_table = ((w1 >> 19) | ((w5 >> 28) << 13) | ((w6 >> 25) << 17)) & 0xFFFFF;

	Attribs saved = attr;
	Mat4 saved_m = cur;
	uint32_t saved_path = path;
	path = path * 31 + addr;
	depth++;
	if (w0 & 0x10) {
		Mat4 t = Mat4::identity();
		t.m[12] = as_float(n[2]);
		t.m[13] = as_float(n[3]);
		t.m[14] = as_float(n[4]);
		cur = cur * t;
	} else if (w1 & 0xFFF) {
		mult_matrix(w1 & 0xFFF);
		if (vp && vp->root_matrix < 0) { vp->root = cur; vp->root_matrix = int(w1 & 0xFFF); }
	}
	if (w0 & 0x80) {
		// the rotation taken out (the transposed 3x3, its columns normalised, multiplied in)
		Mat4 m = cur;
		std::swap(m.m[1], m.m[4]);
		std::swap(m.m[2], m.m[8]);
		std::swap(m.m[6], m.m[9]);
		m.m[12] = m.m[13] = m.m[14] = 0;
		m.m[15] = 1;
		for (int c = 0; c < 3; c++) {
			float s = std::sqrt(m.m[c * 4] * m.m[c * 4] + m.m[c * 4 + 1] * m.m[c * 4 + 1] + m.m[c * 4 + 2] * m.m[c * 4 + 2]);
			if (s != 0)
				for (int r = 0; r < 3; r++) m.m[c * 4 + r] /= s;
		}
		cur = cur * m;
	}
	const float x = cur.m[12], y = cur.m[13], z = cur.m[14];
	const float cull_r = float16(w7 & 0xFFFF) * attr.scale;
	const float blend_r = float16(w7 >> 16) * attr.scale;
	const bool outside = (z * planes.lu - x * planes.lv) > cull_r || (z * planes.tu + y * planes.tw) > cull_r ||
		(z * planes.ru - x * planes.rv) > cull_r || (z * planes.bu + y * planes.bw) > cull_r;
	float lod_scale = attr.no_cull ? std::numeric_limits<float>::max() : blend_r / std::sqrt(x * x + y * y + z * z);
	if (!(lod_scale >= 0)) lod_scale = 0;   // NaN too
	const int lod_entry = (w1 >> 12) & 0x7F;
	// the LOD table: 128 entries of 4 (delete size, blend factor)
	auto lod_del = [&](int i) { return lod_table ? lod_table[lod_entry * 8 + i * 2] : 0.0f; };
	auto lod_blend = [&](int i) { return lod_table ? lod_table[lod_entry * 8 + i * 2 + 1] : 0.0f; };
	if (attr.no_cull || (!outside && lod_scale >= lod_del(3))) {
		if (w0 & 8) {
			if (const uint32_t *lod = culling(child)) {
				int k;
				for (k = 0; k < 3; k++)
					if (lod_scale >= lod_del(k) && (lod[k] & 0x1000000))
						break;
				float before = attr.alpha;
				float a = std::clamp(lod_blend(k) * (lod_scale - lod_del(k)), 0.0f, 1.0f);
				if (!(a == a)) a = 0;
				if (a > 31.0f / 32.0f) a = 1;
				else if (a < 1.0f / 32.0f) a = 0;
				attr.alpha *= a;
				bool nodes = (w1 & 0x20000000) != 0;
				if (nodes) node(lod[k] & 0xFFFFFF);
				else draw_model(lod[k] & 0xFFFFFF);
				if (a < 1.0f && k != 3) {
					attr.alpha = (1.0f - a) * before;
					if (nodes) node(lod[k + 1] & 0xFFFFFF);
					else draw_model(lod[k + 1] & 0xFFFFFF);
				}
			}
		} else {
			float a = std::clamp(lod_blend(3) * (lod_scale - lod_del(3)), 0.0f, 1.0f);
			if (!(a == a)) a = 0;
			attr.alpha *= a;
			node_ptr(child);
		}
	}
	depth--;
	cur = saved_m;
	attr = saved;
	path = saved_path;
}

void SceneBuilder::draw_model(uint32_t addr)
{
	addr &= 0xFFFFFF;
	const uint32_t *data = model_at(addr);
	Model m;
	m.addr = addr;
	m.matrix = cur;
	m.alpha = attr.alpha;
	m.scale = attr.scale;
	m.page = attr.page;
	m.node_path = path;
	// ROM models without colour table indexes are the same every frame
	bool dynamic = addr < 0x100000;
	if (!dynamic) {
		const uint32_t *h = data;
		const uint32_t *end_rom = vrom + 0x1000000;
		for (int i = 0; i < 100000 && h + 7 <= end_rom; i++) {
			if (!(h[1] & 2)) { dynamic = true; break; }
			if (h[1] & 4) break;
			h += 7 + (((h[0] & 0x40) ? 4 : 3) - shared_count[h[0] & 0xF]) * 4;
		}
	}
	if (!dynamic) {
		auto it = rom_cache.find(addr);
		if (it != rom_cache.end()) {
			m.meshes = it->second;
		} else {
			m.meshes = parse(data, false);
			rom_cache[addr] = m.meshes;
		}
	} else if (!(data[0] & 0xF)) {
		// (a model whose first polygon shares no vertices with the model before: parsed again only when its words
		// (built in polygon RAM: three in four of them the same as the frame before) or an entry of the colour table it
		// reads have changed; about 80 a frame were parsed each frame)
		uint64_t hs = 1469598103934665603ull;
		if (addr < 0x100000) {
			const uint32_t *h = data, *end = poly + 0x100000;
			for (int i = 0; i < 100000; i++) {
				if (h + 7 > end || h[6] == 0) break;
				const size_t n = 7 + size_t(((h[0] & 0x40) ? 4 : 3) - shared_count[h[0] & 0xF]) * 4;
				for (size_t k = 0; k < n && h + k < end; k++) hs = (hs ^ h[k]) * 1099511628211ull;
				if (h[1] & 4) break;
				h += n;
			}
		}
		DynEntry &e = dyn_cache[addr];
		bool same = e.meshes && e.hash == hs && e.table == colour_table;
		for (size_t i = 0; same && i < e.refs.size(); i++) same = poly[e.refs[i].first] == e.refs[i].second;
		if (same) {
			m.meshes = e.meshes;
			std::copy(&e.prev[0], &e.prev[0] + 4, &prev[0]);
			memcpy(prev_uv, e.prev_uv, sizeof prev_uv);
		} else {
			e.refs.clear();
			colour_refs = &e.refs;
			m.meshes = parse(data, true);
			colour_refs = nullptr;
			e.meshes = m.meshes;
			e.hash = hs;
			e.table = colour_table;
			std::copy(&prev[0], &prev[0] + 4, &e.prev[0]);
			memcpy(e.prev_uv, prev_uv, sizeof prev_uv);
		}
	} else {
		m.meshes = parse(data, true);
	}
	if (!m.meshes->empty())
		vp->models.push_back(std::move(m));
}

namespace {

uint64_t header_key(const uint32_t *h)
{
	uint64_t k = h[3] & 0xFF;
	k |= uint64_t(((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1)) << 8;
	k |= uint64_t(h[5] & 0x1F) << 14;
	k |= uint64_t((h[4] & 0xC0) >> 6) << 19;
	k |= uint64_t((h[1] >> 4) & 1) << 21;
	k |= uint64_t(h[6] >> 31) << 22;
	k |= uint64_t(!(h[6] & 0x800000)) << 23;
	k |= uint64_t(h[2] & 0xFF) << 24;
	k |= uint64_t((h[0] >> 7) & 1) << 32;
	k |= uint64_t((h[1] >> 3) & 1) << 33;
	k |= uint64_t((h[1] >> 5) & 1) << 34;
	k |= uint64_t(h[0] >> 26) << 35;
	k |= uint64_t(h[6] & 0x3FFFF) << 41;
	k |= uint64_t(h[1] & 1) << 59;
	return k;
}

void mesh_state(Mesh &m, const uint32_t *h)
{
	m.textured = (h[6] & 0x400) != 0;
	m.alpha_test = (h[6] & 0x80000000) != 0;
	m.texture_alpha = (h[6] & 7) != 0;
	m.poly_alpha = !(h[6] & 0x800000);
	m.lighting = !(h[6] & 0x10000);
	m.smooth_shading = (h[1] & 8) != 0;
	m.fixed_shading = (h[1] & 0x20) && !m.smooth_shading;
	m.high_priority = (h[6] & 0x10) != 0;
	m.trans_select = (h[6] & 0x20000) != 0;
	m.layered = (h[6] & 8) != 0;
	m.specular = (h[0] & 0x80) != 0;
	m.shininess = float((h[6] >> 5) & 3);
	m.specular_value = (h[0] >> 26) / 63.0f;
	m.fog_intensity = ((h[6] >> 11) & 0x1F) / 16.0f;
	m.translator_map = (h[4] & 0x80) != 0;
	if (m.textured) {
		int f = (h[6] >> 7) & 7;
		if (m.alpha_test && f >= 1 && f <= 4) f += 7;   // contour: the 4-bit alpha formats lose their alpha
		m.format = f;
		if (f == 7) m.alpha_test = false;
		m.x = (32 * (((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1))) & 2047;
		m.y = (32 * (h[5] & 0x1F)) & 2047;
		unsigned w = (h[3] >> 3) & 7, hh = h[3] & 7;
		m.width = 32 << (w >= 6 ? 0 : w);
		m.height = 32 << (hh >= 6 ? 0 : hh);
		m.micro = (h[2] & 0x10) != 0;
		m.micro_id = (h[2] >> 5) & 7;
		m.micro_min_lod = float((h[2] >> 2) & 3);
		m.inverted = ((h[6] >> 24) & 0x7F) == 2;
		m.page = (h[4] >> 6) & 1;
		bool su = (h[3] & 0x80) && !m.alpha_test, sv = (h[3] & 0x40) && !m.alpha_test;
		m.wrap_u = (h[2] & 2) ? (su ? 2 : 3) : (su ? 0 : 1);
		m.wrap_v = (h[2] & 1) ? (sv ? 2 : 3) : (sv ? 0 : 1);
	}
}

} // namespace

Meshes SceneBuilder::parse(const uint32_t *h, bool dynamic)
{
	(void)dynamic;
	auto out = std::make_shared<std::vector<Mesh>>();
	std::map<std::pair<uint64_t, uint32_t>, size_t> by_key;
	uint64_t last_key = ~0ull;
	uint32_t last_rgba = 0;
	Mesh *mesh = nullptr;
	const uint32_t *end_lo = poly + 0x100000, *end_rom = vrom + 0x1000000;
	const uint32_t *limit = (h >= poly && h < end_lo) ? end_lo : end_rom;
	for (int guard = 0; guard < 200000; guard++) {
		if (h + 7 > limit || h[6] == 0)
			break;
		uint8_t colour[4];
		if (!(h[1] & 2)) {
			const uint32_t ci = (colour_table + ((h[4] >> 8) & 0xFFF)) & 0xFFFFF;
			uint32_t c = poly[ci];
			if (colour_refs) colour_refs->push_back({ ci, c });
			colour[0] = uint8_t(c >> 16); colour[1] = uint8_t(c >> 8); colour[2] = uint8_t(c);
		} else {
			colour[0] = uint8_t(h[4] >> 24); colour[1] = uint8_t(h[4] >> 16); colour[2] = uint8_t(h[4] >> 8);
		}
		colour[3] = (h[6] & 0x800000) ? 255 : uint8_t((((h[6] >> 18) & 0x3F) * 255) / 32);
		if ((h[0] & 0x200) && !(h[0] & 0x100))
			colour[3] /= 2;
		// a mesh per state and colour (the renderers take a mesh's colour from its first polygon)
		uint64_t key = header_key(h);
		uint32_t rgba;
		memcpy(&rgba, colour, 4);
		if (key != last_key || rgba != last_rgba) {
			auto it = by_key.find({ key, rgba });
			if (it == by_key.end()) {
				by_key[{ key, rgba }] = out->size();
				out->emplace_back();
				mesh = &out->back();
				mesh->key = key;
				mesh_state(*mesh, h);
			} else {
				mesh = &(*out)[it->second];
			}
		}
		const int count = (h[0] & 0x40) ? 4 : 3;
		const float uv_scale = (h[1] & 0x40) ? 1.0f : 1.0f / 8.0f;
		float face[3] = { float(int32_t(h[1]) >> 8) / 4194304.0f, float(int32_t(h[2]) >> 8) / 4194304.0f,
			float(int32_t(h[3]) >> 8) / 4194304.0f };
		Vertex v[4] = {};
		uint16_t uv[4][2] = {};
		int j = 0;
		for (int i = 0; i < 4; i++)
			if (h[0] & (1u << i)) {
				v[j] = prev[i];
				uv[j][0] = prev_uv[i][0];
				uv[j][1] = prev_uv[i][1];
				if (key != last_key && mesh->textured) {
					v[j].uv[0] = uv[j][0] * uv_scale / mesh->width;
					v[j].uv[1] = uv[j][1] * uv_scale / mesh->height;
				}
				j++;
			}
		last_key = key;
		last_rgba = rgba;
		const bool smooth = (h[1] & 8) != 0, fixed = (h[1] & 0x20) != 0, spec = (h[0] & 0x80) != 0;
		if (!smooth)
			for (int i = 0; i < count; i++) memcpy(v[i].normal, face, sizeof(face));
		const float np = float(h[5] >> 8) / 4096.0f;
		const uint32_t *d = h + 7;
		for (; j < count; j++, d += 4) {
			if (d + 4 > limit) break;
			v[j].pos[0] = float(int32_t(d[0]) >> 8) / 128.0f;
			v[j].pos[1] = float(int32_t(d[1]) >> 8) / 128.0f;
			v[j].pos[2] = float(int32_t(d[2]) >> 8) / 128.0f;
			if (smooth) {
				v[j].normal[0] = byte_to_float(int8_t(d[0]));
				v[j].normal[1] = byte_to_float(int8_t(d[1]));
				v[j].normal[2] = byte_to_float(int8_t(d[2]));
			}
			if (fixed && !smooth)
				v[j].fixed_shade = spec ? (d[0] & 0xFF) / 255.0f : byte_to_float(int8_t(d[0]));
			uv[j][0] = uint16_t(d[3] >> 16);
			uv[j][1] = uint16_t(d[3]);
			v[j].uv[0] = mesh->textured ? uv[j][0] * uv_scale / mesh->width : 0;
			v[j].uv[1] = mesh->textured ? uv[j][1] * uv_scale / mesh->height : 0;
		}
		for (int i = 0; i < count; i++) {
			memcpy(v[i].face_normal, face, sizeof(face));
			v[i].texture_np = np;
			memcpy(v[i].colour, colour, 4);
		}
		auto emit = [&](const Vertex *p) {
			if (p[0].colour[3] == 0) return;
			mesh->verts.push_back(p[0]);
			mesh->verts.push_back(p[1]);
			mesh->verts.push_back(p[2]);
			if (count == 4) {
				mesh->verts.push_back(p[0]);
				mesh->verts.push_back(p[2]);
				mesh->verts.push_back(p[3]);
			}
		};
		const bool discard = (h[0] & 0x300) == 0x300;
		if ((h[1] & 0x10) && !discard) {
			Vertex b[4];
			for (int i = 0; i < 4; i++) {
				b[i] = v[i];
				for (int k = 0; k < 3; k++) { b[i].face_normal[k] = -b[i].face_normal[k]; b[i].normal[k] = -b[i].normal[k]; }
			}
			emit(b);
		}
		if (!discard)
			emit(v);
		for (int i = 0; i < 4; i++) {
			prev[i] = v[i];
			prev_uv[i][0] = uv[i][0];
			prev_uv[i][1] = uv[i][1];
		}
		if (h[1] & 4)
			break;
		h += 7 + (count - shared_count[h[0] & 0xF]) * 4;
	}
	// meshes with no triangles are dropped
	out->erase(std::remove_if(out->begin(), out->end(), [](const Mesh &m) { return m.verts.empty(); }), out->end());
	return out;
}

namespace {

float moved2(const Model &p, const Model &m)
{
	float d = 0;
	for (int r = 0; r < 3; r++) {
		const float e = p.matrix.m[12 + r] - m.matrix.m[12 + r];
		d += e * e;
	}
	return d;
}

// An affine matrix's inverse (false if it has none)
bool affine_inverse(const Mat4 &a, Mat4 &r)
{
	const float *m = a.m;
	const float c00 = m[5] * m[10] - m[9] * m[6], c01 = m[9] * m[2] - m[1] * m[10], c02 = m[1] * m[6] - m[5] * m[2];
	const float det = m[0] * c00 + m[4] * c01 + m[8] * c02;
	if (!(std::fabs(det) > 1e-20f)) return false;
	const float k = 1.0f / det;
	r = Mat4::identity();
	r.m[0] = c00 * k; r.m[1] = c01 * k; r.m[2] = c02 * k;
	r.m[4] = (m[8] * m[6] - m[4] * m[10]) * k; r.m[5] = (m[0] * m[10] - m[8] * m[2]) * k; r.m[6] = (m[4] * m[2] - m[0] * m[6]) * k;
	r.m[8] = (m[4] * m[9] - m[8] * m[5]) * k; r.m[9] = (m[8] * m[1] - m[0] * m[9]) * k; r.m[10] = (m[0] * m[5] - m[4] * m[1]) * k;
	for (int i = 0; i < 3; i++) r.m[12 + i] = -(r.m[i] * m[12] + r.m[4 + i] * m[13] + r.m[8 + i] * m[14]);
	return true;
}

// Where a's model would be in b if it stood still while the camera moved by c (b = c a)
float predicted2(const Mat4 &c, const Model &p, const Model &m)
{
	float d = 0;
	for (int r = 0; r < 3; r++) {
		const float x = c.m[r] * p.matrix.m[12] + c.m[4 + r] * p.matrix.m[13] + c.m[8 + r] * p.matrix.m[14] + c.m[12 + r];
		const float e = x - m.matrix.m[12 + r];
		d += e * e;
	}
	return d;
}

// How differently two models point (their axes, a's turned by the camera's move c if known): 0 the same, up to 12
float turned2(const Mat4 *c, const Model &p, const Model &m)
{
	float d = 0;
	for (int j = 0; j < 3; j++)
		for (int r = 0; r < 3; r++) {
			float x = p.matrix.m[j * 4 + r];
			if (c) x = c->m[r] * p.matrix.m[j * 4] + c->m[4 + r] * p.matrix.m[j * 4 + 1] + c->m[8 + r] * p.matrix.m[j * 4 + 2];
			const float e = x - m.matrix.m[j * 4 + r];
			d += e * e;
		}
	return d;
}

float dist2(const Model &m)
{
	return m.matrix.m[12] * m.matrix.m[12] + m.matrix.m[13] * m.matrix.m[13] + m.matrix.m[14] * m.matrix.m[14];
}

// a model's shape: its meshes and triangles
uint64_t shape_of(const Model &m)
{
	uint64_t n = m.meshes ? m.meshes->size() : 0, verts = 0;
	if (m.meshes)
		for (auto &mesh : *m.meshes) verts += mesh.verts.size();
	return (n << 32) ^ verts;
}

// its shape and the kind of picture on each mesh (its size, format and sheet, not its place): a flipbook's steps
// alike, the sand grains around them not
uint64_t look_of(const Model &m, uint64_t shape)
{
	uint64_t h = shape * 0x9E3779B97F4A7C15ull;
	if (m.meshes)
		for (auto &mesh : *m.meshes) {
			const uint64_t k = mesh.textured ? (uint64_t(mesh.width) | uint64_t(mesh.height) << 12 | uint64_t(mesh.format) << 24 | uint64_t(mesh.page ^ m.page) << 28 | 1ull << 31) : 0;
			h = (h ^ k) * 0x100000001B3ull;
		}
	return h;
}

} // namespace

// The camera's move between two frames' viewport (b = camera a, for what stands still), from the models alone at
// their address in both: the one most of the others agree with (the fighters move on their own)
bool camera_move(const Viewport &va, const Viewport &vb, Mat4 &camera)
{
	const std::vector<Model> &was = va.models, &now = vb.models;
	std::unordered_map<uint32_t, int> n_was, n_now;
	for (auto &m : was) n_was[m.addr]++;
	for (auto &m : now) n_now[m.addr]++;
	std::vector<std::pair<int, int>> sure;
	{
		std::unordered_map<uint32_t, int> at;
		for (size_t i = 0; i < was.size(); i++)
			if (n_was[was[i].addr] == 1) at[was[i].addr] = int(i);
		for (size_t j = 0; j < now.size(); j++)
			if (n_now[now[j].addr] == 1)
				if (auto it = at.find(now[j].addr); it != at.end()) sure.push_back({ it->second, int(j) });
	}
	// Only the farther half count, by their distance: most of the models alone at their address are the fighters'
	// parts, near the camera and moving together, and the stage's far parts show the camera's turn
	{
		std::vector<float> d;
		for (auto &q : sure) d.push_back(dist2(now[size_t(q.second)]));
		if (d.size() < 3) return false;
		std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
		const float median = d[d.size() / 2];
		sure.erase(std::remove_if(sure.begin(), sure.end(), [&](const std::pair<int, int> &q) { return dist2(now[size_t(q.second)]) < median; }), sure.end());
	}
	camera = Mat4::identity();
	double best = 0, all = 0;
	size_t best_n = 0;
	for (auto &q : sure) all += std::sqrt(dist2(now[size_t(q.second)]));
	const size_t step = std::max<size_t>(1, sure.size() / 48);
	for (size_t k = 0; k < sure.size(); k += step) {
		Mat4 inv, c;
		if (!affine_inverse(was[size_t(sure[k].first)].matrix, inv)) continue;
		c = now[size_t(sure[k].second)].matrix * inv;
		double agree = 0;
		size_t n = 0;
		for (auto &q : sure)
			if (predicted2(c, was[size_t(q.first)], now[size_t(q.second)]) < 4e-4f * dist2(now[size_t(q.second)]) + 1e-2f) {
				agree += std::sqrt(dist2(now[size_t(q.second)]));
				n++;
			}
		if (agree > best) { best = agree; best_n = n; camera = c; }
	}
	return best_n >= 3 && best * 3 >= all;
}

namespace {

// b's model m's meshes with their vertices blended from p's (the same meshes made again)
// A model the game builds each frame (a fighter's joint, between two limbs): its vertices where they are between the
// two frames in the view (each from where a's matrix put it to where b's does), given in the blended matrix's terms.
// Blending the vertices and the matrix each on their own put them elsewhere when both change (a knee bending as the
// leg turns: the cross term), the joint off the limbs' ends, which blend exactly (a rigid model's vertex goes from a's
// place to b's): Lau's trousers in pieces for a picture as he changed stance.
void blend_vertices(const Model &p, Model &m, float t)
{
	const float *A = p.matrix.m, *B = m.matrix.m;
	float M[16];
	for (int i = 0; i < 16; i++) M[i] = A[i] + (B[i] - A[i]) * t;
	// the blended matrix's 3x3 inverse (column-major: column c, row r at c * 4 + r)
	auto at = [&](int r, int c) { return M[c * 4 + r]; };
	const float det = at(0, 0) * (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) - at(0, 1) * (at(1, 0) * at(2, 2) - at(1, 2) * at(2, 0)) +
	                  at(0, 2) * (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0));
	const bool view = std::fabs(det) > 1e-12f;
	float inv[3][3] = {};
	if (view) {
		const float k = 1.0f / det;
		inv[0][0] = (at(1, 1) * at(2, 2) - at(1, 2) * at(2, 1)) * k;
		inv[0][1] = (at(0, 2) * at(2, 1) - at(0, 1) * at(2, 2)) * k;
		inv[0][2] = (at(0, 1) * at(1, 2) - at(0, 2) * at(1, 1)) * k;
		inv[1][0] = (at(1, 2) * at(2, 0) - at(1, 0) * at(2, 2)) * k;
		inv[1][1] = (at(0, 0) * at(2, 2) - at(0, 2) * at(2, 0)) * k;
		inv[1][2] = (at(0, 2) * at(1, 0) - at(0, 0) * at(1, 2)) * k;
		inv[2][0] = (at(1, 0) * at(2, 1) - at(1, 1) * at(2, 0)) * k;
		inv[2][1] = (at(0, 1) * at(2, 0) - at(0, 0) * at(2, 1)) * k;
		inv[2][2] = (at(0, 0) * at(1, 1) - at(0, 1) * at(1, 0)) * k;
	}
	// x in the view: a's (or b's) matrix on a model-space vector (w 1 for a point, 0 for a direction)
	auto apply = [](const float *X, const float *v, float w, float *o) {
		for (int r = 0; r < 3; r++) o[r] = X[r] * v[0] + X[4 + r] * v[1] + X[8 + r] * v[2] + X[12 + r] * w;
	};
	auto blend = [&](const float *from, float *to, float w) {
		if (!view) {   // (a matrix that flattens it: as before)
			for (int c = 0; c < 3; c++) to[c] = from[c] + (to[c] - from[c]) * t;
			return;
		}
		float x[3], y[3], z[3];
		apply(A, from, w, x);
		apply(B, to, w, y);
		for (int c = 0; c < 3; c++) z[c] = x[c] + (y[c] - x[c]) * t - M[12 + c] * w;
		for (int r = 0; r < 3; r++) to[r] = inv[r][0] * z[0] + inv[r][1] * z[1] + inv[r][2] * z[2];
	};
	auto blended = std::make_shared<std::vector<Mesh>>(*m.meshes);
	for (size_t k = 0; k < blended->size(); k++) {
		const std::vector<Vertex> &from = (*p.meshes)[k].verts;
		std::vector<Vertex> &to = (*blended)[k].verts;
		for (size_t i = 0; i < to.size(); i++) {
			blend(from[i].pos, to[i].pos, 1);
			blend(from[i].normal, to[i].normal, 0);
			blend(from[i].face_normal, to[i].face_normal, 0);
		}
	}
	m.meshes = blended;
}

// The pairing of two frames' models, kept for the next picture between the same two (at 120 a second there are two;
// the camera's move and the pairing don't depend on where between them a picture is): for each of b's models, a's it
// is blended from (-1: shown as it is in b) and whether its vertices are too
// A flipbook (Shun's harbour water: a 64x64 picture and wave shapes that step every 3 frames, ~19 a second): a model
// whose texture moved to another place since the frame before, its shapes the same in number. Its last step is
// remembered (from: the model as it was before it) with the frames since (age); once two steps have come at most 8
// frames apart (period), each picture shows the step faded in from the one before over a period, a period late: it
// moves at the pictures' rate, not 19 steps a second. A step that doesn't repeat (a face's texture changed once) isn't.
struct Flip {
	Meshes from;
	int age = -1;      // frames since the step (-1: none remembered)
	int period = 0;    // frames between its last two steps (0: not known yet)
};
bool flip_step(const Model &p, const Model &m)
{
	if (!p.meshes || !m.meshes || p.meshes == m.meshes || p.meshes->size() != m.meshes->size()) return false;
	bool moved = false;
	for (size_t k = 0; k < m.meshes->size(); k++) {
		const Mesh &a = (*p.meshes)[k], &c = (*m.meshes)[k];
		if (a.verts.size() != c.verts.size() || a.textured != c.textured) return false;
		if (c.textured && a.width == c.width && a.height == c.height && a.page == c.page && a.format == c.format && (a.x != c.x || a.y != c.y)) moved = true;
	}
	return moved;
}

struct Plan {
	uint64_t a = 0, b = 0;
	bool ok = false;
	InterpStats st;
	std::vector<int> av;                     // a's viewport each of b's is paired with (-1: none)
	std::vector<std::vector<int>> from;
	std::vector<std::vector<int>> verts;     // a's model each of b's vertices are blended from, + 1 (0: not blended)
	std::vector<std::vector<int>> paired;    // every one of b's models' pair in a (blended or not)
	std::vector<std::vector<Flip>> flips;    // per b's model
	std::vector<std::vector<char>> swarm;    // b's models shown as a swarm's grains (snap 4)
};
Plan plan;
// the addresses a swarm's grains were seen at, and the frames they stay a swarm's (interpolate)
std::unordered_map<uint32_t, int> swarms;
constexpr int SWARM_FRAMES = 115;

// the flipbooks' steps faded in (after the rest of the blend): their shapes and the two pictures, by where between its
// frames the picture is
void apply_flips(const Scene &b, float t, Scene &out)
{
	static const bool off = getenv("VF3_NO_FLIPS") != nullptr;   // (tests: as before)
	if (off) return;
	for (size_t v = 0; v < out.viewports.size() && v < plan.flips.size(); v++)
		for (size_t j = 0; j < out.viewports[v].models.size() && j < plan.flips[v].size(); j++) {
			const Flip &f = plan.flips[v][j];
			if (f.age < 0 || f.period <= 0) continue;
			const float w = (float(f.age) + t) / float(f.period);   // (the newer step's share)
			if (w >= 1) continue;
			const Meshes &to = b.viewports[v].models[j].meshes;
			if (!to || !f.from || to->size() != f.from->size()) continue;
			auto blended = std::make_shared<std::vector<Mesh>>(*to);
			for (size_t k = 0; k < blended->size(); k++) {
				Mesh &n = (*blended)[k];
				const Mesh &o = (*f.from)[k];
				if (n.verts.size() != o.verts.size()) continue;
				for (size_t i = 0; i < n.verts.size(); i++)
					for (int c = 0; c < 3; c++) {
						n.verts[i].pos[c] = o.verts[i].pos[c] + (n.verts[i].pos[c] - o.verts[i].pos[c]) * w;
						n.verts[i].normal[c] = o.verts[i].normal[c] + (n.verts[i].normal[c] - o.verts[i].normal[c]) * w;
						n.verts[i].face_normal[c] = o.verts[i].face_normal[c] + (n.verts[i].face_normal[c] - o.verts[i].face_normal[c]) * w;
					}
				if (n.textured && (o.x != n.x || o.y != n.y) && o.width == n.width && o.height == n.height && o.page == n.page) {
					n.flip = 1 - w;
					n.flip_x = o.x;
					n.flip_y = o.y;
				}
			}
			out.viewports[v].models[j].meshes = blended;
		}
}

// A swarm's grain (the beach's spray, Wolf's desert sand: 1 to 6 polygons): under 40 vertices
bool grain(const Model &m)
{
	size_t n = 0;
	if (m.meshes)
		for (auto &mesh : *m.meshes) n += mesh.verts.size();
	return n < 40;
}

// How far the scene moved between a and b: the median move, over its distance, of the models alone at their address
// in both (not a swarm's grains) in the viewports paired (av: a's for each of b's); -1 if under 8 of them
float scene_move(const Scene &a, const Scene &b, const std::vector<int> &av)
{
	std::vector<float> moved;
	for (size_t v = 0; v < av.size(); v++) {
		if (av[v] < 0) continue;
		const std::vector<Model> &was = a.viewports[size_t(av[v])].models, &now = b.viewports[v].models;
		std::unordered_map<uint32_t, int> n_was, n_now;
		std::unordered_map<uint32_t, const Model *> at;
		for (auto &m : was) { n_was[m.addr]++; at[m.addr] = &m; }
		for (auto &m : now) n_now[m.addr]++;
		for (auto &m : now)
			if (n_now[m.addr] == 1 && n_was[m.addr] == 1 && !grain(m))
				moved.push_back(std::sqrt(moved2(*at[m.addr], m) / std::max(dist2(m), 1.0f)));
	}
	if (moved.size() < 8) return -1;
	std::nth_element(moved.begin(), moved.begin() + moved.size() / 2, moved.end());
	return moved[moved.size() / 2];
}

// A model's size: the square of its farthest vertex from its origin, in the view's units
float radius2(const Model &m)
{
	float r = 0;
	if (m.meshes)
		for (auto &mesh : *m.meshes)
			for (auto &v : mesh.verts) r = std::max(r, v.pos[0] * v.pos[0] + v.pos[1] * v.pos[1] + v.pos[2] * v.pos[2]);
	const float *c = m.matrix.m;
	return r * (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
}

// Which of a's viewports each of b's is (-1: none); false for a cut. The same number: the same, in order. Otherwise
// by the models they have in common (their addresses), the most alike first, each once, at least half of the larger's
// in common: the game puts viewports in the list and takes them out (6 small ones in a throw at the beach, the ones
// after them moved along), and the whole scene was shown unblended for a frame, a stop then a jump in the fighters'
// motion. It does at cuts too (the stage's models the same, from elsewhere): a cut unless the scene moved under 10%
// (scene_move: a throw's 2% at most; a cut's 15-40%).
bool pair_viewports(const Scene &a, const Scene &b, std::vector<int> &av)
{
	av.assign(b.viewports.size(), -1);
	if (a.viewports.size() == b.viewports.size()) {
		for (size_t v = 0; v < av.size(); v++) av[v] = int(v);
		return true;
	}
	std::vector<std::tuple<float, int, int>> all;
	for (size_t u = 0; u < a.viewports.size(); u++) {
		std::unordered_map<uint32_t, int> n_was;
		for (auto &m : a.viewports[u].models) n_was[m.addr]++;
		for (size_t v = 0; v < b.viewports.size(); v++) {
			std::unordered_map<uint32_t, int> left = n_was;
			size_t common = 0;
			for (auto &m : b.viewports[v].models)
				if (auto it = left.find(m.addr); it != left.end() && it->second > 0) { it->second--; common++; }
			const size_t most = std::max(a.viewports[u].models.size(), b.viewports[v].models.size());
			if (common * 2 >= most && common) all.push_back({ -float(common) / float(most), int(u), int(v) });
		}
	}
	std::sort(all.begin(), all.end());
	std::vector<char> taken(a.viewports.size(), 0);
	for (auto &[score, u, v] : all)
		if (!taken[size_t(u)] && av[size_t(v)] < 0) { av[size_t(v)] = u; taken[size_t(u)] = 1; }
	const float moved = scene_move(a, b, av);
	return moved >= 0 && moved < 0.1f;
}

} // namespace

const std::vector<std::vector<int>> *interp_paired(const Scene &a, const Scene &b, std::vector<int> *viewports)
{
	if (!(a.id && plan.a == a.id && plan.b == b.id && plan.paired.size() == b.viewports.size())) return nullptr;
	if (viewports) *viewports = plan.av;
	return &plan.paired;
}

bool interpolate(const Scene &a, const Scene &b, float t, Scene &out, InterpStats *st)
{
	out = b;
	std::vector<int> av;
	if (a.blank || b.blank || !pair_viewports(a, b, av))
		return false;
	static const bool no_plan = getenv("VF3_NO_INTERP_PLAN") != nullptr;   // (tests: every picture paired afresh)
	if (!no_plan && a.id && b.id && plan.a == a.id && plan.b == b.id && plan.from.size() == out.viewports.size()) {
		if (st) *st = plan.st;
		if (!plan.ok) { out = b; return false; }
		for (size_t v = 0; v < out.viewports.size(); v++) {
			if (plan.av[v] < 0) continue;
			const std::vector<Model> &was = a.viewports[size_t(plan.av[v])].models;
			std::vector<Model> &now = out.viewports[v].models;
			for (size_t j = 0; j < now.size() && j < plan.from[v].size(); j++) {
				const int i = plan.from[v][j];
				if (i < 0) continue;
				const Model &p = was[size_t(i)];
				Model &m = now[j];
				if (plan.verts[v][j]) blend_vertices(was[size_t(plan.verts[v][j] - 1)], m, t);
				for (int k = 0; k < 16; k++) m.matrix.m[k] = p.matrix.m[k] + (m.matrix.m[k] - p.matrix.m[k]) * t;
				m.alpha = p.alpha + (m.alpha - p.alpha) * t;
			}
		}
		apply_flips(b, t, out);
		return true;
	}
	Plan np;
	np.a = a.id;
	np.b = b.id;
	InterpStats local;
	if (!st) st = &local;
	*st = InterpStats();
	size_t total = 0, matched = 0, voters = 0, voted = 0, strays = 0;
	for (auto it = swarms.begin(); it != swarms.end();) it = --it->second <= 0 ? swarms.erase(it) : std::next(it);
	np.av = std::move(av);
	for (size_t v = 0; v < out.viewports.size(); v++) {
		np.from.emplace_back(out.viewports[v].models.size(), -1);
		np.verts.emplace_back(out.viewports[v].models.size(), 0);
		np.paired.emplace_back(out.viewports[v].models.size(), -1);
		const int u = np.av[v];
		if (u < 0) {   // (a viewport new this frame: its models shown as they are)
			np.swarm.emplace_back(out.viewports[v].models.size(), 0);
			for (auto &m : out.viewports[v].models) voters += !grain(m);
			total += out.viewports[v].models.size();
			st->no_key += out.viewports[v].models.size();
			continue;
		}
		const Viewport &va = a.viewports[size_t(u)];
		const std::vector<Model> &was = va.models;
		std::vector<Model> &now = out.viewports[v].models;
		std::vector<char> taken(was.size(), 0);
		std::vector<int> pair(now.size(), -1);
		std::unordered_map<uint32_t, int> n_was, n_now;
		for (auto &m : was) n_was[m.addr]++;
		for (auto &m : now) n_now[m.addr]++;
		Mat4 camera;
		const bool have_camera = camera_move(va, b.viewports[v], camera);
		// how far a's model is from b's: from where the camera's move puts it, if known
		auto apart2 = [&](const Model &p, const Model &m) { return have_camera ? predicted2(camera, p, m) : moved2(p, m); };
		// The nearest of a's models with the same key not yet paired (several share a model's address: a stage's
		// palms, the fighters' limbs; the culling nodes above them are rebuilt elsewhere most frames, so they don't
		// tell them apart: in order, a model would be paired with its neighbour and drawn halfway to it)
		// (the nearest pairs of all of a key's first: taken in the drawing order, one could take another's nearest, the
		// two small flags on a tower each drawn halfway to the other)
		std::vector<char> crowded(now.size(), 0);   // (paired among several of its key)
		// An address pair more than 0.5% of its distance from where it would be (the camera's move, or none known: where
		// it was), when another model of its shape is 4 times nearer: left for the shape pass. Animations step through the same
		// models in ROM: a tower's two small flags, one this frame in the model the other had in the last, and a flock
		// of birds on Pai's rooftop sharing theirs; paired by address, each was drawn halfway to another. (Only with
		// another much nearer: a fighter's limb moves far on its own.)
		// (each model's shape once, and a's models by shape: looked through for each of b's models far from where it
		// would be, which on a big camera move is most of them, every one of a's models with its meshes counted made
		// a picture 3-4 ms)
		std::vector<uint64_t> was_shape(was.size()), now_shape(now.size());
		for (size_t i = 0; i < was.size(); i++) was_shape[i] = shape_of(was[i]);
		for (size_t j = 0; j < now.size(); j++) now_shape[j] = shape_of(now[j]);
		std::unordered_map<uint64_t, std::vector<int>> was_by_shape;
		for (size_t i = 0; i < was.size(); i++) was_by_shape[was_shape[i]].push_back(int(i));
		auto elsewhere = [&](int i, int j) {
			const Model &m = now[size_t(j)];
			const float own = apart2(was[size_t(i)], m);
			if (own <= 2.5e-5f * dist2(m) + 1e-2f) return false;
			auto it = was_by_shape.find(now_shape[size_t(j)]);
			if (it == was_by_shape.end() || it->second.size() > 32) return false;   // (a swarm of hundreds: each looked through all, a ms a picture)
			for (int k : it->second)
				if (!taken[size_t(k)] && k != i && apart2(was[size_t(k)], m) * 16 < own) return true;
			return false;
		};
		auto pair_by = [&](auto key, bool by_addr) {
			std::unordered_map<uint64_t, std::vector<int>> by, by_now;
			for (size_t i = 0; i < was.size(); i++)
				if (!taken[i]) by[key(was[i])].push_back(int(i));
			for (size_t j = 0; j < now.size(); j++)
				if (pair[j] < 0) by_now[key(now[j])].push_back(int(j));
			for (auto &[k, js] : by_now) {
				auto it = by.find(k);
				if (it == by.end()) continue;
				const std::vector<int> &is = it->second;
				if (is.size() == 1 && js.size() == 1) {
					if (elsewhere(is[0], js[0])) continue;
					pair[size_t(js[0])] = is[0];
					taken[size_t(is[0])] = 1;
					continue;
				}
				// (by place and by the way each points: a fighter's trouser segments share an address and nearly their
				// place, at the hip; by place alone one was paired with its neighbour, pointing another way, and drawn
				// halfway to it, the legs in pieces for a picture. Pointing counts as much as a tenth of the distance.)
				// A big group (a swarm of dust or sand, hundreds of one shape): each of b's, in turn, with the nearest of a's
				// not yet taken. Every pair sorted was the square of them, 5-9 ms a picture on a Steam Deck in Wolf's
				// desert; a dust grain paired with its neighbour's last place isn't seen.
				if (is.size() * js.size() > 1024) {
					for (int j : js) {
						int best = -1;
						float best_d = 0;
						for (int i : is) {
							if (taken[size_t(i)]) continue;
							const float d = apart2(was[size_t(i)], now[size_t(j)]);
							if (best < 0 || d < best_d) { best = i; best_d = d; }
						}
						if (best >= 0 && !(by_addr && elsewhere(best, j))) { pair[size_t(j)] = best; taken[size_t(best)] = 1; crowded[size_t(j)] = 1; }
					}
					continue;
				}
				std::vector<std::tuple<float, int, int>> all;
				for (int j : js)
					for (int i : is) {
						const Model &p = was[size_t(i)], &m = now[size_t(j)];
						all.push_back({ apart2(p, m) + turned2(have_camera ? &camera : nullptr, p, m) * 0.01f * dist2(m), i, j });
					}
				std::sort(all.begin(), all.end());
				for (auto &[d, i, j] : all)
					if (!taken[size_t(i)] && pair[size_t(j)] < 0 && !(by_addr && elsewhere(i, j))) { pair[size_t(j)] = i; taken[size_t(i)] = 1; crowded[size_t(j)] = 1; }
				static const bool pair_log = getenv("VF3_PAIR_LOG") != nullptr;
				if (pair_log && is.size() <= 6) {   // (tests)
					fprintf(stderr, "pair group cam %d:", have_camera);
					for (int j : js) {
						const Model &mm = now[size_t(j)];
						fprintf(stderr, " [now %08x (%.0f %.0f %.0f) <- %d", mm.addr, mm.matrix.m[12], mm.matrix.m[13], mm.matrix.m[14], pair[size_t(j)]);
						if (pair[size_t(j)] >= 0) { const Model &pp = was[size_t(pair[size_t(j)])]; fprintf(stderr, " %08x (%.0f %.0f %.0f) d %.1f", pp.addr, pp.matrix.m[12], pp.matrix.m[13], pp.matrix.m[14], std::sqrt(apart2(pp, mm))); }
						fprintf(stderr, "]");
					}
					fprintf(stderr, "\n");
				}
			}
		};
		pair_by([](const Model &m) { return uint64_t(m.addr); }, true);
		// then by shape and the pictures' kind: a flipbook's next step (Wolf's desert's dust puffs, a quad each, their
		// picture stepping through a sheet every 2 frames, a new model each step) with the step before. By shape alone
		// they were among hundreds of sand grains of their shape, each paired in turn with the nearest left: a puff
		// new that frame took another's step before, and that one another's, the puffs faded from each other's
		// pictures (a puff changing for a frame, flickering at 120 a second) and some from none (a jump)
		std::vector<uint64_t> was_look(was.size()), now_look(now.size());
		for (size_t i = 0; i < was.size(); i++) was_look[i] = look_of(was[i], was_shape[i]);
		for (size_t j = 0; j < now.size(); j++) now_look[j] = look_of(now[j], now_shape[j]);
		pair_by([&](const Model &m) { return (&m >= now.data() && &m < now.data() + now.size()) ? now_look[size_t(&m - now.data())] : was_look[size_t(&m - was.data())]; }, false);
		// then by shape: an animation's next model (the birds' wings, a splash: the game steps through models in
		// ROM), a model the game builds each frame (in polygon RAM, wherever there was room)
		pair_by([&](const Model &m) { return (&m >= now.data() && &m < now.data() + now.size()) ? now_shape[size_t(&m - now.data())] : was_shape[size_t(&m - was.data())]; }, false);
		// then by place: another model where one was (a detail level for another, the same nodes over it)
		for (size_t j = 0; j < now.size(); j++) {
			if (pair[j] >= 0) continue;
			float d0 = 0;
			for (int r = 0; r < 3; r++) d0 += now[j].matrix.m[12 + r] * now[j].matrix.m[12 + r];
			int best = -1;
			float best_d = 1e-6f * d0 + 1e-4f;
			for (size_t i = 0; i < was.size(); i++)
				if (!taken[i] && moved2(was[i], now[j]) < best_d) { best = int(i); best_d = moved2(was[i], now[j]); }
			if (best >= 0) { pair[j] = best; taken[best] = 1; }
		}
		if (const char *pn = getenv("VF3_PAIR_NEAR")) {   // (tests: "x,y,r": the models whose place is near (x, y) on screen, and their pairs)
			float tx = 0, ty = 0, tr = 0;
			sscanf(pn, "%f,%f,%f", &tx, &ty, &tr);
			const Viewport &vp = b.viewports[v];
			auto proj = [&](const Model &mm, float &sx, float &sy) {
				const float z = -mm.matrix.m[14];
				if (z <= 0) return false;
				sx = vp.vp_x + vp.vp_w * (mm.matrix.m[12] / z - vp.left) / (vp.right - vp.left);
				sy = vp.vp_y + vp.vp_h * (vp.top - mm.matrix.m[13] / z) / (vp.top - vp.bottom);
				return true;
			};
			for (size_t j = 0; j < now.size(); j++) {
				float sx, sy;
				if (!proj(now[j], sx, sy) || std::fabs(sx - tx) > tr || std::fabs(sy - ty) > tr) continue;
				fprintf(stderr, "near vp %zu now %zu %08x at %.0f,%.0f", v, j, now[j].addr, sx, sy);
				if (pair[j] >= 0) {
					float px, py;
					proj(was[size_t(pair[j])], px, py);
					fprintf(stderr, " <- %d %08x at %.0f,%.0f crowded %d", pair[j], was[size_t(pair[j])].addr, px, py, int(crowded[j]));
				}
				fprintf(stderr, "\n");
				if (pair[j] >= 0 && getenv("VF3_PAIR_MAT")) {
					const Model &pp = was[size_t(pair[j])];
					for (int k = 0; k < 16; k++) fprintf(stderr, " %.2f/%.2f", pp.matrix.m[k], now[j].matrix.m[k]);
					fprintf(stderr, " meshes %zu/%zu same %d\n", pp.meshes ? pp.meshes->size() : 0, now[j].meshes ? now[j].meshes->size() : 0, int(pp.meshes == now[j].meshes));
				}
			}
			for (size_t i = 0; i < was.size(); i++) {
				float sx, sy;
				if (!proj(was[i], sx, sy) || std::fabs(sx - tx) > tr || std::fabs(sy - ty) > tr) continue;
				fprintf(stderr, "near vp %zu was %zu %08x at %.0f,%.0f taken %d\n", v, i, was[i].addr, sx, sy, int(taken[i]));
			}
		}
		// too far in a frame: shown where it is now (0 blended, 1 turned, 2 moved, 3 another of several, 4 a swarm's grain)
		std::vector<uint8_t> snap(now.size(), 0);
		std::vector<char> loose(now.size(), 0), far(now.size(), 0);
		for (size_t j = 0; j < now.size(); j++) {
			if (pair[j] < 0) continue;
			const Model &m = now[j], &p = was[size_t(pair[j])];
			float rot = 0, dist = 0;
			for (int c = 0; c < 3; c++)
				for (int r = 0; r < 3; r++) rot = std::max(rot, std::fabs(p.matrix.m[c * 4 + r] - m.matrix.m[c * 4 + r]));
			for (int r = 0; r < 3; r++) dist += m.matrix.m[12 + r] * m.matrix.m[12 + r];
			const float move = moved2(p, m);
			// One of several sharing an address (a stage's flags) far from where the camera's move puts the one it
			// was paired with (more than 15% of its distance): another of them, not blended (it was drawn halfway
			// between the two, a flag in the sky for a picture)
			const bool wrong = (n_was[p.addr] > 1 || n_now[m.addr] > 1 || crowded[j]) && apart2(p, m) > 0.0225f * dist + 1.0f;
			static const bool no_crowded = getenv("VF3_NO_CROWDED") != nullptr;   // (tests: crowded pairs not blended)
			// (a swarm's grain not alone at its address in both frames, far from where the camera's move puts its
			// partner: below)
			loose[j] = grain(m) && !(p.addr == m.addr && n_was[p.addr] == 1 && n_now[m.addr] == 1);
			far[j] = loose[j] && apart2(p, m) > std::max(4 * radius2(m), 0.0004f * dist);
			snap[j] = rot > 0.75f ? 1 : move > 0.25f * dist + 1.0f ? 2 : wrong || (no_crowded && crowded[j]) ? 3 : 0;
		}
		// A swarm's grains (Wolf's desert sand, the beach's spray: kicked up and put somewhere new each frame, dozens to an
		// address): blended, each was drawn along a line between it and another grain, the sand around a fighter's feet
		// swirling and flickering at 120 a second. Shown where they are, as the arcade's frames have them. An address is
		// a swarm's when most of its grains not alone at it are further from where the camera's move puts their partners
		// than twice their size and 2% of their distance, and stays one for 2 seconds after, as does a grain whose partner
		// was one: by the grain, a swarm's near its partner by chance glided one frame and jumped the next. (A grain alone
		// at its address, a spark, glides; so do a stage's small models that share one, flags, birds, a fighter's hair,
		// which stay near where the camera's move puts them.)
		{
			std::unordered_map<uint32_t, std::pair<int, int>> group;   // (grains not alone at the address, and of them far)
			for (size_t j = 0; j < now.size(); j++)
				if (loose[j]) { auto &g = group[now[j].addr]; g.first++; g.second += far[j]; }
			for (auto &[addr, g] : group)
				if (g.first >= 2 && g.second * 2 > g.first) swarms[addr] = SWARM_FRAMES;
			// (or its partner was one the frame before: a grain's next step of its animation is at another address)
			const bool chain = plan.b == a.id && a.id && size_t(u) < plan.swarm.size();
			np.swarm.emplace_back(now.size(), 0);
			for (size_t j = 0; j < now.size(); j++) {
				const bool was_one = chain && pair[j] >= 0 && size_t(pair[j]) < plan.swarm[size_t(u)].size() && plan.swarm[size_t(u)][size_t(pair[j])];
				if (loose[j] && (snap[j] == 0 || snap[j] == 3) && (far[j] || was_one || swarms.count(now[j].addr))) snap[j] = 4;
				np.swarm[v][j] = snap[j] == 4;
			}
		}
		for (size_t j = 0; j < now.size(); j++) {
			Model &m = now[j];
			total++;
			strays += snap[j] == 4;
			voters += !grain(m);
			np.paired[v][j] = pair[j];
			if (pair[j] < 0) { if (st) st->no_key++; continue; }
			const Model &p = was[size_t(pair[j])];
			if (snap[j]) {
				if (st) (snap[j] == 1 ? st->turned : st->moved)++;
				continue;
			}
			matched++;
			voted += !grain(m);
			np.from[v][j] = pair[j];
			int matrix_from = pair[j];   // (the partner its vertices are blended from, when another: its matrix too)
			// A model the game builds each frame (the fighters' joints, between their limbs): its vertices blended
			// too, when it is the same model (its meshes made the same way, with the same textures: their texture
			// coordinates may change, a shiny surface's are made each frame), or it stays where it is in b while the
			// limbs move between. Another model built in the same place (one face for another) isn't blended into it.
			if (m.meshes && p.meshes && m.meshes != p.meshes && m.meshes->size() == p.meshes->size()) {
				// The same model made the same way (its meshes with the same textures), and how far its vertices are
				// from b's in the view (the farthest one, squared; size: b's across)
				auto alike = [&](const Model &q) {
					if (!q.meshes || q.meshes->size() != m.meshes->size()) return false;
					for (size_t k = 0; k < m.meshes->size(); k++) {
						const Mesh &x = (*m.meshes)[k], &y = (*q.meshes)[k];
						if (!(x.verts.size() == y.verts.size() && x.key == y.key && x.x == y.x && x.y == y.y && x.page == y.page &&
						      x.width == y.width && x.height == y.height && x.format == y.format)) return false;
					}
					return true;
				};
				float size = 0;
				auto apart = [&](const Model &q, bool &uv_same) {
					float most = 0, lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
					uv_same = true;
					for (size_t k = 0; k < m.meshes->size(); k++) {
						const std::vector<Vertex> &x = (*m.meshes)[k].verts, &y = (*q.meshes)[k].verts;
						for (size_t i = 0; i < x.size(); i++) {
							uv_same &= x[i].uv[0] == y[i].uv[0] && x[i].uv[1] == y[i].uv[1];
							float d = 0;
							for (int r = 0; r < 3; r++) {
								const float *u = y[i].pos, *w = x[i].pos;
								const float e = q.matrix.m[r] * u[0] + q.matrix.m[4 + r] * u[1] + q.matrix.m[8 + r] * u[2] + q.matrix.m[12 + r];
								const float f = m.matrix.m[r] * w[0] + m.matrix.m[4 + r] * w[1] + m.matrix.m[8 + r] * w[2] + m.matrix.m[12 + r];
								d += (e - f) * (e - f);
								lo[r] = std::min(lo[r], f);
								hi[r] = std::max(hi[r], f);
							}
							most = std::max(most, d);
						}
					}
					size = 0;
					for (int r = 0; r < 3; r++) size += (hi[r] - lo[r]) * (hi[r] - lo[r]);
					return most;
				};
				int from = -1;
				bool uv_same = false;
				float best = 0;
				if (alike(p)) { from = pair[j]; best = apart(p, uv_same); }
				const bool by_place = from >= 0;
				// Another of a's models of its shape, made the same way, with its vertices 4 times nearer: the one it is.
				// The game makes the fighters' joints each frame wherever there is room, and both knees use the same
				// texture: the left knee made where the right one was, paired by its place, was blended into the other
				// (the same texture coordinates passed it), Lau's trousers in pieces for a picture as he changed stance.
				// (only when its place's partner is a quarter of its size or more away, and among a few: the knees are 2
				// to 4 of a shape; a swarm of sand or splashes is dozens, and looking through them all for each of them
				// took a picture 24 ms on a Steam Deck)
				if (auto it = was_by_shape.find(now_shape[j]); it != was_by_shape.end() && it->second.size() <= 8 && (from < 0 || best * 16 >= size))
					for (int k : it->second) {
						const Model &q = was[size_t(k)];
						if (k == pair[j] || !q.meshes || q.meshes == m.meshes || !alike(q)) continue;
						bool us = false;
						const float d = apart(q, us);
						if (from < 0 || (by_place ? d * 16 < best : d < best)) { from = k; best = d; uv_same = us; }   // (with no place partner: the nearest)
					}
				// The same texture coordinates, or (a shiny surface's are made each frame) every vertex moved, its move less
				// the model's average move, less than a quarter of the mesh's size: a joint bends and goes with the limbs
				// (Lau's shiny trousers moved 300-765 against sizes of 380-785 as he switched stance, nearly all of it
				// together: by the whole move they weren't blended while the rigid pieces between them were, his legs in
				// pieces for a picture), where one face put in another's place (the select screen's portraits, a face
				// shutting its eyes) moves its vertices every which way, the average little.
				auto bent = [&](const Model &q) {
					double mean[3] = { 0, 0, 0 };
					size_t n = 0;
					auto moved = [&](size_t k, size_t i, float *d) {
						const float *u = (*q.meshes)[k].verts[i].pos, *w = (*m.meshes)[k].verts[i].pos;
						for (int r = 0; r < 3; r++)
							d[r] = (m.matrix.m[r] * w[0] + m.matrix.m[4 + r] * w[1] + m.matrix.m[8 + r] * w[2] + m.matrix.m[12 + r]) -
							       (q.matrix.m[r] * u[0] + q.matrix.m[4 + r] * u[1] + q.matrix.m[8 + r] * u[2] + q.matrix.m[12 + r]);
					};
					for (size_t k = 0; k < m.meshes->size(); k++)
						for (size_t i = 0; i < (*m.meshes)[k].verts.size(); i++) {
							float d[3];
							moved(k, i, d);
							for (int r = 0; r < 3; r++) mean[r] += d[r];
							n++;
						}
					if (n) for (double &x : mean) x /= double(n);
					float most = 0;
					for (size_t k = 0; k < m.meshes->size(); k++)
						for (size_t i = 0; i < (*m.meshes)[k].verts.size(); i++) {
							float d[3], e = 0;
							moved(k, i, d);
							for (int r = 0; r < 3; r++) e += (d[r] - float(mean[r])) * (d[r] - float(mean[r]));
							most = std::max(most, e);
						}
					return most;
				};
				const bool same = from >= 0 && ((uv_same && by_place) || best < size / 16 || bent(was[size_t(from)]) < size / 16);   // (one found by its vertices: near them)
				if (st) (same ? st->blended : st->not_blended)++;
				if (same) {
					np.verts[v][j] = from + 1;
					blend_vertices(was[size_t(from)], m, t);
					// (its vertices are where they are in the view from that one's matrix: the matrix blended from it
					// too, or the joint was put off by the two partners' difference)
					matrix_from = from;
					np.from[v][j] = from;
				}
			}
			const Model &q = was[size_t(matrix_from)];
			for (int i = 0; i < 16; i++) m.matrix.m[i] = q.matrix.m[i] + (m.matrix.m[i] - q.matrix.m[i]) * t;
			m.alpha = q.alpha + (m.alpha - q.alpha) * t;
		}
	}
	st->total = total;
	st->matched = matched;
	np.st = *st;
	// Most models too far in a frame (or new): a cut, the whole scene shown as it is in b. But not when the rest, a
	// swarm's grains left out, are 32 or more and 70% of them blended: the game puts hundreds of grains somewhere new
	// each frame (Wolf's desert sand, the beach's spray around a fighter's feet: 400-900 of a scene's 900-1400 models,
	// or all put in at once), and they outvoted the rest, the whole scene unblended for frames on end as the fighters
	// moved, a stop then a jump each frame. (A real cut, and a thrown fighter's spin whose pieces turned too far, left
	// 58-63% of the rest blended: those stay cuts, the thrown fighter's pieces not drawn apart for a frame.)
	// And the scene moved over 25% of its distance in a frame (scene_move): a cut, whatever the models' vote (VF1's:
	// always a cut). The stage's models moved less than a quarter of their distance each, or are many of an address,
	// and most were blended across the attract's cuts to a fighter's introduction, his pieces flying across the screen.
	// (The grains shown where they are on purpose, 4, don't vote either way.)
	const size_t vote = total - strays;
	const bool cut = (vote && matched * 2 < vote && !(voters >= 32 && voted * 10 >= voters * 7)) || scene_move(a, b, np.av) > 0.25f;
	np.ok = !cut;
	// the flipbooks: a step from a to b remembered, or the one remembered for a's model carried on (when a's frame
	// was the last plan's b: one frame on)
	{
		const bool chain = plan.b == a.id && a.id && plan.flips.size() == a.viewports.size();
		const std::vector<int> &av = np.av;
		np.flips.resize(np.paired.size());
		for (size_t v = 0; v < np.paired.size(); v++) {
			np.flips[v].resize(np.paired[v].size());
			for (size_t j = 0; j < np.paired[v].size(); j++) {
				const int i = np.paired[v][j];
				// (only a model blended from its partner: one too far from it, or a swarm's grain, is another thing,
				// and its picture isn't this one's step before)
				if (i < 0 || np.from[v][j] < 0) continue;
				const size_t u = size_t(av[v]);   // (paired: in a viewport of a's)
				const Flip *pr = chain && size_t(i) < plan.flips[u].size() && plan.flips[u][size_t(i)].age >= 0 ? &plan.flips[u][size_t(i)] : nullptr;
				Flip &f = np.flips[v][j];
				if (flip_step(a.viewports[u].models[size_t(i)], b.viewports[v].models[j])) {
					f.from = a.viewports[u].models[size_t(i)].meshes;
					f.age = 0;
					f.period = pr && pr->age + 1 >= 2 && pr->age + 1 <= 8 ? pr->age + 1 : 0;
				} else if (pr && pr->age + 1 <= 8) {
					f = *pr;
					f.age++;
				}
			}
		}
	}
	if (a.id && b.id) plan = std::move(np);
	if (cut) {
		out = b;
		return false;
	}
	if (a.id && b.id) apply_flips(b, t, out);
	return true;
}

} // namespace video
