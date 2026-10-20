// The Real3D's pictures drawn on the CPU from a Scene (r3d_scene.h): the checked reference for the GPU
// renderers, and the renderer where there is no GPU.
//
// The frame is drawn as the Real3D layers it: for each viewport priority (0-3), the opaque polygons, then two
// layers of translucent ones (each keeps only the nearest translucent polygon at a pixel); polygons marked high
// priority are drawn again over the rest with the depth cleared. The result is an opaque layer (with the fog
// layers under it) and the two translucent layers, put together over the tile generator's lower layer.
//
// Size: any; the 496x384 screen is scaled to the height and centred, and viewports that cover the whole screen
// are widened to the full width (widescreen).
#pragma once
#include "r3d_scene.h"
#include <cstdint>
#include <vector>

namespace video {

class Raster
{
public:
	// Output size, and the width the game's 496 x 384 screen takes in it (centred; 0: 496 / 384 of the height,
	// square pixels as the reference draws them; the host's is 4:3, as the arcade's monitor shows it)
	void resize(int width, int height, int game_width = 0);
	// the drawing buffers let go (the 3D on the GPU: only the triangles prepared are used); render() makes them again
	void drop_buffers();
	int width() const { return w; }
	int height() const { return h; }
	int game_x() const { return gx; }
	int game_width() const { return gw; }

	void render(const Scene &scene, const uint16_t *tex_ram);

	// The textures' mipmap levels as the arcade picks them, for its 384 lines (true), or for the picture's own
	// height (false, the default): the level by distance alone, the same at any size, left the distance's textures
	// a level coarser for each doubling of the render scale than the arcade showed them on its screen
	static bool arcade_lod;

	// The 3D over `under` (RGBA8, the tile generator's lower layer at this size), into out (may be `under`)
	void compose(const uint32_t *under, uint32_t *out) const;

	// The 3D as one layer to put over the tile generator's lower layer: RGBA8 (red in the low byte),
	// premultiplied by its alpha
	// (k: antialiasing, each pixel written the average of k x k drawn: width / k by height / k written)
	void to_premultiplied(uint8_t *dst, size_t pitch, int k = 1) const;

	// RGBA float layers (premultiplied not), alpha 0 where nothing was drawn
	struct Px { float r, g, b, a; };
	std::vector<Px> base, t1, t2;

	// The polygons of a picture as drawn: transformed, clipped by the near plane and placed on the output, in the
	// passes' order (the GPU renderer draws from these too)
	struct Draw
	{
		const Viewport *vp;
		const Model *model;
		const Mesh *mesh;
		int x0, y0, x1, y1;   // the viewport's rectangle (clipped to the output)
		float vx, vy, vw, vh; // and as the projection maps it (not clipped)
		float l, r, b, t;     // the frustum's tangents (widened for widescreen)
	};

	struct Tri
	{
		int draw;
		int n;                // 3 to 8 (a triangle clipped by the near plane and the view's sides)
		float sx[8], sy[8], iw[8];
		float a[8][11];       // the attributes over w
	};
	struct Batch { size_t begin = 0, end = 0; };
	struct Priority { bool any = false, has_overlay = false; };
	std::vector<Draw> draws;
	std::vector<Tri> tris;
	Batch batches[4 * 2 * 3];   // per priority, pass (high priority second) and layer
	Priority prio[4];
	void prepare(const Scene &scene);
	float game_xscale() const { return xscale; }
	float game_yscale() const { return scale; }

private:
	void alloc();   // (the drawing buffers at w x h)
	int w = 0, h = 0, gx = 0, gw = 0;
	float scale = 1, xscale = 1;
	std::vector<float> depth, depth_saved;
	std::vector<uint8_t> stencil, stencil_saved;
	const uint16_t *tex = nullptr;
	bool sun_clamp = true;

	void gather(const Viewport &vp, int layer, bool overlay, std::vector<int> &items);
	void prepare_mesh(int draw, std::vector<Tri> &out) const;
	std::vector<std::vector<Tri>> parts_out;
	void draw_band(const Scene &scene, int y0, int y1);
	void fog_layers(const Scene &scene, int y0, int y1);
	void draw_tri(const Tri &t, int layer, int y0, int y1);
};

} // namespace video
