#pragma once
#include <d3dcompiler.h>
#include <chrono>
#include <cstring>

// Actual dependent shader arithmetic, calibrated on the selected hardware. This
// is deliberately not NR and does not reproduce its memory/queue contention.
class AlternatingLoad
{
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> parameters_, output_;
    ComPtr<ID3D11UnorderedAccessView> uav_;
    ComPtr<ID3D11Query> done_;
    ComPtr<ID3D11Query> disjoint_, start_, end_;
    unsigned loops_{1024};
public:
    void Record(ID3D11DeviceContext* context)
    {
        const unsigned data[4]{loops_, 0, 0, 0};
        context->UpdateSubresource(parameters_.Get(), 0, nullptr, data, 0, 0);
        auto* buffer = parameters_.Get(); auto* uav = uav_.Get();
        context->CSSetShader(shader_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &buffer);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch(64, 1, 1);
        uav = nullptr; context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context, double milliseconds)
    {
        const char* source = R"(
cbuffer Params : register(b0) { uint iterations; };
RWStructuredBuffer<float> result : register(u0);
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    float x = 0.01 + id.x * 0.000001;
    for (uint i=0; i<iterations; ++i) x = frac(x * 1.00013 + 0.00017);
    result[id.x] = x;
})";
        ComPtr<ID3DBlob> code, error;
        Check(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &error), "alternating shader compile");
        Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_), "alternating shader");
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 16; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&desc, nullptr, &parameters_), "load parameters");
        desc.ByteWidth = 4096 * sizeof(float); desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride = sizeof(float);
        Check(device->CreateBuffer(&desc, nullptr, &output_), "load output");
        Check(device->CreateUnorderedAccessView(output_.Get(), nullptr, &uav_), "load UAV");
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        Check(device->CreateQuery(&query, &done_), "load completion");
        query.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        Check(device->CreateQuery(&query, &disjoint_), "load frequency");
        query.Query = D3D11_QUERY_TIMESTAMP;
        Check(device->CreateQuery(&query, &start_), "load start");
        Check(device->CreateQuery(&query, &end_), "load end");
        double elapsed{}, gpuMs{};
        for (unsigned i = 0; i < 5; ++i) {
            const auto start = std::chrono::steady_clock::now();
            context->Begin(disjoint_.Get()); context->End(start_.Get());
            Record(context); context->End(end_.Get()); context->End(disjoint_.Get());
            context->End(done_.Get()); context->Flush();
            while (context->GetData(done_.Get(), nullptr, 0, 0) == S_FALSE) Sleep(0);
            elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            UINT64 begin{}, end{};
            Require(context->GetData(disjoint_.Get(), &frequency, sizeof(frequency), 0) == S_OK && !frequency.Disjoint && frequency.Frequency,
                "valid load GPU frequency");
            Require(context->GetData(start_.Get(), &begin, sizeof(begin), 0) == S_OK &&
                context->GetData(end_.Get(), &end, sizeof(end), 0) == S_OK && end >= begin, "valid load GPU timestamps");
            gpuMs = 1000. * double(end - begin) / double(frequency.Frequency);
            if (i < 4) loops_ = std::clamp(static_cast<unsigned>(loops_ * milliseconds / (std::max)(gpuMs, .01)), 1u, 10000000u);
        }
        std::printf("BEAT_LOAD calibratedCpuCompletionMs=%.3f gpuMs=%.3f requestedMs=%.3f iterations=%u syntheticShader=true\n", elapsed, gpuMs, milliseconds, loops_);
        Require(gpuMs >= milliseconds * .85 && gpuMs <= milliseconds * 1.15, "load calibration near target");
    }
};
