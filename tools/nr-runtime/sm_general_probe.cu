// Developer-only allocator probe. Worker counts and shared-memory occupancy
// pressure do NOT emulate physical SM counts, native-chain admission or WDDM.
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "sm_general_slots.cuh"

__device__ unsigned long long nextTicket, turns[64];
__device__ int owners[64], scratch[64][128], errors;

template<unsigned Shift>
__global__ void probe(int tasks, unsigned delay)
{
    extern __shared__ unsigned char occupancyPressure[];
    __shared__ unsigned long long ticket;
    for(int task=blockIdx.x;task<tasks;task+=gridDim.x) {
        // Finite producer delay before holding scratch; no external CTA wait.
        if(threadIdx.x==0) {
            auto start=clock64();
            while(clock64()-start < delay*(1+(task*17u)%31u)) {}
            ticket=trp_slot_acquire<Shift>(&nextTicket,turns);
        }
        __syncthreads();
        unsigned slot=unsigned(ticket)&((1u<<Shift)-1);
        if(threadIdx.x==0&&atomicCAS(owners+slot,-1,task)!=-1)atomicAdd(&errors,1);
        __syncthreads();
        scratch[slot][threadIdx.x]=task;
        auto start=clock64();
        while(clock64()-start < delay*(1+(task*13u)%7u)) {}
        if(scratch[slot][threadIdx.x]!=task)atomicAdd(&errors,1);
        __threadfence();__syncthreads();
        if(threadIdx.x==0) {
            if(atomicExch(owners+slot,-1)!=task)atomicAdd(&errors,1);
            trp_slot_release<Shift>(turns,ticket);
        }
        __syncthreads();
    }
    // Keep the requested dynamic-shared reservation live in compiled code.
    if(threadIdx.x==0)occupancyPressure[0]=0;
}

static void check(cudaError_t result)
{
    if(result!=cudaSuccess) { std::fprintf(stderr,"CUDA: %s\n",cudaGetErrorString(result));std::exit(1); }
}

int main()
{
    cudaDeviceProp prop{};check(cudaGetDeviceProperties(&prop,0));
    std::printf("GPU %s sm_%d%d physicalSMs=%d\n",prop.name,prop.major,prop.minor,prop.multiProcessorCount);
    if(prop.major!=6||prop.minor!=1)return 2;
    int initializedOwners[64];for(auto& x:initializedOwners)x=-1;
    unsigned long long zero=0, initialTurns[64]{};
    int cases=0;
    for(int logicalCount=1;logicalCount<=32;++logicalCount)
    for(int cap: {1,2,4})
    for(int shared: {1,32768,49136}) {
        check(cudaMemcpyToSymbol(nextTicket,&zero,sizeof(zero)));
        check(cudaMemcpyToSymbol(turns,initialTurns,sizeof(initialTurns)));
        check(cudaMemcpyToSymbol(owners,initializedOwners,sizeof(initializedOwners)));
        int error=0;check(cudaMemcpyToSymbol(errors,&error,sizeof(error)));
        int occupancy=0;check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&occupancy,probe<6>,128,shared));
        probe<6><<<logicalCount*cap,128,shared>>>(1031,100);
        check(cudaGetLastError());check(cudaDeviceSynchronize());
        check(cudaMemcpyFromSymbol(&error,errors,sizeof(error)));
        std::printf("CASE logicalWorkers=%d cap=%d shared=%d predictedCTAsPerPhysicalSM=%d errors=%d\n",logicalCount,cap,shared,occupancy,error);
        if(error)return 3;
        ++cases;
    }
    std::printf("PASS %d cases; logical worker/occupancy pressure only, not physical SM emulation\n",cases);
}
