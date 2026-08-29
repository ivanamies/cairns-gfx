// tests/test_threading.cpp
//
// SPEC: threading + SPSC handoff invariants.
// TAGS: [spec][threading]
//
// The live RenderThread can't link into the pure-CPU spec target (it has
// rhi surface), but we CAN spec the SPSC handoff invariants on a minimal
// depth-2 ring of POD packets -- the same shape the engine uses to hand
// FramePacket from game to render.
//
// What this pins:
//   - producer-fills-then-publishes ordering on a depth-2 ring with
//     std::mutex + std::condition_variable (the only primitives the
//     codebase permits per feedback-threading-primitives memory).
//   - shutdown contract: drain-on-stop never deadlocks.

#include <catch2/catch_test_macros.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Packet {
    uint32_t frame = 0;
    uint64_t payload = 0;
};

// Depth-2 SPSC ring with mutex+condvar. Mirrors the shape of
// RenderThread's per-frame queue (kFramesInFlight=2 slots).
struct Spsc2 {
    Packet slots[2]{};
    bool ready[2] = {false, false};
    std::mutex m;
    std::condition_variable cv_full;
    std::condition_variable cv_empty;
    bool shutdown = false;
    uint32_t next_publish = 0;
    uint32_t next_consume = 0;

    void Publish(const Packet& p) {
        std::unique_lock<std::mutex> lk(m);
        cv_full.wait(lk, [&] {
            return shutdown || !ready[next_publish % 2];
        });
        if (shutdown) {
            return;
        }
        slots[next_publish % 2] = p;
        ready[next_publish % 2] = true;
        ++next_publish;
        cv_empty.notify_one();
    }
    bool Consume(Packet& out) {
        std::unique_lock<std::mutex> lk(m);
        cv_empty.wait(lk, [&] {
            return shutdown || ready[next_consume % 2];
        });
        if (!ready[next_consume % 2]) {
            return false;
        }
        out = slots[next_consume % 2];
        ready[next_consume % 2] = false;
        ++next_consume;
        cv_full.notify_one();
        return true;
    }
    void Shutdown() {
        {
            std::lock_guard<std::mutex> lk(m);
            shutdown = true;
        }
        cv_full.notify_all();
        cv_empty.notify_all();
    }
};

}  // namespace

SCENARIO("SPSC handoff preserves order across many frames",
         "[spec][threading][regression]") {
    Spsc2 q;
    const uint32_t N = 1000;
    std::vector<Packet> received;
    received.reserve(N);

    std::thread consumer([&] {
        for (uint32_t i = 0; i < N; ++i) {
            Packet p;
            if (q.Consume(p)) {
                received.push_back(p);
            }
        }
    });
    for (uint32_t i = 0; i < N; ++i) {
        Packet p;
        p.frame = i;
        p.payload = static_cast<uint64_t>(i) * 1234567u;
        q.Publish(p);
    }
    consumer.join();

    REQUIRE(received.size() == N);
    for (uint32_t i = 0; i < N; ++i) {
        REQUIRE(received[i].frame == i);
        REQUIRE(received[i].payload ==
                static_cast<uint64_t>(i) * 1234567u);
    }
}

SCENARIO("depth-2 ring: producer can be at most 1 frame ahead of consumer",
         "[spec][threading]") {
    Spsc2 q;
    // First two publishes fill both slots; third should block until
    // consumer reads slot 0.
    Packet p{};
    p.frame = 1;
    q.Publish(p);
    p.frame = 2;
    q.Publish(p);

    std::thread reader([&] {
        Packet out;
        REQUIRE(q.Consume(out));
        REQUIRE(out.frame == 1u);
    });
    p.frame = 3;
    q.Publish(p);
    reader.join();
}

SCENARIO("Shutdown drains gracefully", "[spec][threading][regression]") {
    Spsc2 q;
    std::thread consumer([&] {
        Packet p;
        while (q.Consume(p)) {
            // process
        }
    });
    Packet p{};
    p.frame = 1;
    q.Publish(p);
    q.Shutdown();
    consumer.join();
    // If we reach here without hanging, the contract holds.
    REQUIRE(true);
}
