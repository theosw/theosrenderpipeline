#include "KernelProfiler.h"

#include <Windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace KernelProfiler
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using Handle = void*;
		struct Dim3 { std::uint32_t x, y, z; };
		// NVAPI SDK (MIT): NVAPI_CU_KERNEL_LAUNCH_PARAMS and the _EX variant.
		struct Launch { Handle function; Dim3 grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; };
		struct LaunchEx { Handle function; Dim3 grid, block; std::uint32_t sharedBytes; const void* params; std::uint32_t paramSize; void** kernelParams; };
		using QueryInterfaceFn = void* (__cdecl*)(std::uint32_t);
		using CreateCuFunctionFn = int (__cdecl*)(ID3D12Device*, Handle, const char*, Handle*);
		using LaunchChainFn = int (__cdecl*)(ID3D12GraphicsCommandList*, const Launch*, std::uint32_t);
		using LaunchChainExFn = int (__cdecl*)(ID3D12GraphicsCommandList*, const LaunchEx*, std::uint32_t);
		using LaunchCubinFn = int (__cdecl*)(ID3D12GraphicsCommandList*, Handle, std::uint32_t, std::uint32_t, std::uint32_t, const void*, std::uint32_t);
		using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);
		using LoadLibraryExWFn = HMODULE (WINAPI*)(LPCWSTR, HANDLE, DWORD);
		// nvapi_interface.h
		constexpr std::uint32_t kCreateCuFunction = 0xe2436e22, kLaunchCuKernelChain = 0x24973538,
			kLaunchCuKernelChainEx = 0x846a9bf0, kLaunchCubinShader = 0x5c52bb86;
		constexpr UINT kQueries = 8192;

		std::mutex mutex;
		QueryInterfaceFn realQuery{};
		CreateCuFunctionFn realCreate{};
		LaunchChainFn realChain{};
		LaunchChainExFn realChainEx{};
		LaunchCubinFn realCubin{};
		GetProcAddressFn realGetProcAddress{};
		LoadLibraryExWFn realLoadLibraryExW{};
		std::map<Handle, std::string> names;
		std::set<std::string> procRequests;
		std::map<std::uint32_t, unsigned> queryRequests;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12QueryHeap> heap;
		ComPtr<ID3D12Resource> readback;
		struct Pending { std::string name; Dim3 grid, block; std::uint32_t sharedBytes; UINT query; };
		std::vector<Pending> pending;
		UINT nextQuery{};
		bool collecting{};
		// TRP_NR_KERNEL_PROFILE=2 times each chain as NVIDIA submits it, keeping
		// the overlap between chained kernels; =1 splits chains per kernel.
		bool wholeChains{};

		// Replaces one imported function in a loaded module's import table.
		void* PatchImport(HMODULE module, const char* dll, const char* function, void* replacement)
		{
			auto* base = reinterpret_cast<std::uint8_t*>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!directory.VirtualAddress) { return nullptr; }
			for (auto* entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry) {
				if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), dll) != 0) { continue; }
				auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + entry->OriginalFirstThunk);
				auto* thunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++thunks) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) { continue; }
					const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) { continue; }
					DWORD protection{};
					VirtualProtect(&thunks->u1.Function, sizeof(void*), PAGE_READWRITE, &protection);
					auto* original = reinterpret_cast<void*>(thunks->u1.Function);
					thunks->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
					VirtualProtect(&thunks->u1.Function, sizeof(void*), protection, &protection);
					return original;
				}
			}
			return nullptr;
		}

		bool EnsureQueries(ID3D12GraphicsCommandList* list)
		{
			if (heap) { return true; }
			if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) { return false; }
			D3D12_QUERY_HEAP_DESC desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kQueries, 0 };
			D3D12_HEAP_PROPERTIES heapProperties{ D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = kQueries * sizeof(std::uint64_t);
			buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return SUCCEEDED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap))) &&
				SUCCEEDED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &buffer,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
		}

		// Brackets one launch with a timestamp pair resolved on the same list.
		template<class F> int Timed(ID3D12GraphicsCommandList* list, Handle function, Dim3 grid, Dim3 block, std::uint32_t shared, F launch,
			std::uint32_t chainLength = 1)
		{
			if (!collecting || nextQuery + 2 > kQueries || !EnsureQueries(list)) { return launch(); }
			const auto query = nextQuery; nextQuery += 2;
			list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
			const auto result = launch();
			list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2, readback.Get(), query * sizeof(std::uint64_t));
			const auto found = names.find(function);
			auto name = found == names.end() ? std::string("<unnamed>") : found->second;
			if (chainLength > 1) { name = "chain x" + std::to_string(chainLength) + " from " + name; }
			pending.push_back({ std::move(name), grid, block, shared, query });
			return result;
		}

		int __cdecl CreateCuFunction(ID3D12Device* d, Handle module, const char* name, Handle* function)
		{
			const auto result = realCreate(d, module, name, function);
			std::scoped_lock lock(mutex);
			if (result == 0 && function && *function) { names[*function] = name ? name : "<null>"; }
			return result;
		}
		int __cdecl LaunchChain(ID3D12GraphicsCommandList* list, const Launch* kernels, std::uint32_t count)
		{
			std::scoped_lock lock(mutex);
			if (wholeChains && count) {
				return Timed(list, kernels[0].function, kernels[0].grid, kernels[0].block, kernels[0].sharedBytes,
					[&] { return realChain(list, kernels, count); }, count);
			}
			int result = 0;
			for (std::uint32_t i = 0; i < count && result == 0; ++i) {
				result = Timed(list, kernels[i].function, kernels[i].grid, kernels[i].block, kernels[i].sharedBytes,
					[&] { return realChain(list, kernels + i, 1); });
			}
			return result;
		}
		int __cdecl LaunchChainEx(ID3D12GraphicsCommandList* list, const LaunchEx* kernels, std::uint32_t count)
		{
			std::scoped_lock lock(mutex);
			int result = 0;
			for (std::uint32_t i = 0; i < count && result == 0; ++i) {
				result = Timed(list, kernels[i].function, kernels[i].grid, kernels[i].block, kernels[i].sharedBytes,
					[&] { return realChainEx(list, kernels + i, 1); });
			}
			return result;
		}
		int __cdecl LaunchCubin(ID3D12GraphicsCommandList* list, Handle shader, std::uint32_t x, std::uint32_t y, std::uint32_t z,
			const void* params, std::uint32_t size)
		{
			std::scoped_lock lock(mutex);
			return Timed(list, shader, { x, y, z }, {}, 0, [&] { return realCubin(list, shader, x, y, z, params, size); });
		}

		void* __cdecl QueryInterface(std::uint32_t id)
		{
			void* function = realQuery(id);
			std::scoped_lock lock(mutex);
			++queryRequests[id];
			if (!function) { return nullptr; }
			switch (id) {
			case kCreateCuFunction: realCreate = reinterpret_cast<CreateCuFunctionFn>(function); return reinterpret_cast<void*>(&CreateCuFunction);
			case kLaunchCuKernelChain: realChain = reinterpret_cast<LaunchChainFn>(function); return reinterpret_cast<void*>(&LaunchChain);
			case kLaunchCuKernelChainEx: realChainEx = reinterpret_cast<LaunchChainExFn>(function); return reinterpret_cast<void*>(&LaunchChainEx);
			case kLaunchCubinShader: realCubin = reinterpret_cast<LaunchCubinFn>(function); return reinterpret_cast<void*>(&LaunchCubin);
			default: return function;
			}
		}

		FARPROC WINAPI GetProcAddressHook(HMODULE module, LPCSTR name)
		{
			auto* result = realGetProcAddress(module, name);
			if (reinterpret_cast<std::uintptr_t>(name) > 0xFFFF) {
				std::scoped_lock lock(mutex);
				procRequests.insert(name);
				if (result && std::strcmp(name, "nvapi_QueryInterface") == 0) {
					realQuery = reinterpret_cast<QueryInterfaceFn>(result);
					return reinterpret_cast<FARPROC>(&QueryInterface);
				}
			}
			return result;
		}

		HMODULE WINAPI LoadLibraryExWHook(LPCWSTR path, HANDLE file, DWORD flags)
		{
			const auto module = realLoadLibraryExW(path, file, flags);
			if (module && path && wcsstr(path, L"dlssnr")) {
				// NR resolves NVAPI with its own GetProcAddress import.
				if (auto* original = PatchImport(module, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&GetProcAddressHook))) {
					realGetProcAddress = reinterpret_cast<GetProcAddressFn>(original);
				}
			}
			return module;
		}
	}

	bool Install()
	{
		char value[8]{};
		if (!GetEnvironmentVariableA("TRP_NR_KERNEL_PROFILE", value, sizeof(value)) || (value[0] != '1' && value[0] != '2')) { return false; }
		wholeChains = value[0] == '2';
		realGetProcAddress = &::GetProcAddress;
		auto* original = PatchImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "LoadLibraryExW", reinterpret_cast<void*>(&LoadLibraryExWHook));
		realLoadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(original);
		return original != nullptr;
	}

	std::string Diagnostics()
	{
		std::scoped_lock lock(mutex);
		std::string text = "procs:";
		for (const auto& name : procRequests) { if (std::strstr(name.c_str(), "nvapi") || std::strstr(name.c_str(), "cu")) { text += " " + name; } }
		text += " | nvapi ids:";
		char buffer[32];
		for (const auto& [id, count] : queryRequests) { std::snprintf(buffer, sizeof(buffer), " %08x", id); text += buffer; }
		std::snprintf(buffer, sizeof(buffer), " | named=%zu", names.size());
		return text + buffer;
	}

	void BeginFrame()
	{
		std::scoped_lock lock(mutex);
		pending.clear(); nextQuery = 0; collecting = true;
	}

	std::vector<Sample> EndFrame(std::uint64_t frequency)
	{
		std::scoped_lock lock(mutex);
		collecting = false;
		std::vector<Sample> samples;
		if (pending.empty() || !readback || !frequency) { return samples; }
		void* data{};
		D3D12_RANGE range{ 0, nextQuery * sizeof(std::uint64_t) };
		if (FAILED(readback->Map(0, &range, &data))) { return samples; }
		const auto* t = static_cast<const std::uint64_t*>(data);
		for (const auto& p : pending) {
			Sample s{ p.name, { p.grid.x, p.grid.y, p.grid.z }, { p.block.x, p.block.y, p.block.z }, p.sharedBytes,
				t[p.query + 1] >= t[p.query] ? double(t[p.query + 1] - t[p.query]) * 1000.0 / double(frequency) : -1.0 };
			samples.push_back(std::move(s));
		}
		D3D12_RANGE none{};
		readback->Unmap(0, &none);
		return samples;
	}
}
