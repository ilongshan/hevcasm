// Copyright (C) 2016 Parabola Research Limited
//
// Use of this source code is governed by a BSD-style license that
// can be found in the COPYING file in the root of the source tree.


#include "quantize.h"
#include "hevcasm_test.h"
#include <Jit.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


static int Clip3(int min, int max, int value)
{
	if (value < min) return min;
	if (value > max) return max;
	return value;
}


static void hevcasm_quantize_inverse_c_ref(int16_t *dst, const int16_t *src, int scale, int shift, int n)
{
	while (n--)
	{
		*dst++ = (int16_t)Clip3(
			-32768,
			32767,
			((*src++ * scale) + (1 << (shift - 1))) >> shift);
	}
}

struct InverseQuantise
	:
	Jit::Function
{
	InverseQuantise(Jit::Buffer *buffer)
		:
		Jit::Function(buffer, Jit::CountArguments<hevcasm_quantize_inverse>::value)
	{
		this->build();
	}

	Xbyak::Label ones_w;

	void data()
	{
		align();
		L(ones_w);
		dw({ 1 }, 8);
	}

	void assemble()
	{
		auto &r0 = arg64(0);
		auto &r1 = arg64(1);
		auto &r2 = arg64(2);
		auto &r3 = arg64(3);
		auto &r4 = arg64(4);

		auto &m0 = regXmm(0);
		auto &m1 = regXmm(1);
		auto &m2 = regXmm(2);
		auto &m3 = regXmm(3);
		auto &m4 = regXmm(4);
		auto &m5 = regXmm(5);
		auto &m6 = regXmm(6);
		auto &m7 = regXmm(7);

		movdqa(m0, ptr[rip + ones_w]);

		Xbyak::Reg32 r2d(r2.getIdx());
		Xbyak::Reg32 r3d(r3.getIdx());
		Xbyak::Reg32 r4d(r4.getIdx());

		movd(m1, r3d);
		//  m1 = shift

		Xbyak::Reg8 r3b(r3.getIdx());

		add(r3b, 15);
		bts(r2d, r3d);
		// r2d = (0x10000 << (shift - 1)) + scale

		movd(m2, r2d);
		pshufd(m2, m2, 0);
		// m2 = 1 << (shift - 1), scale, 1 << (shift - 1), scale, 1 << (shift - 1), scale, 1 << (shift - 1), scale

		shr(r4d, 4);
		// r4 = n / 16

		L("loop");
		{
			for (int offset = 0; offset < 32; offset += 16)
			{
				movdqa(m4, ptr[r1 + offset]);
				// m4 = src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0]

				movdqa(m5, m4);

				punpcklwd(m5, m0);
				// m5 = 1, src[3], 1, src[2], 1, src[1], 1, src[0]

				punpckhwd(m4, m0);
				// m4 = 1, src[7], 1, src[6], 1, src[5], 1, src[4]

				pmaddwd(m4, m2);
				// m4 = (1 << (shift - 1)) + src[7] * scale, (1 << (shift - 1)) + src[6] * scale, (1 << (shift - 1)) + src[5] * scale, (1 << (shift - 1)) + src[4] * scale

				pmaddwd(m5, m2);
				// m5 = (1 << (shift - 1)) + src[3] * scale, (1 << (shift - 1)) + src[2] * scale, (1 << (shift - 1)) + src[1] * scale, (1 << (shift - 1)) + src[0] * scale

				psrad(m4, m1);
				// m4 = ((1 << (shift - 1)) + src[7] * scale) >> shift, ((1 << (shift - 1)) + src[6] * scale) >> shift, ((1 << (shift - 1)) + src[5] * scale) >> shift, ((1 << (shift - 1)) + src[4] * scale) >> shift

				psrad(m5, m1);
				// m5 = ((1 << (shift - 1)) + src[3] * scale) >> shift, ((1 << (shift - 1)) + src[2] * scale) >> shift, ((1 << (shift - 1)) + src[1] * scale) >> shift, ((1 << (shift - 1)) + src[0] * scale) >> shift

				packssdw(m5, m4);
				// m5 = ((1 << (shift - 1)) + src[7] * scale) >> shift, ((1 << (shift - 1)) + src[6] * scale) >> shift, ((1 << (shift - 1)) + src[5] * scale) >> shift, ((1 << (shift - 1)) + src[4] * scale) >> shift, ((1 << (shift - 1)) + src[3] * scale) >> shift, ((1 << (shift - 1)) + src[2] * scale) >> shift, ((1 << (shift - 1)) + src[1] * scale) >> shift, ((1 << (shift - 1)) + src[0] * scale) >> shift

				movdqa(ptr[r0 + offset], m5);
			}

			add(r1, 32);
			add(r0, 32);
		}
		dec(r4d);
		jg("loop");
	}
};


