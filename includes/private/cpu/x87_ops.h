#ifndef _X87_OPS_H_
#define _X87_OPS_H_
#include <math.h>
#include <fenv.h>
#include "x87_timings.h"

#define fplog 0

static int rounding_modes[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};

#define ST(x) cpu_state.ST[((cpu_state.TOP+(x))&7)]

#define STATUS_ZERODIVIDE 4

#define FPCW_DISI (1 << 7)

#define x87_div(dst, src1, src2) do                             \
        {                                                       \
                if (((double)src2) == 0.0)                      \
                {                                               \
                        cpu_state.npxs |= STATUS_ZERODIVIDE;    \
                        if (cpu_state.npxc & STATUS_ZERODIVIDE) \
                                dst = src1 / (double)src2;      \
                        else                                    \
                        {                                       \
                                pclog("FPU : divide by zero\n"); \
                                picint(1 << 13);                \
                                return 1;                       \
                        }                                       \
                }                                               \
                else                                            \
                        dst = src1 / (double)src2;              \
        } while (0)

static inline void x87_checkexceptions() {
}

static inline void x87_push(double i) {
	cpu_state.TOP--;
	cpu_state.ST[cpu_state.TOP & 7] = i;
	cpu_state.tag[cpu_state.TOP & 7] = TAG_VALID;
}

static inline void x87_push_u64(uint64_t i) {
	union {
		double d;
		uint64_t ll;
	} td;

	td.ll = i;

	cpu_state.TOP--;
	cpu_state.ST[cpu_state.TOP & 7] = td.d;
	cpu_state.tag[cpu_state.TOP & 7] = TAG_VALID;
}

static inline double x87_pop() {
	double t = cpu_state.ST[cpu_state.TOP & 7];
	cpu_state.tag[cpu_state.TOP & 7] = TAG_EMPTY;
	cpu_state.TOP++;
	return t;
}

static inline int64_t x87_fround(double b) {
	int64_t a, c;

	switch ((cpu_state.npxc >> 10) & 3) {
	case 0: /*Nearest*/
		a = (int64_t)floor(b);
		c = (int64_t)floor(b + 1.0);
		if ((b - a) < (c - b))
			return a;
		else if ((b - a) > (c - b))
			return c;
		else
			return (a & 1) ? c : a;
	case 1: /*Down*/
		return (int64_t)floor(b);
	case 2: /*Up*/
		return (int64_t)ceil(b);
	case 3: /*Chop*/
		return (int64_t)b;
	}

	return 0;
}
#define BIAS80 16383
#define BIAS64 1023

static inline double x87_ld80() {
	struct {
		int16_t begin;
		union {
			double d;
			uint64_t ll;
		} eind;
	} test;
	test.eind.ll = readmeml(easeg, cpu_state.eaaddr);
	test.eind.ll |= (uint64_t)readmeml(easeg, cpu_state.eaaddr + 4) << 32;
	test.begin = readmemw(easeg, cpu_state.eaaddr + 8);

	int64_t exp64 = (((test.begin & 0x7fff) - BIAS80));
	int64_t blah = ((exp64 > 0) ? exp64 : -exp64) & 0x3ff;
	int64_t exp64final = ((exp64 > 0) ? blah : -blah) + BIAS64;

	int64_t mant64 = (test.eind.ll >> 11) & (0xfffffffffffff);
	int64_t sign = (test.begin & 0x8000) ? 1 : 0;

	if ((test.begin & 0x7fff) == 0x7fff)
		exp64final = 0x7ff;
	if ((test.begin & 0x7fff) == 0)
		exp64final = 0;
	if (test.eind.ll & 0x400)
		mant64++;

	test.eind.ll = (sign << 63) | (exp64final << 52) | mant64;

	return test.eind.d;
}

static inline void x87_st80(double d) {
	struct {
		int16_t begin;
		union {
			double d;
			uint64_t ll;
		} eind;
	} test;

	test.eind.d = d;

	int64_t sign80 = (test.eind.ll & (0x8000000000000000)) ? 1 : 0;
	int64_t exp80 = test.eind.ll & (0x7ff0000000000000);
	int64_t exp80final = (exp80 >> 52);
	int64_t mant80 = test.eind.ll & (0x000fffffffffffff);
	int64_t mant80final = (mant80 << 11);

	if (exp80final == 0x7ff) /*Infinity / Nan*/
	{
		exp80final = 0x7fff;
		mant80final |= (0x8000000000000000);
	} else if (d != 0) { //Zero is a special case
		// Elvira wants the 8 and tcalc doesn't
		mant80final |= (0x8000000000000000);
		//Ca-cyber doesn't like this when result is zero.
		exp80final += (BIAS80 - BIAS64);
	}
	test.begin = (((int16_t)sign80) << 15) | (int16_t)exp80final;
	test.eind.ll = mant80final;

	writememl(easeg, cpu_state.eaaddr, test.eind.ll);
	writememl(easeg, cpu_state.eaaddr + 4, test.eind.ll >> 32);
	writememw(easeg, cpu_state.eaaddr + 8, test.begin);
}

