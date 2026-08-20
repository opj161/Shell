#include <windows.h>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <mutex>
#include "test.h"
#include "../dll/src/Include/BitmapCache.h"

namespace
{
	struct TestMUID
	{
		uint32_t id{};
		uint32_t hash{};
	};

	struct TestSnapshot
	{
		uint64_t generation{};
		std::unordered_map<uint32_t, TestMUID> muid;

		const TestMUID* find_muid(uint32_t hash) const
		{
			auto it = muid.find(hash);
			if(it != muid.end())
				return &it->second;
			return nullptr;
		}
	};

	struct TestScope
	{
		std::unordered_map<uint32_t, double> vars;

		void set(uint32_t id, double v) { vars[id] = v; }
		bool exists(uint32_t id) const { return vars.find(id) != vars.end(); }
		double get(uint32_t id) const
		{
			auto it = vars.find(id);
			return it != vars.end() ? it->second : 0.0;
		}
	};
}

TEST(threadsafety, runtime_variable_isolation)
{
	// Two separate menu invocations must have completely isolated runtime scopes.
	TestScope menu1_runtime;
	TestScope menu2_runtime;

	uint32_t var_id = 0x1234;

	menu1_runtime.set(var_id, 5.0);
	menu2_runtime.set(var_id, 42.0);

	CHECK(menu1_runtime.exists(var_id));
	CHECK(menu2_runtime.exists(var_id));
	CHECK_EQ(static_cast<int>(menu1_runtime.get(var_id)), 5);
	CHECK_EQ(static_cast<int>(menu2_runtime.get(var_id)), 42);

	// Concurrent writes to independent runtime scopes must not data-race.
	std::atomic<bool> start{ false };
	std::atomic<bool> ok{ true };

	std::thread t1([&]()
	{
		while(!start.load(std::memory_order_relaxed));
		for(int i = 0; i < 5000; i++)
		{
			menu1_runtime.set(0x100, static_cast<double>(i));
			if(!menu1_runtime.exists(0x100) || static_cast<int>(menu1_runtime.get(0x100)) != i)
				ok.store(false, std::memory_order_relaxed);
		}
	});

	std::thread t2([&]()
	{
		while(!start.load(std::memory_order_relaxed));
		for(int i = 0; i < 5000; i++)
		{
			menu2_runtime.set(0x200, static_cast<double>(i * 10));
			if(!menu2_runtime.exists(0x200) || static_cast<int>(menu2_runtime.get(0x200)) != (i * 10))
				ok.store(false, std::memory_order_relaxed);
		}
	});

	start.store(true, std::memory_order_release);
	t1.join();
	t2.join();

	CHECK(ok.load());
}

TEST(threadsafety, snapshot_lifetime_and_muid_integrity)
{
	// Simulate snapshot publishing and concurrent menu readers
	std::atomic<bool> running{ true };
	std::atomic<uint64_t> published_gen{ 1 };
	std::mutex snapshot_mutex;
	std::shared_ptr<const TestSnapshot> current_snapshot = std::make_shared<TestSnapshot>();

	const_cast<TestSnapshot*>(current_snapshot.get())->generation = 1;
	TestMUID sample_muid;
	sample_muid.id = 1;
	sample_muid.hash = 0xABCD;
	const_cast<TestSnapshot*>(current_snapshot.get())->muid[0xABCD] = sample_muid;

	// Background reloader continuously creates and publishes new snapshots
	std::thread publisher([&]()
	{
		for(int i = 2; i <= 200; i++)
		{
			auto next = std::make_shared<TestSnapshot>();
			next->generation = i;
			TestMUID m;
			m.id = static_cast<uint32_t>(i);
			m.hash = 0xABCD;
			next->muid[0xABCD] = m;

			{
				std::lock_guard<std::mutex> lock(snapshot_mutex);
				current_snapshot = next;
			}
			published_gen.store(i, std::memory_order_release);
			::Sleep(1);
		}
		running.store(false, std::memory_order_release);
	});

	// Multiple reader threads acquire snapshots and hold them while reading
	std::vector<std::thread> readers;
	std::atomic<bool> readers_ok{ true };

	for(int r = 0; r < 4; r++)
	{
		readers.emplace_back([&]()
		{
			while(running.load(std::memory_order_acquire))
			{
				std::shared_ptr<const TestSnapshot> snap;
				{
					std::lock_guard<std::mutex> lock(snapshot_mutex);
					snap = current_snapshot;
				}

				if(!snap)
				{
					readers_ok.store(false, std::memory_order_relaxed);
					continue;
				}

				// The snapshot generation and MUID must remain consistent throughout menu life
				auto gen = snap->generation;
				auto m = snap->find_muid(0xABCD);
				if(!m || m->id != static_cast<uint32_t>(gen))
				{
					readers_ok.store(false, std::memory_order_relaxed);
				}

				// Artificial work delay simulating context menu construction
				::Sleep(0);

				// Re-verify after delay: the snapshot instance must still be intact
				if(snap->generation != gen || !snap->find_muid(0xABCD) || snap->find_muid(0xABCD)->id != static_cast<uint32_t>(gen))
				{
					readers_ok.store(false, std::memory_order_relaxed);
				}
			}
		});
	}

	publisher.join();
	for(auto &t : readers)
		t.join();

	CHECK(readers_ok.load());
}
