// The Real3D's pictures on the GPU. See r3d_gpu.h; the shaders are src/gpu/r3d.hlsl.
//
// A picture: the texture RAM's changes uploaded (rows of the 2048 x 2048 16-bit texels, into a storage buffer the
// shaders read as words), the polygons' vertices and each mesh's and viewport's state uploaded, then for each
// viewport priority (and its high priority polygons again): the opaque layer into base (the translucent layers
// cleared where it draws, its depth kept as a colour too), the first translucent layer into t1 and the second into
// t2 (each with its own depth: only the nearest translucent polygon of a layer is kept, and only in front of the
// opaque layer), and last the three put together into the output, premultiplied, as the CPU does.
#include "r3d_gpu.h"
#include "r3d_shaders.h"
#include "../machine/model3.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

struct GDraw
{
	float colour[4];
	uint32_t flags;
	int32_t format, tx, ty;
	int32_t tw, th, page, wrap;
	float levels, shininess, spec_value, fog_intensity;
	uint32_t vp;
	float flip;               // a flipbook's step before mixed in (0: none), its picture at flip_x, flip_y
	int32_t flip_x, flip_y;
};
static_assert(sizeof(GDraw) == 80, "as the shader's Draw");

struct GVp
{
	float sun[4], light[4], spot_ellipse[4], spot_range[4], spot_colour[4], spot_fog_colour[4], fog[4], fog2[4];
};

enum : uint32_t { F_TEXTURED = 1, F_ALPHA_TEST = 2, F_TEX_ALPHA = 4, F_LIGHTING = 8, F_FIXED = 16, F_SMOOTH = 32, F_SPECULAR = 64, F_INVERTED = 128 };

const float NEAR = 1e-3f;

} // namespace

R3dGpu::~R3dGpu() { shutdown(); }

void R3dGpu::shutdown()
{
	if (!dev) return;
	SDL_WaitForGPUIdle(dev);
	free_targets();
	for (SDL_GPUGraphicsPipeline *p : { p_opq, p_opq_l, p_tr, p_tr_l, p_fog, p_out })
		if (p) SDL_ReleaseGPUGraphicsPipeline(dev, p);
	for (SDL_GPUBuffer *b : { vbuf, dbuf, pbuf, texram })
		if (b) SDL_ReleaseGPUBuffer(dev, b);
	if (tb) SDL_ReleaseGPUTransferBuffer(dev, tb);
	if (sampler) SDL_ReleaseGPUSampler(dev, sampler);
	p_opq = p_opq_l = p_tr = p_tr_l = p_fog = p_out = nullptr;
	vbuf = dbuf = pbuf = texram = nullptr;
	tb = nullptr;
	sampler = nullptr;
	dev = nullptr;
}

SDL_GPUShader *colour_shader(SDL_GPUDevice *dev)
{
	const SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(dev);
	SDL_GPUShaderCreateInfo ci = {};
	if (have & SDL_GPU_SHADERFORMAT_SPIRV) { ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.code = r3d_fs_colour_spv; ci.code_size = sizeof(r3d_fs_colour_spv); }
	else if (have & SDL_GPU_SHADERFORMAT_DXIL) { ci.format = SDL_GPU_SHADERFORMAT_DXIL; ci.code = r3d_fs_colour_dxil; ci.code_size = sizeof(r3d_fs_colour_dxil); }
	else if (have & SDL_GPU_SHADERFORMAT_MSL) { ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.code = r3d_fs_colour_msl; ci.code_size = sizeof(r3d_fs_colour_msl) - 1; }
	else { SDL_SetError("no shader format this build has"); return nullptr; }
	ci.entrypoint = ci.format == SDL_GPU_SHADERFORMAT_MSL ? "main0" : "main";
	ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	ci.num_samplers = 1;
	ci.num_uniform_buffers = 1;
	return SDL_CreateGPUShader(dev, &ci);
}

const char *R3dGpu::driver() const { return dev ? SDL_GetGPUDeviceDriver(dev) : "none"; }

