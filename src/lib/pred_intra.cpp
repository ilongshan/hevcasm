/*
The copyright in this software is being made available under the BSD
License, included below. This software may be subject to other third party
and contributor rights, including patent rights, and no such rights are
granted under this license.


Copyright(c) 2011 - 2015, Parabola Research Limited
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met :

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and / or other materials provided with the distribution.
* Neither the name of the copyright holder nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
*/


//#include "pred_intra_a.h"
#include "pred_intra.h"
#include "hevcasm_test.h"
#include "Jit.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>




static int Clip3(int min, int max, int value)
{
	if (value < min) return min;
	if (value > max) return max;
	return value;
}



static uint8_t p(const uint8_t *neighbours, int dx, int dy)
{
	assert(dx == -1 || dy == -1);
	assert(dx != dy);

	return neighbours[dy < 0 ? 64 + dx : 63 - dy];
}


void HEVCASM_API hevcasm_pred_intra_dc_ref(uint8_t *dst, const uint8_t *neighbours, int intraPredMode, hevcasm_pred_intra_packed packed)
{
	const int k = (packed >> 1) & 0x7f;
	const int nTbS = 1 << k;

	int dcVal = nTbS;
	for (int x1 = 0; x1 < nTbS; ++x1)
	{
		dcVal += p(neighbours, x1, -1);
	}
	for (int y1 = 0; y1 < nTbS; ++y1)
	{
		dcVal += p(neighbours, -1, y1);
	}
	dcVal >>= (k + 1);

	const int filter_edge = (packed & 1);

	int start = 0;
	if (filter_edge)
	{
		assert(nTbS < 32);

		start = 1;
		dst[0 + 0 * nTbS] = (p(neighbours, -1, 0) + 2 * dcVal + p(neighbours, 0, -1) + 2) >> 2;
		for (int x = 1; x<nTbS; ++x)
		{
			dst[x + 0*nTbS] = (p(neighbours, x, -1) + 3 * dcVal + 2) >> 2;
		}
		for (int y = 1; y<nTbS; ++y)
		{
			dst[0 + y*nTbS] = (p(neighbours, -1, y) + 3 * dcVal + 2) >> 2;
		}
	}

	for (int y = start; y<nTbS; ++y)
	{
		memset(&dst[start + y*nTbS], dcVal, nTbS - start);
	}
}



