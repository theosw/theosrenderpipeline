"""Bounded parallel creation of independent per-kernel Vulkan pipelines."""
from pathlib import Path
import os
core=Path(os.environ['TRP_VKNR_BUILD'])/'engine-src/src/core'
p=core/'nr_graph.cpp';t=p.read_text()
t='#include <thread>\n#include <exception>\n'+t
start=t.index('void NrSession::create_kernels(')
end=t.index('\n// A runtime at another source extent',start)
body=t[start:end]
old='''    for (const Disp& d : disp) {
        if (kern.count(d.kern)) continue;
        struct Tick'''
new='''    // Construct every map node on the owner thread. Workers own distinct Kernel
    // values; Context buffers/twins/images are immutable during creation, and pin
    // plus the global shared-pipeline registry already have their own mutex.
    struct Job { const Disp* disp; nrvk::Kernel* kernel; };
    std::vector<Job> jobs;
    for(const Disp& d:disp)if(!kern.count(d.kern)){
        auto it=kern.try_emplace(d.kern).first;
        jobs.push_back({&d,&it->second});
    }
    auto create_one=[&](const Job& job){
        const Disp& d=*job.disp;auto& kernel=*job.kernel;
        struct Tick'''
assert old in body;body=body.replace(old,new)
body=body.replace('kern[d.kern].create','kernel.create')
needle='''    }
    for (const Disp& d : disp)   // a push longer'''
replacement='''    };
    const char* requested=std::getenv("NR_PIPELINE_THREADS");
    const unsigned count=std::min<unsigned>(unsigned(jobs.size()),
        requested?unsigned(std::clamp(std::atoi(requested),1,8)):std::min(4u,std::max(1u,std::thread::hardware_concurrency())));
    if(count<=1){for(const auto& job:jobs)create_one(job);}
    else {
        // The cache is created with flags=0, hence normal internally synchronized
        // vkCreateComputePipelines access. Save/destruction happen after joins.
        const auto instance=nr::runtime_dispatch::instance;
        const auto next=nr::runtime_dispatch::next;
        const auto tag=nr::vram::t_tag;
        const auto ticket=nr::gpu::current();
        std::atomic<size_t> next_job{0};std::mutex error_mutex;std::exception_ptr error;
        auto worker=[&]{
            nr::runtime_dispatch::Scope scope(instance,next);
            nr::vram::Scope vram_scope(tag.kind,tag.owner);
            nr::gpu::current()=ticket;
            for(;;){const size_t index=next_job.fetch_add(1);if(index>=jobs.size())break;
                try{create_one(jobs[index]);}
                catch(...){std::lock_guard<std::mutex> guard(error_mutex);if(!error)error=std::current_exception();}
            }
        };
        std::vector<std::thread> workers;
        try{for(unsigned i=1;i<count;++i)workers.emplace_back(worker);}
        catch(...){for(auto& w:workers)w.join();throw;}
        worker();for(auto& w:workers)w.join();
        if(error)std::rethrow_exception(error);
    }
    for (const Disp& d : disp)   // a push longer'''
assert needle in body;body=body.replace(needle,replacement)
t=t[:start]+body+t[end:];p.write_text(t)
print('Prepared bounded per-stage pipeline creation')