bool R3dGpu::init(SDL_GPUDevice *device)
{
	dev = device;
	const SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(dev);
	SDL_GPUShaderFormat fmt;
	if (have & SDL_GPU_SHADERFORMAT_SPIRV) fmt = SDL_GPU_SHADERFORMAT_SPIRV;
	else if (have & SDL_GPU_SHADERFORMAT_DXIL) fmt = SDL_GPU_SHADERFORMAT_DXIL;
	else if (have & SDL_GPU_SHADERFORMAT_MSL) fmt = SDL_GPU_SHADERFORMAT_MSL;
	else { err = "no shader format this build has"; return false; }
	auto shader = [&](const uint8_t *spv, size_t nspv, const uint8_t *dxil, size_t ndxil, const uint8_t *msl, size_t nmsl,
					  SDL_GPUShaderStage stage, int samplers, int storage, int uniforms) -> SDL_GPUShader * {
		SDL_GPUShaderCreateInfo ci = {};
		ci.format = fmt;
		if (fmt == SDL_GPU_SHADERFORMAT_SPIRV) { ci.code = spv; ci.code_size = nspv; }
		else if (fmt == SDL_GPU_SHADERFORMAT_DXIL) { ci.code = dxil; ci.code_size = ndxil; }
		else { ci.code = msl; ci.code_size = nmsl - 1; }
		ci.entrypoint = fmt == SDL_GPU_SHADERFORMAT_MSL ? "main0" : "main";
		ci.stage = stage;
		ci.num_samplers = uint32_t(samplers);
		ci.num_storage_buffers = uint32_t(storage);
		ci.num_uniform_buffers = uint32_t(uniforms);
		SDL_GPUShader *s = SDL_CreateGPUShader(dev, &ci);
		if (!s) err = std::string("a shader: ") + SDL_GetError();
		return s;
	};
#define SH(n) r3d_##n##_spv, sizeof(r3d_##n##_spv), r3d_##n##_dxil, sizeof(r3d_##n##_dxil), r3d_##n##_msl, sizeof(r3d_##n##_msl)
	SDL_GPUShader *vs_poly = shader(SH(vs_poly), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
	SDL_GPUShader *fs_opq = shader(SH(fs_opq), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 3, 1);
	SDL_GPUShader *fs_tr = shader(SH(fs_tr), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 3, 1);
	SDL_GPUShader *vs_quad = shader(SH(vs_quad), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 1);
	SDL_GPUShader *fs_fog = shader(SH(fs_fog), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
	SDL_GPUShader *fs_out = shader(SH(fs_out), SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 0, 1);
#undef SH
	auto release = [&] {
		for (SDL_GPUShader *s : { vs_poly, fs_opq, fs_tr, vs_quad, fs_fog, fs_out })
			if (s) SDL_ReleaseGPUShader(dev, s);
	};
	if (!vs_poly || !fs_opq || !fs_tr || !vs_quad || !fs_fog || !fs_out) { release(); return false; }

	// the depth and stencil format: 32-bit float depth if it's here, else 24-bit
	for (SDL_GPUTextureFormat f : { SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT })
		if (SDL_GPUTextureSupportsFormat(dev, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) { ds_format = f; break; }
	if (ds_format == SDL_GPU_TEXTUREFORMAT_INVALID) { release(); err = "no depth and stencil format"; return false; }

	SDL_GPUVertexBufferDescription vb = { 0, sizeof(GV), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
	SDL_GPUVertexAttribute va[6] = {
		{ 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GV, clip) },
		{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GV, view) },
		{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(GV, normal) },
		{ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GV, misc) },
		{ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(GV, lodbase) },
		{ 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(GV, draw) },
	};
	const SDL_GPUTextureFormat F16 = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, F32 = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
	auto pipeline = [&](SDL_GPUShader *vs, SDL_GPUShader *fs, std::initializer_list<SDL_GPUTextureFormat> targets, bool poly,
						bool depth, SDL_GPUCompareOp cmp, bool layered, bool strip) -> SDL_GPUGraphicsPipeline * {
		SDL_GPUGraphicsPipelineCreateInfo ci = {};
		ci.vertex_shader = vs;
		ci.fragment_shader = fs;
		if (poly) {
			ci.vertex_input_state.vertex_buffer_descriptions = &vb;
			ci.vertex_input_state.num_vertex_buffers = 1;
			ci.vertex_input_state.vertex_attributes = va;
			ci.vertex_input_state.num_vertex_attributes = 6;
		}
		ci.primitive_type = strip ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		ci.rasterizer_state.enable_depth_clip = true;   // (no depth clamping: not every GPU has it)
		ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		if (depth) {
			ci.depth_stencil_state.enable_depth_test = true;
			ci.depth_stencil_state.enable_depth_write = true;
			ci.depth_stencil_state.compare_op = cmp;
			if (layered) {
				// the layered polygons: only where none has drawn yet (the stencil's low 7 bits 0), counting each
				SDL_GPUStencilOpState s = { SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP,
					SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP, SDL_GPU_COMPAREOP_EQUAL };
				ci.depth_stencil_state.front_stencil_state = s;
				ci.depth_stencil_state.back_stencil_state = s;
				ci.depth_stencil_state.compare_mask = 0x7F;
				ci.depth_stencil_state.write_mask = 0x7F;
				ci.depth_stencil_state.enable_stencil_test = true;
			}
			ci.target_info.depth_stencil_format = ds_format;
			ci.target_info.has_depth_stencil_target = true;
		}
		std::vector<SDL_GPUColorTargetDescription> cts;
		for (SDL_GPUTextureFormat f : targets) {
			SDL_GPUColorTargetDescription d = {};
			d.format = f;
			cts.push_back(d);
		}
		ci.target_info.color_target_descriptions = cts.data();
		ci.target_info.num_color_targets = uint32_t(cts.size());
		SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &ci);
		if (!p) err = std::string("a pipeline: ") + SDL_GetError();
		return p;
	};
	p_opq = pipeline(vs_poly, fs_opq, { F16, F16, F16, F32 }, true, true, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, false, false);
	p_opq_l = pipeline(vs_poly, fs_opq, { F16, F16, F16, F32 }, true, true, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, true, false);
	p_tr = pipeline(vs_poly, fs_tr, { F16 }, true, true, SDL_GPU_COMPAREOP_GREATER, false, false);
	p_tr_l = pipeline(vs_poly, fs_tr, { F16 }, true, true, SDL_GPU_COMPAREOP_GREATER, true, false);
	p_fog = pipeline(vs_quad, fs_fog, { F16, F16, F16 }, false, false, SDL_GPU_COMPAREOP_ALWAYS, false, true);
	p_out = pipeline(vs_quad, fs_out, { SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM }, false, false, SDL_GPU_COMPAREOP_ALWAYS, false, true);
	release();
	if (!p_opq || !p_opq_l || !p_tr || !p_tr_l || !p_fog || !p_out) return false;
	SDL_GPUSamplerCreateInfo si = {};
	si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
	si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	sampler = SDL_CreateGPUSampler(dev, &si);
	SDL_GPUBufferCreateInfo bi = {};
	bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
	bi.size = 2048 * 2048 * 2;
	texram = SDL_CreateGPUBuffer(dev, &bi);
	if (!sampler || !texram) { err = std::string("its buffers: ") + SDL_GetError(); return false; }
	texram_full = true;
	return true;
}

