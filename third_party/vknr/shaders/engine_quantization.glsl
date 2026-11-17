// Original integer quantization replacement for the portable engine.
// rounding.glsl is included by coopmm before this header.
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
float16_t nr_e4m3_decode(uint code) {return float16_t(nr_safe_e4m3_decode(code));}
float nr_e4m3_round(float x) {return nr_safe_e4m3_round(x);}
f16vec2 nr_e4m3_round_pair(f16vec2 x) {return f16vec2(nr_safe_e4m3_round(float(x.x)),nr_safe_e4m3_round(float(x.y)));}
float16_t nr_quant_e4m3(float16_t x) {return float16_t(nr_safe_e4m3_round(float(x)));}
float16_t nr_quant_e4m3(float x) {return float16_t(nr_safe_e4m3_round(x));}
