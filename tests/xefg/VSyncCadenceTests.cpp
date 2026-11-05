// Exercise the production hook admission and lifetime code without installing
// hooks, creating a graphics device or loading any private runtime.
#include "FrameGen/XeFGExtension/pacing/sdk_pacing.cpp"
#include <cstdio>
#include <cstdlib>

#include "FrameGen/XeFGExtension/PacingBridge.cpp"

namespace {
using namespace sdk_pacing;
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
template<class T, size_t N> void Put(std::array<unsigned char, N>& bytes, size_t offset, T value) {
    Require(offset + sizeof(value) <= N, "fixture bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
unsigned calledPace{}, calledPresent{}, calledRefresh{};
UINT expectedSync = 1;
HRESULT presentResult = S_FALSE;
NvFlipConfigV2 lastPacket{};
int driverOffResult{};
int __cdecl FakeDriver(IUnknown*, UINT source, void* packet) {
    Require(source==0,"unchanged V2 source id");
    auto* value=static_cast<NvFlipConfigV2*>(packet); lastPacket=*value;
    return value->nFramesPerBatch ? 0 : driverOffResult;
}
bool FakePace(void*, const void*, uint8_t, const void*, uint32_t) { ++calledPace; return true; }
int64_t FakeRefresh(Owner&, int64_t) noexcept { ++calledRefresh; return 1000000; }
HRESULT FakePresent(void*, UINT sync, UINT flags, uint64_t, const DXGI_PRESENT_PARAMETERS*, ID3D12Resource* const*, bool) {
    Require(sync == expectedSync && flags == 0, "forward submitted sync unchanged");
    ++calledPresent; SetLastError(1234); return presentResult;
}
struct Fixture {
    alignas(8) std::array<unsigned char, 0x400> presenter{};
    alignas(8) std::array<unsigned char, 0x200> chain{};
    alignas(8) std::array<unsigned char, 0x40> implementation{}, handle{};
    alignas(8) std::array<unsigned char, 0x110> stack{};
    alignas(8) std::array<unsigned char, 0xc8> bundle{};
    std::array<ID3D12Resource*, 5> resources{};
    Route current;
    std::shared_ptr<Owner> owner{std::make_shared<Owner>()};
    Invocation call{};
    Fixture() {
        vsyncCadenceEnabled.store(true); unsafe.store(false); flipFunction.store(nullptr); flipEnabled.store(false);
        current.module = reinterpret_cast<HMODULE>(0x10000000);
        current.original = FakePresent; current.pace = FakePace; current.refreshQuery = FakeRefresh; current.enabled.store(true);
        owner->handle = handle.data(); owner->generation = 1;
        owner->implementation = uintptr_t(implementation.data()); owner->chain = uintptr_t(chain.data());
        owner->presenter = uintptr_t(presenter.data()); owner->native = 0x20000000; owner->device = 0x30000000;
        Put(handle, 0, uint32_t(0x47466558)); Put(handle, 8, owner->implementation);
        Put(implementation, kImplementationChain, owner->chain);
        Put(chain, kChainPresenter, owner->presenter); Put(chain, kChainNative, owner->native);
        Put(presenter, 0, owner->device); Put(presenter, 0x18, owner->native);
        Put(presenter, 0x341, uint8_t(1));
        for (size_t i = 0; i < resources.size(); ++i) resources[i] = reinterpret_cast<ID3D12Resource*>(0x40000000 + i * 256);
        call = {presenter.data(), 1, 0, 100, reinterpret_cast<DXGI_PRESENT_PARAMETERS*>(bundle.data() + 0x38),
            resources.data() + 1, true, uintptr_t(current.module) + kExtraReturn, uintptr_t(stack.data())};
        Put(stack, 0, call.returnAddress); Put(stack, kBundleFromReturn, uintptr_t(bundle.data()));
        Put(stack, kSummaryFromReturn, int64_t(6)); Put(stack, kSummaryFromReturn + 8, int64_t(55000000));
        Put(bundle, 0, uintptr_t(resources.data())); Put(bundle, 8, uint64_t(3));
        Mode(1, 0); Put(bundle, 0x30, call.frame); Put(bundle, 0xc0, uint8_t(1));
        Seed(4,1);
        current.owners.store(std::make_shared<const Owners>(Owners{owner}));
    }
    void Mode(UINT sync, UINT flags) { call.sync = sync; call.flags = flags; Put(bundle, 0x28, sync); Put(bundle, 0x2c, flags); }
    void Seed(uint32_t outputs, UINT sync) {
        const auto now=NowNs(); owner->vsyncCadence.Reset();
        owner->vsyncCadence.Begin(99,outputs,sync,now-54000000,1000000);
        owner->vsyncCadence.Begin(100,outputs,sync,now,1000000);
    }
    void FirstCall() {
        firstLayout = true;
        call.returnAddress = uintptr_t(current.module) + kFirstReturn;
        call.resource = resources.data();
        Put(stack,0,call.returnAddress); Put(stack,0xc8,call.sync); Put(stack,0xd0,call.flags);
        Put(stack,0xd8,call.frame); Put(stack,0xe0,uintptr_t(call.parameters));
        Put(stack,0xe8,uintptr_t(bundle.data())); Put(stack,0xf0,uint32_t(1));
        owner->vsyncCadence.Reset();
    }
    const char* PrepareCall() { Extra extra{}; return Prepare(current, *owner, call, extra); }
};
template<class Mutation> void Refuses(Mutation mutation, const char* message) {
    Fixture f; mutation(f); Require(f.PrepareCall() != nullptr, message);
}
}

int main() {
    using namespace sdk_pacing;
    Require(!vsyncCadenceEnabled.load(), "VSync cadence defaults off");
    {
        TheosRenderPipeline::XeFGPacing::API api;
        api.version=1;
        Require(TRPXeFGPacingGetAPI(&api)==E_NOINTERFACE && !api.setVSyncCadence, "v1 caller refused without writing callbacks");
        api.version=TheosRenderPipeline::XeFGPacing::Version; api.size-=sizeof(void*);
        Require(TRPXeFGPacingGetAPI(&api)==E_NOINTERFACE, "old API size refused");
        api.size=sizeof(api);
        Require(TRPXeFGPacingGetAPI(&api)==S_OK && api.setVSyncCadence, "v2 switch exported");
        api.setVSyncCadence(true); Require(vsyncCadenceEnabled.load(), "API enables live cadence");
        api.setVSyncCadence(false); Require(!vsyncCadenceEnabled.load(), "API disables live cadence");
    }

    {
        Fixture f; route.store(&f.current);
        f.owner->vsyncRefresh.periodNs=6944444;
        f.owner->flip.anchorNs=123456; f.owner->flip.calls=42;
        const auto flipBefore = f.owner->flip;
        sdk_pacing::SetVSyncCadence(false);
        Require(std::strcmp(f.PrepareCall(), "different_present_mode")==0, "off gives exact legacy refusal");
        Require(!f.owner->vsyncCadence.Matches(100,4) && !f.owner->vsyncRefresh.periodNs, "off clears sync state");
        f.Mode(0,0); Require(!f.PrepareCall(), "off preserves sync zero");
        expectedSync=0; calledPace=0; calledPresent=0;
        Dispatch(f.current,f.call);
        Require(calledPace==1 && calledPresent==1, "off dispatch still invokes supplemental sync-zero Pace");
        expectedSync=1;
        f.Mode(1,0); sdk_pacing::SetVSyncCadence(true);
        Require(f.PrepareCall()!=nullptr, "on requires fresh witness");
        f.Seed(4,1); Require(!f.PrepareCall(), "live on admits fresh batch");
        sdk_pacing::SetVSyncCadence(false);
        Require(std::strcmp(f.PrepareCall(), "different_present_mode")==0, "live off restores legacy refusal");
        Require(std::memcmp(&flipBefore,&f.owner->flip,sizeof(flipBefore))==0, "toggle leaves driver state byte exact");
        route.store(nullptr);
    }
    {
        Fixture f;
        for (UINT sync : {0u, 1u, 2u, 3u, 4u}) {
            f.Mode(sync, 0); if(sync) f.Seed(4,sync);
            Require(!f.PrepareCall(), "valid sync admission");
        }
        f.Mode(0, DXGI_PRESENT_ALLOW_TEARING); Require(!f.PrepareCall(), "existing immediate admission");
        f.Mode(1, 0);
        Put(f.bundle, 8, uint64_t(2)); f.Seed(3,1);
        Require(!f.PrepareCall(), "x3 admission");
        Put(f.bundle, 8, uint64_t(3)); f.Seed(4,1);
        Require(!f.PrepareCall(), "x4 admission");
        uint32_t generated{};
        f.call.returnAddress = uintptr_t(f.current.module) + kFirstReturn;
        Require(!First(f.current, *f.owner, f.call, generated), "driver first-output admission still refuses sync 1");
    }
    Refuses([](auto& f){f.Mode(5,0);}, "invalid sync");
    Refuses([](auto& f){f.Mode(1,DXGI_PRESENT_ALLOW_TEARING);}, "sync with tearing");
    Refuses([](auto& f){f.Mode(1,DXGI_PRESENT_TEST);}, "test Present");
    Refuses([](auto& f){f.call.returnAddress++;}, "unverified caller");
    Refuses([](auto& f){f.call.generated=false;}, "real output is not supplemental");
    Refuses([](auto& f){f.call.presenter=nullptr;}, "presenter identity");
    Refuses([](auto& f){f.owner->retired=true;}, "retired owner");
    Refuses([](auto& f){f.owner->retiring=true;}, "retiring owner");
    Refuses([](auto& f){f.owner->disabled=true;}, "disabled owner");
    Refuses([](auto&){unsafe=true;}, "unsafe latch");
    Refuses([](auto& f){f.owner->flip.configured=true;}, "off acknowledgement required");
    Refuses([](auto& f){f.owner->flip.metered=true;}, "metered owner");
    Refuses([](auto& f){f.owner->vsyncCadence.Reset();}, "unverified first output");
    Refuses([](auto& f){
        const auto now=NowNs(); f.owner->vsyncCadence.Reset();
        f.owner->vsyncCadence.Begin(99,4,1,now-27777776,6944444);
        f.owner->vsyncCadence.Begin(100,4,1,now,6944444);
    }, "native refresh-limited batch refuses supplemental wait");
    Refuses([](auto& f){f.Mode(2,0);}, "first and supplemental sync must match");
    Refuses([](auto& f){f.call.returnSlot++;}, "stack alignment");
    Refuses([](auto& f){f.call.returnSlot=uintptr_t(f.owner.get());}, "stack bounds");
    Refuses([](auto& f){Put(f.stack,0,uintptr_t(0));}, "saved return identity");
    Refuses([](auto& f){Put(f.stack,kBundleFromReturn,uintptr_t(0));}, "bundle identity");
    Refuses([](auto& f){Put(f.chain,kChainPresenter,uintptr_t(0));}, "chain identity");
    Refuses([](auto& f){Put(f.presenter,0x18,uintptr_t(0));}, "native identity");
    Refuses([](auto& f){Put(f.presenter,0x64,uint8_t(1));}, "Intel pacing mode");
    Refuses([](auto& f){Put(f.presenter,0x340,uint8_t(1));}, "hardware pacing mode");
    Refuses([](auto& f){Put(f.presenter,0x341,uint8_t(0));}, "software pacing unavailable");
    Refuses([](auto& f){Put(f.bundle,8,uint64_t(1));}, "no supplemental x2 output");
    Refuses([](auto& f){Put(f.bundle,8,uint64_t(4));}, "unqualified x5 VSync remains refused");
    Refuses([](auto& f){f.call.resource=f.resources.data();}, "first resource is not supplemental");
    Refuses([](auto& f){f.call.resource=f.resources.data()+3;}, "resource bounds");
    Refuses([](auto& f){Put(f.bundle,0x28,UINT(0));}, "bundle sync mismatch");
    Refuses([](auto& f){Put(f.bundle,0x2c,UINT(1));}, "bundle flags mismatch");
    Refuses([](auto& f){Put(f.bundle,0x30,uint64_t(99));}, "bundle frame mismatch");
    Refuses([](auto& f){f.call.parameters=nullptr;}, "parameter identity");
    Refuses([](auto& f){f.resources[1]=nullptr;}, "missing output resource");
    Refuses([](auto& f){Put(f.stack,kSummaryFromReturn,int64_t(0));}, "missing timing history");
    Refuses([](auto& f){Put(f.stack,kSummaryFromReturn+8,int64_t(250000001));}, "paused timing history");
    {
        Fixture f; f.FirstCall(); uint32_t generated{};
        Require(FirstVSync(f.current,*f.owner,f.call,generated) && generated==3,"verified first-output identity");
        firstLayout=false; Require(!FirstVSync(f.current,*f.owner,f.call,generated),"first caller profile required"); firstLayout=true;
        Put(f.stack,0xc8,UINT(0)); Require(!FirstVSync(f.current,*f.owner,f.call,generated),"first sync stack identity");
        Put(f.stack,0xc8,UINT(1)); Put(f.stack,0xf0,uint32_t(2));
        Require(!FirstVSync(f.current,*f.owner,f.call,generated),"exact first output required"); Put(f.stack,0xf0,uint32_t(1));
        f.call.resource++;
        Require(!FirstVSync(f.current,*f.owner,f.call,generated),"first resource identity"); f.call.resource--;
        f.owner->flip.configured=true;
        Require(!FirstVSync(f.current,*f.owner,f.call,generated),"outstanding driver state refuses software observation");
        f.owner->flip.configured=false;
        presentResult=S_OK;
        Require(Dispatch(f.current,f.call)==S_OK && f.owner->vsyncCadence.Matches(100,4),"first arrival recorded before forwarding");
        presentResult=DXGI_STATUS_OCCLUDED;
        Require(Dispatch(f.current,f.call)==DXGI_STATUS_OCCLUDED && !f.owner->vsyncCadence.Matches(100,4),
            "occluded first Present clears sync history");
        presentResult=S_FALSE;
        f.Mode(0,0); Put(f.stack,0xc8,UINT(0));
        Require(First(f.current,*f.owner,f.call,generated) && generated==3,"original sync-0 driver first admission unchanged");
        f.owner->vsyncCadence.Begin(100,4,1,NowNs(),1000000); expectedSync=0;
        Require(Dispatch(f.current,f.call)==S_FALSE && !f.owner->vsyncCadence.Matches(100,4),
            "immediate first output clears stale VSync identity even if the frame id repeats");
        expectedSync=1;
    }
    {
        // A successful witness can be stale by the time observation takes the
        // retirement lock. Exercise that boundary without a racy thread test.
        for (unsigned state=0; state<4; ++state) {
            Fixture f; f.FirstCall(); uint32_t generated{};
            const bool admitted=FirstVSync(f.current,*f.owner,f.call,generated);
            Require(admitted,"live witness before retirement boundary");
            f.Seed(4,1);
            if(state==0) f.owner->retiring=true;
            if(state==1) f.owner->retired=true;
            if(state==2) f.owner->disabled=true;
            if(state==3) unsafe=true;
            const auto queries=calledRefresh;
            Require(!ObserveVSyncFirst(f.current,*f.owner,f.call,generated,admitted)
                && calledRefresh==queries && !f.owner->vsyncCadence.Matches(100,4),
                "stale admission cannot query a retired native chain or retain cadence");
        }
        Fixture f; f.FirstCall(); uint32_t generated{};
        Require(FirstVSync(f.current,*f.owner,f.call,generated),"live observation witness");
        const auto queries=calledRefresh;
        Require(ObserveVSyncFirst(f.current,*f.owner,f.call,generated,true)
            && calledRefresh==queries+1 && f.owner->vsyncCadence.Matches(100,4),
            "live observation invokes one refresh query under the retirement lock");
        Put(f.bundle,8,uint64_t(1));
        Require(!FirstVSync(f.current,*f.owner,f.call,generated),"native x2 first output is refused before the new owner walk");
    }
    {
        Fixture f; calledPace=calledPresent=0;
        Require(Dispatch(f.current,f.call)==S_FALSE && calledPace==1 && calledPresent==1 && GetLastError()==1234,
            "supplemental pacing preserves Present result and last error");
        f.owner->retiring=true;
        Require(Dispatch(f.current,f.call)==S_FALSE && calledPace==1 && calledPresent==2, "retiring forwards without extra wait");
    }
    {
        Fixture f;
        const auto start=NowNs()-60000000;
        f.owner->vsyncCadence.Reset();
        f.owner->vsyncCadence.Begin(99,4,1,start,1000000);
        f.owner->vsyncCadence.Begin(100,4,1,start+54000000,1000000);
        alignas(8) std::array<unsigned char,0x28> record{};
        Put(record,0,start+55000000); Put(record,0x10,uint32_t(100));
        Put(record,0x14,9.0f); Put(record,0x20,start+59000000);
        int64_t output=0;
        NativeDeadline(f.current,f.presenter.data()+0x168,&output,record.data(),1,4);
        Require(output==start+59000000+13500000-9000000,"production hook samples arrival but retains completion anchor and latency credit");
        Require(f.owner->batchInterval.Samples()==0,"new sync path does not contaminate driver/x2 history");
        Put(record,0x20,start+59500000); Put(record,0x14,9.0f);
        NativeDeadline(f.current,f.presenter.data()+0x168,&output,record.data(),1,4);
        Require(output==start+59500000+13500000-9000000 && f.owner->vsyncCadence.Interval().Typical()==54000000,
            "completion wait shifts this batch phase without training frame time");
        Put(record,0x20,int64_t(0)); output=12345;
        NativeDeadline(f.current,f.presenter.data()+0x168,&output,record.data(),1,4);
        Require(output==12345,"missing SDK first fence stamp retains SDK deadline");
    }
    {
        Fixture f; route.store(&f.current);
        f.owner->batchInterval.Observe(10,4,1000000000);
        f.owner->batchInterval.Observe(11,4,1050000000);
        Reset(f.owner->handle,2); Require(f.owner->batchInterval.Samples()==1,"wrong generation cannot reset");
        Reset(f.owner->handle,1); Require(!f.owner->batchInterval.Samples() && !f.owner->vsyncCadence.Matches(100,4),
            "reset clears both histories and VSync batch identity");
        Require(Unregister(f.owner->handle,1) && f.current.owners.load()->empty() && f.owner->retired,
            "teardown retires and removes only the acknowledged owner");
        route.store(nullptr);
    }
    {
        Fixture f;
        f.owner->vsyncCadence.Reset();
        for(uint32_t total : {2u,3u,4u,6u}) {
            const auto start=NowNs()-60000000;
            f.owner->batchInterval.Reset();
            f.owner->batchInterval.Observe(99,total,start);
            f.owner->batchInterval.Observe(100,total,start+54000000);
            alignas(8) std::array<unsigned char,0x28> record{};
            Put(record,0,start+55000000); Put(record,0x10,uint32_t(100));
            Put(record,0x14,2.0f); Put(record,0x20,start+54000000);
            int64_t output=0;
            NativeDeadline(f.current,f.presenter.data()+0x168,&output,record.data(),1,total);
            Require(output==start+54000000+54000000/total-2000000,"legacy x2 and sync-0 deadline result unchanged");
            if(total==4) {
                f.owner->flip.metered=true; f.owner->flip.outputs=4; f.owner->flip.spacing=13500000;
                f.owner->flip.batchNs=54000000; f.owner->flip.anchorFrame=100; f.owner->flip.anchorNs=start+53000000;
                SetFlipMetering(FakeDriver,true,2500000);
                NativeDeadline(f.current,f.presenter.data()+0x168,&output,record.data(),1,total);
                Require(output==start+64000000,"V2 metered anchor, spacing and lead remain exact");
                f.owner->flip.metered=false; SetFlipMetering(nullptr,false,2500000);
            }
        }
    }
    {
        Fixture f;
        // A non-owning sentinel is never invoked as an interface; Owner's
        // existing quarantine detaches it instead of calling Release.
        f.owner->flipQueue.Attach(reinterpret_cast<ID3D12CommandQueue*>(0x12340));
        SetFlipMetering(FakeDriver,true,2500000);
        driverOffResult=0;
        ProbeFlip(*f.owner);
        const NvFlipConfigV2 off{kFlipConfigVersion2,0,0,1,-1.0f,0.0f};
        Require(std::memcmp(&lastPacket,&off,sizeof(off))==0,"unchanged V2 off probe bytes");
        ConfigureFlip(*f.owner,4,54000000,nullptr);
        const NvFlipConfigV2 on{kFlipConfigVersion2,4,0,4,54.0f,0.0f};
        Require(std::memcmp(&lastPacket,&on,sizeof(on))==0 && f.owner->flip.metered,"unchanged V2 enable bytes and feedback");
        route.store(&f.current); driverOffResult=-1;
        Require(!Unregister(f.owner->handle,1) && unsafe && f.owner->flip.configured
            && f.owner->retiring && !f.owner->retired && f.current.owners.load()->size()==1,
            "failed off acknowledgement retains owner and unsafe latch");
        Require(std::memcmp(&lastPacket,&off,sizeof(off))==0,"unchanged V2 retirement packet");
        route.store(nullptr); SetFlipMetering(nullptr,false,2500000);
        driverOffResult=0;
    }
    {
        Fixture f;
        // A rejected capability probe never configured a driver batch. It may
        // admit software VSync; a failed off after enable must still retain.
        f.owner->flipQueue.Attach(reinterpret_cast<ID3D12CommandQueue*>(0x12340));
        SetFlipMetering(FakeDriver,true,2500000); driverOffResult=-1;
        ProbeFlip(*f.owner);
        Require(f.owner->flip.probed && !f.owner->flip.admitted && !f.owner->flip.configured
            && !unsafe && !f.PrepareCall(), "rejected V2 probe allows verified software VSync");
        SetFlipMetering(nullptr,false,2500000); driverOffResult=0;
    }
    {
        Fixture f;
        Require(ReadVSyncRefresh(*f.owner,NowNs())==0,"unverified native GetDesc cannot supply a display budget");
        f.owner->vsyncRefresh.periodNs=6944444;
        Revoke(*f.owner);
        Require(!f.owner->vsyncRefresh.periodNs && !f.owner->vsyncRefresh.window,"reset clears display budget cache");
        cadence::VSyncCadence sync;
        constexpr int64_t refresh=6944444;
        int64_t ready=1000000000;
        for(uint32_t frame=1;frame<=8;++frame){
            ready+=refresh*4; sync.Begin(frame,4,1,ready,refresh);
        }
        cadence::BatchIntervalEstimator::Decision d;
        Require(!sync.NeedsCadence() && !sync.Plan(8,4,1,ready+1000000,ready+1000000,0,d),
            "one native VSync quantum per output never adds a deadline");
        for(uint32_t frame=9;frame<=16;++frame){ready+=54000000;sync.Begin(frame,4,1,ready,refresh);}
        Require(sync.NeedsCadence(),"slower producer admits cadence beyond native refresh budget");
        for(uint32_t frame=17;frame<=24;++frame){ready+=31000000;sync.Begin(frame,4,1,ready,refresh);}
        Require(!sync.NeedsCadence(),"near-refresh return escapes earlier software back-pressure");
        sync.Begin(25,4,1,ready+27777776,8333333);
        Require(!sync.Interval().Samples(),"refresh-rate change invalidates prior interval history");
        Require(!sync.Begin(26,4,1,ready+54000000,0),"unknown refresh retains native pacing");
        sync.Begin(30,4,2,2000000000,refresh); sync.Begin(31,4,2,2056000000,refresh);
        Require(!sync.NeedsCadence(),"sync greater than one scales native refresh budget");
    }
    {
        // Real arrivals stay at 54 ms while the first Present/fence and later
        // deadlines vary. Only the arrival observes frame time; SDK completion
        // and the current deadline's invocation time must not train it.
        cadence::VSyncCadence sync;
        for (uint32_t frame=1; frame<=12; ++frame) {
            const int64_t ready=1000000000+int64_t(frame)*54000000;
            Require(sync.Begin(frame,4,1,ready,1000000), "sync batch arrival");
            for (uint32_t index=1; index<4; ++index) {
                cadence::BatchIntervalEstimator::Decision d;
                const auto now=ready+int64_t(index)*13000000+(frame%2?1000000:8000000);
                const bool planned=sync.Plan(frame,4,index,now,ready+(frame%2?1000000:8000000),1000000,d);
                if(frame>1) Require(planned && d.interval==13500000, "native and deadline waits do not train the sync interval");
            }
        }
        Require(sync.Interval().Typical()==54000000, "sync interval remains stable after repeated waits");
        sync.Begin(13,3,1,1702000000,1000000);
        Require(sync.Interval().Outputs()==3 && sync.Interval().Samples()>0, "count change uses existing estimator policy");
        sync.Begin(14,4,1,1756000000,1000000);
        Require(sync.Interval().Outputs()==4 && !sync.Matches(13,3), "new multiplier replaces old batch identity");
        sync.Begin(15,4,2,1810000000,1000000);
        Require(sync.Interval().Samples()==0, "sync interval change clears history");
        sync.Reset();
        cadence::BatchIntervalEstimator::Decision d;
        Require(!sync.Plan(15,4,1,1820000000,1811000000,1000000,d), "no deadline after reset");
        Require(!sync.Begin(16,2,1,1864000000,1000000), "native x2 stays outside added VSync adapter");
        sync.Begin(20,4,1,2000000000,1000000); sync.Begin(21,4,1,2054000000,1000000);
        Require(!sync.Plan(21,4,1,2055000000,2053000000,0,d), "completion cannot precede verified arrival");
        Require(!sync.Plan(21,4,1,2055000000,2056000000,0,d), "completion cannot be in the future");
        Require(!sync.Plan(21,4,1,2355000000,2055000000,0,d), "stale sync batch refuses deadline");
        sync.Begin(22,4,1,2500000000,1000000);
        Require(sync.Interval().Samples()==0, "pause discards old sync interval history");
    }
    std::puts("PASS production VSync admission, refusal gates, forwarding, reset and teardown");
}
