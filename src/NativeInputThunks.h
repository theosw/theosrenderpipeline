#pragma once

#include <cstdint>
#include <xbyak/xbyak.h>

namespace TheosRenderPipeline::NativeInput
{
    enum class Target { CursorBounds, ScreenSize };

    // Tail forward the original ABI, including unknown stack arguments and
    // return value. Avoid a guessed C++ function signature at these game call
    // sites; the source replacements use only scratch registers here.
    class Thunk final : public Xbyak::CodeGenerator
    {
    public:
        Thunk(Target target, const std::uint64_t* extent, std::uintptr_t original) :
            CodeGenerator(128)
        {
            Xbyak::Label forward, destination;
            mov(rax, reinterpret_cast<std::uintptr_t>(extent));
            mov(rax, ptr[rax]);
            test(rax, rax);
            jz(forward);
            if (target == Target::CursorBounds) {
                cvtsi2ss(xmm0, eax);
                movss(ptr[rcx + 0x14], xmm0);
                shr(rax, 32);
                cvtsi2ss(xmm0, eax);
                movss(ptr[rcx + 0x18], xmm0);
            } else {
                mov(ptr[rdx], rax);
            }
            L(forward);
            jmp(ptr[rip + destination]);
            L(destination);
            dq(original);
            ready();
        }
    };

    // Publish State::screenWidth/Height and frameBufferViewport before the
    // original display call. Cursor bounds alone do not establish the
    // coordinate space used to draw Skyrim's Cursor Menu.
    class EngineDimensionsThunk final : public Xbyak::CodeGenerator
    {
    public:
        EngineDimensionsThunk(const std::uint64_t* extent, void* dimensions,
            std::uintptr_t original) : CodeGenerator(128)
        {
            Xbyak::Label forward, destination;
            mov(rax, reinterpret_cast<std::uintptr_t>(extent));
            mov(rax, ptr[rax]);
            test(rax, rax);
            jz(forward);
            mov(r10, reinterpret_cast<std::uintptr_t>(dimensions));
            mov(ptr[r10], eax);
            mov(ptr[r10 + 8], eax);
            shr(rax, 32);
            mov(ptr[r10 + 4], eax);
            mov(ptr[r10 + 12], eax);
            L(forward);
            jmp(ptr[rip + destination]);
            L(destination);
            dq(original);
            ready();
        }
    };
}
