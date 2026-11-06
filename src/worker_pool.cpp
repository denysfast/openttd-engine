/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file worker_pool.cpp A pool of threads that run a batch of independent jobs at once. */

#include "stdafx.h"
#include "worker_pool.h"
#include "thread.h"

#include <cstdlib>

#include "safeguards.h"

/**
 * Create the pool; threads are started on first use.
 * @param thread_name Name of the worker threads.
 * @param env_threads Environment variable that may set the number of threads (including the calling one); by default all hardware threads are used.
 */
WorkerPool::WorkerPool(std::string_view thread_name, const char *env_threads) : thread_name(thread_name)
{
	uint threads = std::thread::hardware_concurrency();
	if (const char *env = std::getenv(env_threads); env != nullptr) {
		threads = 0;
		for (; *env >= '0' && *env <= '9'; env++) threads = threads * 10 + (*env - '0');
	}
	this->wanted_workers = std::min<uint>(threads > 1 ? threads - 1 : 0, MAX_WORKERS);
}

WorkerPool::~WorkerPool()
{
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->stop = true;
	}
	this->start.notify_all();
	for (auto &t : this->workers) {
		if (t.joinable()) t.join();
	}
}

/**
 * Run \a job for every index in [0, \a count) on all threads; returns when all are done.
 * @param count Number of jobs.
 * @param job Function called with the job index.
 */
void WorkerPool::Run(uint count, const std::function<void(uint)> &job)
{
	if (count == 0) return;
	if (count == 1 || this->wanted_workers == 0) {
		for (uint i = 0; i < count; i++) job(i);
		return;
	}

	this->StartWorkers();
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->job = &job;
		this->job_count = count;
		this->next_job.store(0, std::memory_order_relaxed);
		this->busy = static_cast<uint>(this->workers.size());
		this->generation++;
	}
	this->start.notify_all();
	this->Work(job, count);

	std::unique_lock<std::mutex> lock(this->mutex);
	this->done.wait(lock, [this] { return this->busy == 0; });
	this->job = nullptr;
}

void WorkerPool::StartWorkers()
{
	while (this->workers.size() < this->wanted_workers) {
		std::thread &t = this->workers.emplace_back();
		if (!StartNewThread(&t, this->thread_name, [this]() { this->WorkerMain(); })) {
			this->workers.pop_back();
			this->wanted_workers = static_cast<uint>(this->workers.size());
			break;
		}
	}
}

void WorkerPool::Work(const std::function<void(uint)> &job, uint count)
{
	for (uint i; (i = this->next_job.fetch_add(1, std::memory_order_relaxed)) < count;) job(i);
}

void WorkerPool::WorkerMain()
{
	uint seen = 0;
	for (;;) {
		const std::function<void(uint)> *job;
		uint count;
		{
			std::unique_lock<std::mutex> lock(this->mutex);
			this->start.wait(lock, [this, seen] { return this->stop || this->generation != seen; });
			if (this->stop) return;
			seen = this->generation;
			job = this->job;
			count = this->job_count;
		}
		this->Work(*job, count);
		{
			std::lock_guard<std::mutex> lock(this->mutex);
			if (--this->busy == 0) this->done.notify_one();
		}
	}
}

/**
 * Get the threads that do parts of the game loop at once (the results never depend on the number of threads).
 * The environment variable OTTD_SIM_THREADS sets their number (including the game loop's own thread).
 * @return The pool.
 */
WorkerPool &GetSimulationWorkers()
{
	static WorkerPool pool("ottd:sim", "OTTD_SIM_THREADS");
	return pool;
}
