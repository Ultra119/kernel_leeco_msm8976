/* IEEE754 floating point arithmetic
 * double precision: common utilities
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


#include "ieee754dp.h"

int ieee754dp_cmp(union ieee754dp x, union ieee754dp y, int cmp, int sig)
{
	COMPXDP;
	COMPYDP;

	EXPLODEXDP;
	EXPLODEYDP;
	FLUSHXDP;
	FLUSHYDP;
	ieee754_clearcx();	/* Even clear inexact flag here */

	if (ieee754dp_isnan(x) || ieee754dp_isnan(y)) {
		if (sig || xc == IEEE754_CLASS_SNAN || yc == IEEE754_CLASS_SNAN)
			ieee754_setcx(IEEE754_INVALID_OPERATION);
		if (cmp & IEEE754_CUN)
			return 1;
		if (cmp & (IEEE754_CLT | IEEE754_CGT)) {
			if (sig && ieee754_setandtestcx(IEEE754_INVALID_OPERATION))
				return ieee754di_xcpt(0, "fcmpf", x);
		}
		return 0;
	} else {
		s64 vx = x.bits;
		s64 vy = y.bits;

		if (vx < 0)
			vx = -vx ^ DP_SIGN_BIT;
		if (vy < 0)
			vy = -vy ^ DP_SIGN_BIT;

		if (vx < vy)
			return (cmp & IEEE754_CLT) != 0;
		else if (vx == vy)
			return (cmp & IEEE754_CEQ) != 0;
		else
			return (cmp & IEEE754_CGT) != 0;
	}
}