static inline void x87_st_fsave(int reg) {
	reg = (cpu_state.TOP + reg) & 7;

	if (cpu_state.tag[reg] & TAG_UINT64) {
		writememl(easeg, cpu_state.eaaddr, cpu_state.MM[reg].q & 0xffffffff);
		writememl(easeg, cpu_state.eaaddr + 4, cpu_state.MM[reg].q >> 32);
		writememw(easeg, cpu_state.eaaddr + 8, 0x5555);
	} else
		x87_st80(cpu_state.ST[reg]);
}

static inline void x87_ld_frstor(int reg) {
	reg = (cpu_state.TOP + reg) & 7;

	cpu_state.MM[reg].q = readmemq(easeg, cpu_state.eaaddr);
	cpu_state.MM_w4[reg] = readmemw(easeg, cpu_state.eaaddr + 8);

	if ((cpu_state.MM_w4[reg] == 0x5555) && (cpu_state.tag[reg] & TAG_UINT64)) {
		cpu_state.ST[reg] = (double)cpu_state.MM[reg].q;
	} else {
		cpu_state.tag[reg] &= ~TAG_UINT64;
		cpu_state.ST[reg] = x87_ld80();
	}
}

static inline void x87_ldmmx(MMX_REG *r, uint16_t *w4) {
	r->l[0] = readmeml(easeg, cpu_state.eaaddr);
	r->l[1] = readmeml(easeg, cpu_state.eaaddr + 4);
	*w4 = readmemw(easeg, cpu_state.eaaddr + 8);
}

static inline void x87_stmmx(MMX_REG r) {
	writememl(easeg, cpu_state.eaaddr, r.l[0]);
	writememl(easeg, cpu_state.eaaddr + 4, r.l[1]);
	writememw(easeg, cpu_state.eaaddr + 8, 0xffff);
}

static inline uint16_t x87_compare(double a, double b) {
#if defined i386 || defined __i386 || defined __i386__ || defined _X86_ || defined __amd64__
	uint32_t out;

	/* Memory barrier, to force GCC to write to the input parameters
         * before the compare rather than after */
	asm volatile ("" : : : "memory");

	asm(
		"fldl %2\n"
		"fldl %1\n"
		"fclex\n"
		"fcompp\n"
		"fnstsw %0\n"
		: "=m" (out)
		: "m" (a), "m" (b)
		);

	return out & (C0 | C2 | C3);
#else
																/* Generic C version is known to give incorrect results in some
         * situations, eg comparison of infinity (Unreal) */
        uint32_t out = 0;

        if (a == b)
                out |= C3;
        else if (a < b)
                out |= C0;

        return out;
#endif
}

static inline uint16_t x87_ucompare(double a, double b) {
#if defined i386 || defined __i386 || defined __i386__ || defined _X86_ || defined __amd64__
	uint32_t out;

	/* Memory barrier, to force GCC to write to the input parameters
         * before the compare rather than after */
	asm volatile ("" : : : "memory");

	asm(
		"fldl %2\n"
		"fldl %1\n"
		"fclex\n"
		"fucompp\n"
		"fnstsw %0\n"
		: "=m" (out)
		: "m" (a), "m" (b)
		);

	return out & (C0 | C2 | C3);
#else
																/* Generic C version is known to give incorrect results in some
         * situations, eg comparison of infinity (Unreal) */
        uint32_t out = 0;

        if (a == b)
                out |= C3;
        else if (a < b)
                out |= C0;

        return out;
#endif
}

typedef union {
	float s;
	uint32_t i;
} x87_ts;

typedef union {
	double d;
	uint64_t i;
} x87_td;

#ifdef X8087
#define FP_ENTER() fpucount++
#else
#define FP_ENTER() do                   \
        {                               \
                if (cr0 & 0xc)          \
                {                       \
                        x86_int(7);     \
                        return 1;       \
                }                       \
                fpucount++;             \
        } while (0)
