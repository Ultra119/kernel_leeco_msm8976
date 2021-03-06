/*
 * IEEE754 floating point
 * double precision internal header file
 */
/*
 * MIPS floating point support
 * Copyright (C) 1994-2000 Algorithmics Ltd.
 *
 * ########################################################################
 *
 *  This program is free software; you can distribute it and/or modify it
 *  under the terms of the GNU General Public License (Version 2) as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * ########################################################################
 */

#include <linux/compiler.h>

#include "ieee754int.h"

#define assert(expr) ((void)0)

#define DP_EBIAS	1023
#define DP_EMIN		(-1022)
#define DP_EMAX		1023
#define DP_FBITS	52
#define DP_MBITS	52

#define DP_MBIT(x)	((u64)1 << (x))
#define DP_HIDDEN_BIT	DP_MBIT(DP_FBITS)
#define DP_SIGN_BIT	DP_MBIT(63)

#define DPSIGN(dp)	(dp.parts.sign)
#define DPBEXP(dp)	(dp.parts.bexp)
#define DPMANT(dp)	(dp.parts.mant)

static inline int ieee754dp_finite(union ieee754dp x)
{
	return DPBEXP(x) != DP_EMAX + 1 + DP_EBIAS;
}

/* 3bit extended double precision sticky right shift */
#define XDPSRS(v,rs)	\
	((rs > (DP_FBITS+3))?1:((v) >> (rs)) | ((v) << (64-(rs)) != 0))

#define XDPSRSX1() \
	(xe++, (xm = (xm >> 1) | (xm & 1)))

#define XDPSRS1(v)	\
	(((v) >> 1) | ((v) & 1))

/* convert denormal to normalized with extended exponent */
#define DPDNORMx(m,e) \
	while ((m >> DP_FBITS) == 0) { m <<= 1; e--; }
#define DPDNORMX	DPDNORMx(xm, xe)
#define DPDNORMY	DPDNORMx(ym, ye)

static inline union ieee754dp builddp(int s, int bx, u64 m)
{
	union ieee754dp r;

	assert((s) == 0 || (s) == 1);
	assert((bx) >= DP_EMIN - 1 + DP_EBIAS
	       && (bx) <= DP_EMAX + 1 + DP_EBIAS);
	assert(((m) >> DP_FBITS) == 0);

	r.parts.sign = s;
	r.parts.bexp = bx;
	r.parts.mant = m;
	return r;
}

extern int ieee754dp_isnan(union ieee754dp);
extern int __cold ieee754si_xcpt(int, const char *, ...);
extern s64 __cold ieee754di_xcpt(s64, const char *, ...);
extern union ieee754dp __cold ieee754dp_xcpt(union ieee754dp, const char *, ...);
extern union ieee754dp __cold ieee754dp_nanxcpt(union ieee754dp, const char *, ...);
extern union ieee754dp ieee754dp_format(int, int, u64);


#define DPNORMRET2(s, e, m, name, a0, a1)				\
{									\
	union ieee754dp V = ieee754dp_format(s, e, m);			\
	if (ieee754_tstx())						\
		return ieee754dp_xcpt(V, name, a0, a1);			\
	else								\
		return V;						\
}

#define DPNORMRET1(s, e, m, name, a0)  DPNORMRET2(s, e, m, name, a0, a0)