static hevcasm_quantize_inverse * get_quantize_inverse(hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);

	hevcasm_quantize_inverse *f = 0;
	
	if (buffer.isa & (HEVCASM_C_REF | HEVCASM_C_OPT)) f = hevcasm_quantize_inverse_c_ref;

	//if (buffer.isa & HEVCASM_SSE41)
	//{
	//	InverseQuantise inverseQuantise(&buffer);
	//	f = inverseQuantise;
	//}
	return f;
}


void hevcasm_populate_quantize_inverse(hevcasm_table_quantize_inverse *table, hevcasm_code code)
{
	table->p = get_quantize_inverse(code);
}


typedef struct
{
	int16_t *src;
	HEVCASM_ALIGN(32, int16_t, dst[32*32]);
	hevcasm_quantize_inverse *f;
	int scale;
	int shift;
	int log2TrafoSize;
}
hevcasm_bound_quantize_inverse;


int init_quantize_inverse(void *p, hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);

	hevcasm_bound_quantize_inverse *s = (hevcasm_bound_quantize_inverse *)p;

	hevcasm_table_quantize_inverse table;

	hevcasm_populate_quantize_inverse(&table, code);

	s->f = *hevcasm_get_quantize_inverse(&table);
	
	if (s->f && buffer.isa == HEVCASM_C_REF)
	{
		const int nCbS = 1 << s->log2TrafoSize;
		printf("\t%dx%d : ", nCbS, nCbS);
	}

	return !!s->f;
}


void invoke_quantize_inverse(void *p, int count)
{
	hevcasm_bound_quantize_inverse *s = (hevcasm_bound_quantize_inverse *)p;
	s->dst[0] = rand();
	while (count--)
	{
		const int n = 1 << (2 * s->log2TrafoSize);
		s->f(s->dst, s->src, s->scale, s->shift, n);
	}
}


int mismatch_quantize_inverse(void *boundRef, void *boundTest)
{
	hevcasm_bound_quantize_inverse *ref = (hevcasm_bound_quantize_inverse *)boundRef;
	hevcasm_bound_quantize_inverse *test = (hevcasm_bound_quantize_inverse *)boundTest;

	const int nCbS = 1 << ref->log2TrafoSize;

	return memcmp(ref->dst, test->dst, nCbS * nCbS * sizeof(int16_t));
}


void HEVCASM_API hevcasm_test_quantize_inverse(int *error_count, hevcasm_instruction_set mask)
{
	printf("\nhevcasm_quantize_inverse - Inverse Quantization (\"scaling\")\n");

	HEVCASM_ALIGN(32, int16_t, src[32 * 32]);

	for (int x = 0; x < 32 * 32; ++x) src[x] = (rand() & 0xff) - 0x100;
		
	hevcasm_bound_quantize_inverse b[2];
	b[0].src = src;
	b[0].scale = 51;
	b[0].shift = 14;

	for (b[0].log2TrafoSize = 2; b[0].log2TrafoSize <= 5; ++b[0].log2TrafoSize)
	{
		b[1] = b[0];
		*error_count += hevcasm_test(&b[0], &b[1], init_quantize_inverse, invoke_quantize_inverse, mismatch_quantize_inverse, mask, 100000);
	}
}


