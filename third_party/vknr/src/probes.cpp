#include "probes.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace nrtest {
static uint32_t float_bits(float value){uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
static float half_value(uint16_t bits) {
    const uint32_t exponent=(bits>>10)&31,mantissa=bits&1023;
    float v=exponent?std::ldexp(float(1024+mantissa),int(exponent)-25):std::ldexp(float(mantissa),-24);
    if(exponent==31)v=mantissa?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity();
    return std::copysign(v,(bits&0x8000)?-1.0f:1.0f);
}
// Independent CPU oracle: nearest-neighbour search over representable values,
// using exact double distances and code parity for ties; no shader bit algorithm.
struct Oracle {
    std::vector<float> half,fp8;
    Oracle(){for(unsigned i=0;i<0x7c00;++i)half.push_back(half_value(uint16_t(i)));
        for(unsigned i=0;i<127;++i)fp8.push_back(i<8?std::ldexp(float(i),-9):std::ldexp(float(8+(i&7)),int(i>>3)-10));}
    static unsigned nearest(const std::vector<float>& values,float v){
        auto it=std::lower_bound(values.begin(),values.end(),v);if(it==values.begin())return 0;if(it==values.end())return unsigned(values.size()-1);
        const auto hi=unsigned(it-values.begin()),lo=hi-1;const double low=double(v)-values[lo],high=double(values[hi])-v;
        return low<high?lo:high<low?hi:(lo%2==0?lo:hi);
    }
    uint16_t h(float v) const {unsigned sign=std::signbit(v)?0x8000:0;
        if(std::isnan(v))return uint16_t(sign|0x7e00);if(std::fabs(v)>=65520)return uint16_t(sign|0x7c00);return uint16_t(sign|nearest(half,std::fabs(v)));}
    uint8_t e(float v) const {unsigned sign=std::signbit(v)?0x80:0;if(std::isnan(v))return uint8_t(sign|0x7f);return uint8_t(sign|nearest(fp8,std::fabs(v)));}
    float rounded(float v)const {const auto code=e(half_value(h(v)));return std::copysign(fp8[code&127],(code&128)?-1.0f:1.0f);}
};
ProbeResult run_probes(Context& c,const std::filesystem::path& shaders) {
    Oracle oracle;
    std::vector<float> inputs;for(unsigned h=0;h<65536;++h)inputs.push_back(half_value(uint16_t(h)));
    // Test both neighbours and exact midpoint at every finite positive half boundary.
    for(unsigned h=0;h+1<oracle.half.size();++h){float mid=float((double(oracle.half[h])+oracle.half[h+1])*0.5);
        for(float v:{std::nextafter(mid,0.0f),mid,std::nextafter(mid,std::numeric_limits<float>::infinity())}){inputs.push_back(v);inputs.push_back(-v);}}
    for(float v:{65519.0f,65520.0f,65521.0f,1e-30f,448.0f,464.0f,1e30f}){inputs.push_back(v);inputs.push_back(-v);}
    Buffer in(c,inputs.size()*4),out(c,inputs.size()*8);std::memcpy(in.mapped,inputs.data(),inputs.size()*4);std::memset(out.mapped,0,out.bytes);
    Compute rounding(c,shaders/"rounding_probe.spv",{&in,&out});uint32_t n=uint32_t(inputs.size());auto cmd=c.begin();rounding.record(cmd,(n+31)/32,1,1,&n,4);c.submit_and_wait(cmd);
    auto* results=static_cast<uint32_t*>(out.mapped);size_t half_errors=0,e4m3_errors=0;for(size_t i=0;i<inputs.size();++i){half_errors+=results[2*i]!=oracle.h(inputs[i]);e4m3_errors+=results[2*i+1]!=oracle.e(inputs[i]);}
    Compute upstream(c,shaders/"upstream_quant.spv",{&in,&out});cmd=c.begin();upstream.record(cmd,(n+31)/32,1,1,&n,4);c.submit_and_wait(cmd);
    size_t upstream_float_errors=0,upstream_half_errors=0;
    auto expected=[&](float value){const auto code=oracle.e(value);return std::copysign(oracle.fp8[code&127],(code&128)?-1.f:1.f);};
    for(size_t i=0;i<inputs.size();++i){if(std::isnan(inputs[i]))continue;float actual[2];std::memcpy(actual,results+2*i,8);upstream_float_errors+=actual[0]!=expected(inputs[i]);upstream_half_errors+=actual[1]!=expected(half_value(oracle.h(inputs[i])));}

    Buffer a(c,512),b(c,512),d(c,1024);
    auto* ah=static_cast<uint16_t*>(a.mapped);auto* bh=static_cast<uint16_t*>(b.mapped);
    float av[256],bv[256],first[256]{},second[256]{},nohalf[256]{};
    for(unsigned i=0;i<256;++i){av[i]=float(1024+int((i*7+3)%31)-15)/1024.0f;bv[i]=float(int((i*15+9)%997)-498)/1024.0f;ah[i]=oracle.h(av[i]);bh[i]=oracle.h(bv[i]);}
    for(unsigned m=0;m<16;++m)for(unsigned n0=0;n0<16;++n0)for(unsigned k=0;k<16;++k)first[m*16+n0]+=av[m*16+k]*bv[k*16+n0];
    for(unsigned m=0;m<16;++m)for(unsigned n0=0;n0<16;++n0)for(unsigned k=0;k<16;++k)second[m*16+n0]+=oracle.rounded(first[m*16+k])*bv[k*16+n0];
    for(unsigned m=0;m<16;++m)for(unsigned n0=0;n0<16;++n0)for(unsigned k=0;k<16;++k)nohalf[m*16+n0]+=expected(first[m*16+k])*bv[k*16+n0];
    float boundary_sensitivity=0;for(unsigned i=0;i<256;++i)boundary_sensitivity=std::max(boundary_sensitivity,std::fabs(second[i]-nohalf[i]));
    Compute gemm(c,shaders/"portable_gemm.spv",{&a,&b,&d});float errors[3]{};
    for(uint32_t pass=0;pass<3;++pass){cmd=c.begin();gemm.record(cmd,1,1,1,&pass,4);c.submit_and_wait(cmd);const float* values=static_cast<float*>(d.mapped);
        for(unsigned i=0;i<256;++i)errors[pass]=std::max(errors[pass],std::fabs(values[i]-(pass==2?nohalf:pass?second:first)[i]));}

    Compute actual(c,shaders/"engine_fused.spv",{&a,&b,&d});float engine_errors[3]{};
    for(uint32_t pass=0;pass<3;++pass){cmd=c.begin();actual.record(cmd,1,1,1,&pass,4);c.submit_and_wait(cmd);const float* values=static_cast<float*>(d.mapped);
        for(unsigned i=0;i<256;++i)engine_errors[pass]=std::max(engine_errors[pass],std::fabs(values[i]-(pass==2?nohalf:pass?second:first)[i]));}
    Buffer coords(c,4224*4);std::memset(coords.mapped,0,coords.bytes);
    bool half_accumulator=false;for(const auto& t:c.tuples)half_accumulator|=t.MSize==16&&t.NSize==16&&t.KSize==16&&t.scope==VK_SCOPE_SUBGROUP_KHR&&t.CType==VK_COMPONENT_TYPE_FLOAT16_KHR;
    if(half_accumulator){Compute layout(c,shaders/"fragment_probe.spv",{&coords});cmd=c.begin();layout.record(cmd);c.submit_and_wait(cmd);}
    const float* observed=static_cast<float*>(coords.mapped);size_t mapping_errors=0;
    for(unsigned lane=0;lane<32;++lane){mapping_errors+=observed[1024+lane]!=8;
        mapping_errors+=observed[3168+1024+lane]!=8;
        for(unsigned component=0;component<8;++component)mapping_errors+=observed[3168+lane*32+component]!=float((2*component+lane/16)*16+lane%16+1);
        for(unsigned component=0;component<8;++component)mapping_errors+=observed[lane*32+component]!=float((2*component+lane/16)*16+lane%16+1);
        mapping_errors+=observed[1056+1024+lane]!=16;mapping_errors+=observed[2112+1024+lane]!=16;
        for(unsigned k=0;k<16;++k){mapping_errors+=observed[1056+lane*32+k]!=float((lane%16)*16+k+1);mapping_errors+=observed[2112+lane*32+k]!=float(k*16+lane%16+1);}}
    ProbeResult result;result.portable=!half_errors&&!e4m3_errors&&errors[0]==0&&errors[1]==0&&errors[2]==0&&engine_errors[0]==0&&engine_errors[1]==0&&engine_errors[2]==0&&boundary_sensitivity>0&&!c.validation_errors;result.rdna3_layout=!mapping_errors&&!upstream_float_errors&&!upstream_half_errors;
    std::ostringstream o;o << std::boolalpha << "{\n\"device\":" << json_quote(c.properties.deviceName)
      << ",\n\"portable_pass\":" << result.portable << ",\"rounding_inputs\":" << inputs.size()
      << ",\"half_rne_errors\":" << half_errors << ",\"e4m3_rne_errors\":" << e4m3_errors
      << ",\"gemm_max_error\":" << errors[0] << ",\"fused_rounding_gemm_max_error\":" << errors[1]
      << ",\"engine_gemm_max_error\":" << engine_errors[0] << ",\"engine_fused_rounding_max_error\":" << engine_errors[1]
      << ",\"removed_half_boundary_changes_output_by\":" << boundary_sensitivity << ",\"negative_control_gemm_max_error\":" << errors[2] << ",\"engine_negative_control_max_error\":" << engine_errors[2]
      << ",\"upstream_quant_float_errors\":" << upstream_float_errors << ",\"upstream_quant_half_errors\":" << upstream_half_errors
      << ",\n\"rdna3_fragment_layout_pass\":" << result.rdna3_layout << ",\"rdna3_fragment_mismatches\":" << mapping_errors
      << ",\"fragment_probe_is_diagnostic_only\":true,\n\"observed_fragments\":[";
    for(unsigned lane=0;lane<32;++lane){if(lane)o<<',';o<<"{\"lane\":"<<lane<<",\"length\":"<<observed[1024+lane]<<",\"coordinates\":[";
        for(unsigned j=0;j<std::min(32u,unsigned(observed[1024+lane]));++j){if(j)o<<',';o<<observed[lane*32+j];}o<<"]}";}
    o << "],\n\"validation_errors\":" << c.validation_errors << ",\"validation_warnings\":" << c.validation_warnings << "\n}\n";result.json=o.str();return result;
}
}
