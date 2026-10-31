#pragma once

// One FIFO lane per scratch allocation, independent of SM IDs and residency.
// These functions are called by ONE CTA leader, never by a divergent warp.
// The caller publishes the ticket through shared memory + __syncthreads().
// All CTA threads fence scratch writes, then rendezvous, before release.
//
// PROGRESS PRECONDITION: slot holders finish without waiting for unlaunched
// producers. The host must order dependent dispatches; moving flag polls before
// acquire removes a pool cycle but does not by itself guarantee chain progress.
template<unsigned Shift>
__device__ __forceinline__ unsigned long long trp_slot_acquire(
    unsigned long long* next, unsigned long long* turns)
{
    static_assert(Shift > 0 && Shift < 32, "power-of-two slot capacity");
    const auto ticket = atomicAdd(next, 1ull);
    const unsigned slot = unsigned(ticket) & ((1u << Shift) - 1u);
    const auto generation = ticket >> Shift;
    while (atomicAdd(turns + slot, 0ull) != generation) {}
    __threadfence();
    return ticket;
}

template<unsigned Shift>
__device__ __forceinline__ void trp_slot_release(
    unsigned long long* turns, unsigned long long ticket)
{
    const unsigned slot = unsigned(ticket) & ((1u << Shift) - 1u);
    // Mask generation as well as ticket: the protocol survives uint64 wrap.
    const auto nextGeneration = ((ticket >> Shift) + 1ull) & (~0ull >> Shift);
    atomicExch(turns + slot, nextGeneration);
}
