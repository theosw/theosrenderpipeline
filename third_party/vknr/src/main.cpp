#include "vk_context.hpp"
#include "weights.hpp"
#include "probes.hpp"
#include "engine.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) try {
    std::string mode="",backend="portable",luid;
    std::filesystem::path output,dll,shaders=std::filesystem::path(argv[0]).parent_path()/"shaders";
    bool validation=false;int index=-1;
    nrtest::EngineRequest run;
    for(int i=1;i<argc;++i){std::string key=argv[i];
        auto value=[&](){if(i+1>=argc)throw std::runtime_error("Missing value for "+key);return std::string(argv[++i]);};
        if(key=="--mode")mode=value();else if(key=="--backend")backend=value();else if(key=="--output")output=value();
        else if(key=="--dll")dll=value();else if(key=="--shaders")shaders=value();else if(key=="--luid")luid=value();
        else if(key=="--device")index=std::stoi(value());else if(key=="--validation")validation=true;
        else if(key=="--input")run.input=value();else if(key=="--root")run.root=value();
        else if(key=="--width")run.width=std::stoul(value());else if(key=="--height")run.height=std::stoul(value());
        else if(key=="--frames")run.frames=std::stoul(value());else if(key=="--warmup")run.warmup=std::stoul(value());
        else if(key=="--capture")run.capture=std::stoul(value());else if(key=="--shift")run.shift=std::stoi(value());
        else if(key=="--stage-timestamps")run.stage_timestamps=true;
        else if(key=="--handoff")run.handoff=true;else if(key=="--layers")run.layers=true;
        else if(key=="--help"){std::cout<<"nr_vk_runner --mode capabilities|weights|probes|network --output report.json [--dll path] [--backend portable|rdna3] [--validation] [--device N|--luid bytes]\nNetwork: --root engine-directory --input RGBA32F --width W --height H --frames N --warmup N --capture N --shift 0|2 [--handoff] [--layers] [--stage-timestamps]\nNo weights are written. --layers writes private activation tensors; exclude them from result archives.\n";return 0;}
        else throw std::runtime_error("Unknown option: "+key);
    }
    if(output.empty())throw std::runtime_error("--output is required");
    if(backend!="portable"&&backend!="rdna3")throw std::runtime_error("Unknown backend");
    if(run.root.empty()){run.root=std::filesystem::path(argv[0]).parent_path()/"engines"/backend;}
    if(mode=="weights"){
        if(dll.empty())throw std::runtime_error("--dll is required");
        auto weights=nrtest::load_small_weights(dll);nrtest::write_text(output,nrtest::weights_metadata_json(weights));
        std::cout<<"Verified 77 records and 255 decoded entries in memory; no weight files written.\n";return 0;
    }
    if(mode!="capabilities"&&mode!="probes"&&mode!="network")throw std::runtime_error("Expected capabilities, weights, probes or network mode");
    nrtest::Context context(validation,index,luid);
    if(mode=="capabilities"){nrtest::write_text(output,context.capabilities_json());std::cout<<context.properties.deviceName<<": capability report written\n";return context.validation_errors?1:0;}
    context.create_device();
    if(mode=="network"){
        if(backend=="rdna3"){
            auto probe=nrtest::run_probes(context,shaders);
            nrtest::write_text(output.u8string()+".gate.json",probe.json);
            if(!probe.rdna3_layout||!probe.portable){context.destroy_device();nrtest::write_text(output.u8string()+".capabilities.json",context.capabilities_json());std::cerr<<"RDNA 3 backend refused: fragment or numerical probe failed.\n";return context.validation_errors?1:3;}
            if(!context.workgroup_layout.workgroupMemoryExplicitLayout||!context.workgroup_layout.workgroupMemoryExplicitLayout8BitAccess||!context.workgroup_layout.workgroupMemoryExplicitLayout16BitAccess)throw std::runtime_error("RDNA3 shaders require explicit workgroup memory layout with 8/16-bit access");
        }
        run.dll=dll;run.output=output;run.backend=backend;
        try {auto json=nrtest::run_engine(context.instance,context.physical,context.device,context.queue,context.family,run);nrtest::write_text(output,json);}
        catch(...){context.destroy_device();nrtest::write_text(output.u8string()+".capabilities.json",context.capabilities_json());throw;}
        context.destroy_device();nrtest::write_text(output.u8string()+".capabilities.json",context.capabilities_json());return context.validation_errors?1:0;
    }
    auto result=nrtest::run_probes(context,shaders);
    context.destroy_device();
    nrtest::write_text(output,result.json);nrtest::write_text(output.u8string()+".capabilities.json",context.capabilities_json());
    if(backend=="rdna3"&&!result.rdna3_layout){std::cerr<<"RDNA 3 backend refused: fragment-coordinate probe does not match RADV layout.\n";return 3;}
    if(!result.portable||context.validation_errors){std::cerr<<"Portable correctness probe failed; see report.\n";return 1;}
    std::cout<<"Portable cooperative-matrix and rounding probes passed. RDNA 3 fragment match: "<<result.rdna3_layout<<'\n';return 0;
} catch(const std::exception& error){std::cerr<<"error: "<<error.what()<<'\n';return 1;}
