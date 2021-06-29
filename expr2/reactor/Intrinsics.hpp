#ifndef rr_Intrinsics_hpp
#define rr_Intrinsics_hpp

namespace rr {

template<typename T> class RValue;
enum class Precision;

template<typename FloatT> RValue<FloatT> Sin(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Cos(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Tan(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Asin(RValue<FloatT> v, Precision p);
template<typename FloatT> RValue<FloatT> Acos(RValue<FloatT> v, Precision p);
template<typename FloatT> RValue<FloatT> Atan(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Sinh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Cosh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Tanh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Asinh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Acosh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Atanh(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Atan2(RValue<FloatT> x, RValue<FloatT> y);
template<typename FloatT> RValue<FloatT> Pow(RValue<FloatT> x, RValue<FloatT> y);
template<typename FloatT> RValue<FloatT> Exp(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Log(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Exp2(RValue<FloatT> v);
template<typename FloatT> RValue<FloatT> Log2(RValue<FloatT> v);

// non-emulated, native llvm builtin
template<typename FloatT> RValue<FloatT> BuiltinPow(RValue<FloatT> x, RValue<FloatT> y);

}  // namespace rr

#endif  // rr_Intrinsics_hpp
