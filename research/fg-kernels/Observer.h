#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <filesystem>
#include <cstdint>

namespace FGObserver {
// Installed only in the hash-verified provider's resolver import. Process-resident
// state survives provider shutdown because NVIDIA may cache resolved callbacks.
void Install(HMODULE provider, const std::filesystem::path& output, bool profile);
void Begin(ID3D12GraphicsCommandList* list, ID3D12QueryHeap* queries, unsigned group, unsigned generated);
unsigned End(); // Throws on observer failure; caller must not submit the list.
void Flush(const uint64_t* timestamps, uint64_t frequency); // Only after GPU retirement.
void Finish(); // Flush lifetime records after provider shutdown; callbacks stay resident.
}