static int hevcasm_quantize_c_ref(int16_t *dst, const int16_t *src, int scale, int shift, int offset, int n)
{
	assert(scale < 0x8000);
	assert(offset < 0x8000);
	assert(shift >= 16);
	assert(shift <= 27);

	offset <<= (shift - 16);
	assert(offset < 0x4000000);

	int cbf = 0;
	while (n--)
	{
		int x = *src++;
		int sign = x < 0 ? -1 : 1;

		x = abs(x);
		x = ((x * scale) + offset) >> shift;
		x *= sign;
		x = Clip3(-32768, 32767, x);

		cbf |= x;

		*dst++ = x;
	}
	return cbf;
}


struct Quantise
	:
	Jit::Function
{
	Quantise(Jit::Buffer *buffer)
		:
		Jit::Function(buffer, Jit::CountArguments<hevcasm_quantize>::value)
	{
		this->build();
	}

	void assemble()
	{
		auto &r0 = arg64(0);
		auto &r1 = arg64(1);
		auto &r2 = arg64(2);
		auto &r3 = arg64(3);
		auto &r4 = arg64(4);
		auto &r5 = arg64(5);

		auto &m0 = regXmm(0);
		auto &m1 = regXmm(1);
		auto &m2 = regXmm(2);
		auto &m3 = regXmm(3);
		auto &m4 = regXmm(4);
		auto &m5 = regXmm(5);
		auto &m6 = regXmm(6);
		auto &m7 = regXmm(7);

		Xbyak::Reg32 r2d(r2.getIdx());
		Xbyak::Reg32 r3d(r3.getIdx());
		Xbyak::Reg32 r4d(r4.getIdx());
		Xbyak::Reg32 r5d(r5.getIdx());

		movd(m1, r3d);
		// m1 = shift

		bts(r2d, r3d);
		// r2d = (1 << shift) + scale
			
		movd(m2, r2d);
		pshufd(m2, m2, 0);
		// m2 = 1 << (shift - 16), scale, 1 << (shift - 16), scale, 1 << (shift - 16), scale, 1 << (shift - 16), scale

		movd(m3, r4d);
		pshuflw(m3, m3, 0);
		pshufd(m3, m3, 0);
		// m3 = offset, offset, offset, offset, offset, offset, offset, offset

		pxor(m0, m0);
		// m0 = 0

		shr(r5d, 4);
		// r5 = n / 16

		L("loop");
		{
			for (int offset = 0; offset < 32; offset += 16)
			{
				movdqa(m4, ptr[r1 + offset]);
				// m4 = src[7], src[6], src[5], src[4], src[3], src[2], src[1], src[0]

				pabsw(m5, m4);
				// m5 = abs(src[7]), abs(src[6]), abs(src[5]), abs(src[4]), abs(src[3]), abs(src[2]), abs(src[1]), abs(src[0])

				movdqa(m6, m5);
				punpcklwd(m6, m3);
				// m6 = offset, abs(src[3]), offset, abs(src[2]), offset, abs(src[1]), offset, abs(src[0])

				punpckhwd(m5, m3);
				// m5 = offset, abs(src[7]), offset, abs(src[6]), offset, abs(src[5]), offset, abs(src[4])

				pmaddwd(m6, m2);
				// m6 = (offset << (shift - 16)) + abs(src[3])*scale, (offset << (shift - 16)) + abs(src[2])*scale, (offset << (shift - 16)) + abs(src[1])*scale, (offset << (shift - 16)) + abs(src[0])*scale

				psrad(m6, m1);
				// m6 = (offset << (shift - 16)) + abs(src[3])*scale >> shift, (offset << (shift - 16)) + abs(src[2])*scale >> shift, (offset << (shift - 16)) + abs(src[1])*scale >> shift, (offset << (shift - 16)) + abs(src[0])*scale >> shift

				pmaddwd(m5, m2);
				// m5 = (offset << (shift - 16)) + abs(src[7])*scale, (offset << (shift - 16)) + abs(src[6])*scale, (offset << (shift - 16)) + abs(src[5])*scale, (offset << (shift - 16)) + abs(src[4])*scale,

				psrad(m5, m1);
				// m5 = (offset << (shift - 16)) + abs(src[7])*scale >> shift, (offset << (shift - 16)) + abs(src[6])*scale >> shift, (offset << (shift - 16)) + abs(src[5])*scale >> shift, (offset << (shift - 16)) + abs(src[4])*scale >> shift

				punpcklwd(m7, m4);
				// m7 = (src[3] << 16) + 0x ? ? ? ? , (src[2] << 16) + 0x ? ? ? ? , (src[1] << 16) + 0x ? ? ? ? , (src[0] << 16) + 0x ? ? ? ?

				psignd(m6, m7);
				// m6 = ((offset << (shift - 16)) + abs(src[3])*scale >> shift)*sign(src[3]), ((offset << (shift - 16)) + abs(src[2])*scale >> shift)*sign(src[2]), ((offset << (shift - 16)) + abs(src[1])*scale >> shift)*sign(src[1]), ((offset << (shift - 16)) + abs(src[0])*scale >> shift)*sign(src[0])
				// m6 = dst[3], dst[2], dst[1], dst[0]

				punpckhwd(m4, m4);
				// m4 = (src[7] << 16) + 0x ? ? ? ? , (src[6] << 16) + 0x ? ? ? ? , (src[5] << 16) + 0x ? ? ? ? , (src[4] << 16) + 0x ? ? ? ?

				psignd(m5, m4);
				// m5 = ((offset << (shift - 16)) + abs(src[7])*scale >> shift)*sign(src[7]), ((offset << (shift - 16)) + abs(src[6])*scale >> shift)*sign(src[6]), ((offset << (shift - 16)) + abs(src[5])*scale >> shift)*sign(src[5]), ((offset << (shift - 16)) + abs(src[4])*scale >> shift)*sign(src[4])
				// m5 = dst[7], dst[6], dst[5], dst[4]

				packssdw(m6, m5);
				// m6 = dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]

				por(m0, m6);
				// m0 is non - zero if we have seen any non - zero quantized coefficients

				movdqa(ptr[r0 + offset], m6);
			}

			add(r1, 32);
			add(r0, 32);
		}
		dec(r5d);
		jg("loop");

		// return zero only if m0 is zero - no non - zero quantized coefficients seen(cbf = 0)
		packsswb(m0, m0);
		packsswb(m0, m0);
		movd(eax, m0);
	}
};



