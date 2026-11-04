// Included inside KernelProfiler's private namespace. Manual single-feature
// experiment only; never linked into the renderer. Original weights are immutable
// for this fixture's lifetime. RetirePreparedWeights runs after GPU completion.
bool preparedEnabled{};
std::string preparedHelperPath;
std::set<Handle> preparedFunctions;
struct PreparedCache { ComPtr<ID3D12Resource> buffer; std::uint32_t banks{}; bool valid{}; };
std::map<std::uint64_t, PreparedCache> preparedCaches;
ComPtr<ID3D12Device> preparedDevice;
Handle preparedModule{}, preparedHelper{};
std::uint64_t preparedBytes{}, preparedHits{};

bool PreparedName(const char* name)
{
    return name && (std::strcmp(name, "cc_vit_1d_ffn_expand_chained_fp8") == 0 ||
        std::strcmp(name, "cc_vit_1d_ffn_expand_publish_fp8") == 0);
}

bool PreparedContract(Dim3 grid, Dim3 block, const void* params, std::uint32_t bytes,
    std::uint64_t& source)
{
    if (!params || bytes != 72 || block.x != 32 || block.y != 4 || block.z != 1 ||
        grid.y != 1 || !grid.z || grid.z > 8) { return false; }
    std::uint32_t w{}, h{};
    std::memcpy(&w, static_cast<const std::uint8_t*>(params) + 64, 4);
    std::memcpy(&h, static_cast<const std::uint8_t*>(params) + 68, 4);
    std::memcpy(&source, static_cast<const std::uint8_t*>(params) + 24, 8);
    // 32 tiles of 128 output channels, 1024 K, 4 MiB of packed weights per z.
    // The last vector ends exactly at the bank boundary. Check before GPU reads.
    return w && h && w <= 5120 && h <= 2880 && source && !(source & 15) &&
        grid.x == 32 * ((std::uint64_t(w) * h + 127) / 128);
}

bool EnsurePreparedHelper(ID3D12GraphicsCommandList* list)
{
    ComPtr<ID3D12Device> d;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&d)))) { return false; }
    if (preparedHelper) { return d.Get() == preparedDevice.Get(); }
    preparedDevice = d;
    const auto createModule = reinterpret_cast<CreateCuModuleFn>(realQuery(kCreateCuModule));
    if (!realChain) { realChain = reinterpret_cast<LaunchChainFn>(realQuery(kLaunchCuKernelChain)); }
    if (!createModule || !realChain || !realCreate || !realQuery(0xdf295ea6) || !realQuery(0x41c65285)) { return false; }
    auto* file = std::fopen(preparedHelperPath.c_str(), "rb");
    if (!file) { return false; }
    std::fseek(file, 0, SEEK_END);
    const auto size = std::ftell(file);
    std::rewind(file);
    if (size <= 0 || size > 1024 * 1024) { std::fclose(file); return false; }
    std::vector<std::uint8_t> blob(size);
    const auto read = std::fread(blob.data(), 1, blob.size(), file);
    std::fclose(file);
    if (read != blob.size() || createModule(d.Get(), blob.data(), static_cast<std::uint32_t>(blob.size()), &preparedModule)) { return false; }
    return realCreate(d.Get(), preparedModule, "trp_prepare_ffn_v1", &preparedHelper) == 0;
}

bool PrepareCache(ID3D12GraphicsCommandList* list, std::uint64_t source, std::uint32_t banks, std::uint64_t& address)
{
    if (const auto it = preparedCaches.find(source); it != preparedCaches.end()) {
        if (it->second.banks != banks || !it->second.valid) { return false; }
        address = it->second.buffer->GetGPUVirtualAddress();
        ++preparedHits;
        return true;
    }
    const std::uint64_t bytes = std::uint64_t(banks) * 8388608;
    if (preparedBytes + bytes > 256ull * 1024 * 1024 || !EnsurePreparedHelper(list)) { return false; }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES properties{ D3D12_HEAP_TYPE_DEFAULT };
    PreparedCache cache; cache.banks = banks;
    if (FAILED(preparedDevice->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&cache.buffer)))) { return false; }
    address = cache.buffer->GetGPUVirtualAddress();
    const CopyParams params{ source, address, bytes / 8 };
    const Launch decode{ preparedHelper, {256, 1, 1}, {256, 1, 1}, 0, &params, sizeof(params) };
    const auto barrier = UavBarrier();
    list->ResourceBarrier(1, &barrier);
    const auto result = realChain(list, &decode, 1);
    // Retain even if NVAPI fails after recording partial work. The caller drops
    // the evaluation on error; only explicit GPU-retired teardown frees buffers.
    cache.valid = result == 0;
    preparedCaches.emplace(source, std::move(cache));
    preparedBytes += bytes;
    if (result) { return false; }
    list->ResourceBarrier(1, &barrier);
    std::printf("PREPARED_CACHE banks=%u bytes=%llu total=%llu\n", banks, bytes, preparedBytes);
    return true;
}

template<class T> bool PrepareLaunches(ID3D12GraphicsCommandList* list, const T*& kernels, std::uint32_t count,
    std::vector<T>& rewritten, std::vector<std::array<std::uint8_t, 80>>& storage)
{
    if (!preparedEnabled) { return true; }
    storage.resize(count); // Stable addresses until realChain/Ex consumes the descriptors.
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!preparedFunctions.contains(kernels[i].function)) { continue; }
        if constexpr (std::is_same_v<T, LaunchEx>) {
            if (kernels[i].kernelParams) { return false; } // Only the observed packed ABI.
        }
        std::uint64_t source{}, cache{};
        if (!PreparedContract(kernels[i].grid, kernels[i].block, kernels[i].params, kernels[i].paramSize, source)) {
            std::printf("PREPARED rejected: ABI/grid block=%ux%ux%u grid=%ux%ux%u bytes=%u\n",
                kernels[i].block.x, kernels[i].block.y, kernels[i].block.z,
                kernels[i].grid.x, kernels[i].grid.y, kernels[i].grid.z, kernels[i].paramSize);
            return false;
        }
        if (!PrepareCache(list, source, kernels[i].grid.z, cache)) { return false; }
        if (rewritten.empty()) { rewritten.assign(kernels, kernels + count); }
        std::memcpy(storage[i].data(), kernels[i].params, 72);
        std::memcpy(storage[i].data() + 72, &cache, 8);
        rewritten[i].params = storage[i].data(); rewritten[i].paramSize = 80;
    }
    if (!rewritten.empty()) { kernels = rewritten.data(); }
    return true;
}
