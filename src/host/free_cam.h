// The free camera (Display > Free camera, F4, a pad's View + R3): the player flies a camera around the fight, FPS
// style, while the game goes on (or stands still, paused). Drawing only: the board, its RAM and its inputs are never
// changed (the players' inputs are held back from the game while it's on), so a watched match stays in step.
//
// The game's camera is the full-screen viewport's root matrix (r3d_scene.h: the first node's with one, where VF3
// puts its camera; SkyLead reads it too). The free camera is anchored in the world: at its start it is the game's
// camera (the view then, E0), and it is kept as a place and a look in E0's terms (yaw about the world's up, pitch, no
// roll), so the game's camera moving on under it moves nothing. Each picture's models then go through
//     F = V * R0 * R^-1   (V: E0 to the free camera's view; R0: the game's camera when anchored; R: the game's now)
// applied to the model-to-view matrices after smooth motion's in-between (R blended between the two frames as the
// models are), before the one CPU step every renderer draws from (Raster::prepare). The sun's direction (in the view)
// turns with F; the spotlight (a screen-space ellipse) and the fog (by depth) stay the game's.
#pragma once
#include "../video/r3d_scene.h"
#include <algorithm>
#include <cmath>

namespace freecam {

using video::Mat4;
using video::Scene;
using video::Viewport;

constexpr float PI = 3.14159265358979f;
constexpr float DEG = PI / 180.0f;
// The game's sky (the stage's 2D backdrop) as its camera turns: 3968 pixels a whole turn, across and up and down (the
// backdrop's place the game keeps against its camera's yaw, frame by frame over the attract's fights: 631.3 pixels a
// radian across, R2 0.98, whatever the view's angle; 3968 / 2 pi is 631.5. A stage's map is 992 pixels wide, a
// quarter of it, or 3968). The backdrop on screen is that place's 3 frames late: sky_lead.h.
constexpr int SKY_TURN = 3968;
constexpr float SKY_PX = SKY_TURN / (2 * PI);

// What moves it this step: move (right, up, forward: -1 to 1, the stick's or a key's), look in degrees (already
// this step's), speed the multiplier (Shift, Alt, L3)
struct Step
{
	float move[3] = {};
	float yaw = 0, pitch = 0;
	float speed = 1;
};

struct Cam
{
	// The base speed, view units a second: the fighters stand about 5000 units from the game's camera in a fight
	// (measured in the attract's and a 1P game's fights); the ring, about 7500 across, crossed in ~3 s
	static constexpr float BASE_SPEED = 2500.0f, SPEED_MIN = BASE_SPEED / 16, SPEED_MAX = BASE_SPEED * 16;
	bool on = false;
	bool anchored = false;
	float speed = BASE_SPEED;
	Mat4 r0 = Mat4::identity();
	float u[3] = { 0, 1, 0 }, h0[3] = { 0, 0, -1 }, s0[3] = { 1, 0, 0 };   // E0's world up, and the level forward and right at yaw 0
	float p[3] = {};   // the place (in E0)
	float yaw = 0, pitch = 0;
	float pitch0 = 0;   // (the game's camera's when anchored)
	// The last picture's (apply): how far the free camera looks right and up of the game's camera, about the world's
	// up (radians: the sky, a panorama round it, moves by these), and the view's pixels a radian (496 x 384)
	float turn_yaw = 0, turn_pitch = 0, fx = 496, fy = 384;

	static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	static void cross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
	static bool normalise(float *v)
	{
		const float l = std::sqrt(dot(v, v));
		if (!(l > 1e-12f)) return false;
		for (int i = 0; i < 3; i++) v[i] /= l;
		return true;
	}
	// An affine matrix's inverse (false if it has none)
	static bool inverse(const Mat4 &a, Mat4 &r)
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

	// The viewport that is the game's camera: full screen, with a root matrix, the most models (-1: none)
	static int main_viewport(const Scene &s)
	{
		int best = -1;
		for (size_t v = 0; v < s.viewports.size(); v++) {
			const Viewport &vp = s.viewports[v];
			if (!vp.full_screen || vp.root_matrix < 0 || vp.models.empty()) continue;
			if (best < 0 || vp.models.size() > s.viewports[size_t(best)].models.size()) best = int(v);
		}
		return best;
	}

	// The game's camera r as the start: E0 its view; the world's up the axis of the world r turns nearest the view's up
	void anchor(const Mat4 &r)
	{
		r0 = r;
		float best = -1;
		for (int i = 0; i < 3; i++) {
			float c[3] = { r.m[i * 4], r.m[i * 4 + 1], r.m[i * 4 + 2] };
			if (!normalise(c)) continue;
			if (std::fabs(c[1]) > best) { best = std::fabs(c[1]); for (int k = 0; k < 3; k++) u[k] = c[1] < 0 ? -c[k] : c[k]; }
		}
		if (best < 0) { u[0] = 0; u[1] = 1; u[2] = 0; }
		const float f[3] = { 0, 0, -1 };
		const float fu = dot(f, u);
		for (int k = 0; k < 3; k++) h0[k] = f[k] - fu * u[k];
		if (!normalise(h0)) { h0[0] = 0; h0[1] = 0; h0[2] = -1; }   // (looking straight up or down: any level forward)
		cross(h0, u, s0);
		normalise(s0);
		p[0] = p[1] = p[2] = 0;
		yaw = 0;
		pitch = std::asin(std::clamp(fu, -1.0f, 1.0f));
		pitch = std::clamp(pitch, -89 * DEG, 89 * DEG);
		pitch0 = pitch;
		anchored = true;
	}