static hevcasm_quantize * get_quantize(hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);

	hevcasm_quantize *f = 0;

	if (buffer.isa & (HEVCASM_C_REF | HEVCASM_C_OPT)) f = hevcasm_quantize_c_ref;

	Quantise quantise(&buffer);

	if (buffer.isa & HEVCASM_SSE41) f = quantise;

	return f;
}


void hevcasm_populate_quantize(hevcasm_table_quantize *table, hevcasm_code code)
{
	table->p = get_quantize(code);
}




typedef struct
{
	int16_t *src;
	HEVCASM_ALIGN(32, int16_t, dst[32 * 32]);
	hevcasm_quantize *f;
	int scale;
	int shift;
	int offset;
	int log2TrafoSize;
	int cbf;
}
hevcasm_bound_quantize;


int init_quantize(void *p, hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);
	auto mask = buffer.isa;

	hevcasm_bound_quantize *s = (hevcasm_bound_quantize *)p;
	hevcasm_table_quantize table;
	hevcasm_populate_quantize(&table, code);
	s->f = *hevcasm_get_quantize(&table);

	if (buffer.isa == HEVCASM_C_REF)
	{
		const int nCbS = 1 << s->log2TrafoSize;
		printf("\t%dx%d : ", nCbS, nCbS);
	}
	return !!s->f;
}


void invoke_quantize(void *p, int iterations)
{
	hevcasm_bound_quantize *s = (hevcasm_bound_quantize *)p;
	s->dst[0] = rand();
	while (iterations--)
	{
		const int n = 1 << (2 * s->log2TrafoSize);
		s->cbf = s->f(s->dst, s->src, s->scale, s->shift, s->offset, n);
	}
}


int mismatch_quantize(void *boundRef, void *boundTest)
{
	hevcasm_bound_quantize *ref = (hevcasm_bound_quantize *)boundRef;
	hevcasm_bound_quantize *test = (hevcasm_bound_quantize *)boundTest;

	const int n = 1 << (2 * ref->log2TrafoSize);

	if (!!ref->cbf != !!test->cbf) return 1;

	return memcmp(ref->dst, test->dst, n * sizeof(int16_t));
}


