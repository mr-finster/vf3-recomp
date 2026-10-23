// The tile generator's lines drawn on a thread of their own, alongside the game's frame: as the game reaches each
// line its 64 registers are copied (all that changes line by line) and the line is queued; the worker draws it from
// that copy and the tile RAM, a moment behind. A frame's end waits for the last line (finish), so the picture is
// that frame's, as when the lines were drawn in the game's own time. The layers are only seen, never read by the
// game: what they show can't change what the game does (a netplay match's frames are the same either way).
#pragma once
#include "tilegen.h"
#include "../machine/model3.h"
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace video {

class TileWorker
{
public:
	TileWorker(TileGen &t, const m3::Model3 &b) : tiles(t), board(b) {}
	~TileWorker()
	{
		{
			std::lock_guard<std::mutex> lk(m);
			quit = true;
		}
		cv.notify_all();
		if (th.joinable()) th.join();
	}
	// At the start of active line y (the board's on_line): its registers taken, the line queued
	void line(unsigned y)
	{
		if (y >= unsigned(H)) return;
		if (!th.joinable()) th = std::thread([this] { work(); });
		if (y == 0) finish();   // (a frame's first line: the last one's all drawn, the queue empty)
		std::unique_lock<std::mutex> lk(m);
		if (pushed >= H) return;   // (a frame with more lines than the screen's: never)
		std::memcpy(regs[pushed], board.tile_regs, sizeof regs[0]);
		// The layers' order against the 3D and their depths (register 0x20) as the frame began, for all its lines: VF3
		// changes it only between scenes, and a change landing part way down a frame split it (a round's wipe, the stage's
		// sky layer put over the 3D while the next is made ready, taken off a frame early above the line it changed at:
		// a frame's flash of the sky where the game had parked it, pink on the sunset stage)
		static const bool per_line = getenv("VF3_LAYERS_PER_LINE") != nullptr;   // (tests: as it was, line by line)
		if (y == 0) layers_order = board.tile_regs[0x20 / 4];
		else if (!per_line) regs[pushed][0x20 / 4] = layers_order;
		ys[pushed] = int(y);
		pushed++;
		const bool wake = (pushed & 15) == 0 || y == unsigned(H) - 1;   // (a wake every 16 lines, not every line)
		lk.unlock();
		if (wake) cv.notify_one();
	}
	// Once every queued line is drawn, `job` too, on the worker (the layers' composition, while the game's thread
	// builds the frame's 3D); finish waits for it
	void then(std::function<void()> job)
	{
		if (!th.joinable()) { job(); return; }
		{
			std::lock_guard<std::mutex> lk(m);
			after = std::move(job);
		}
		cv.notify_one();
	}
	// Every queued line drawn, and the job after (the frame's end, before its layers are read); the queue emptied
	void finish()
	{
		std::unique_lock<std::mutex> lk(m);
		if (done < pushed || after || running) {
			lk.unlock();
			cv.notify_one();
			lk.lock();
			idle.wait(lk, [this] { return done >= pushed && !after && !running; });
		}
		pushed = done = 0;
	}

private:
	TileGen &tiles;
	const m3::Model3 &board;
	std::thread th;
	std::mutex m;
	std::condition_variable cv, idle;
	uint32_t regs[H][64];
	int ys[H] = {};
	int pushed = 0, done = 0;
	uint32_t layers_order = 0;   // (register 0x20 at the frame's first line)
	bool quit = false, running = false;   // (running: the job after, being done)
	std::function<void()> after;
	void work()
	{
		std::unique_lock<std::mutex> lk(m);
		for (;;) {
			cv.wait(lk, [this] { return quit || done < pushed || (after && done >= pushed); });
			if (quit) return;
			while (done < pushed) {
				const int i = done;
				lk.unlock();
				if (ys[i] == 0) tiles.clear();
				tiles.line(board, unsigned(ys[i]), regs[i]);
				lk.lock();
				done++;
			}
			if (after) {
				std::function<void()> job;
				job.swap(after);   // (after empty now, for certain; running says the job's not done)
				running = true;
				lk.unlock();
				job();
				lk.lock();
				running = false;
			}
			idle.notify_all();
		}
	}
};

} // namespace video