#endif

#include "x87_ops_arith.h"
#include "x87_ops_misc.h"
#include "x87_ops_loadstore.h"

static int op_nofpu_a16(uint32_t fetchdat) {
	if (cr0 & 0xc) {
		x86_int(7);
		return 1;
	} else {
		fetch_ea_16(fetchdat);
		return 0;
	}
}
static int op_nofpu_a32(uint32_t fetchdat) {
	if (cr0 & 0xc) {
		x86_int(7);
		return 1;
	} else {
		fetch_ea_32(fetchdat);
		return 0;
	}
}

static int FPU_ILLEGAL_a16(uint32_t fetchdat) {
	fetch_ea_16(fetchdat);
	CLOCK_CYCLES(timing_rr);
	PREFETCH_RUN(timing_rr, 2, rmdat, 0, 0, 0, 0, 0);
	return 0;
}
static int FPU_ILLEGAL_a32(uint32_t fetchdat) {
	fetch_ea_32(fetchdat);
	CLOCK_CYCLES(timing_rr);
	PREFETCH_RUN(timing_rr, 2, rmdat, 0, 0, 0, 0, 0);
	return 0;
}

OpFn OP_TABLE(fpu_d8_a16)[32] =
	{
		opFADDs_a16, opFMULs_a16, opFCOMs_a16, opFCOMPs_a16, opFSUBs_a16, opFSUBRs_a16, opFDIVs_a16, opFDIVRs_a16,
		opFADDs_a16, opFMULs_a16, opFCOMs_a16, opFCOMPs_a16, opFSUBs_a16, opFSUBRs_a16, opFDIVs_a16, opFDIVRs_a16,
		opFADDs_a16, opFMULs_a16, opFCOMs_a16, opFCOMPs_a16, opFSUBs_a16, opFSUBRs_a16, opFDIVs_a16, opFDIVRs_a16,
		opFADD, opFMUL, opFCOM, opFCOMP, opFSUB, opFSUBR, opFDIV, opFDIVR
	};
OpFn OP_TABLE(fpu_d8_a32)[32] =
	{
		opFADDs_a32, opFMULs_a32, opFCOMs_a32, opFCOMPs_a32, opFSUBs_a32, opFSUBRs_a32, opFDIVs_a32, opFDIVRs_a32,
		opFADDs_a32, opFMULs_a32, opFCOMs_a32, opFCOMPs_a32, opFSUBs_a32, opFSUBRs_a32, opFDIVs_a32, opFDIVRs_a32,
		opFADDs_a32, opFMULs_a32, opFCOMs_a32, opFCOMPs_a32, opFSUBs_a32, opFSUBRs_a32, opFDIVs_a32, opFDIVRs_a32,
		opFADD, opFMUL, opFCOM, opFCOMP, opFSUB, opFSUBR, opFDIV, opFDIVR
	};

