// tests/test_handle.cpp
//
// SPEC: cairns::Handle<T> + cairns::ResourceManager<T>   (src/core/handle.hpp)
// TAGS: [spec][core][handle][aaltonen]
//
// A generational pool after Sebastian Aaltonen, "Modern Mobile Rendering
// Architecture," the "arrays you walk" pattern. T must expose nested `Hot` and
// `Cold` POD types. A Handle is {uint16 index, uint16 generation}; it is
// STABLE across pool growth (the index never moves) and FAILS SAFE when stale
// (resolves to nullptr instead of aliasing a recycled slot).
//
// The two production bugs this contract exists to forbid:
//   * #228 R1.x  -- a reused slot carried the previous occupant's data.
//                   => Acquire MUST hand back zero-initialized Hot/Cold.
//   * #222       -- a Hot* fetched before an Acquire dangled after the
//                   underlying vector reallocated.
//                   => GetHot's result is a TRANSIENT view, invalid across the
//                   next Acquire. The safe pattern is "snapshot, then Acquire."

#include <catch2/catch_test_macros.hpp>

#include "core/handle.hpp"

namespace {

// Minimal T satisfying the Hot/Cold requirement. `tag` lets us prove a recycled
// slot is reset rather than carrying its predecessor's value.
struct Widget {
    struct Hot {
        int tag = 0;
        float load = 0.0f;
    };
    struct Cold {
        int provenance = 0;
    };
};

using WidgetId = cairns::Handle<Widget>;
using WidgetPool = cairns::ResourceManager<Widget>;

// Pools are chunk-backed + fixed-capacity now: Reserve is mandatory and the
// storage never relocates. Bundle a ChunkAllocator with a Reserve'd pool so
// each scenario gets stable storage. Declaration order matters -- the pool is
// destroyed before the block, so its arrays deallocate back to a live block.
struct ReservedPool {
    cairns::ChunkAllocator block;
    WidgetPool pool;
    explicit ReservedPool(uint16_t cap = 128) {
        block.InitReserved(1u << 20);  // 1 MiB; the tiny Widget pool fits easily
        pool.Reserve(block, cap);
    }
};

}  // namespace

SCENARIO("a default-constructed handle is null", "[spec][core][handle]") {
    GIVEN("a default Handle and the Null sentinel") {
        WidgetId def{};
        THEN("both report IsNull and carry the 0xFFFF index sentinel") {
            REQUIRE(def.IsNull());
            REQUIRE(WidgetId::Null.IsNull());
            REQUIRE(def == WidgetId::Null);
            REQUIRE(def.index == 0xFFFF);
        }
    }
}

SCENARIO("acquiring a slot yields a live, resolvable handle", "[spec][core][handle]") {
    GIVEN("an empty pool") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WHEN("one slot is acquired") {
            WidgetId h = pool.Acquire();
            THEN("the handle is non-null and resolves to fresh, zeroed storage") {
                REQUIRE_FALSE(h.IsNull());
                auto* hot = pool.GetHot(h);
                auto* cold = pool.GetCold(h);
                REQUIRE(hot != nullptr);
                REQUIRE(cold != nullptr);
                REQUIRE(hot->tag == 0);
                REQUIRE(hot->load == 0.0f);
                REQUIRE(cold->provenance == 0);
            }
            AND_THEN("Size reflects the one occupied slot") {
                REQUIRE(pool.Size() == 1);
            }
        }
    }
}

SCENARIO("releasing a handle makes it stale and fail-safe", "[spec][core][handle]") {
    GIVEN("a pool with one acquired, written slot") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WidgetId h = pool.Acquire();
        pool.GetHot(h)->tag = 7;

        WHEN("the handle is released") {
            pool.Release(h);
            THEN("the stale handle resolves to nullptr on both planes") {
                REQUIRE(pool.GetHot(h) == nullptr);
                REQUIRE(pool.GetCold(h) == nullptr);
            }
            AND_THEN("releasing the same stale handle again is a safe no-op") {
                pool.Release(h);
                REQUIRE(pool.GetHot(h) == nullptr);
            }
        }
    }
}

SCENARIO("a recycled index comes back zero-initialized (#228 aatrox reload)",
         "[spec][core][handle][regression]") {
    GIVEN("a slot that was written, then released") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WidgetId first = pool.Acquire();
        pool.GetHot(first)->tag = 0xBEEF;
        pool.GetCold(first)->provenance = 0xBEEF;
        pool.Release(first);

        WHEN("a new slot is acquired (reusing the freed index)") {
            WidgetId second = pool.Acquire();
            THEN("it reuses the same index but with a bumped generation") {
                REQUIRE(second.index == first.index);
                REQUIRE(second.generation != first.generation);
            }
            AND_THEN("the OLD handle is still stale and the new storage is reset") {
                REQUIRE(pool.GetHot(first) == nullptr);
                auto* hot = pool.GetHot(second);
                REQUIRE(hot != nullptr);
                REQUIRE(hot->tag == 0);
                REQUIRE(pool.GetCold(second)->provenance == 0);
            }
        }
    }
}

SCENARIO("indices are assigned densely and recycled LIFO-ish",
         "[spec][core][handle]") {
    GIVEN("three live slots") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WidgetId a = pool.Acquire();
        WidgetId b = pool.Acquire();
        WidgetId c = pool.Acquire();
        REQUIRE(a.index != b.index);
        REQUIRE(b.index != c.index);

        WHEN("the middle one is freed and a fourth acquired") {
            pool.Release(b);
            WidgetId d = pool.Acquire();
            THEN("the freed index is reused rather than growing the pool") {
                REQUIRE(d.index == b.index);
                REQUIRE(pool.Size() == 3);
                REQUIRE(pool.GetHot(b) == nullptr);
                REQUIRE(pool.GetHot(d) != nullptr);
            }
        }
    }
}

SCENARIO("ForEachLive visits exactly the live slots", "[spec][core][handle]") {
    GIVEN("four slots with the second released") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WidgetId ids[4];
        for (int i = 0; i < 4; ++i) {
            ids[i] = pool.Acquire();
            pool.GetHot(ids[i])->tag = i + 1;
        }
        pool.Release(ids[1]);

        WHEN("the live set is walked") {
            int visited = 0;
            int tag_sum = 0;
            pool.ForEachLive([&](Widget::Hot& hot, Widget::Cold&) {
                ++visited;
                tag_sum += hot.tag;
            });
            THEN("only the three survivors are seen") {
                REQUIRE(visited == 3);
                REQUIRE(tag_sum == 1 + 3 + 4);
            }
        }
    }
}

SCENARIO("GetHot pointers stay put across Acquire -- fixed storage (#222)",
         "[spec][core][handle][regression]") {
    GIVEN("a live, written handle in a chunk-backed pool") {
        ReservedPool rp;
        WidgetPool& pool = rp.pool;
        WidgetId keep = pool.Acquire();
        pool.GetHot(keep)->tag = 99;
        Widget::Hot* before = pool.GetHot(keep);

        WHEN("many subsequent slots are acquired (no realloc -- fixed cap)") {
            for (int i = 0; i < 64; ++i) {
                (void)pool.Acquire();
            }
            THEN("the original pointer is unmoved and still holds its data") {
                // #222 inverted: storage is fixed-capacity chunk-backed, so a
                // Hot* no longer dangles across Acquire (it used to, when the
                // vector reallocated). Still DON'T store it -- a Release +
                // re-Acquire of this slot would alias a new occupant.
                Widget::Hot* after = pool.GetHot(keep);
                REQUIRE(after == before);
                REQUIRE(after->tag == 99);
            }
        }
    }
}
