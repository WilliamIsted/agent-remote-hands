// Minimal Microsoft::WRL::ComPtr shim for the windows-legacy v141_xp build.
//
// WRL's SDK header (winrt/wrl/def.h) gates on NTDDI_VERSION >= NTDDI_VISTA.
// sdkddkver.h enforces that NTDDI_VERSION >> 16 == _WIN32_WINNT, so these two
// guards are mutually exclusive when targeting XP (_WIN32_WINNT=0x0501).
//
// This shim is placed before the SDK winrt/ directory in the legacy include path
// so the compiler finds it instead of WRL's gated version. Only ComPtr is
// provided — the subset actually used by element.cpp, element_table.cpp,
// and watch.cpp.
#pragma once

#include <unknwn.h>

namespace Microsoft {
namespace WRL {

template<typename T>
class ComPtr {
public:
    ComPtr() noexcept : ptr_(nullptr) {}

    ComPtr(T* ptr) noexcept : ptr_(ptr) { InternalAddRef(); }

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) { InternalAddRef(); }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ~ComPtr() noexcept { InternalRelease(); }

    ComPtr& operator=(T* ptr) noexcept {
        if (ptr_ != ptr) { InternalRelease(); ptr_ = ptr; InternalAddRef(); }
        return *this;
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (ptr_ != other.ptr_) { InternalRelease(); ptr_ = other.ptr_; InternalAddRef(); }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (ptr_ != other.ptr_) { InternalRelease(); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }

    T*  Get()                const noexcept { return ptr_; }
    T** GetAddressOf()             noexcept { return &ptr_; }
    T** ReleaseAndGetAddressOf()   noexcept { InternalRelease(); return &ptr_; }
    T*  operator->()         const noexcept { return ptr_; }
    T** operator&()                noexcept { return ReleaseAndGetAddressOf(); }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    void Reset() noexcept { InternalRelease(); }

    T* Detach() noexcept { T* p = ptr_; ptr_ = nullptr; return p; }

    template<typename U>
    HRESULT As(ComPtr<U>* other) const noexcept {
        return ptr_->QueryInterface(
            __uuidof(U),
            reinterpret_cast<void**>(other->ReleaseAndGetAddressOf()));
    }

private:
    T* ptr_;

    void InternalAddRef()  noexcept { if (ptr_) ptr_->AddRef(); }
    void InternalRelease() noexcept {
        T* p = ptr_;
        if (p) { ptr_ = nullptr; p->Release(); }
    }
};

} // namespace WRL
} // namespace Microsoft