	void axes(float fwd[3], float right[3], float up[3]) const
	{
		const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		for (int k = 0; k < 3; k++) {
			fwd[k] = cp * (cy * h0[k] + sy * s0[k]) + sp * u[k];
			right[k] = cy * s0[k] - sy * h0[k];
		}
		cross(right, fwd, up);
	}

	// E0 to the free camera's view
	Mat4 view() const
	{
		float f[3], r[3], up[3];
		axes(f, r, up);
		Mat4 v = Mat4::identity();
		for (int k = 0; k < 3; k++) { v.m[k * 4] = r[k]; v.m[k * 4 + 1] = up[k]; v.m[k * 4 + 2] = -f[k]; }
		v.m[12] = -dot(r, p);
		v.m[13] = -dot(up, p);
		v.m[14] = dot(f, p);
		return v;
	}

	// A step of dt seconds
	void step(const Step &s, float dt)
	{
		yaw += s.yaw * DEG;
		yaw = std::remainder(yaw, 2 * PI);
		pitch = std::clamp(pitch + s.pitch * DEG, -89 * DEG, 89 * DEG);
		float f[3], r[3], up[3];
		axes(f, r, up);
		const float d = speed * s.speed * dt;
		for (int k = 0; k < 3; k++) p[k] += (r[k] * s.move[0] + u[k] * s.move[1] + f[k] * s.move[2]) * d;
	}
	void faster(int notches) { speed = std::clamp(speed * std::pow(1.25f, float(notches)), SPEED_MIN, SPEED_MAX); }
	// (tests: moved by x, y, z: right, world up, forward as it looks)
	void move_by(float x, float y, float z)
	{
		float f[3], r[3], up[3];
		axes(f, r, up);
		for (int k = 0; k < 3; k++) p[k] += r[k] * x + u[k] * y + f[k] * z;
	}

	// The scene s seen from the free camera: its full-screen viewports' models through F (the game's camera R the
	// main viewport's root; with prev, the frame before's scene smooth motion blended s from, t of the way, R
	// blended the same). Anchors at the game's camera first if it isn't yet. sky_x, sky_y: its turn from the game's
	// camera (turn_yaw, turn_pitch) in the backdrop's pixels as SkyLead gives a turn (the sky layer moved that much the
	// other way follows it). False: nothing to do (no game camera in it: the game's view).
	bool apply(Scene &s, const Scene *prev, float t, float &sky_x, float &sky_y)
	{
		sky_x = sky_y = 0;
		const int k = main_viewport(s);
		if (k < 0) return false;
		const Viewport &mv = s.viewports[size_t(k)];
		Mat4 r = mv.root;
		if (prev && t < 1)
			for (const Viewport &pv : prev->viewports)
				if (pv.full_screen && pv.root_matrix == mv.root_matrix) {
					for (int i = 0; i < 16; i++) r.m[i] = pv.root.m[i] + (r.m[i] - pv.root.m[i]) * t;
					break;
				}
		if (!anchored) anchor(r);
		Mat4 inv;
		if (!inverse(r, inv)) return false;
		const Mat4 f = view() * (r0 * inv);
		for (Viewport &v : s.viewports) {
			if (!v.full_screen) continue;
			for (video::Model &m : v.models) m.matrix = f * m.matrix;
			// the sun (a direction in the view) turned with it, its length kept
			float sun[3];
			for (int i = 0; i < 3; i++) sun[i] = f.m[i] * v.sun[0] + f.m[4 + i] * v.sun[1] + f.m[8 + i] * v.sun[2];
			const float l0 = std::sqrt(dot(v.sun, v.sun)), l1 = std::sqrt(dot(sun, sun));
			if (l1 > 1e-12f) for (int i = 0; i < 3; i++) v.sun[i] = sun[i] * (l0 / l1);
		}
		// where the game's camera looks now, in E0 (its forward through R0 R^-1), as a yaw and a pitch about the
		// world's up: the free camera's from it
		const Mat4 g = r0 * inv;
		float gf[3] = { -g.m[8], -g.m[9], -g.m[10] };
		turn_yaw = turn_pitch = 0;
		if (normalise(gf)) {
			const float gp = std::asin(std::clamp(dot(gf, u), -1.0f, 1.0f));
			const float gy = std::fabs(dot(gf, u)) < 0.9999f ? std::atan2(dot(gf, s0), dot(gf, h0)) : yaw;
			turn_yaw = std::remainder(yaw - gy, 2 * PI);
			turn_pitch = pitch - gp;
		}
		fx = 496.0f / (mv.right - mv.left);
		fy = 384.0f / (mv.top - mv.bottom);
		sky_x = SKY_PX * turn_yaw;
		sky_y = -SKY_PX * turn_pitch;
		if (!std::isfinite(sky_x) || !std::isfinite(sky_y)) sky_x = sky_y = turn_yaw = turn_pitch = 0;
		return true;
	}
};

} // namespace freecam
