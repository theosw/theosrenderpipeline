// A test-only COM C-interface timing forwarder. It presents a distinct context
// pointer over the same WARP immediate queue; no renderer alias registry is used.
#define COBJMACROS
#include <d3d11.h>
#include <string.h>
#include <stdlib.h>

static ID3D11DeviceContext alias;
static ID3D11DeviceContextVtbl methods;
static ID3D11DeviceContext* underlying;
static unsigned ends, inspections;
enum { MaxFrames = 64, MaxTimestamps = 24 };
typedef struct ScriptFrame {
    ID3D11Asynchronous* disjoint;
    ID3D11Asynchronous* timestamps[MaxTimestamps];
    unsigned timestampCount, polls, edgePolls[MaxTimestamps];
    HRESULT result, edgeResult;
    int blockedEdge;
    BOOL scripted, disjointValue, underlyingReady, edgeReady[MaxTimestamps];
    UINT64 frequency, ticks[MaxTimestamps];
} ScriptFrame;
static ScriptFrame frames[MaxFrames];
static unsigned frameCount, getDataCalls;
static int currentFrame = -1;
static BOOL scripting;

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
{
    (void)self;
    if (scripting) {
        if (frameCount == MaxFrames) { abort(); }
        currentFrame = (int)frameCount++;
        frames[currentFrame].disjoint = query;
    }
    ID3D11DeviceContext_Begin(underlying, query);
}
static void STDMETHODCALLTYPE End(ID3D11DeviceContext* self, ID3D11Asynchronous* query)
{
    (void)self; ++ends;
    if (scripting && currentFrame >= 0) {
        ScriptFrame* frame = &frames[currentFrame];
        if (query == frame->disjoint) { currentFrame = -1; }
        else {
            if (frame->timestampCount == MaxTimestamps) { abort(); }
            frame->timestamps[frame->timestampCount++] = query;
        }
    }
    ID3D11DeviceContext_End(underlying, query);
}
static HRESULT STDMETHODCALLTYPE GetData(ID3D11DeviceContext* self, ID3D11Asynchronous* query,
    void* data, UINT size, UINT flags)
{
    HRESULT actual;
    int index;
    (void)self; ++getDataCalls;
    if (flags != D3D11_ASYNC_GETDATA_DONOTFLUSH) { abort(); }
    // Never synthesize a value/outcome before this underlying query reports
    // S_OK. This makes no claim about the readiness of other query objects.
    actual = ID3D11DeviceContext_GetData(underlying, query, data, size, flags);
    if (!scripting) { return actual; }
    for (index = (int)frameCount - 1; index >= 0; --index) {
        ScriptFrame* frame = &frames[index];
        unsigned edge;
        if (frame->disjoint == query) {
            ++frame->polls;
            if (actual == S_OK) { frame->underlyingReady = TRUE; }
            if (actual != S_OK || !frame->scripted) { return actual; }
            if (frame->result != S_OK) { return frame->result; }
            if (size != sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT) || !data) { abort(); }
            ((D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*)data)->Frequency = frame->frequency;
            ((D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*)data)->Disjoint = frame->disjointValue;
            return S_OK;
        }
        for (edge = 0; edge < frame->timestampCount; ++edge) {
            if (frame->timestamps[edge] != query) { continue; }
            ++frame->edgePolls[edge];
            if (actual == S_OK) { frame->edgeReady[edge] = TRUE; }
            if (actual != S_OK || !frame->scripted) { return actual; }
            if ((int)edge == frame->blockedEdge && frame->edgeResult != S_OK) { return frame->edgeResult; }
            if (size != sizeof(UINT64) || !data) { abort(); }
            *(UINT64*)data = frame->ticks[edge];
            return S_OK;
        }
    }
    return actual;
}

// The caller keeps the underlying context alive and uses only these timing APIs.
ID3D11DeviceContext* TRPTimingContextAlias(ID3D11DeviceContext* context)
{
    underlying = context; ends = inspections = getDataCalls = frameCount = 0;
    scripting = FALSE; currentFrame = -1; memset(frames, 0, sizeof(frames));
    methods.QueryInterface = Query; methods.AddRef = AddRef; methods.Release = Release;
    methods.GetDevice = GetDevice; methods.GetType = GetType;
    methods.Begin = Begin; methods.End = End; methods.GetData = GetData;
    alias.lpVtbl = &methods;
    return &alias;
}
unsigned TRPTimingContextAliasEnds(void) { return ends; }
unsigned TRPTimingContextAliasInspections(void) { return inspections; }

void TRPTimingScriptFrame(unsigned index, HRESULT result, UINT64 frequency, BOOL disjoint,
    UINT64 copyBegin, UINT64 copyEnd)
{
    ScriptFrame* frame;
    if (index >= MaxFrames) { abort(); }
    scripting = TRUE; frame = &frames[index]; frame->scripted = TRUE;
    frame->result = result; frame->frequency = frequency; frame->disjointValue = disjoint;
    frame->blockedEdge = -1; frame->edgeResult = S_OK;
    frame->ticks[0] = 1000; frame->ticks[1] = copyBegin;
    frame->ticks[2] = copyEnd; frame->ticks[3] = 100000;
}
void TRPTimingScriptEdge(unsigned frame, int edge, HRESULT result)
{
    if (frame >= MaxFrames || edge < 0 || edge >= MaxTimestamps) { abort(); }
    frames[frame].blockedEdge = edge; frames[frame].edgeResult = result;
}
unsigned TRPTimingScriptPolls(unsigned frame) { if (frame >= MaxFrames) { abort(); } return frames[frame].polls; }
unsigned TRPTimingScriptEdgePolls(unsigned frame, unsigned edge)
{ if (frame >= MaxFrames || edge >= MaxTimestamps) { abort(); } return frames[frame].edgePolls[edge]; }
unsigned TRPTimingScriptCalls(void) { return getDataCalls; }
unsigned TRPTimingScriptFrames(void) { return frameCount; }
BOOL TRPTimingScriptReady(unsigned frame) { if (frame >= MaxFrames) { abort(); } return frames[frame].underlyingReady; }
BOOL TRPTimingScriptEdgeReady(unsigned frame, unsigned edge)
{ if (frame >= MaxFrames || edge >= MaxTimestamps) { abort(); } return frames[frame].edgeReady[edge]; }