void HEVCASM_API hevcasm_test_quantize(int *error_count, hevcasm_instruction_set mask)
{
	printf("\nhevcasm_quantize - Quantization\n");

	HEVCASM_ALIGN(32, int16_t, src[32 * 32]);

	for (int x = 0; x < 32 * 32; ++x)
	{
		src[x] = rand() - rand();
	}

	hevcasm_bound_quantize b[2];

	b[0].src = src;
	b[0].scale = 51;
	b[0].shift = 20;
	b[0].offset = 14;

	for (b[0].log2TrafoSize = 2; b[0].log2TrafoSize <= 5; ++b[0].log2TrafoSize)
	{
		b[1] = b[0];
		*error_count += hevcasm_test(&b[0], &b[1], init_quantize, invoke_quantize, mismatch_quantize, mask, 100000);
	}
}





static void hevcasm_quantize_reconstruct_c_ref(uint8_t *rec, ptrdiff_t stride_rec, const uint8_t *predSamples, ptrdiff_t stride_pred, const int16_t *resSamples, int n)
{
	for (int y = 0; y < n; ++y)
	{
		for (int x = 0; x < n; ++x)
		{
			rec[x + y * stride_rec] = (uint8_t)Clip3(0, 255, predSamples[x + y * stride_pred] + resSamples[x + y * n]);
		}
	}

}


#define ORDER(a, b, c, d) ((a << 6) | (b << 4) | (c << 2) | d)

struct QuantiseReconstruct
	:
	Jit::Function
{
	QuantiseReconstruct(Jit::Buffer *buffer, int nCbS)
		:
		Jit::Function(buffer, 6),
		nCbS(nCbS)
	{
		this->build();
	}

	int const nCbS;

	void assemble()
	{
		auto &r0 = arg64(0);
		auto &r1 = arg64(1);
		auto &r2 = arg64(2);
		auto &r3 = arg64(3);
		auto &r4 = arg64(4);
		auto &r5 = arg64(5);

		auto &m0 = regXmm(0);
		auto &m1 = regXmm(1);
		auto &m2 = regXmm(2);
		auto &m3 = regXmm(3);

		Xbyak::Reg32 r5d(r5.getIdx());

		pxor(m0, m0);

		if (nCbS == 4)
		{
			mov(r5d, 2);

			L("loop");
			{
				movd(m1, ptr[r2]);
				movd(m2, ptr[r2 + r3]);

				lea(r2, ptr[r2 + r3 * 2]);

				punpckldq(m1, m2);
				punpcklbw(m1, m0);

				movdqu(m3, ptr[r4]);
				paddw(m1, m3);
				lea(r4, ptr[r4 + 16]);
				packuswb(m1, m1);

				movd(ptr[r0], m1);
				pshufd(m1, m1, ORDER(0, 0, 0, 1));
				movd(ptr[r0 + r1], m1);
				lea(r0, ptr[r0 + r1 * 2]);
			}
			dec(r5d);
			jg("loop");
		}
		else
		{
			mov(r5d, nCbS);

			L("loop");
			{
				movq(m1, ptr[r2]);
				lea(r2, ptr[r2 + r3]);
				if (nCbS >= 16)
				{
					movdqa(m2, m1);
					punpckhbw(m2, m0);
				}
				punpcklbw(m1, m0);

				paddw(m1, ptr[r4]);
				lea(r4, ptr[r4 + 2*nCbS]);

				packuswb(m1, m1);

				movq(ptr[r0], m1);
				lea(r0, ptr[r0 + r1]);
			}
			dec(r5d);
			jg("loop");
		}
	}
};


