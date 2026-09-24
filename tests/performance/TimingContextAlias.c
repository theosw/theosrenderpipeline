// A test-only COM C-interface timing forwarder. It presents a distinct context
// pointer over the same WARP immediate queue; no renderer alias registry is used.
#define COBJMACROS
#include <d3d11.h>

static ID3D11DeviceContext alias;
static ID3D11DeviceContextVtbl methods;
static ID3D11DeviceContext* underlying;
static unsigned ends, inspections;

static HRESULT STDMETHODCALLTYPE Query(ID3D11DeviceContext* self, REFIID iid, void** result)
{
    (void)self; ++inspections;
    return ID3D11DeviceContext_QueryInterface(underlying, iid, result);
}
static ULONG STDMETHODCALLTYPE AddRef(ID3D11DeviceContext* self)
{ (void)self; return ID3D11DeviceContext_AddRef(underlying); }
static ULONG STDMETHODCALLTYPE Release(ID3D11DeviceContext* self)
{ (void)self; return ID3D11DeviceContext_Release(underlying); }
static void STDMETHODCALLTYPE GetDevice(ID3D11DeviceContext* self, ID3D11Device** device)
{ (void)self; ++inspections; ID3D11DeviceContext_GetDevice(underlying, device); }
static D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType(ID3D11DeviceContext* self)
{ (void)self; ++inspections; return ID3D11DeviceContext_GetType(underlying); }
static void STDMETHODCALLTYPE Begin(ID3D11DeviceContext* self, ID3D11Asynchronous* query)
{ (void)self; ID3D11DeviceContext_Begin(underlying, query); }
static void STDMETHODCALLTYPE End(ID3D11DeviceContext* self, ID3D11Asynchronous* query)
{ (void)self; ++ends; ID3D11DeviceContext_End(underlying, query); }
static HRESULT STDMETHODCALLTYPE GetData(ID3D11DeviceContext* self, ID3D11Asynchronous* query,
    void* data, UINT size, UINT flags)
{ (void)self; return ID3D11DeviceContext_GetData(underlying, query, data, size, flags); }

// The caller keeps the underlying context alive and uses only these timing APIs.
ID3D11DeviceContext* TRPTimingContextAlias(ID3D11DeviceContext* context)
{
    underlying = context; ends = inspections = 0;
    methods.QueryInterface = Query; methods.AddRef = AddRef; methods.Release = Release;
    methods.GetDevice = GetDevice; methods.GetType = GetType;
    methods.Begin = Begin; methods.End = End; methods.GetData = GetData;
    alias.lpVtbl = &methods;
    return &alias;
}
unsigned TRPTimingContextAliasEnds(void) { return ends; }
unsigned TRPTimingContextAliasInspections(void) { return inspections; }