void R3dGpu::free_targets()
{
	for (SDL_GPUTexture **t : { &base, &t1, &t2, &odepth, &ds, &out })
		if (*t) { SDL_ReleaseGPUTexture(dev, *t); *t = nullptr; }
	w = h = 0;
}

bool R3dGpu::make_targets(int nw, int nh, int nss)
{
	if (nw == w && nh == h && nss == ss && out) return true;
	SDL_WaitForGPUIdle(dev);
	free_targets();
	auto tex = [&](SDL_GPUTextureFormat f, SDL_GPUTextureUsageFlags usage, int k = 1) {
		SDL_GPUTextureCreateInfo ci = {};
		ci.type = SDL_GPU_TEXTURETYPE_2D;
		ci.format = f;
		ci.usage = usage;
		ci.width = uint32_t(nw / k);
		ci.height = uint32_t(nh / k);
		ci.layer_count_or_depth = 1;
		ci.num_levels = 1;
		ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
		return SDL_CreateGPUTexture(dev, &ci);
	};
	const auto CT = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	base = tex(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, CT);
	t1 = tex(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, CT);
	t2 = tex(SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, CT);
	odepth = tex(SDL_GPU_TEXTUREFORMAT_R32_FLOAT, CT);
	ds = tex(ds_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
	out = tex(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, CT, nss);
	if (!base || !t1 || !t2 || !odepth || !ds || !out) {
		err = std::string("its targets: ") + SDL_GetError();
		free_targets();
		return false;
	}
	w = nw;
	h = nh;
	ss = nss;
	return true;
}

SDL_GPUTexture *R3dGpu::output(int nw, int nh, int nss)
{
	return make_targets(nw, nh, std::max(1, nss)) ? out : nullptr;
}

bool R3dGpu::ensure_buffer(SDL_GPUBuffer *&b, uint32_t &size, uint32_t need, SDL_GPUBufferUsageFlags usage)
{
	if (b && size >= need) return true;
	if (b) SDL_ReleaseGPUBuffer(dev, b);
	uint32_t n = std::max<uint32_t>(need + need / 2, 65536);
	SDL_GPUBufferCreateInfo bi = {};
	bi.usage = usage;
	bi.size = n;
	b = SDL_CreateGPUBuffer(dev, &bi);
	size = b ? n : 0;
	if (!b) err = std::string("a buffer: ") + SDL_GetError();
	return b != nullptr;
}

bool R3dGpu::render(const video::Scene &scene, const video::Raster &prep, m3::Model3 &board)
{
	return render(scene, prep, board.tex_ram, board.tex_uploads);
}

bool R3dGpu::render(const video::Scene &scene, const video::Raster &prep, const std::vector<uint16_t> &tex_ram, std::vector<m3::Model3::TexUpload> &tex_uploads)
{
	const auto t0 = std::chrono::steady_clock::now();
	if (!make_targets(prep.width(), prep.height(), ss)) return false;
	using video::Raster;
	// ---- the vertices, meshes' and viewports' state ----
	std::vector<GDraw> gd(prep.draws.size());
	std::vector<GVp> gv(std::max<size_t>(1, scene.viewports.size()));
	for (size_t i = 0; i < scene.viewports.size(); i++) {
		const video::Viewport &v = scene.viewports[i];
		GVp &o = gv[i];
		o = {};
		o.sun[0] = v.sun[0]; o.sun[1] = v.sun[1]; o.sun[2] = v.sun[2]; o.sun[3] = v.sun_intensity;
		o.light[0] = v.ambient; o.light[1] = 1.0f;   // (the sun clamped: Step 1.0's)
		memcpy(o.spot_ellipse, v.spot_ellipse, sizeof(o.spot_ellipse));
		o.spot_range[0] = v.spot_range[0]; o.spot_range[1] = v.spot_range[1];
		memcpy(o.spot_colour, v.spot_colour, 12);
		memcpy(o.spot_fog_colour, v.spot_fog_colour, 12);
		memcpy(o.fog, v.fog_colour, 12);
		o.fog[3] = v.fog_density;
		o.fog2[0] = v.fog_start; o.fog2[1] = v.fog_attenuation; o.fog2[2] = v.fog_ambient;
	}
	for (size_t i = 0; i < prep.draws.size(); i++) {
		const Raster::Draw &d = prep.draws[i];
		const video::Mesh &m = *d.mesh;
		GDraw &o = gd[i];
		o = {};
		const video::Vertex &pv = m.verts[0];
		float k = m.translator_map ? 16.0f : 1.0f;
		o.colour[0] = pv.colour[0] / 255.0f * k; o.colour[1] = pv.colour[1] / 255.0f * k; o.colour[2] = pv.colour[2] / 255.0f * k;
		o.colour[3] = pv.colour[3] / 255.0f * d.model->alpha;
		o.flags = (m.textured ? F_TEXTURED : 0) | (m.alpha_test ? F_ALPHA_TEST : 0) | (m.texture_alpha ? F_TEX_ALPHA : 0) |
			(m.lighting ? F_LIGHTING : 0) | (m.fixed_shading ? F_FIXED : 0) | (m.smooth_shading ? F_SMOOTH : 0) |
			(m.specular ? F_SPECULAR : 0) | (m.inverted ? F_INVERTED : 0);
		o.format = m.format; o.tx = m.x; o.ty = m.y; o.tw = m.width; o.th = m.height;
		o.page = m.page ^ d.model->page;
		o.wrap = m.wrap_u | (m.wrap_v << 2);
		o.levels = m.textured ? std::floor(std::log2(float(std::min(m.width, m.height)))) - 1.0f : 0.0f;
		o.shininess = m.shininess; o.spec_value = m.specular_value; o.fog_intensity = m.fog_intensity;
		o.vp = uint32_t(d.vp - scene.viewports.data());
		o.flip = m.flip; o.flip_x = m.flip_x; o.flip_y = m.flip_y;
	}
	// each polygon (a triangle, or four sides after the near plane) as triangles; their raw attributes (the
	// prepared ones are over w) and clip positions in their viewport's projection
	verts.clear();
	struct Range { size_t first, count; int draw; };
	std::vector<Range> ranges;
	size_t batch_first[24], batch_end[24];
	for (int b = 0; b < 24; b++) {
		batch_first[b] = ranges.size();
		for (size_t ti = prep.batches[b].begin; ti < prep.batches[b].end; ti++) {
			const Raster::Tri &t = prep.tris[ti];
			const Raster::Draw &d = prep.draws[t.draw];
			GV g[8];
			for (int k = 0; k < t.n; k++) {
				const float wv = 1.0f / t.iw[k];
				const float nx = (t.sx[k] - d.vx) / d.vw * 2.0f - 1.0f, ny = 1.0f - (t.sy[k] - d.vy) / d.vh * 2.0f;
				GV &o = g[k];
				o.clip[0] = nx * wv; o.clip[1] = ny * wv; o.clip[2] = NEAR; o.clip[3] = wv;
				for (int q = 0; q < 3; q++) { o.view[q] = t.a[k][q] * wv; o.normal[q] = t.a[k][3 + q] * wv; }
				o.misc[0] = t.a[k][6] * wv; o.misc[1] = t.a[k][7] * wv; o.misc[2] = t.a[k][8] * wv; o.misc[3] = t.a[k][9] * wv;
				o.lodbase = t.a[k][10] * wv;
				o.draw = uint32_t(t.draw);
			}
			if (ranges.size() == batch_first[b] || ranges.back().draw != t.draw)
				ranges.push_back({ verts.size(), 0, t.draw });
			for (int f = 1; f + 1 < t.n; f++) {
				verts.push_back(g[0]); verts.push_back(g[f]); verts.push_back(g[f + 1]);
				ranges.back().count += 3;
			}
		}
		batch_end[b] = ranges.size();
	}
	// ---- the uploads ----
	std::vector<video::Raster::Draw> dummy;
	(void)dummy;
	const uint32_t vbytes = uint32_t(std::max<size_t>(1, verts.size()) * sizeof(GV));
	const uint32_t dbytes = uint32_t(std::max<size_t>(1, gd.size()) * sizeof(GDraw));
	const uint32_t pbytes = uint32_t(gv.size() * sizeof(GVp));
	// texture RAM: whole rows of the areas written (all of it the first time, or after many changes)
	std::vector<std::pair<uint32_t, uint32_t>> rows;   // first row, count
	if (texram_full || tex_uploads.size() > 256) {
		rows.push_back({ 0, 2048 });
	} else {
		for (auto &u : tex_uploads) rows.push_back({ u.y & 2047, std::min<uint32_t>(u.h, 2048 - (u.y & 2047)) });
		std::sort(rows.begin(), rows.end());
		std::vector<std::pair<uint32_t, uint32_t>> merged;
		for (auto &r : rows) {
			if (!merged.empty() && r.first <= merged.back().first + merged.back().second)
				merged.back().second = std::max(merged.back().second, r.first + r.second - merged.back().first);
			else merged.push_back(r);
		}
		rows = merged;
	}
	// (the rows asked for are taken now; if a step below fails, the whole of texture memory goes up next time)
	tex_uploads.clear();
	texram_full = true;
	if (tex_ram.size() < size_t(2048) * 2048) { err = "no texture memory yet"; return false; }
	uint32_t tbytes = 0;
	for (auto &r : rows) tbytes += r.second * 4096;
	if (!ensure_buffer(vbuf, vbuf_size, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX) ||
		!ensure_buffer(dbuf, dbuf_size, dbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
		!ensure_buffer(pbuf, pbuf_size, pbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ))
		return false;
	const uint32_t total = vbytes + dbytes + pbytes + tbytes;
	if (!tb || tb_size < total) {
		if (tb) SDL_ReleaseGPUTransferBuffer(dev, tb);
		SDL_GPUTransferBufferCreateInfo ti = {};
		ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		ti.size = std::max<uint32_t>(total + total / 2, 1 << 20);
		tb = SDL_CreateGPUTransferBuffer(dev, &ti);
		tb_size = tb ? ti.size : 0;
		if (!tb) { err = std::string("a transfer buffer: ") + SDL_GetError(); return false; }
	}
	uint8_t *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, tb, true));
	if (!map) { err = std::string("mapping: ") + SDL_GetError(); return false; }
	if (!verts.empty()) memcpy(map, verts.data(), verts.size() * sizeof(GV));
	if (!gd.empty()) memcpy(map + vbytes, gd.data(), gd.size() * sizeof(GDraw));
	memcpy(map + vbytes + dbytes, gv.data(), pbytes);
	{
		uint32_t at = vbytes + dbytes + pbytes;
		for (auto &r : rows) {
			memcpy(map + at, tex_ram.data() + size_t(r.first) * 2048, size_t(r.second) * 4096);
			at += r.second * 4096;
		}
	}
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmd) { err = std::string("a command buffer: ") + SDL_GetError(); return false; }
	{
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		auto up = [&](uint32_t from, SDL_GPUBuffer *b, uint32_t to, uint32_t n) {
			if (!n) return;
			SDL_GPUTransferBufferLocation src = { tb, from };
			SDL_GPUBufferRegion dst = { b, to, n };
			SDL_UploadToGPUBuffer(cp, &src, &dst, false);
		};
		up(0, vbuf, 0, vbytes);
		up(vbytes, dbuf, 0, dbytes);
		up(vbytes + dbytes, pbuf, 0, pbytes);
		uint32_t at = vbytes + dbytes + pbytes;
		for (auto &r : rows) { up(at, texram, r.first * 4096, r.second * 4096); at += r.second * 4096; }
		SDL_EndGPUCopyPass(cp);
	}
	// ---- the passes ----
	auto colour = [](SDL_GPUTexture *t, bool clear) {
		SDL_GPUColorTargetInfo c = {};
		c.texture = t;
		c.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		c.store_op = SDL_GPU_STOREOP_STORE;
		c.clear_color = { 0, 0, 0, 0 };
		return c;
	};
	auto depth_target = [&] {
		SDL_GPUDepthStencilTargetInfo d = {};
		d.texture = ds;
		d.clear_depth = 0.0f;
		d.load_op = SDL_GPU_LOADOP_CLEAR;
		d.store_op = SDL_GPU_STOREOP_DONT_CARE;
		d.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
		d.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		d.clear_stencil = 0;
		return d;
	};
	const float frame_u[4] = { float(prep.game_x()), prep.game_xscale(), prep.game_yscale(), 0 };
	// the fog layers first, into the cleared layers
	{
		SDL_GPUColorTargetInfo c[3] = { colour(base, true), colour(t1, true), colour(t2, true) };
		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, c, 3, nullptr);
		if (scene.blank) {
			// block culling: the 3D white
			SDL_BindGPUGraphicsPipeline(rp, p_fog);
			const float rect[4] = { -1, -1, 1, 1 };
			const float fog[16] = { 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, frame_u[0], frame_u[1], frame_u[2], 0 };
			SDL_PushGPUVertexUniformData(cmd, 0, rect, sizeof(rect));
			SDL_PushGPUFragmentUniformData(cmd, 0, fog, sizeof(fog));
			SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
		} else {
			auto vp_px = [&](const video::Viewport &v, int &x0, int &y0, int &x1, int &y1) {
				const float sc = prep.game_yscale(), xs = prep.game_xscale();
				if (v.full_screen) { x0 = 0; x1 = prep.width(); }
				else { x0 = prep.game_x() + int(v.vp_x * xs); x1 = x0 + int(v.vp_w * xs); }
				y0 = int(v.vp_y * sc); y1 = y0 + int(v.vp_h * sc);
				x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(prep.width(), x1); y1 = std::min(prep.height(), y1);
			};
			auto fill = [&](const video::Viewport &v, const float *fog) {
				int x0, y0, x1, y1;
				vp_px(v, x0, y0, x1, y1);
				if (x0 >= x1 || y0 >= y1) return;
				SDL_GPUViewport gvp = { float(x0), float(y0), float(x1 - x0), float(y1 - y0), 0, 1 };
				SDL_SetGPUViewport(rp, &gvp);
				SDL_Rect sc = { x0, y0, x1 - x0, y1 - y0 };
				SDL_SetGPUScissor(rp, &sc);
				SDL_BindGPUGraphicsPipeline(rp, p_fog);
				const float rect[4] = { -1, -1, 1, 1 };
				SDL_PushGPUVertexUniformData(cmd, 0, rect, sizeof(rect));
				SDL_PushGPUFragmentUniformData(cmd, 0, fog, 16 * sizeof(float));
				SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
			};
			// the ambient fog (as the CPU's fog_layers)
			{
				float amb = 1.0f;
				const video::Viewport *pick = nullptr;
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
					const float fog[16] = { 0, 0, 0, 1.0f - amb, 0, 0, 0, 0, 0, 0, 1, 1, frame_u[0], frame_u[1], frame_u[2], 0 };
					fill(*pick, fog);
				}
			}
			const video::Viewport *pick = nullptr;
			for (int i = 0; i < 4 && !pick; i++)
				for (auto &v : scene.viewports)
					if (v.priority == i && v.scroll_fog > 0 && (!pick || pick->scroll_fog < v.scroll_fog)) pick = &v;
			if (pick)
				for (auto &n : scene.viewports) {
					if (memcmp(n.fog_colour, pick->fog_colour, sizeof(n.fog_colour))) continue;
					bool spot_on_fog = n.spot_fog_colour[0] > 0 || n.spot_fog_colour[1] > 0 || n.spot_fog_colour[2] > 0;
					bool fogon = n.fog_density > 0 || n.fog_start > 0 || n.scroll_att > 0;
					bool spot_fog = n.fog_attenuation > 0 && spot_on_fog;
					if (!fogon && !spot_fog) continue;
					float att = (fogon && spot_fog) ? 0.0f : n.fog_attenuation;
					float fog[16] = { pick->fog_colour[0] * n.fog_ambient, pick->fog_colour[1] * n.fog_ambient, pick->fog_colour[2] * n.fog_ambient,
						pick->scroll_fog, att * n.spot_fog_colour[0] * pick->fog_colour[0], att * n.spot_fog_colour[1] * pick->fog_colour[1],
						att * n.spot_fog_colour[2] * pick->fog_colour[2], 1, n.spot_ellipse[0], n.spot_ellipse[1], n.spot_ellipse[2],
						n.spot_ellipse[3], frame_u[0], frame_u[1], frame_u[2], 0 };
					fill(*pick, fog);
					break;
				}
		}
		SDL_EndGPURenderPass(rp);
	}
	if (!scene.blank) {
		SDL_GPUBufferBinding vb = { vbuf, 0 };
		SDL_GPUBuffer *storage[3] = { texram, dbuf, pbuf };
		auto draw_batch = [&](SDL_GPURenderPass *rp, int b, SDL_GPUGraphicsPipeline *plain, SDL_GPUGraphicsPipeline *layered, SDL_GPUTexture *od) {
			SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
			SDL_GPUTextureSamplerBinding sb = { od, sampler };
			int last_vp = -1;
			SDL_GPUGraphicsPipeline *bound = nullptr;
			for (size_t r = batch_first[b]; r < batch_end[b]; r++) {
				const Range &rg = ranges[r];
				const Raster::Draw &d = prep.draws[rg.draw];
				SDL_GPUGraphicsPipeline *p = d.mesh->layered ? layered : plain;
				if (p != bound) {
					SDL_BindGPUGraphicsPipeline(rp, p);
					SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
					SDL_BindGPUFragmentStorageBuffers(rp, 0, storage, 3);
					SDL_PushGPUFragmentUniformData(cmd, 0, frame_u, sizeof(frame_u));
					SDL_SetGPUStencilReference(rp, 0);
					bound = p;
				}
				const int vi = int(d.vp - scene.viewports.data());
				if (vi != last_vp) {
					SDL_GPUViewport gvp = { d.vx, d.vy, d.vw, d.vh, 0, 1 };
					SDL_SetGPUViewport(rp, &gvp);
					SDL_Rect sc = { d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0 };
					SDL_SetGPUScissor(rp, &sc);
					last_vp = vi;
				}
				SDL_DrawGPUPrimitives(rp, uint32_t(rg.count), 1, uint32_t(rg.first), 0);
			}
		};
		for (int pri = 0; pri < 4; pri++) {
			if (!prep.prio[pri].any) continue;
			for (int pass = 0; pass < 2; pass++) {
				const int b = (pri * 2 + pass) * 3;
				{
					SDL_GPUColorTargetInfo c[4] = { colour(base, false), colour(t1, false), colour(t2, false), colour(odepth, true) };
					SDL_GPUDepthStencilTargetInfo dt = depth_target();
					SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, c, 4, &dt);
					// (the sampled texture must not be a target of this pass: the translucent layer's, unused here)
					draw_batch(rp, b, p_opq, p_opq_l, out);
					SDL_EndGPURenderPass(rp);
				}
				for (int layer = 1; layer <= 2; layer++) {
					if (prep.batches[b + layer].begin == prep.batches[b + layer].end) continue;
					SDL_GPUColorTargetInfo c = colour(layer == 1 ? t1 : t2, false);
					SDL_GPUDepthStencilTargetInfo dt = depth_target();
					SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &c, 1, &dt);
					draw_batch(rp, b + layer, p_tr, p_tr_l, odepth);
					SDL_EndGPURenderPass(rp);
				}
				if (!prep.prio[pri].has_overlay) break;
			}
		}
	}
	// the three layers put together
	{
		SDL_GPUColorTargetInfo c = colour(out, true);
		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &c, 1, nullptr);
		SDL_BindGPUGraphicsPipeline(rp, p_out);
		SDL_GPUTextureSamplerBinding sb[3] = { { base, sampler }, { t1, sampler }, { t2, sampler } };
		SDL_BindGPUFragmentSamplers(rp, 0, sb, 3);
		SDL_GPUViewport gvp = { 0, 0, float(w / ss), float(h / ss), 0, 1 };
		SDL_SetGPUViewport(rp, &gvp);
		const float rect[4] = { -1, -1, 1, 1 };
		SDL_PushGPUVertexUniformData(cmd, 0, rect, sizeof(rect));
		const int32_t k[4] = { ss, 0, 0, 0 };
		SDL_PushGPUFragmentUniformData(cmd, 0, k, sizeof(k));
		SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
		SDL_EndGPURenderPass(rp);
	}
	last_cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (!SDL_SubmitGPUCommandBuffer(cmd)) { err = std::string("submitting: ") + SDL_GetError(); return false; }
	texram_full = false;
	return true;
}

