/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file null_v.cpp The video driver that doesn't blit. */

#include "../stdafx.h"
#include "../gfx_func.h"
#include "../blitter/factory.hpp"
#include "../saveload/saveload_func.h"
#include "../window_func.h"
#include "../openttd.h"
#include "../linkgraph/linkgraphschedule.h"
#include "../screenshot.h"
#include "null_v.h"

#include <chrono>
#include <numeric>
#include <thread>

#include "../safeguards.h"

/** Factory for the null video driver. */
static FVideoDriver_Null iFVideoDriver_Null;

std::optional<std::string_view> VideoDriver_Null::Start(const StringList &parm)
{
#ifdef _MSC_VER
	/* Disable the MSVC assertion message box. */
	_set_error_mode(_OUT_TO_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

	this->UpdateAutoResolution();

	this->ticks = GetDriverParamInt(parm, "ticks", 1000);
	this->bench = GetDriverParamBool(parm, "bench");
	this->draw = GetDriverParamBool(parm, "draw");
	this->full_redraw = GetDriverParamBool(parm, "fullredraw");
	if (auto shot = GetDriverParam(parm, "shot"); shot.has_value()) this->shot = *shot;
	this->verify_draw = GetDriverParamBool(parm, "verifydraw");
	_screen.width  = _screen.pitch = _cur_resolution.width;
	_screen.height = _cur_resolution.height;
	_screen.dst_ptr = nullptr;

	if (this->draw) {
		/* Benchmark the drawing too: keep the selected blitter and draw into memory. */
		int bpp = BlitterFactory::GetCurrentBlitter()->GetScreenDepth();
		if (bpp != 0) this->video_mem = std::make_unique<uint8_t[]>(static_cast<size_t>(_cur_resolution.width) * _cur_resolution.height * (bpp / 8));
		_screen.dst_ptr = this->video_mem.get();
		ScreenSizeChanged();
		BlitterFactory::GetCurrentBlitter()->PostResize();
		return std::nullopt;
	}
	ScreenSizeChanged();

	/* Do not render, nor blit */
	Debug(Facility::Misc, Severity::Error, "Forcing blitter 'null'...");
	BlitterFactory::SelectBlitter("null");
	return std::nullopt;
}

void VideoDriver_Null::Stop() { }

void VideoDriver_Null::MakeDirty(int, int, int, int) {}

void VideoDriver_Null::MainLoop()
{
	uint i;

	/* Benchmark mode: wall time of every running tick after the first one (which loads the game).
	 * Ticks spent paused while waiting for a link graph job are not counted (a job restarted
	 * by loading a savegame may be due long before it can finish); the wait is reported. */
	std::vector<double> times;
	double paused_ms = 0;
	if (this->bench) times.reserve(this->ticks);

	for (i = 0; i < this->ticks; i++) {
		if (this->bench) {
			/* Wait for a link graph job that is due before the game loop notices and pauses the game;
			 * the pause itself depends on timing, and paused ticks still change a little state.
			 * Waiting keeps a benchmark run deterministic, so its end state can be compared. */
			auto wait_start = std::chrono::steady_clock::now();
			while (LinkGraphSchedule::instance.IsJoinWithUnfinishedJobDue()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			paused_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
		}

		auto start = std::chrono::steady_clock::now();
		::GameLoop();
		::InputLoop();
		if (this->full_redraw && i > 0) MarkWholeScreenDirty();
		::UpdateWindows();
		if (this->bench && i > 0) {
			if (_pause_mode.Test(PauseMode::LinkGraph)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				paused_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				if (paused_ms < 600000) i--; // retry this tick, but give up after 10 minutes of waiting
				continue;
			}
			times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		}
	}

	if (this->bench && !times.empty()) {
		double total = std::accumulate(times.begin(), times.end(), 0.0);
		std::sort(times.begin(), times.end());
		auto pct = [&times](double p) { return times[std::min(times.size() - 1, static_cast<size_t>(p * times.size()))]; };
		fmt::print(stderr, "bench: ticks {} mean {:.3f} ms  p10 {:.3f}  median {:.3f}  p90 {:.3f}  max {:.3f}  link-graph wait {:.0f} ms\n",
			times.size(), total / times.size(), pct(0.1), pct(0.5), pct(0.9), times.back(), paused_ms);
		extern void PrintFramerateToStderr();
		PrintFramerateToStderr();
	}

	if (this->draw && this->verify_draw && this->video_mem != nullptr) {
		/* Draw the same frame with one thread and with several; the screen must be identical. */
		extern bool _viewport_draw_parallel;
		size_t bytes = static_cast<size_t>(_screen.pitch) * _screen.height * (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() / 8);
		auto draw_all = [](bool parallel) {
			_viewport_draw_parallel = parallel;
			MarkWholeScreenDirty();
			DrawDirtyBlocks();
		};
		draw_all(false); // warm up: resolves sprites that are only resolved on drawing
		draw_all(false);
		std::vector<uint8_t> serial(this->video_mem.get(), this->video_mem.get() + bytes);
		auto compare = [&](std::string_view what) {
			size_t diff = 0;
			size_t bpp = BlitterFactory::GetCurrentBlitter()->GetScreenDepth() / 8;
			std::string where;
			for (size_t b = 0; b < bytes; b++) {
				if (serial[b] == this->video_mem[b]) continue;
				if (diff++ < 12) where += fmt::format(" ({},{})", (b / bpp) % _screen.pitch, (b / bpp) / _screen.pitch);
			}
			fmt::print(stderr, "bench: verify draw ({}): {} of {} bytes differ{}\n", what, diff, bytes, where);
		};
		draw_all(false);
		compare("serial again");
		draw_all(true);
		compare("parallel");

		/* Many small dirty rectangles, like moving vehicles cause: drawn all at once by the main viewport. */
		auto draw_scattered = [](bool parallel) {
			_viewport_draw_parallel = parallel;
			for (int y = 0; y < _screen.height; y += 96) {
				for (int x = (y / 96 % 2) * 128; x < _screen.width; x += 320) AddDirtyBlock(x, y, x + 200, y + 40);
			}
			DrawDirtyBlocks();
		};
		draw_all(false);
		draw_scattered(false);
		std::copy_n(this->video_mem.get(), bytes, serial.begin());
		draw_all(false);
		draw_scattered(false);
		compare("serial, scattered rectangles again");
		draw_all(false);
		draw_scattered(true);
		compare("parallel, scattered rectangles");
		_viewport_draw_parallel = true;
	}

	/* Benchmark: a raw screenshot of the last frame, to compare drawing between builds. */
	if (this->draw && !this->shot.empty()) MakeScreenshot(SC_CRASHLOG, this->shot);

	/* If requested, make a save just before exit. The normal exit-flow is
	 * not triggered from this driver, so we have to do this manually. */
	if (_settings_client.gui.autosave_on_exit) {
		DoExitSave();
	}
}

bool VideoDriver_Null::ChangeResolution(int, int) { return false; }

bool VideoDriver_Null::ToggleFullscreen(bool) { return false; }
