#include "GPU.h"
#include "FrameGen/StartupFallback.h"

int main() {
    GPU gpu;
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"TRPNormalTransportFixture";
    RegisterClassW(&wc);
    auto window=CreateWindowW(wc.lpszClassName,L"TRP standalone transport",WS_POPUP,0,0,64,32,nullptr,nullptr,wc.hInstance,nullptr);
    Require(window!=nullptr,"hidden transport window");
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<IDXGIAdapter> adapter; Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"same WARP adapter");
    ComPtr<ID3D11Device> device11; ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device11,nullptr,&context),"D3D11 producer");
    Interop interop; Check(interop.Initialize(device11.Get(),gpu.device.Get(),gpu.queue.Get()),"production shared transport");
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferDesc.Width=64; desc.BufferDesc.Height=32; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.OutputWindow=window;
    desc.Windowed=TRUE; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain> native; Check(factory->CreateSwapChain(gpu.queue.Get(),&desc,&native),"normal D3D12 presenter");
    ComPtr<IDXGISwapChain3> chain; Check(native.As(&chain),"indexed native buffers");
    for(unsigned size:{64u,96u}) {
        if(size!=64) Check(chain->ResizeBuffers(2,size,32,DXGI_FORMAT_UNKNOWN,0),"retired normal transport resize");
        D3D11_TEXTURE2D_DESC td{}; td.Width=size; td.Height=32; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1; td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        SharedTexture source; Check(interop.CreateSharedTexture(td,source),"shared producer output");
        ComPtr<ID3D11RenderTargetView> rtv; Check(device11->CreateRenderTargetView(source.texture11.Get(),nullptr,&rtv),"source RTV");
        for(unsigned frame=0;frame<3;++frame) {
            const float color[4]{.2f+frame*.1f,.4f,.6f,1}; context->ClearRenderTargetView(rtv.Get(),color);
            ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"native backbuffer");
            Check(interop.SignalD3D11(Work::SwapChain),"producer ready"); ID3D12GraphicsCommandList* list{};
            Check(interop.Begin(Work::SwapChain,&list),"native copy begin");
            Check(Interop::RecordCopy(list,source.texture12.Get(),back.Get()),"production normal output copy");
            Check(interop.Submit(Work::SwapChain),"native copy submit"); Check(interop.Drain(),"copy retired");
            auto read=gpu.Read(back.Get());
            for(unsigned c=0;c<3;++c) Near(read[0][c],color[c],1.1/255,"normal transport actual pixels");
            Check(chain->Present(0,0),"normal Present completes");
            Check(interop.WaitForInputReaders(nullptr,0),"post-Present fence bridge");
            Check(interop.Drain(),"present retirement");
        }
    }
    chain.Reset(); native.Reset(); DestroyWindow(window);
    std::puts("PASS: normal native presenter, production D3D11/D3D12 handoff, pixels, repeated Presents and resize without vendor SDK");
}