hevcasm_quantize_reconstruct * HEVCASM_API get_quantize_reconstruct(int log2TrafoSize, hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);
	auto mask = buffer.isa;

	hevcasm_quantize_reconstruct *f = 0;
		
	if (mask & (HEVCASM_C_REF | HEVCASM_C_OPT)) f = hevcasm_quantize_reconstruct_c_ref;

	if (mask & HEVCASM_SSE41)
	{
		const int nCbS = 1 << log2TrafoSize;
		if (nCbS == 4)
		{
			QuantiseReconstruct qr(&buffer, nCbS);
			f = qr;
		}
		if (nCbS == 8)
		{
			QuantiseReconstruct qr(&buffer, nCbS);
			f = qr;
		}
		if (nCbS == 16)
		{
			QuantiseReconstruct qr(&buffer, nCbS);
			f = qr;
		}
		if (nCbS == 32)
		{
			QuantiseReconstruct qr(&buffer, nCbS);
			f = qr;
		}
	}

	return f;
}


void HEVCASM_API hevcasm_populate_quantize_reconstruct(hevcasm_table_quantize_reconstruct *table, hevcasm_code code)
{
	for (int log2TrafoSize = 2; log2TrafoSize < 6; ++log2TrafoSize)
	{
		*hevcasm_get_quantize_reconstruct(table, log2TrafoSize) = get_quantize_reconstruct(log2TrafoSize, code);
	}
}


typedef struct
{
	HEVCASM_ALIGN(32, uint8_t, rec[32 * 32]);
	ptrdiff_t stride_rec;
	const uint8_t *pred;
	ptrdiff_t stride_pred;
	const int16_t *res;
	int log2TrafoSize;
	hevcasm_quantize_reconstruct *f;
}
bound_quantize_reconstruct;


int init_quantize_reconstruct(void *p, hevcasm_code code)
{
	auto &buffer = *reinterpret_cast<Jit::Buffer *>(code.implementation);

	bound_quantize_reconstruct *s = (bound_quantize_reconstruct *)p;

	hevcasm_table_quantize_reconstruct table;

	hevcasm_populate_quantize_reconstruct(&table, code);

	s->f = *hevcasm_get_quantize_reconstruct(&table, s->log2TrafoSize);

	if (buffer.isa == HEVCASM_C_REF)
	{
		const int nCbS = 1 << s->log2TrafoSize;
		printf("\t%dx%d : ", nCbS, nCbS);
	}

	return !!s->f;
}


void invoke_quantize_reconstruct(void *p, int n)
{
	bound_quantize_reconstruct *s = (bound_quantize_reconstruct *)p;
	while (n--)
	{
		const int nCbS = 1 << s->log2TrafoSize;
		s->f(s->rec, s->stride_rec, s->pred, s->stride_pred, s->res, nCbS);
	}
}

int mismatch_quantize_reconstruct(void *boundRef, void *boundTest)
{
	bound_quantize_reconstruct *ref = (bound_quantize_reconstruct *)boundRef;
	bound_quantize_reconstruct *test = (bound_quantize_reconstruct *)boundTest;

	const int nCbS = 1 << ref->log2TrafoSize;

	int mismatch = 0;
	for (int y = 0; y < nCbS; ++y)
	{
		mismatch |= memcmp(
			&ref->rec[y * ref->stride_rec],
			&test->rec[y * test->stride_rec],
			nCbS);
	}

	return mismatch;
}



void HEVCASM_API hevcasm_test_quantize_reconstruct(int *error_count, hevcasm_instruction_set mask)
{
	printf("\nhevcasm_quantize_reconstruct - Reconstruction\n");

	HEVCASM_ALIGN(32, uint8_t, pred[32 * 32]);
	HEVCASM_ALIGN(32, int16_t, res[32 * 32]);

	for (int x = 0; x < 32 * 32; ++x)
	{
		pred[x] = rand() & 0xff;
		res[x] = (rand() & 0x1ff) - 0x100;
	}

	bound_quantize_reconstruct b[2];

	b[0].stride_rec = 32;
	b[0].pred = pred;
	b[0].stride_pred = 32;
	b[0].res = res;

	for (b[0].log2TrafoSize = 2; b[0].log2TrafoSize <= 5; ++b[0].log2TrafoSize)
	{
		b[1] = b[0];
		*error_count += hevcasm_test(&b[0], &b[1], init_quantize_reconstruct, invoke_quantize_reconstruct, mismatch_quantize_reconstruct, mask, 100000);
	}
}
