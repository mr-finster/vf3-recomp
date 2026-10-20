// The Real3D's pictures on the GPU, with SDL_GPU (Vulkan, Direct3D 12, Metal): the CPU's polygons as prepared for
// its own rasteriser (video::Raster::prepare: transformed, clipped by the near plane, in the passes' order), drawn
// by the GPU's rasteriser with the CPU's shading (src/gpu/r3d.hlsl) in the same layers and passes, into a texture
// of the host's renderer: the picture is put together with the 2D there, on the GPU, and never read back (but for
// the start-up check against the CPU's picture).
#pragma once
#include "../video/r3d_raster.h"
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

#include "../machine/model3.h"

// The CRT colours' fragment shader (r3d.hlsl's FS_COLOUR) on a device, for the host renderer's SDL_GPURenderState:
// its picture the texture drawn, sampler 0; uniforms 0: three float4, the matrix's rows, the first's w the gamma.
// Null if it can't be made here (SDL_GetError()).
SDL_GPUShader *colour_shader(SDL_GPUDevice *dev);

class R3dGpu
{
public:
	~R3dGpu();
	// Lets go of everything on the device (before the renderer that owns it goes)
	void shutdown();
	// On the host renderer's device (SDL_GetGPURendererDevice); false if its shaders can't be made here
	bool init(SDL_GPUDevice *device);
	const std::string &error() const { return err; }
	const char *driver() const;

	// The output texture (made here, RGBA8, premultiplied), for the renderer to show: the picture drawn w x h, shown
	// w / ss x h / ss (antialiasing: each pixel the average of ss x ss drawn)
	SDL_GPUTexture *output(int w, int h, int ss = 1);
	// Draws a picture: the scene as `prep` prepared it (prep.prepare(scene) done, at the output's size), the
	// texture RAM from the board (its changes since the last picture: board.tex_uploads, taken here).
	// Submitted to the GPU; the renderer's own commands after it see the picture.
	bool render(const video::Scene &scene, const video::Raster &prep, m3::Model3 &board);
	// (the texture RAM and its areas written given apart from the board: a copy of them, the game running alongside)
	bool render(const video::Scene &scene, const video::Raster &prep, const std::vector<uint16_t> &tex_ram, std::vector<m3::Model3::TexUpload> &tex_uploads);
	// The output read back (tests and the start-up check only): RGBA8
	bool read_output(std::vector<uint32_t> &px);

	double last_cpu_ms = 0;   // the CPU's share of the last picture (setting it up)

private:
	SDL_GPUDevice *dev = nullptr;
	std::string err;
	SDL_GPUGraphicsPipeline *p_opq = nullptr, *p_opq_l = nullptr, *p_tr = nullptr, *p_tr_l = nullptr, *p_fog = nullptr, *p_out = nullptr;
	SDL_GPUSampler *sampler = nullptr;
	SDL_GPUTextureFormat ds_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	int w = 0, h = 0, ss = 1;
	SDL_GPUTexture *base = nullptr, *t1 = nullptr, *t2 = nullptr, *odepth = nullptr, *ds = nullptr, *out = nullptr;
	SDL_GPUBuffer *vbuf = nullptr, *dbuf = nullptr, *pbuf = nullptr, *texram = nullptr;
	uint32_t vbuf_size = 0, dbuf_size = 0, pbuf_size = 0;
	SDL_GPUTransferBuffer *tb = nullptr;
	uint32_t tb_size = 0;
	bool texram_full = true;
	struct GV { float clip[4], view[3], normal[3], misc[4], lodbase; uint32_t draw; };
	std::vector<GV> verts;
	void free_targets();
	bool make_targets(int w, int h, int ss);
	bool ensure_buffer(SDL_GPUBuffer *&b, uint32_t &size, uint32_t need, SDL_GPUBufferUsageFlags usage);
};
