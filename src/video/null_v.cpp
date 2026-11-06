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
	_screen.width  = _screen.pitch = _cur_resolution.width;
	_screen.height = _cur_resolution.height;
	_screen.dst_ptr = nullptr;
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

	/* If requested, make a save just before exit. The normal exit-flow is
	 * not triggered from this driver, so we have to do this manually. */
	if (_settings_client.gui.autosave_on_exit) {
		DoExitSave();
	}
}

bool VideoDriver_Null::ChangeResolution(int, int) { return false; }

bool VideoDriver_Null::ToggleFullscreen(bool) { return false; }