struct IntraPredDc8
	:
	Jit::Function
{
	IntraPredDc8(Jit::Buffer *buffer) 
		: 
		Jit::Function(buffer, Jit::CountArguments<hevcasm_pred_intra>::value)
	{ 
		this->build(); 
	}

	Xbyak::Label pat_w_2048;
	Xbyak::Label ang_hor_8;
	Xbyak::Label pat_q_255;

	void data()
	{
		align(32);

		// from f265

		// Repeat values on a whole 8x8 row.Inversed for use in pure horizontal.
		{
			L(ang_hor_8);
			db({ 3 }, 8);
			db({ 2 }, 8);
			db({ 1 }, 8);
			db({ 0 }, 8);
		}

		{
			L(pat_q_255);
			dq({ 255 });
		}

		{
			L(pat_w_2048);
			dw({ 2048 }, 2);
		}
	}

	void assemble()
	{
		auto rcx = reg64(0);
		auto rdx = reg64(1);
		auto r8 = reg64(2);
		auto r8d = Xbyak::Reg32(r8.getIdx());
		auto r9 = reg64(3);
		auto r9d = Xbyak::Reg32(r9.getIdx());

		regXmm(16);
		reg64(9);

		// from f265

		//Logic:
		//Sum all neighbours, except the corners.
		//Divide with bias by the number of samples.

		vpmovzxbw(xmm1, ptr[rdx + 0x38]); // Load all data.
		vpmovzxbw(xmm0, ptr[rdx + 0x40]);

		vinserti128(ymm2, ymm0, xmm1, 1); // Keep a copy for filtering.

		vpaddw(ymm1, ymm1, ymm0); // Add them together.

		vpalignr(ymm0, ymm0, ymm1, 8); // At each step, fold the register in 2...
		vpaddw(ymm1, ymm1, ymm0); // ... then add each value together.

		vpalignr(ymm0, ymm0, ymm1, 4);
		vpaddw(ymm1, ymm1, ymm0);

		vpalignr(ymm0, ymm0, ymm1, 2);
		vpaddw(ymm1, ymm1, ymm0);

		vmovd(xmm0, ptr[rip + pat_w_2048]);
		vpmulhrsw(ymm1, ymm1, ymm0); // Round.

		vpbroadcastb(ymm1, xmm1); // Replicate the value.
		vmovdqa(ymm0, ymm1);

		and(r9, 1);
		je("skip filter", T_NEAR);
		{
			// 3 cases:
			// -Top - left = 2 * base + top + left.
			// -Top = 3 * base + top.
			// -Left = 3 * base + left.

			vmovd(r8d, xmm1); // Extract base.
			and (r8, 0xFF);

			lea(r9, ptr[r8 + r8 * 2 + 2]); // Base * 3 + rounding bias.
			vmovd(xmm3, r9d);
			vpbroadcastw(ymm3, xmm3); // Broadcast base * 3 + rounding bias.

			movzx(r9, ptr[rdx + 0x40]); // Load the first top and left value.
			movzx(rax, ptr[rdx + 0x3F]);

			vpaddw(ymm2, ymm2, ymm3); // 3 * base + neighbours + rounding bias.
			vpsrlw(ymm2, ymm2, 2); // Divide by 4.

			vpackuswb(ymm2, ymm2, ymm2); // Word to byte.

			vpblendd(ymm0, ymm2, ymm0, 0xFC); // Save in top row.

			vpermq(ymm2, ymm2, 0xAA); // Broadcast left column.

			vmovdqu(ymm3, ptr[rip + ang_hor_8]);
			vpbroadcastq(ymm5, ptr[rip + pat_q_255]);
			
			vpshufb(ymm4, ymm2, ymm3); //  Replicate 8x the 4 lower values.
			vpsrldq(ymm2, ymm2, 4); //  Shift by 4 to do the 4 last rows.
			vpblendvb(ymm1, ymm1, ymm4, ymm5); // Blend only the first value of each row.4, ymm5);

			vpshufb(ymm4, ymm2, ymm3); // Replicate 8x the 4 lower values.
			vpblendvb(ymm0, ymm0, ymm4, ymm5); // Blend only the first value of each row.

			// Do top left.
			add(r9, rax); // Top + left
			lea(r8, ptr[r9 + r8 * 2 + 2]); // Top + left + 2*base + bias.
			shr(r8, 2); // Get the average.

			vmovdqa(ymm2, ymm0);
			vpinsrb(xmm2, xmm2, r8d, 0);
			vinserti128(ymm0, ymm0, xmm2, 0);
		}
		L("skip filter");
	
		vmovdqu(ptr[rcx], ymm0);
		vmovdqu(ptr[rcx + 0x20], ymm1); // Save the value.

		vzeroupper();
	}
};


#ifdef WIN32
#define FASTCALL __fastcall
#else
#define FASTCALL
#endif


void wrap(uint8_t *dst, const uint8_t *neighbours, int intraPredMode, hevcasm_pred_intra_packed packed)
{
	static Jit::Buffer buffer(1000);
	static IntraPredDc8 intraPredDc8(&buffer);
	hevcasm_pred_intra *f = intraPredDc8;
	f(dst, neighbours, intraPredMode, packed);
}


void FASTCALL f265_lbd_predict_intra_dc_8_avx2(uint8_t *dst, const uint8_t *neighbours, int intraPredMode, hevcasm_pred_intra_packed packed);