#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_d9_a16)[256] =
	{
		opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16,
		opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16,
		opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16,
		opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16,
		opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16,
		opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16,

		opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16,
		opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16,
		opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16,
		opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16,
		opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16,
		opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16,

		opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16, opFLDs_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16, opFSTs_a16,
		opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16, opFSTPs_a16,
		opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16, opFLDENV_a16,
		opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16, opFLDCW_a16,
		opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16, opFSTENV_a16,
		opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16, opFSTCW_a16,

		opFLD, opFLD, opFLD, opFLD, opFLD, opFLD, opFLD, opFLD,
		opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH,
		opFNOP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP,  /*Invalid*/
		opFCHS, opFABS, ILLEGAL, ILLEGAL, opFTST, opFXAM, ILLEGAL, ILLEGAL,
		opFLD1, opFLDL2T, opFLDL2E, opFLDPI, opFLDEG2, opFLDLN2, opFLDZ, ILLEGAL,
		opF2XM1, opFYL2X, opFPTAN, opFPATAN, ILLEGAL, opFPREM1, opFDECSTP, opFINCSTP,
		opFPREM, opFYL2XP1, opFSQRT, opFSINCOS, opFRNDINT, opFSCALE, opFSIN, opFCOS
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_d9_a32)[256] =
	{
		opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32,
		opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32,
		opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32,
		opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32,
		opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32,
		opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32,

		opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32,
		opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32,
		opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32,
		opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32,
		opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32,
		opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32,

		opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32, opFLDs_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32, opFSTs_a32,
		opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32, opFSTPs_a32,
		opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32, opFLDENV_a32,
		opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32, opFLDCW_a32,
		opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32, opFSTENV_a32,
		opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32, opFSTCW_a32,

		opFLD, opFLD, opFLD, opFLD, opFLD, opFLD, opFLD, opFLD,
		opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH, opFXCH,
		opFNOP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP,  /*Invalid*/
		opFCHS, opFABS, ILLEGAL, ILLEGAL, opFTST, opFXAM, ILLEGAL, ILLEGAL,
		opFLD1, opFLDL2T, opFLDL2E, opFLDPI, opFLDEG2, opFLDLN2, opFLDZ, ILLEGAL,
		opF2XM1, opFYL2X, opFPTAN, opFPATAN, ILLEGAL, opFPREM1, opFDECSTP, opFINCSTP,
		opFPREM, opFYL2XP1, opFSQRT, opFSINCOS, opFRNDINT, opFSCALE, opFSIN, opFCOS
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_da_a16)[256] =
	{
		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFUCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_da_a32)[256] =
	{
		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFUCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_686_da_a16)[256] =
	{
		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16, opFADDil_a16,
		opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16, opFMULil_a16,
		opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16, opFCOMil_a16,
		opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16, opFCOMPil_a16,
		opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16, opFSUBil_a16,
		opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16, opFSUBRil_a16,
		opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16, opFDIVil_a16,
		opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16, opFDIVRil_a16,

		opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB,
		opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE,
		opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE,
		opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFUCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_686_da_a32)[256] =
	{
		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32, opFADDil_a32,
		opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32, opFMULil_a32,
		opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32, opFCOMil_a32,
		opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32, opFCOMPil_a32,
		opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32, opFSUBil_a32,
		opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32, opFSUBRil_a32,
		opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32, opFDIVil_a32,
		opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32, opFDIVRil_a32,

		opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB, opFCMOVB,
		opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE, opFCMOVE,
		opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE, opFCMOVBE,
		opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU, opFCMOVU,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFUCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_db_a16)[256] =
	{
		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFENI, opFDISI, opFCLEX, opFINIT, opFNOP, opFNOP, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_db_a32)[256] =
	{
		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFENI, opFDISI, opFCLEX, opFINIT, opFNOP, opFNOP, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_686_db_a16)[256] =
	{
		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16, opFILDil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16, opFISTil_a16,
		opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16, opFISTPil_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16, opFLDe_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16, opFSTPe_a16,

		opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB,
		opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE,
		opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE,
		opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU,
		ILLEGAL, opFNOP, opFCLEX, opFINIT, opFNOP, opFNOP, ILLEGAL, ILLEGAL,
		opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI,
		opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_686_db_a32)[256] =
	{
		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32, opFILDil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32, opFISTil_a32,
		opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32, opFISTPil_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32, opFLDe_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32, opFSTPe_a32,

		opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB, opFCMOVNB,
		opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE, opFCMOVNE,
		opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE, opFCMOVNBE,
		opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU, opFCMOVNU,
		ILLEGAL, opFNOP, opFCLEX, opFINIT, opFNOP, opFNOP, ILLEGAL, ILLEGAL,
		opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI, opFUCOMI,
		opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI, opFCOMI,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL

OpFn OP_TABLE(fpu_dc_a16)[32] =
	{
		opFADDd_a16, opFMULd_a16, opFCOMd_a16, opFCOMPd_a16, opFSUBd_a16, opFSUBRd_a16, opFDIVd_a16, opFDIVRd_a16,
		opFADDd_a16, opFMULd_a16, opFCOMd_a16, opFCOMPd_a16, opFSUBd_a16, opFSUBRd_a16, opFDIVd_a16, opFDIVRd_a16,
		opFADDd_a16, opFMULd_a16, opFCOMd_a16, opFCOMPd_a16, opFSUBd_a16, opFSUBRd_a16, opFDIVd_a16, opFDIVRd_a16,
		opFADDr, opFMULr, opFCOM, opFCOMP, opFSUBRr, opFSUBr, opFDIVRr, opFDIVr
	};
OpFn OP_TABLE(fpu_dc_a32)[32] =
	{
		opFADDd_a32, opFMULd_a32, opFCOMd_a32, opFCOMPd_a32, opFSUBd_a32, opFSUBRd_a32, opFDIVd_a32, opFDIVRd_a32,
		opFADDd_a32, opFMULd_a32, opFCOMd_a32, opFCOMPd_a32, opFSUBd_a32, opFSUBRd_a32, opFDIVd_a32, opFDIVRd_a32,
		opFADDd_a32, opFMULd_a32, opFCOMd_a32, opFCOMPd_a32, opFSUBd_a32, opFSUBRd_a32, opFDIVd_a32, opFDIVRd_a32,
		opFADDr, opFMULr, opFCOM, opFCOMP, opFSUBRr, opFSUBr, opFDIVRr, opFDIVr
	};

#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_dd_a16)[256] =
	{
		opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16,
		opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16,
		opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16,
		opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16,

		opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16,
		opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16,
		opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16,
		opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16,

		opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16, opFLDd_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16, opFSTd_a16,
		opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16, opFSTPd_a16,
		opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16, opFSTOR_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16, opFSAVE_a16,
		opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16, opFSTSW_a16,

		opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFST, opFST, opFST, opFST, opFST, opFST, opFST, opFST,
		opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP,
		opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM,
		opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_dd_a32)[256] =
	{
		opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32,
		opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32,
		opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32,
		opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32,

		opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32,
		opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32,
		opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32,
		opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32,

		opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32, opFLDd_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32, opFSTd_a32,
		opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32, opFSTPd_a32,
		opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32, opFSTOR_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32, opFSAVE_a32,
		opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32, opFSTSW_a32,

		opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE, opFFREE,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFST, opFST, opFST, opFST, opFST, opFST, opFST, opFST,
		opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP, opFSTP,
		opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM, opFUCOM,
		opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP, opFUCOMP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_de_a16)[256] =
	{
		opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16,
		opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16,
		opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16,
		opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16,
		opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16,
		opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16,
		opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16,
		opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16,

		opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16,
		opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16,
		opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16,
		opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16,
		opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16,
		opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16,
		opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16,
		opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16,

		opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16, opFADDiw_a16,
		opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16, opFMULiw_a16,
		opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16, opFCOMiw_a16,
		opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16, opFCOMPiw_a16,
		opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16, opFSUBiw_a16,
		opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16, opFSUBRiw_a16,
		opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16, opFDIViw_a16,
		opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16, opFDIVRiw_a16,

		opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP,
		opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP,
		opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP,
		opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP,
		opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_de_a32)[256] =
	{
		opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32,
		opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32,
		opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32,
		opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32,
		opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32,
		opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32,
		opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32,
		opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32,

		opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32,
		opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32,
		opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32,
		opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32,
		opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32,
		opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32,
		opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32,
		opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32,

		opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32, opFADDiw_a32,
		opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32, opFMULiw_a32,
		opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32, opFCOMiw_a32,
		opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32, opFCOMPiw_a32,
		opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32, opFSUBiw_a32,
		opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32, opFSUBRiw_a32,
		opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32, opFDIViw_a32,
		opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32, opFDIVRiw_a32,

		opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP, opFADDP,
		opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP, opFMULP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, opFCOMPP, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP, opFSUBRP,
		opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP, opFSUBP,
		opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP, opFDIVRP,
		opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP, opFDIVP,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_df_a16)[256] =
	{
		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTSW_AX, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_df_a32)[256] =
	{
		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTSW_AX, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};

#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a16
OpFn OP_TABLE(fpu_686_df_a16)[256] =
	{
		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16, opFILDiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16, opFISTiw_a16,
		opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16, opFISTPiw_a16,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16, opFILDiq_a16,
		FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16, FBSTP_a16,
		FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16, FISTPiq_a16,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTSW_AX, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP,
		opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL
#define ILLEGAL FPU_ILLEGAL_a32
OpFn OP_TABLE(fpu_686_df_a32)[256] =
	{
		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32, opFILDiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32, opFISTiw_a32,
		opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32, opFISTPiw_a32,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32, opFILDiq_a32,
		FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32, FBSTP_a32,
		FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32, FISTPiq_a32,

		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFSTSW_AX, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
		opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP, opFUCOMIP,
		opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP, opFCOMIP,
		ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
	};
#undef ILLEGAL

OpFn OP_TABLE(nofpu_a16)[256] =
	{
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,

		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,

		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,

		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
		op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16, op_nofpu_a16,
	};
OpFn OP_TABLE(nofpu_a32)[256] =
	{
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,

		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,

		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,

		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
		op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32, op_nofpu_a32,
	};

#endif /* _X87_OPS_H_ */