bool R3dGpu::read_output(std::vector<uint32_t> &px)
{
	if (!out) return false;
	SDL_GPUTransferBufferCreateInfo ti = {};
	ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	const int ow = w / ss, oh = h / ss;
	ti.size = uint32_t(ow) * uint32_t(oh) * 4;
	SDL_GPUTransferBuffer *dl = SDL_CreateGPUTransferBuffer(dev, &ti);
	if (!dl) return false;
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmd) { SDL_ReleaseGPUTransferBuffer(dev, dl); return false; }
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = {};
	src.texture = out;
	src.w = uint32_t(ow);
	src.h = uint32_t(oh);
	src.d = 1;
	SDL_GPUTextureTransferInfo dst = { dl, 0, uint32_t(ow), uint32_t(oh) };
	SDL_DownloadFromGPUTexture(cp, &src, &dst);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	bool ok = f && SDL_WaitForGPUFences(dev, true, &f, 1);
	if (f) SDL_ReleaseGPUFence(dev, f);
	if (ok) {
		const uint8_t *m = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, dl, false));
		if (m) {
			px.resize(size_t(ow) * oh);
			memcpy(px.data(), m, px.size() * 4);
			SDL_UnmapGPUTransferBuffer(dev, dl);
		} else ok = false;
	}
	SDL_ReleaseGPUTransferBuffer(dev, dl);
	return ok;
}
