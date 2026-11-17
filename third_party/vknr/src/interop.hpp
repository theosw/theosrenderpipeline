#pragma once
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include "nrvk.hpp"
#include <array>
#include <memory>

namespace nrtest {
// Dedicated shared D3D12 resources, matched by Vulkan device LUID. No registry or hooks.
class Interop {
 template<class T> using Com=Microsoft::WRL::ComPtr<T>;
 struct Texture {Com<ID3D12Resource> resource;nrvk::Context::Image image;DXGI_FORMAT format;unsigned bpp;};
 struct Job {
  Com<ID3D12CommandAllocator> before_allocator,after_allocator;
  Com<ID3D12GraphicsCommandList> before,after;
  std::vector<Com<ID3D12Resource>> uploads;
  Com<ID3D12Resource> readback;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  VkCommandBuffer command{};
 };
 nrvk::Context& ctx;
 Com<ID3D12Device> device;Com<ID3D12CommandQueue> queue;
 Com<ID3D12Fence> timeline,drain;
 Com<ID3D12QueryHeap> queries;Com<ID3D12Resource> timestamp_readback;
 uint64_t frequency=0;
 VkSemaphore semaphore{};VkCommandPool pool{};
 std::array<Texture,4> textures;std::vector<Job> jobs;
 bool finished=false;unsigned width,height;
 Com<ID3D12Resource> buffer(UINT64,D3D12_HEAP_TYPE);
 void make_texture(Texture&,DXGI_FORMAT,VkFormat,unsigned);
 void upload(Job&,Texture&,const void*);
 void ownership(VkCommandBuffer,bool,bool initial=false);
 static void hr(HRESULT,const char*);
public:
 Interop(nrvk::Context&,unsigned,unsigned,unsigned);
 ~Interop();
 const nrvk::Context::Image& colour()const{return textures[0].image;}
 const nrvk::Context::Image& motion()const{return textures[1].image;}
 const nrvk::Context::Image& depth()const{return textures[2].image;}
 const nrvk::Context::Image& output()const{return textures[3].image;}
 VkCommandBuffer begin(unsigned,const uint16_t*,const float*,const float*);
 void acquire(VkCommandBuffer c){ownership(c,true);}
 void release(VkCommandBuffer c){ownership(c,false);}
 void submit(unsigned,bool capture);
 void finish();
 void read(unsigned,std::vector<uint16_t>&);
 std::array<double,3> copy_times(unsigned);
};
}
