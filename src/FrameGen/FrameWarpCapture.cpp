#include "FrameWarpCapture.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace TheosRenderPipeline::FrameWarp {
namespace {
constexpr std::array<int, 5> kinds{fw::kBackbuffer, fw::kHudless, fw::kUi, fw::kDepth, fw::kMotion};
void Close(HANDLE& h) { if (h) CloseHandle(h); h = nullptr; }
bool Same(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format;
}
bool Valid(ID3D12Device* device, ID3D12Resource* r, int kind) {
    if (!r) return false;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if (FAILED(r->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device) return false;
    const auto d = r->GetDesc();
    const auto format = kind < 3 ? DXGI_FORMAT_R8G8B8A8_UNORM : kind == 3 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width > 0 && d.Width <= 16384 &&
        d.Height > 0 && d.Height <= 16384 && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
        d.SampleDesc.Count == 1 && d.Format == format;
}
bool ValidCamera(const fw::Camera& c) {
    if (!std::isfinite(c.near_plane) || !std::isfinite(c.far_plane) || !std::isfinite(c.fov) || !std::isfinite(c.aspect) || !c.valid || !(c.near_plane > 0) || !(c.far_plane > c.near_plane) || !(c.aspect > 0) || !(c.fov > 0 && c.fov < 3.14f)) return false;
    for (const auto* matrix : {c.view_to_clip, c.clip_to_view, c.clip_to_prev_clip})
        for (int i = 0; i < 16; ++i) if (!std::isfinite(matrix[i])) return false;
    for (const auto* vector : {c.pos, c.up, c.right, c.fwd})
        for (int i = 0; i < 3; ++i) if (!std::isfinite(vector[i])) return false;
    return std::isfinite(c.jitter[0]) && std::isfinite(c.jitter[1]) && std::isfinite(c.mvec_scale[0]) && std::isfinite(c.mvec_scale[1]);
}
}
Capture::~Capture() {
    if (!Reset()) {
        // Process-resident retention if GPU ownership cannot be proved retired.
        for (auto& slot : slots_) for (auto& r : slot.textures) (void)r.Detach();
        (void)fence_.Detach(); (void)device_.Detach();
        (void)timestamps_.Detach(); (void)timestampReadback_.Detach();
    }
}
void Capture::Suspend(const char* reason) {
    if (shared_) InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->settings.enabled), 0);
    if (active_) { ++epoch_; resetCamera_ = true; }
    active_ = false; message_ = reason;
}
void Capture::Fail(const char* reason) { faulted_ = true; Suspend(reason); }
Status Capture::Snapshot() const {
    Status s{requested_, original_, active_, faulted_, published_, dropped_};
    s.message = message_;
    s.copyGPUSamples = copyGPUSamples_; s.copyGPUAverageMs = copyGPUSamples_ ? copyGPUMs_ / copyGPUSamples_ : 0;
    s.copyGPUMaxMs = copyGPUMaxMs_;
    if (shared_) {
        const auto& p = shared_->presenter;
        s.sourceFPS = p.source_fps; s.outputFPS = p.output_fps;
        s.warpGPUMs = p.warp_gpu_ms; s.sourceAgeMs = p.source_age_ms;
        s.calibratedX = p.calibrated_x != 0; s.calibratedY = p.calibrated_y != 0;
    }
    return s;
}
bool Capture::Open(ID3D12Device* device, HWND window, const D3D12_RESOURCE_DESC& color) {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(fw::Shared), fw::map_name(GetCurrentProcessId()).c_str());
    if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) { Close(mapping_); Fail("native FrameWarp mapping already owned or unavailable"); return false; }
    shared_ = static_cast<fw::Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(fw::Shared)));
    if (!shared_) { Close(mapping_); Fail("native FrameWarp mapping view failed"); return false; }
    std::memset(shared_, 0, sizeof(*shared_));
    shared_->version = fw::kVersion; shared_->producer_pid = GetCurrentProcessId();
    shared_->session = static_cast<std::uint64_t>(fw::qpc_now()); shared_->adapter = device->GetAdapterLuid();
    shared_->game_hwnd = reinterpret_cast<std::uint64_t>(window);
    shared_->backbuffer_format = color.Format; shared_->color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    shared_->backbuffer_width = static_cast<UINT>(color.Width); shared_->backbuffer_height = color.Height;
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency); shared_->qpc_frequency = frequency.QuadPart;
    auto& s = shared_->settings;
    s.use_mouse = s.use_ui_tags = s.use_controller = s.warp_engine = 1;
    s.rotation_extrapolation = s.translation_extrapolation = 1;
    s.max_horizon_ms = 100; s.prediction_ms = -16; s.present_lead_ms = 6;
    s.present_lead_auto = 1; s.auto_prediction = 6;
    s.floor_release = s.hold_reticles = s.keep_attached = s.near_camera_rule = 1;
    s.no_warp_mask = s.hud_fill = s.turn_rule = s.background_memory = s.stretch_width = s.edge_fill = 1;
    s.hud_from_scene = 2; s.gpu_priority = 1;
    // Host Reflex owns frame pacing. Upstream add-on cap and generation stay off.
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_))) ||
        FAILED(device->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL,
            fw::object_name(shared_->producer_pid, shared_->session, L"fence").c_str(), &fenceHandle_))) {
        Fail("native FrameWarp shared fence failed"); return false;
    }
    // Optional asynchronous timestamps cover the five native copies/barriers,
    // excluding host rendering and the helper's intake/warp work.
    D3D12_QUERY_HEAP_DESC query{}; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count = fw::kSlots * 2;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = sizeof(std::uint64_t) * fw::kSlots * 2;
    buffer.Height = buffer.SampleDesc.Count = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamps_))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&timestampReadback_)))) { timestamps_.Reset(); timestampReadback_.Reset(); }
    device_ = device; shared_->magic = fw::kMagic;
    return true;
}
bool Capture::Allocate(ID3D12Device* device, int index, const std::array<ID3D12Resource*, 5>& inputs) {
    auto& slot = slots_[index];
    for (int i = 0; i < 5; ++i) {
        const auto source = inputs[i]->GetDesc();
        if (!slot.textures[i] || !Same(slot.textures[i]->GetDesc(), source)) {
            Resource replacement; HANDLE handle{};
            auto desc = source; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            const auto generation = slot.generations[i] + 1;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&replacement))) ||
                FAILED(device->CreateSharedHandle(replacement.Get(), nullptr, GENERIC_ALL,
                    fw::object_name(shared_->producer_pid, shared_->session, L"tex", index, kinds[i], generation).c_str(), &handle))) {
                if (handle) CloseHandle(handle); return false;
            }
            Close(slot.handles[i]); slot.textures[i] = std::move(replacement); slot.handles[i] = handle; slot.generations[i] = generation;
        }
    }
    return true;
}
bool Capture::Launch(const std::filesystem::path& helper) {
    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
        Fail("FrameWarp presenter exited; host retirement or restart required"); return false;
    }
    if (helper.empty()) return true; // Offline producer fixture does not launch anything.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(helper, ec)) { Fail("TRPFrameWarpPresenter.exe missing beside the plugin's FrameWarp directory"); return false; }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) { Fail("FrameWarp helper job unavailable"); return false; }
    std::wstring command = L"\"" + helper.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, helper.parent_path().c_str(), &startup, &process)) { Fail("FrameWarp helper launch failed"); return false; }
    if (!AssignProcessToJobObject(job_, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Fail("FrameWarp helper job ownership failed"); return false;
    }
    process_ = process.hProcess;
    const auto resumed = ResumeThread(process.hThread); CloseHandle(process.hThread);
    if (resumed == DWORD(-1)) { Fail("FrameWarp helper resume failed"); return false; }
    return true;
}
bool Capture::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, HWND window,
    const std::array<ID3D12Resource*, 5>& inputs, const fw::Camera& camera, std::int64_t cameraQpc,
    bool eligible, const std::filesystem::path& helper) {
    if (!requested_) { Suspend("disabled"); return false; }
    if (faulted_) return false;
    if (!eligible || !device || !list || cameraQpc <= 0 || !ValidCamera(camera)) { Suspend("waiting for fresh world camera, native UI, SDR, and frame generation off"); return false; }
    for (int i = 0; i < 5; ++i) if (!Valid(device, inputs[i], i)) { Suspend("unsupported FrameWarp input format/device/extent"); return false; }
    const auto color = inputs[0]->GetDesc();
    if (!Same(color, inputs[1]->GetDesc()) || !Same(color, inputs[2]->GetDesc()) ||
        inputs[3]->GetDesc().Width != inputs[4]->GetDesc().Width || inputs[3]->GetDesc().Height != inputs[4]->GetDesc().Height) {
        Suspend("FrameWarp layer extents differ"); return false;
    }
    if (pending_ >= 0) { Fail("FrameWarp copy recording has not been submitted"); return false; }
    if (!shared_ && !Open(device, window, color)) return false;
    if (device_.Get() != device || shared_->game_hwnd != reinterpret_cast<std::uint64_t>(window) ||
        shared_->backbuffer_width != color.Width || shared_->backbuffer_height != color.Height) {
        Suspend("FrameWarp requires host retirement before resize/device replacement"); return false;
    }
    if (InterlockedCompareExchange(&shared_->game_camera_check, 0, 0) == 1) {
        Suspend("XPAR rejected native camera/motion agreement; host retirement or restart required"); return false;
    }
    if (!Launch(helper)) return false;
    int index = -1;
    const auto completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX) { Fail("FrameWarp producer device removed"); return false; }
    Collect(completed);
    for (int i = 0; i < fw::kSlots && index < 0; ++i) {
        if (slots_[i].submitted > completed) continue;
        auto& state = shared_->slots[i].state;
        if (InterlockedCompareExchange(&state, fw::kWriting, fw::kFree) == fw::kFree ||
            InterlockedCompareExchange(&state, fw::kWriting, fw::kReady) == fw::kReady) index = i;
    }
    if (index < 0) { ++dropped_; message_ = "FrameWarp ring busy; skipped capture without waiting"; return false; }
    // Allocation and metadata are complete before recording any copy. Allocation
    // failure cannot leave a half-recorded command list that reads freed objects.
    if (!Allocate(device, index, inputs)) {
        InterlockedExchange(&shared_->slots[index].state, fw::kFree); Fail("FrameWarp slot allocation failed"); return false;
    }
    auto& meta = shared_->slots[index];
    meta.frame_id = ++nextFrame_; meta.fence_value = ++nextFence_;
    meta.qpc_sim_start = 0; meta.qpc_constants = cameraQpc; meta.qpc_present = fw::qpc_now();
    meta.camera = camera; meta.camera.position_epoch = epoch_;
    meta.camera.reset |= resetCamera_ ? 1u : 0u; resetCamera_ = false;
    std::memset(meta.tex, 0, sizeof(meta.tex));
    pending_ = index; slots_[index].submitted = meta.fence_value;
    shared_->settings.show_original = original_;
    if (timestamps_) list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
    for (int i = 0; i < 5; ++i) {
        const auto desc = inputs[i]->GetDesc();
        auto& info = meta.tex[kinds[i]];
        info.width = info.ext_w = static_cast<UINT>(desc.Width); info.height = info.ext_h = desc.Height;
        info.format = desc.Format; info.generation = slots_[index].generations[i]; info.valid = 1;
        D3D12_RESOURCE_BARRIER b[2]{};
        for (auto& v : b) { v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            v.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; v.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; }
        b[0].Transition.pResource = inputs[i]; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1].Transition.pResource = slots_[index].textures[i].Get(); b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(2, b); list->CopyResource(slots_[index].textures[i].Get(), inputs[i]);
        for (auto& v : b) std::swap(v.Transition.StateBefore, v.Transition.StateAfter);
        list->ResourceBarrier(2, b);
    }
    if (timestamps_) {
        list->EndQuery(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
        list->ResolveQueryData(timestamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2, 2,
            timestampReadback_.Get(), sizeof(std::uint64_t) * index * 2);
        slots_[index].timestampPending = true;
    }
    return true;
}
void Capture::Submitted(ID3D12CommandQueue* queue) {
    if (pending_ < 0) return;
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device_.Get() ||
        FAILED(queue->Signal(fence_.Get(), shared_->slots[pending_].fence_value))) {
        Fail("FrameWarp copy Signal failed; captured resources retained"); return;
    }
    if (!gpuFrequency_ && FAILED(queue->GetTimestampFrequency(&gpuFrequency_))) gpuFrequency_ = 0;
    InterlockedExchange(&shared_->slots[pending_].state, fw::kReady);
    InterlockedExchange64(&shared_->latest_ready_frame, static_cast<LONG64>(shared_->slots[pending_].frame_id));
    InterlockedExchange64(&shared_->producer_heartbeat_qpc, fw::qpc_now());
    InterlockedIncrement(&shared_->presents_total); InterlockedIncrement(&shared_->frames_total);
    InterlockedExchange(&shared_->presents_without_frame, 0);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&shared_->settings.enabled), 1);
    pending_ = -1; active_ = true; ++published_; message_ = "native FrameWarp source published (own engine, FG off)";
}
void Capture::Collect(std::uint64_t completed) {
    if (!timestampReadback_ || !gpuFrequency_) return;
    for (int i = 0; i < fw::kSlots; ++i) {
        auto& slot = slots_[i];
        if (!slot.timestampPending || slot.submitted > completed) continue;
        slot.timestampPending = false;
        const SIZE_T start = sizeof(std::uint64_t) * i * 2;
        const D3D12_RANGE range{start, start + sizeof(std::uint64_t) * 2};
        void* mapped{};
        if (FAILED(timestampReadback_->Map(0, &range, &mapped))) continue;
        const auto* stamp = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(mapped) + start);
        const auto begin = stamp[0], end = stamp[1];
        const D3D12_RANGE written{0, 0}; timestampReadback_->Unmap(0, &written);
        if (end < begin) continue;
        const auto ms = double(end - begin) * 1000.0 / double(gpuFrequency_);
        ++copyGPUSamples_; copyGPUMs_ += ms; copyGPUMaxMs_ = (std::max)(copyGPUMaxMs_, ms);
    }
}
bool Capture::Reset(bool hostRetired) {
    Suspend("disabled; capture retired");
    const auto completed = fence_ ? fence_->GetCompletedValue() : 0;
    if (!hostRetired && (pending_ >= 0 || faulted_ || completed == UINT64_MAX ||
        std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.submitted > completed; }))) return false;
    if (shared_) { shared_->magic = 0; UnmapViewOfFile(shared_); shared_ = nullptr; }
    Close(job_); Close(process_);
    for (auto& slot : slots_) { for (auto& h : slot.handles) Close(h); slot = {}; }
    Close(fenceHandle_); Close(mapping_); fence_.Reset(); device_.Reset(); timestamps_.Reset(); timestampReadback_.Reset(); gpuFrequency_ = 0;
    pending_ = -1; nextFence_ = nextFrame_ = 0; faulted_ = false; resetCamera_ = true; ++epoch_;
    return true;
}
}
