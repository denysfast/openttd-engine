/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file worker_pool.h A pool of threads that run a batch of independent jobs at once. */

#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

/**
 * Threads that run a batch of independent jobs; the calling thread takes part as well.
 * Jobs are handed out one by one in index order, but may run in any order and on any thread,
 * so their results must not depend on that.
 */
class WorkerPool {
public:
	WorkerPool(std::string_view thread_name, const char *env_threads);
	~WorkerPool();

	/** @return Number of threads that run jobs, including the calling thread. */
	uint Threads() const { return this->wanted_workers + 1; }

	void Run(uint count, const std::function<void(uint)> &job);

private:
	static constexpr uint MAX_WORKERS = 15; ///< More threads hardly help: the jobs are memory bound by then.

	void StartWorkers();
	void Work(const std::function<void(uint)> &job, uint count);
	void WorkerMain();

	std::string thread_name; ///< Name of the worker threads.
	std::vector<std::thread> workers; ///< The worker threads.
	uint wanted_workers = 0; ///< Number of worker threads to use.
	std::mutex mutex; ///< Protects everything below except #next_job.
	std::condition_variable start; ///< Signalled when a new run starts (or on #stop).
	std::condition_variable done; ///< Signalled when the last worker finished a run.
	const std::function<void(uint)> *job = nullptr; ///< Job of the current run.
	uint job_count = 0; ///< Number of jobs of the current run.
	std::atomic<uint> next_job = 0; ///< Next job index to take.
	uint busy = 0; ///< Workers still busy with the current run.
	uint generation = 0; ///< Number of the current run.
	bool stop = false; ///< Set when the pool is destroyed.
};

WorkerPool &GetSimulationWorkers();

#endif /* WORKER_POOL_H */
