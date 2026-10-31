#include "KernelProfiler.h"

#include <Windows.h>
#include <wrl/client.h>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

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
		bool profiling{};
		struct LaunchOverride
		{
			Dim3 block{}, grid{};
			std::uint32_t sharedBytes{};
			bool hasGrid{}, hasShared{}, logged{};
		};
		std::map<std::string, LaunchOverride, std::less<>> overrides;

		bool ParseUnsigned(std::string_view text, std::uint32_t& value)
		{
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
		}

		bool ParseDim(std::string_view text, Dim3& dim)
		{
			const auto x = text.find('x'), y = text.find('x', x == text.npos ? text.size() : x + 1);
			return x != text.npos && y != text.npos && ParseUnsigned(text.substr(0, x), dim.x) &&
				ParseUnsigned(text.substr(x + 1, y - x - 1), dim.y) && ParseUnsigned(text.substr(y + 1), dim.z) &&
				dim.x && dim.y && dim.z;
		}

		bool ParseOverrides(std::string_view spec)
		{
			overrides.clear();
			decltype(overrides) parsed;
			while (!spec.empty()) {
				const auto end = spec.find(';');
				const auto entry = spec.substr(0, end);
				std::vector<std::string_view> fields;
				for (std::size_t at = 0;;) {
					const auto colon = entry.find(':', at);
					fields.push_back(entry.substr(at, colon == entry.npos ? colon : colon - at));
					if (colon == entry.npos) { break; }
					at = colon + 1;
				}
				LaunchOverride rule;
				if (fields.size() < 2 || fields.size() > 4 || fields[0].empty() || !ParseDim(fields[1], rule.block) ||
					rule.block.x > 1024 || rule.block.y > 1024 || rule.block.z > 64 ||
					std::uint64_t(rule.block.x) * rule.block.y * rule.block.z > 1024) { return false; }
				if (fields.size() >= 3) {
					rule.hasGrid = !fields[2].empty();
					if ((!rule.hasGrid && fields.size() != 4) || (rule.hasGrid && !ParseDim(fields[2], rule.grid))) { return false; }
				}
				if (fields.size() == 4) {
					rule.hasShared = true;
					if (!ParseUnsigned(fields[3], rule.sharedBytes)) { return false; }
				}
				if (!parsed.emplace(std::string(fields[0]), rule).second) { return false; }
				if (end == spec.npos) { break; }
				spec.remove_prefix(end + 1);
			}
			overrides = std::move(parsed);
			return !overrides.empty();
		}

		// Copy descriptors only. Parameter storage and the runtime's flag protocol stay intact.
		template<class T> const T* OverrideLaunches(const T* kernels, std::uint32_t count, std::vector<T>& rewritten, const char* path)
		{
			if (overrides.empty()) { return kernels; }
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto name = names.find(kernels[i].function);
				if (name == names.end()) { continue; }
				const auto found = overrides.find(name->second);
				if (found == overrides.end()) { continue; }
				if (rewritten.empty()) { rewritten.assign(kernels, kernels + count); }
				auto& rule = found->second;
				auto& launch = rewritten[i];
				launch.block = rule.block;
				if (rule.hasGrid) { launch.grid = rule.grid; }
				if (rule.hasShared) { launch.sharedBytes = rule.sharedBytes; }
				if (!rule.logged) {
					std::printf("LAUNCH_OVERRIDE kernel=%s path=%s block=%ux%ux%u->%ux%ux%u grid=%ux%ux%u->%ux%ux%u shared=%u->%u\n",
						name->second.c_str(), path, kernels[i].block.x, kernels[i].block.y, kernels[i].block.z,
						launch.block.x, launch.block.y, launch.block.z, kernels[i].grid.x, kernels[i].grid.y, kernels[i].grid.z,
						launch.grid.x, launch.grid.y, launch.grid.z, kernels[i].sharedBytes, launch.sharedBytes);
					rule.logged = true;
				}
			}
			return rewritten.empty() ? kernels : rewritten.data();
		}
		// TRP_NR_KERNEL_PARAMS=1 prints each distinct parameter block once.
		bool dumpParams{};
		// TRP_NR_KERNEL_BARRIERS=1 puts a UAV barrier before every split kernel, so no two
		// kernels overlap (checks whether a result depends on the chain's flag protocol).
		bool barriers{};
		std::set<std::string> dumped;
		// TRP_NR_KERNEL_CAPTURE=<kernel>,<launch>,<file>,<param offset>:<bytes>,... copies the
		// buffers behind those parameter-block pointers just before and just after one launch
		// of <kernel> (counted from 0 over profiled frames) and writes them to <file>. Needs
		// TRP_NR_CAPTURE_FATBIN, a fatbin with trp_copy(source, target, count16).
		struct Region { std::uint32_t offset; std::uint64_t bytes; };
		struct CopyParams { std::uint64_t source, target, count; };
		struct Capture
		{
			std::string kernel, file;
			unsigned launch{}, seen{};
			std::vector<Region> regions;
			std::uint64_t total{};
			std::vector<std::uint8_t> params;
			Handle copy{};
			ComPtr<ID3D12Resource> gpu, readback;
			bool recorded{}, written{};
		} capture;
		using CreateCuModuleFn = int (__cdecl*)(ID3D12Device*, const void*, std::uint32_t, Handle*);
		constexpr std::uint32_t kCreateCuModule = 0xad1a677d;

		void DumpParams(Handle function, Dim3 grid, Dim3 block, const void* params, std::uint32_t size)
		{
			if (!dumpParams || !params || dumped.size() >= 4096) { return; }
			const auto found = names.find(function);
			std::string line = (found == names.end() ? std::string("<unnamed>") : found->second) + " grid=" +
				std::to_string(grid.x) + "x" + std::to_string(grid.y) + "x" + std::to_string(grid.z) + " block=" +
				std::to_string(block.x) + "x" + std::to_string(block.y) + "x" + std::to_string(block.z) + " bytes=" + std::to_string(size) + " ";
			char hex[3];
			for (std::uint32_t i = 0; i < size; ++i) {
				std::snprintf(hex, sizeof(hex), "%02x", static_cast<const std::uint8_t*>(params)[i]);
				line += hex;
				if (i % 8 == 7 && i + 1 < size) { line += '.'; }
			}
			if (dumped.insert(line).second) { std::printf("PARAMS %s\n", line.c_str()); }
		}

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
			if (!profiling || !collecting || nextQuery + 2 > kQueries || !EnsureQueries(list)) { return launch(); }
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

		D3D12_RESOURCE_BARRIER UavBarrier()
		{
			D3D12_RESOURCE_BARRIER barrier{ D3D12_RESOURCE_BARRIER_TYPE_UAV };
			barrier.UAV.pResource = nullptr;
			return barrier;
		}

		bool EnsureCapture(ID3D12GraphicsCommandList* list)
		{
			if (capture.gpu) { return true; }
			if (!device && FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) { return false; }
			char path[MAX_PATH]{};
			if (!GetEnvironmentVariableA("TRP_NR_CAPTURE_FATBIN", path, sizeof(path))) { return false; }
			std::FILE* file = std::fopen(path, "rb");
			if (!file) { return false; }
			std::vector<std::uint8_t> blob;
			for (int c; (c = std::fgetc(file)) != EOF;) { blob.push_back(static_cast<std::uint8_t>(c)); }
			std::fclose(file);
			const auto createModule = reinterpret_cast<CreateCuModuleFn>(realQuery(kCreateCuModule));
			Handle module{};
			if (!createModule || createModule(device.Get(), blob.data(), static_cast<std::uint32_t>(blob.size()), &module) != 0 ||
				realCreate(device.Get(), module, "trp_copy", &capture.copy) != 0) { return false; }
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 2 * capture.total;
			buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			D3D12_HEAP_PROPERTIES gpuHeap{ D3D12_HEAP_TYPE_DEFAULT }, readHeap{ D3D12_HEAP_TYPE_READBACK };
			auto uav = buffer; uav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			return SUCCEEDED(device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &uav,
					D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&capture.gpu))) &&
				SUCCEEDED(device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&capture.readback)));
		}

		// Copies every capture region of `kernel` into the capture buffer at `base`.
		void CopyRegions(ID3D12GraphicsCommandList* list, const Launch& kernel, std::uint64_t base)
		{
			const auto barrier = UavBarrier();
			list->ResourceBarrier(1, &barrier);
			auto at = capture.gpu->GetGPUVirtualAddress() + base;
			for (const auto& region : capture.regions) {
				std::uint64_t source{};
				std::memcpy(&source, static_cast<const std::uint8_t*>(kernel.params) + region.offset, sizeof(source));
				const CopyParams params{ source, at, region.bytes / 16 };
				const Launch copy{ capture.copy, { 256, 1, 1 }, { 256, 1, 1 }, 0, &params, sizeof(params) };
				if (source) { realChain(list, &copy, 1); }
				at += region.bytes;
			}
			list->ResourceBarrier(1, &barrier);
		}

		template<class F> int Captured(ID3D12GraphicsCommandList* list, const Launch& kernel, F launch)
		{
			const auto found = names.find(kernel.function);
			if (capture.kernel.empty() || capture.recorded || !collecting || found == names.end() || found->second != capture.kernel ||
				capture.seen++ != capture.launch) { return launch(); }
			if (!EnsureCapture(list)) { std::printf("CAPTURE setup failed\n"); capture.recorded = true; return launch(); }
			const auto* bytes = static_cast<const std::uint8_t*>(kernel.params);
			capture.params.assign(bytes, bytes + kernel.paramSize);
			CopyRegions(list, kernel, 0);
			const auto result = launch();
			CopyRegions(list, kernel, capture.total);
			D3D12_RESOURCE_BARRIER toCopy{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
			toCopy.Transition = { capture.gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
			list->ResourceBarrier(1, &toCopy);
			list->CopyBufferRegion(capture.readback.Get(), 0, capture.gpu.Get(), 0, 2 * capture.total);
			std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
			list->ResourceBarrier(1, &toCopy);
			capture.recorded = true;
			return result;
		}

		void WriteCapture()
		{
			if (!capture.recorded || capture.written || !capture.readback) { return; }
			capture.written = true;
			void* data{};
			D3D12_RANGE range{ 0, 2 * capture.total };
			std::FILE* file = std::fopen(capture.file.c_str(), "wb");
			if (!file || FAILED(capture.readback->Map(0, &range, &data))) { std::printf("CAPTURE write failed\n"); if (file) { std::fclose(file); } return; }
			const std::uint32_t header[3]{ 0x50414354u, static_cast<std::uint32_t>(capture.regions.size()), static_cast<std::uint32_t>(capture.params.size()) };
			std::fwrite(header, sizeof(header), 1, file);
			std::fwrite(capture.params.data(), 1, capture.params.size(), file);
			for (const auto& region : capture.regions) { std::fwrite(&region.offset, 4, 1, file); std::fwrite(&region.bytes, 8, 1, file); }
			std::fwrite(data, 1, 2 * capture.total, file);
			D3D12_RANGE none{};
			capture.readback->Unmap(0, &none);
			std::fclose(file);
			std::printf("CAPTURE %s launch %u -> %s (%llu bytes per side)\n", capture.kernel.c_str(), capture.launch,
				capture.file.c_str(), static_cast<unsigned long long>(capture.total));
		}

		int __cdecl LaunchChain(ID3D12GraphicsCommandList* list, const Launch* kernels, std::uint32_t count)
		{
			std::scoped_lock lock(mutex);
			std::vector<Launch> rewritten;
			kernels = OverrideLaunches(kernels, count, rewritten, "chain");
			// Plain research runs must preserve chain overlap: no queries, copies or barriers.
			if (!profiling) { return realChain(list, kernels, count); }
			if (wholeChains && count) {
				return Timed(list, kernels[0].function, kernels[0].grid, kernels[0].block, kernels[0].sharedBytes,
					[&] { return realChain(list, kernels, count); }, count);
			}
			for (std::uint32_t i = 0; i < count; ++i) { DumpParams(kernels[i].function, kernels[i].grid, kernels[i].block, kernels[i].params, kernels[i].paramSize); }
			int result = 0;
			for (std::uint32_t i = 0; i < count && result == 0; ++i) {
				if (barriers) { const auto barrier = UavBarrier(); list->ResourceBarrier(1, &barrier); }
				result = Timed(list, kernels[i].function, kernels[i].grid, kernels[i].block, kernels[i].sharedBytes,
					[&] { return Captured(list, kernels[i], [&] { return realChain(list, kernels + i, 1); }); });
			}
			return result;
		}
		int __cdecl LaunchChainEx(ID3D12GraphicsCommandList* list, const LaunchEx* kernels, std::uint32_t count)
		{
			std::scoped_lock lock(mutex);
			std::vector<LaunchEx> rewritten;
			kernels = OverrideLaunches(kernels, count, rewritten, "chain-ex");
			if (!profiling) { return realChainEx(list, kernels, count); }
			if (wholeChains && count) {
				return Timed(list, kernels[0].function, kernels[0].grid, kernels[0].block, kernels[0].sharedBytes,
					[&] { return realChainEx(list, kernels, count); }, count);
			}
			int result = 0;
			for (std::uint32_t i = 0; i < count && result == 0; ++i) {
				DumpParams(kernels[i].function, kernels[i].grid, kernels[i].block, kernels[i].params, kernels[i].paramSize);
				if (barriers) { const auto barrier = UavBarrier(); list->ResourceBarrier(1, &barrier); }
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
		const auto length = GetEnvironmentVariableA("TRP_NR_KERNEL_PROFILE", value, sizeof(value));
		profiling = length && length < sizeof(value) && (value[0] == '1' || value[0] == '2');
		wholeChains = value[0] == '2';
		const auto specSize = GetEnvironmentVariableA("TRP_NR_LAUNCH_OVERRIDE", nullptr, 0);
		if (specSize) {
			char research[4]{};
			if (GetEnvironmentVariableA("TRP_NR_PASCAL_RESEARCH", research, sizeof(research)) != 1 || research[0] != '1') {
				std::printf("LAUNCH_OVERRIDE ignored: requires TRP_NR_PASCAL_RESEARCH=1\n");
			} else {
				std::string spec(specSize, '\0');
				const auto read = GetEnvironmentVariableA("TRP_NR_LAUNCH_OVERRIDE", spec.data(), specSize);
				if (!read || read >= specSize || !ParseOverrides(std::string_view(spec.data(), read))) {
					std::printf("LAUNCH_OVERRIDE rejected: invalid specification (no overrides enabled)\n");
				}
			}
		}
		if (!profiling && overrides.empty()) { return false; }
		char params[8]{};
		dumpParams = GetEnvironmentVariableA("TRP_NR_KERNEL_PARAMS", params, sizeof(params)) && params[0] == '1';
		barriers = GetEnvironmentVariableA("TRP_NR_KERNEL_BARRIERS", params, sizeof(params)) && params[0] == '1';
		char spec[1024]{};
		if (GetEnvironmentVariableA("TRP_NR_KERNEL_CAPTURE", spec, sizeof(spec))) {
			std::vector<std::string> fields;
			for (const char* at = spec; *at;) {
				const char* comma = std::strchr(at, ',');
				fields.emplace_back(at, comma ? comma : at + std::strlen(at));
				at = comma ? comma + 1 : at + std::strlen(at);
			}
			if (fields.size() >= 4) {
				capture.kernel = fields[0];
				capture.launch = static_cast<unsigned>(std::strtoul(fields[1].c_str(), nullptr, 10));
				capture.file = fields[2];
				for (std::size_t i = 3; i < fields.size(); ++i) {
					const auto colon = fields[i].find(':');
					if (colon == std::string::npos) { continue; }
					const Region region{ static_cast<std::uint32_t>(std::strtoul(fields[i].c_str() + (fields[i][0] == '+'), nullptr, 10)),
						std::strtoull(fields[i].c_str() + colon + 1, nullptr, 10) & ~15ull };
					capture.regions.push_back(region);
					capture.total += region.bytes;
				}
			}
		}
		realGetProcAddress = &::GetProcAddress;
		auto* original = PatchImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "LoadLibraryExW", reinterpret_cast<void*>(&LoadLibraryExWHook));
		realLoadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(original);
		if (!original) { std::printf("KERNELHOOK failed to intercept LoadLibraryExW\n"); }
		// The caller starts collection only for profiling, even when plain hooks are installed.
		return original != nullptr && profiling;
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
		WriteCapture();
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
