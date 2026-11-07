// Original integer RNE boundaries. No float -> half -> float casts implement rounding.
uint nr_round_shift(uint value,uint shift) {
#if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT)
    // Callers supply <=24-bit significands and shifts 13..24, so adding the
    // RNE bias cannot overflow uint. Ties add the retained low bit exactly.
    return (value+((1u<<(shift-1u))-1u)+((value>>shift)&1u))>>shift;
#else
    uint whole=value>>shift, mask=(1u<<shift)-1u;
    uint remainder=value&mask, halfway=1u<<(shift-1u);
    return whole+uint(remainder>halfway || (remainder==halfway && (whole&1u)!=0u));
#endif
}
uint nr_half_bits(float value) {
    uint bits=floatBitsToUint(value), sign=(bits>>16)&0x8000u;
    uint magnitude=bits&0x7fffffffu, exponent=magnitude>>23, mantissa=magnitude&0x7fffffu;
    if(exponent==255u)return sign|(mantissa==0u?0x7c00u:0x7e00u);
    if(exponent>142u)return sign|0x7c00u;
    if(exponent<102u)return sign;
    if(exponent<113u)return sign|nr_round_shift(mantissa|0x800000u,126u-exponent);
    return sign|(((exponent-112u)<<10)+nr_round_shift(mantissa,13u));
}
float nr_half_round(float value) {return unpackHalf2x16(nr_half_bits(value)).x;}
uint nr_e4m3_bits(float value) {
    uint bits=floatBitsToUint(value),sign=(bits>>24)&0x80u;
    uint mag=bits&0x7fffffffu,exponent=mag>>23,mantissa=mag&0x7fffffu;
    if(exponent==255u&&mantissa!=0u)return sign|0x7fu;
    if(mag>=floatBitsToUint(448.0))return sign|0x7eu;
    if(exponent<117u)return sign;
    uint result=exponent<121u?nr_round_shift(mantissa|0x800000u,141u-exponent):((exponent-120u)<<3)+nr_round_shift(mantissa,20u);
    return sign|min(result,0x7eu);
}
float nr_e4m3_decode(uint bits) {
    uint magnitude=bits&0x7fu,exponent=magnitude>>3,mantissa=magnitude&7u;
    float value=exponent==0u?float(mantissa)*(1.0/512.0):float(8u+mantissa)*exp2(float(int(exponent)-10));
    if(magnitude==0x7fu)value=uintBitsToFloat(0x7fc00000u);
    return uintBitsToFloat(floatBitsToUint(value)|((bits&0x80u)<<24));
}
float nr_e4m3_round(float value) {
#if defined(NR_FUSED_IMAGE_INPUT) || defined(NR_FUSED_IMAGE_OUTPUT)
    // Same E4M3 RNE/saturation, directly in the FP32 representation. This
    // avoids constructing and immediately decoding the intermediate byte.
    uint bits=floatBitsToUint(value),sign=bits&0x80000000u,mag=bits&0x7fffffffu;
    uint exponent=mag>>23u,rounded;
    if(mag>0x7f800000u)rounded=0x7fc00000u;
    else if(mag>=0x43e00000u)rounded=0x43e00000u;
    else if(exponent>=121u)rounded=(mag+0x7ffffu+((mag>>20u)&1u))&0x7ff00000u;
    else if(exponent<117u)rounded=0u;
    else rounded=floatBitsToUint(float(nr_round_shift((mag&0x7fffffu)|0x800000u,141u-exponent))*(1.0/512.0));
    return uintBitsToFloat(sign|rounded);
#else
    return nr_e4m3_decode(nr_e4m3_bits(value));
#endif
}
