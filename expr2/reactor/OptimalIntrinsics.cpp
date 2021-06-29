// Copyright 2020 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//#include "OptimalIntrinsics.hpp"

#ifdef OPTIMAL_IMPLEMENTATION
namespace rr {
namespace {

template<typename FloatT>
struct IntType {
};

template<>
struct IntType<Float4> {
	typedef Int4 IntT;
	typedef UInt4 UIntT;
};

template<>
struct IntType<Float8> {
	typedef Int8 IntT;
	typedef UInt8 UIntT;
};

template<typename FloatT>
FloatT Reciprocal(RValue<FloatT> x, bool pp = false, bool finite = false, bool exactAtPow2 = false)
{
	FloatT rcp = Rcp_pp(x, exactAtPow2);

	if(!pp)
	{
		rcp = (rcp + rcp) - (x * rcp * rcp);
	}

	if(finite)
	{
		int big = 0x7F7FFFFF;
		rcp = Min(rcp, FloatT((float &)big));
	}

	return rcp;
}

template<typename FloatT>
FloatT SinOrCos(RValue<FloatT> x, bool sin)
{
	// Reduce to [-0.5, 0.5] range
	FloatT y = x * FloatT(1.59154943e-1f);  // 1/2pi
	y = y - Round(y);

	// From the paper: "A Fast, Vectorizable Algorithm for Producing Single-Precision Sine-Cosine Pairs"
	// This implementation passes OpenGL ES 3.0 precision requirements, at the cost of more operations:
	// !pp : 17 mul, 7 add, 1 sub, 1 reciprocal
	//  pp : 4 mul, 2 add, 2 abs

	FloatT y2 = y * y;
	FloatT c1 = y2 * (y2 * (y2 * FloatT(-0.0204391631f) + FloatT(0.2536086171f)) + FloatT(-1.2336977925f)) + FloatT(1.0f);
	FloatT s1 = y * (y2 * (y2 * (y2 * FloatT(-0.0046075748f) + FloatT(0.0796819754f)) + FloatT(-0.645963615f)) + FloatT(1.5707963235f));
	FloatT c2 = (c1 * c1) - (s1 * s1);
	FloatT s2 = FloatT(2.0f) * s1 * c1;
	FloatT r = Reciprocal(s2 * s2 + c2 * c2, false, true, false);

	if(sin)
	{
		return FloatT(2.0f) * s2 * c2 * r;
	}
	else
	{
		return ((c2 * c2) - (s2 * s2)) * r;
	}
}

// Approximation of atan in [0..1]
template<typename FloatT>
FloatT Atan_01(FloatT x)
{
	// From 4.4.49, page 81 of the Handbook of Mathematical Functions, by Milton Abramowitz and Irene Stegun
	const FloatT a2(-0.3333314528f);
	const FloatT a4(0.1999355085f);
	const FloatT a6(-0.1420889944f);
	const FloatT a8(0.1065626393f);
	const FloatT a10(-0.0752896400f);
	const FloatT a12(0.0429096138f);
	const FloatT a14(-0.0161657367f);
	const FloatT a16(0.0028662257f);
	FloatT x2 = x * x;
	return (x + x * (x2 * (a2 + x2 * (a4 + x2 * (a6 + x2 * (a8 + x2 * (a10 + x2 * (a12 + x2 * (a14 + x2 * a16)))))))));
}
}  // namespace

namespace optimal {

template<typename FloatT>
FloatT Sin(RValue<FloatT> x)
{
	return SinOrCos(x, true);
}

template<typename FloatT>
FloatT Cos(RValue<FloatT> x)
{
	return SinOrCos(x, false);
}

template<typename FloatT>
FloatT Tan(RValue<FloatT> x)
{
	return SinOrCos(x, true) / SinOrCos(x, false);
}

template<typename FloatT>
FloatT Asin_4_terms(RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;

	// From 4.4.45, page 81 of the Handbook of Mathematical Functions, by Milton Abramowitz and Irene Stegun
	// |e(x)| <= 5e-8
	const FloatT half_pi(1.57079632f);
	const FloatT a0(1.5707288f);
	const FloatT a1(-0.2121144f);
	const FloatT a2(0.0742610f);
	const FloatT a3(-0.0187293f);
	FloatT absx = Abs(x);
	return As<FloatT>(As<IntT>(half_pi - Sqrt(FloatT(1.0f) - absx) * (a0 + absx * (a1 + absx * (a2 + absx * a3)))) ^
	                  (As<IntT>(x) & IntT(0x80000000)));
}

template<typename FloatT>
FloatT Asin_8_terms(RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;

	// From 4.4.46, page 81 of the Handbook of Mathematical Functions, by Milton Abramowitz and Irene Stegun
	// |e(x)| <= 0e-8
	const FloatT half_pi(1.5707963268f);
	const FloatT a0(1.5707963050f);
	const FloatT a1(-0.2145988016f);
	const FloatT a2(0.0889789874f);
	const FloatT a3(-0.0501743046f);
	const FloatT a4(0.0308918810f);
	const FloatT a5(-0.0170881256f);
	const FloatT a6(0.006700901f);
	const FloatT a7(-0.0012624911f);
	FloatT absx = Abs(x);
	return As<FloatT>(As<IntT>(half_pi - Sqrt(FloatT(1.0f) - absx) * (a0 + absx * (a1 + absx * (a2 + absx * (a3 + absx * (a4 + absx * (a5 + absx * (a6 + absx * a7)))))))) ^
	                  (As<IntT>(x) & IntT(0x80000000)));
}

template<typename FloatT>
FloatT Acos_4_terms(RValue<FloatT> x)
{
	// pi/2 - arcsin(x)
	return FloatT(1.57079632e+0f) - Asin_4_terms(x);
}

template<typename FloatT>
FloatT Acos_8_terms(RValue<FloatT> x)
{
	// pi/2 - arcsin(x)
	return FloatT(1.57079632e+0f) - Asin_8_terms(x);
}

template<typename FloatT>
FloatT Atan(RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;

	FloatT absx = Abs(x);
	IntT O = CmpNLT(absx, FloatT(1.0f));
	FloatT y = As<FloatT>((O & As<IntT>(FloatT(1.0f) / absx)) | (~O & As<IntT>(absx)));  // FIXME: Vector select

	const FloatT half_pi(1.57079632f);
	FloatT theta = Atan_01(y);
	return As<FloatT>(((O & As<IntT>(half_pi - theta)) | (~O & As<IntT>(theta))) ^  // FIXME: Vector select
	                  (As<IntT>(x) & IntT(0x80000000)));
}

template<typename FloatT>
FloatT Atan2(RValue<FloatT> y, RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;

	const FloatT pi(3.14159265f);             // pi
	const FloatT minus_pi(-3.14159265f);      // -pi
	const FloatT half_pi(1.57079632f);        // pi/2
	const FloatT quarter_pi(7.85398163e-1f);  // pi/4

	// Rotate to upper semicircle when in lower semicircle
	IntT S = CmpLT(y, FloatT(0.0f));
	FloatT theta = As<FloatT>(S & As<IntT>(minus_pi));
	FloatT x0 = As<FloatT>((As<IntT>(y) & IntT(0x80000000)) ^ As<IntT>(x));
	FloatT y0 = Abs(y);

	// Rotate to right quadrant when in left quadrant
	IntT Q = CmpLT(x0, FloatT(0.0f));
	theta += As<FloatT>(Q & As<IntT>(half_pi));
	FloatT x1 = As<FloatT>((Q & As<IntT>(y0)) | (~Q & As<IntT>(x0)));   // FIXME: Vector select
	FloatT y1 = As<FloatT>((Q & As<IntT>(-x0)) | (~Q & As<IntT>(y0)));  // FIXME: Vector select

	// Mirror to first octant when in second octant
	IntT O = CmpNLT(y1, x1);
	FloatT x2 = As<FloatT>((O & As<IntT>(y1)) | (~O & As<IntT>(x1)));  // FIXME: Vector select
	FloatT y2 = As<FloatT>((O & As<IntT>(x1)) | (~O & As<IntT>(y1)));  // FIXME: Vector select

	// Approximation of atan in [0..1]
	IntT zero_x = CmpEQ(x2, FloatT(0.0f));
	IntT inf_y = IsInf(y2);  // Since x2 >= y2, this means x2 == y2 == inf, so we use 45 degrees or pi/4
	FloatT atan2_theta = Atan_01(y2 / x2);
	theta += As<FloatT>((~zero_x & ~inf_y & ((O & As<IntT>(half_pi - atan2_theta)) | (~O & (As<IntT>(atan2_theta))))) |  // FIXME: Vector select
	                    (inf_y & As<IntT>(quarter_pi)));

	// Recover loss of precision for tiny theta angles
	// This combination results in (-pi + half_pi + half_pi - atan2_theta) which is equivalent to -atan2_theta
	IntT precision_loss = S & Q & O & ~inf_y;

	return As<FloatT>((precision_loss & As<IntT>(-atan2_theta)) | (~precision_loss & As<IntT>(theta)));  // FIXME: Vector select
}

template<typename FloatT>
FloatT Exp2(RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;

	// This implementation is based on 2^(i + f) = 2^i * 2^f,
	// where i is the integer part of x and f is the fraction.

	// For 2^i we can put the integer part directly in the exponent of
	// the IEEE-754 floating-point number. Clamp to prevent overflow
	// past the representation of infinity.
	FloatT x0 = x;
	x0 = Min(x0, As<FloatT>(IntT(0x43010000)));  // 129.00000e+0f
	x0 = Max(x0, As<FloatT>(IntT(0xC2FDFFFF)));  // -126.99999e+0f

	IntT i = RoundInt(x0 - FloatT(0.5f));
	FloatT ii = As<FloatT>((i + IntT(127)) << 23);  // Add single-precision bias, and shift into exponent.

	// For the fractional part use a polynomial
	// which approximates 2^f in the 0 to 1 range.
	FloatT f = x0 - FloatT(i);
	FloatT ff = As<FloatT>(IntT(0x3AF61905));    // 1.8775767e-3f
	ff = ff * f + As<FloatT>(IntT(0x3C134806));  // 8.9893397e-3f
	ff = ff * f + As<FloatT>(IntT(0x3D64AA23));  // 5.5826318e-2f
	ff = ff * f + As<FloatT>(IntT(0x3E75EAD4));  // 2.4015361e-1f
	ff = ff * f + As<FloatT>(IntT(0x3F31727B));  // 6.9315308e-1f
	ff = ff * f + FloatT(1.0f);

	return ii * ff;
}

template<typename FloatT>
FloatT Log2(RValue<FloatT> x)
{
	using IntT =  typename IntType<FloatT>::IntT;
	using UIntT =  typename IntType<FloatT>::UIntT;

	FloatT x0;
	FloatT x1;
	FloatT x2;
	FloatT x3;

	x0 = x;

	x1 = As<FloatT>(As<IntT>(x0) & IntT(0x7F800000));
	x1 = As<FloatT>(As<UIntT>(x1) >> 8);
	x1 = As<FloatT>(As<IntT>(x1) | As<IntT>(FloatT(1.0f)));
	x1 = (x1 - FloatT(1.4960938f)) * FloatT(256.0f);  // FIXME: (x1 - 1.4960938f) * 256.0f;
	x0 = As<FloatT>((As<IntT>(x0) & IntT(0x007FFFFF)) | As<IntT>(FloatT(1.0f)));

	x2 = (FloatT(9.5428179e-2f) * x0 + FloatT(4.7779095e-1f)) * x0 + FloatT(1.9782813e-1f);
	x3 = ((FloatT(1.6618466e-2f) * x0 + FloatT(2.0350508e-1f)) * x0 + FloatT(2.7382900e-1f)) * x0 + FloatT(4.0496687e-2f);
	x2 /= x3;

	x1 += (x0 - FloatT(1.0f)) * x2;

	IntT pos_inf_x = CmpEQ(As<IntT>(x), IntT(0x7F800000));
	return As<FloatT>((pos_inf_x & As<IntT>(x)) | (~pos_inf_x & As<IntT>(x1)));
}

template<typename FloatT>
FloatT Exp(RValue<FloatT> x)
{
	// TODO: Propagate the constant
	return optimal::Exp2(FloatT(1.44269504f) * x);  // 1/ln(2)
}

template<typename FloatT>
FloatT Log(RValue<FloatT> x)
{
	// TODO: Propagate the constant
	return FloatT(6.93147181e-1f) * optimal::Log2(x);  // ln(2)
}

template<typename FloatT>
FloatT Pow(RValue<FloatT> x, RValue<FloatT> y)
{
	FloatT log = Log2(x);
	log *= y;
	return Exp2(log);
}

template<typename FloatT>
FloatT Sinh(RValue<FloatT> x)
{
	return (optimal::Exp(x) - optimal::Exp(-x)) * FloatT(0.5f);
}

template<typename FloatT>
FloatT Cosh(RValue<FloatT> x)
{
	return (optimal::Exp(x) + optimal::Exp(-x)) * FloatT(0.5f);
}

template<typename FloatT>
FloatT Tanh(RValue<FloatT> x)
{
	FloatT e_x = optimal::Exp(x);
	FloatT e_minus_x = optimal::Exp(-x);
	return (e_x - e_minus_x) / (e_x + e_minus_x);
}

template<typename FloatT>
FloatT Asinh(RValue<FloatT> x)
{
	return optimal::Log(x + Sqrt(x * x + FloatT(1.0f)));
}

template<typename FloatT>
FloatT Acosh(RValue<FloatT> x)
{
	return optimal::Log(x + Sqrt(x + FloatT(1.0f)) * Sqrt(x - FloatT(1.0f)));
}

template<typename FloatT>
FloatT Atanh(RValue<FloatT> x)
{
	return optimal::Log((FloatT(1.0f) + x) / (FloatT(1.0f) - x)) * FloatT(0.5f);
}

}  // namespace optimal
}  // namespace rr
#endif // OPTIMAL_IMPLEMENTATION
