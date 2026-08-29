// util/inplace_function.hpp
//
// Fixed-storage, type-erased callable -- like std::function but the closure
// lives in an INLINE buffer and NEVER heap-allocates. Construction
// static_asserts that the callable fits. Used for the render-graph pass
// closures (RenderGraph::SetupFn / ExecuteFn): the graph rebuilds its passes
// every frame, and std::function's small-buffer overflow was mallocing the
// closures (~4+/frame) and freeing them at the next Reset() -- #229 M2. This
// type makes that rebuild allocation-free.
//
// Movable + copyable to match the std::function it replaces. No exceptions, no
// RTTI (cairns_core forbids both). The dispatch vtable is a per-callable
// compile-time constant (constexpr) -- not mutable global state.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace cairns {

template <typename Sig,
          std::size_t Capacity = 128,
          std::size_t Align = alignof(std::max_align_t)>
class InplaceFunction;

template <typename R, typename... Args, std::size_t Capacity, std::size_t Align>
class InplaceFunction<R(Args...), Capacity, Align> {
public:
    InplaceFunction() noexcept = default;

    template <typename F,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<F>, InplaceFunction>>>
    InplaceFunction(F&& f) {
        using DF = std::decay_t<F>;
        static_assert(sizeof(DF) <= Capacity,
                      "callable too large for InplaceFunction buffer");
        static_assert(alignof(DF) <= Align,
                      "callable over-aligned for InplaceFunction buffer");
        ::new (static_cast<void*>(&storage_)) DF(std::forward<F>(f));
        vtable_ = &kVTable<DF>;
    }

    InplaceFunction(const InplaceFunction& o) {
        if (o.vtable_) {
            o.vtable_->copy(&storage_, &o.storage_);
            vtable_ = o.vtable_;
        }
    }
    InplaceFunction(InplaceFunction&& o) noexcept {
        if (o.vtable_) {
            o.vtable_->move(&storage_, &o.storage_);
            vtable_ = o.vtable_;
            o.reset();
        }
    }
    InplaceFunction& operator=(const InplaceFunction& o) {
        if (this != &o) {
            reset();
            if (o.vtable_) {
                o.vtable_->copy(&storage_, &o.storage_);
                vtable_ = o.vtable_;
            }
        }
        return *this;
    }
    InplaceFunction& operator=(InplaceFunction&& o) noexcept {
        if (this != &o) {
            reset();
            if (o.vtable_) {
                o.vtable_->move(&storage_, &o.storage_);
                vtable_ = o.vtable_;
                o.reset();
            }
        }
        return *this;
    }
    ~InplaceFunction() { reset(); }

    explicit operator bool() const noexcept { return vtable_ != nullptr; }

    R operator()(Args... args) const {
        return vtable_->invoke(&storage_, std::forward<Args>(args)...);
    }

private:
    struct VTable {
        R (*invoke)(const void*, Args&&...);
        void (*copy)(void*, const void*);
        void (*move)(void*, void*);
        void (*destroy)(void*);
    };

    template <typename DF>
    static R InvokeImpl(const void* p, Args&&... args) {
        // const_cast matches std::function: operator() is const but may call a
        // mutable lambda. The render-graph closures are not mutable, so this is
        // a no-op there.
        return (*const_cast<DF*>(static_cast<const DF*>(p)))(
            std::forward<Args>(args)...);
    }
    template <typename DF>
    static void CopyImpl(void* dst, const void* src) {
        ::new (dst) DF(*static_cast<const DF*>(src));
    }
    template <typename DF>
    static void MoveImpl(void* dst, void* src) {
        ::new (dst) DF(std::move(*static_cast<DF*>(src)));
    }
    template <typename DF>
    static void DestroyImpl(void* p) {
        static_cast<DF*>(p)->~DF();
    }

    template <typename DF>
    static constexpr VTable kVTable = {
        &InvokeImpl<DF>, &CopyImpl<DF>, &MoveImpl<DF>, &DestroyImpl<DF>,
    };

    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(&storage_);
            vtable_ = nullptr;
        }
    }

    alignas(Align) std::byte storage_[Capacity];
    const VTable* vtable_ = nullptr;
};

}  // namespace cairns