void hevcasm_populate_pred_intra(hevcasm_table_pred_intra *table, hevcasm_instruction_set mask)
{
	for (int k = 2; k <= 5; ++k)
	{
		for (int intraPredModeY = 0; intraPredModeY < 35; ++intraPredModeY)
		{
			*hevcasm_get_pred_intra(table, intraPredModeY, hevcasm_pred_intra_pack(0, k)) = 0;
			*hevcasm_get_pred_intra(table, intraPredModeY, hevcasm_pred_intra_pack(1, k)) = 0;
		}

		for (int cIdx = 0; cIdx < 2; ++cIdx)
		{
			hevcasm_pred_intra **entry = hevcasm_get_pred_intra(table, 1, hevcasm_pred_intra_pack(cIdx, k));
			*entry = 0;
			if (mask & HEVCASM_C_REF) *entry = hevcasm_pred_intra_dc_ref;
			if (mask & HEVCASM_C_OPT) *entry = hevcasm_pred_intra_dc_ref;
		}
	}

#if !defined(WIN32) || defined(_M_X64)
	if (mask & HEVCASM_AVX2)
	{
		hevcasm_pred_intra **entry = hevcasm_get_pred_intra(table, 1, hevcasm_pred_intra_pack(1, 3));
		*entry = wrap;
		entry = hevcasm_get_pred_intra(table, 1, hevcasm_pred_intra_pack(0, 3));
		*entry = wrap;
	}
#endif
}


typedef struct 
{
	hevcasm_pred_intra *f;
	HEVCASM_ALIGN(32, uint8_t, dst[64 * 64]);
	const uint8_t *neighbours;
	int intraPredMode;
	int packed;
}
bound_pred_intra;


static int get_pred_intra(void *p, hevcasm_instruction_set mask)
{
	bound_pred_intra *s = (bound_pred_intra *)p;

	const char *lookup[35] = { 0, "DC", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	const char *name = lookup[s->intraPredMode];
	if (!name) return 0;
		
	hevcasm_table_pred_intra table;

	hevcasm_populate_pred_intra(&table, mask);

	s->f = *hevcasm_get_pred_intra(&table, s->intraPredMode, s->packed);

	if (s->f && mask == HEVCASM_C_REF)
	{
		const int k = (s->packed >> 1) & 0x7f;
		const int nTbS = 1 << k;
		printf("\t%dx%d %s", nTbS, nTbS, name);
		if (s->packed & 1)
		{
			printf(" edge");
		}
	}

	memset(s->dst, 0, 64 * 64);

	return !!s->f;
}


void invoke_pred_intra(void *p, int n)
{
	bound_pred_intra *s = (bound_pred_intra *)p;
	while (n--)
	{
		s->f(s->dst, s->neighbours, s->intraPredMode, s->packed);
	}
}


int mismatch_pred_intra(void *boundRef, void *boundTest)
{
	bound_pred_intra *ref = (bound_pred_intra *)boundRef;
	bound_pred_intra *test = (bound_pred_intra *)boundTest;

	const int k = (ref->packed >> 1) & 0x7f;
	const int nTbS = 1 << k;

	return memcmp(ref->dst, test->dst, nTbS*nTbS);
}


void HEVCASM_API hevcasm_test_pred_intra(int *error_count, hevcasm_instruction_set mask)
{
	printf("\nhevcasm_pred_intra - Intra Prediction\n");

	bound_pred_intra b[2];

	HEVCASM_ALIGN(32, uint8_t, neighbours[256]);
	b[0].neighbours = neighbours + 128;

	for (int x = 0; x < 256; x++) neighbours[x] = rand() & 0xff;

	for (int k = 2; k <= 5; ++k)
	{
		for (b[0].intraPredMode = 1; b[0].intraPredMode < 2; ++b[0].intraPredMode)
		{
			b[0].packed = hevcasm_pred_intra_pack(1, k);
			b[1] = b[0];
			hevcasm_test(&b[0], &b[1], get_pred_intra, invoke_pred_intra, mismatch_pred_intra, mask, 1000);

			if (k <= 4)
			{
				b[0].packed = hevcasm_pred_intra_pack(0, k);
				b[1] = b[0];
				hevcasm_test(&b[0], &b[1], get_pred_intra, invoke_pred_intra, mismatch_pred_intra, mask, 1000);
			}
		}
	}
}
