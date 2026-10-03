#pragma once
#include "GPU.h"
#include <fstream>

// Analytic current-to-previous motion, normalized to texture dimensions.
// Each 1.5-second case starts with a cut; no model or compose history crosses it.
struct ReplayScene {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    ReplayScene(ID3D12Device* d,ID3D12Resource* color,ID3D12Resource* motion,ID3D12Resource* depth) {
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,5};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={1,&range};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
        ComPtr<ID3DBlob> blob,error;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"replay root serialize");
        Check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"replay root");
        const char shader[]=R"(
        RWTexture2D<float4> Color:register(u0);RWTexture2D<float2> Motion:register(u1);RWTexture2D<float> Depth:register(u2);
        cbuffer Params:register(b0){uint Width,Height;float Time,PreviousTime;uint Case;}
        [numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
            if(p.x>=Width||p.y>=Height)return;
            float aspect=(float)Width/Height;float2 screen=(p.xy+.5)/Height;
            float camera=.22*Time,previousCamera=.22*PreviousTime;
            float2 world=screen+float2(camera,0);
            float checker=fmod(floor(world.x*20)+floor(world.y*20),2)!=0?.075:0;
            float3 c=float3(.19+.05*sin(world.x*8),.22+.04*cos(world.y*7),.27)+checker;
            // Fine fixed edges and an oblique line expose history/registration errors.
            if(frac(world.x*45)<.04||abs(world.y-.18-.12*sin(world.x*3))<.005)c*=.35;
            float2 mv=float2(camera-previousCamera,0);float depth=.25;
            if(Case==1 || Case==3){
                float object=.30+.65*Time,previousObject=.30+.65*PreviousTime;
                float2 local=world-float2(object,.5);
                if(abs(local.x)<.16 && abs(local.y)<.26){
                    c=float3(.62,.20,.10)+.09*cos(local.y*80);
                    mv.x+=previousObject-object;depth=.75;
                }
            }
            if(Case==2){
                for(uint i=0;i<18;++i){
                    float x=.12+i*.087+.028*sin(Time*4+i);
                    float previousX=.12+i*.087+.028*sin(PreviousTime*4+i);
                    float thickness=(i%3+1)*.0018;
                    if(abs(world.x-x)<thickness && screen.y>.12 && screen.y<.88){
                        c=float3(.12,.50,.22);depth=.60;mv.x+=previousX-x;
                    }
                }
            }
            if(Case==3)c*=Time<.6?.65:1.6;
            Color[p.xy]=float4(c,1);
            Motion[p.xy]=mv*float2((float)Height/Width,1);
            // Reversed depth: greater values are nearer.
            Depth[p.xy]=depth;
        })";
        const auto hr=D3DCompile(shader,sizeof(shader)-1,"analytic-NR-replay",nullptr,nullptr,"main","cs_5_1",0,0,&blob,&error);
        if(FAILED(hr)&&error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
        Check(hr,"replay shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
        Check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"replay pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"replay descriptors");
        auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        for(auto* r:{color,motion,depth}){
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=r->GetDesc().Format;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            d->CreateUnorderedAccessView(r,nullptr,&uav,cpu);cpu.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    void Record(GPU& gpu,ID3D12Resource* color,ID3D12Resource* motion,ID3D12Resource* depth,float time,float previous,unsigned scene) {
        for(auto* r:{color,motion,depth})gpu.Barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* hd=heap.Get();gpu.list->SetDescriptorHeaps(1,&hd);gpu.list->SetComputeRootSignature(root.Get());gpu.list->SetPipelineState(pipeline.Get());
        const auto d=color->GetDesc();struct {unsigned w,h;float t,p;unsigned scene;}constants{unsigned(d.Width),d.Height,time,previous,scene};
        gpu.list->SetComputeRoot32BitConstants(0,5,&constants,0);
        gpu.list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());gpu.list->Dispatch((constants.w+7)/8,(constants.h+7)/8,1);
        for(auto* r:{color,motion,depth})gpu.Barrier(r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    }
};

// Record copies on the existing host submission. Map/write only after replay.
struct ReplayCapture {
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 size{};
    ReplayCapture(GPU& gpu,ID3D12Resource* r){
        const auto d=r->GetDesc();gpu.device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);
        readback=gpu.Buffer(size,D3D12_HEAP_TYPE_READBACK);
    }
    void Record(GPU& gpu,ID3D12Resource* r){
        gpu.Barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=r;dst.pResource=readback.Get();
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layout;
        gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        gpu.Barrier(r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    void Write(const std::filesystem::path& path){
        std::ofstream file(path,std::ios::binary);Require(bool(file),"capture file");
        void* data{};D3D12_RANGE range{0,size_t(size)};Check(readback->Map(0,&range,&data),"capture map");
        for(unsigned y=0;y<layout.Footprint.Height;++y)
            file.write(static_cast<char*>(data)+y*layout.Footprint.RowPitch,layout.Footprint.Width*8);
        D3D12_RANGE none{};readback->Unmap(0,&none);file.flush();Require(bool(file),"flush capture");
    }
};
