/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include "header/local.h"


#define SPANSTEP_SHIFT	4

byte	**warp_rowptr;
int	*warp_column;

/*
=============
D_WarpScreen

this performs a slight compression of the screen at the same time as
the sine warp, to keep the edges from wrapping
=============
*/
void
D_WarpScreen (void)
{
	int	w, h;
	int	u,v;
	pixel_t	*dest;
	int	*turb;
	byte	**row;

	static int	cached_width, cached_height;

	//
	// these are constant over resolutions, and can be saved
	//
	w = r_newrefdef.width;
	h = r_newrefdef.height;
	if (w != cached_width || h != cached_height)
	{
		cached_width = w;
		cached_height = h;
		for (v=0 ; v<h+AMP2*2 ; v++)
		{
			int v2;

			v2 = (int)((float)v/(h + AMP2 * 2) * r_refdef.vrect.height);
			warp_rowptr[v] = r_warpbuffer + (vid_buffer_width * v2);
		}

		for (u=0 ; u<w+AMP2*2 ; u++)
		{
			int u2;

			u2 = (int)((float)u/(w + AMP2 * 2) * r_refdef.vrect.width);
			warp_column[u] = u2;
		}
	}

	turb = intsintable + ((int)(r_newrefdef.time*SPEED)&(CYCLE-1));
	dest = vid_buffer + r_newrefdef.y * vid_buffer_width + r_newrefdef.x;

	for (v=0 ; v<h ; v++, dest += vid_buffer_width)
	{
		int *col;

		col = warp_column + turb[v];
		row = warp_rowptr + v;
		for (u=0 ; u<w ; u++)
		{
			dest[u] = row[turb[u]][col[u]];
		}
	}
}


/*
=============
D_DrawSpanGetStep

Return safe span step for u/z and v/z
=============
*/
static int
D_DrawSpanGetStep(float d_zistepu, float d_zistepv)
{
	int	spanzshift = SPANSTEP_SHIFT;
	int	spanzshift_value = (1 << spanzshift);
	float	d_zistepu_shifted, d_zistepv_shifted;

	d_zistepu_shifted = d_zistepu * SHIFT16XYZ_MULT;
	d_zistepv_shifted = d_zistepv * SHIFT16XYZ_MULT;

	// check that we can draw parallel surfaces to screen surface
	// (both ends have same z value)
	if ((int)(d_zistepu_shifted * spanzshift_value) == 0 &&
	    (int)(d_zistepv_shifted * spanzshift_value) == 0)
	{
		// search next safe value
		do
		{
			spanzshift ++;
			spanzshift_value <<= 1;

			if ((int)(d_zistepu_shifted * spanzshift_value) != 0 ||
			    (int)(d_zistepv_shifted * spanzshift_value) != 0)
			{
				// step back to last safe value
				return spanzshift - 1;
			}

		}
		while (spanzshift_value < vid_buffer_width);
	}
	return spanzshift;
}

/*
=============
D_DrawZSpanGetStepValue

Return safe span step for izistep
=============
*/
static int
D_DrawZSpanGetStepValue(zvalue_t izistep)
{
	// check that we can draw parallel surfaces to screen surface
	// (both ends have same z value)
	int	count = 1;

	while ((izistep * count) >> SHIFT16XYZ == 0 && count < vid_buffer_width)
	{
		count <<= 1;
	}

	return count;
}

/*
=============
D_DrawTurbulentSpan
=============
*/
static pixel_t *
D_DrawTurbulentSpan (pixel_t *pdest, const pixel_t *pbase,
			 int s, int t,
			 int sstep, int tstep,
			 int spancount,
			 const int *turb)
{
	do
	{
		int sturb, tturb;
		sturb = ((s + turb[(t>>SHIFT16XYZ)&(CYCLE-1)])>>16)&63;
		tturb = ((t + turb[(s>>SHIFT16XYZ)&(CYCLE-1)])>>16)&63;
		*pdest++ = *(pbase + (tturb<<6) + sturb);
		s += sstep;
		t += tstep;
	} while (--spancount > 0);
	return pdest;
}

/*
=============
TurbulentPow2
=============
*/
void
TurbulentPow2 (espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv)
{
	float	spancountminus1;
	float	sdivzpow2stepu, tdivzpow2stepu, zipow2stepu;
	pixel_t	*r_turb_pbase;
	int	*r_turb_turb;
	int	spanstep_shift, spanstep_value;

	spanstep_shift = D_DrawSpanGetStep(d_zistepu, d_zistepv);
	spanstep_value = (1 << spanstep_shift);

	r_turb_turb = sintable + ((int)(r_newrefdef.time*SPEED)&(CYCLE-1));

	r_turb_pbase = (unsigned char *)cacheblock;

	sdivzpow2stepu = d_sdivzstepu * spanstep_value;
	tdivzpow2stepu = d_tdivzstepu * spanstep_value;
	zipow2stepu = d_zistepu * spanstep_value;

	do
	{
		int count, r_turb_s, r_turb_t;
		float sdivz, tdivz, zi, z, du, dv;
		pixel_t	*r_turb_pdest;

		r_turb_pdest = d_viewbuffer + (vid_buffer_width * pspan->v) + pspan->u;

		count = pspan->count;

		// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)
			r_turb_s = bbextents;
		else if (r_turb_s < 0)
			r_turb_s = 0;

		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)
			r_turb_t = bbextentt;
		else if (r_turb_t < 0)
			r_turb_t = 0;

		do
		{
			int snext, tnext;
			int r_turb_sstep, r_turb_tstep;
			int r_turb_spancount;

			r_turb_sstep = 0;	// keep compiler happy
			r_turb_tstep = 0;	// ditto

			// calculate s and t at the far end of the span
			if (count >= spanstep_value)
				r_turb_spancount = spanstep_value;
			else
				r_turb_spancount = count;

			count -= r_turb_spancount;

			if (count)
			{
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivzpow2stepu;
				tdivz += tdivzpow2stepu;
				zi += zipow2stepu;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				r_turb_sstep = (snext - r_turb_s) >> spanstep_shift;
				r_turb_tstep = (tnext - r_turb_t) >> spanstep_shift;
			}
			else
			{
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				if (r_turb_spancount > 1)
				{
					r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
					r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
				}
			}

			r_turb_s = r_turb_s & ((CYCLE<<SHIFT16XYZ)-1);
			r_turb_t = r_turb_t & ((CYCLE<<SHIFT16XYZ)-1);

			r_turb_pdest = D_DrawTurbulentSpan (r_turb_pdest, r_turb_pbase,
							    r_turb_s, r_turb_t,
							    r_turb_sstep, r_turb_tstep,
							    r_turb_spancount,
							    r_turb_turb);

			r_turb_s = snext;
			r_turb_t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

//====================
/*
=============
NonTurbulentPow2 - this is for drawing scrolling textures. they're warping water textures
	but the turbulence is automatically 0.
=============
*/
void
NonTurbulentPow2 (espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv)
{
	float spancountminus1;
	float sdivzpow2stepu, tdivzpow2stepu, zipow2stepu;
	pixel_t	*r_turb_pbase;
	int	*r_turb_turb;
	int	spanstep_shift, spanstep_value;

	spanstep_shift = D_DrawSpanGetStep(d_zistepu, d_zistepv);
	spanstep_value = (1 << spanstep_shift);

	r_turb_turb = blanktable;

	r_turb_pbase = (unsigned char *)cacheblock;

	sdivzpow2stepu = d_sdivzstepu * spanstep_value;
	tdivzpow2stepu = d_tdivzstepu * spanstep_value;
	zipow2stepu = d_zistepu * spanstep_value;

	do
	{
		int count, r_turb_s, r_turb_t;
		float sdivz, tdivz, zi, z, dv, du;
		pixel_t	*r_turb_pdest;

		r_turb_pdest = d_viewbuffer + (vid_buffer_width * pspan->v) + pspan->u;

		count = pspan->count;

		// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

		r_turb_s = (int)(sdivz * z) + sadjust;
		if (r_turb_s > bbextents)
			r_turb_s = bbextents;
		else if (r_turb_s < 0)
			r_turb_s = 0;

		r_turb_t = (int)(tdivz * z) + tadjust;
		if (r_turb_t > bbextentt)
			r_turb_t = bbextentt;
		else if (r_turb_t < 0)
			r_turb_t = 0;

		do
		{
			int snext, tnext;
			int r_turb_sstep, r_turb_tstep;
			int r_turb_spancount;

			r_turb_sstep = 0;	// keep compiler happy
			r_turb_tstep = 0;	// ditto

			// calculate s and t at the far end of the span
			if (count >= spanstep_value)
				r_turb_spancount = spanstep_value;
			else
				r_turb_spancount = count;

			count -= r_turb_spancount;

			if (count)
			{
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivzpow2stepu;
				tdivz += tdivzpow2stepu;
				zi += zipow2stepu;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				r_turb_sstep = (snext - r_turb_s) >> spanstep_shift;
				r_turb_tstep = (tnext - r_turb_t) >> spanstep_shift;
			}
			else
			{
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				spancountminus1 = (float)(r_turb_spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				if (r_turb_spancount > 1)
				{
					r_turb_sstep = (snext - r_turb_s) / (r_turb_spancount - 1);
					r_turb_tstep = (tnext - r_turb_t) / (r_turb_spancount - 1);
				}
			}

			r_turb_s = r_turb_s & ((CYCLE<<SHIFT16XYZ)-1);
			r_turb_t = r_turb_t & ((CYCLE<<SHIFT16XYZ)-1);

			r_turb_pdest = D_DrawTurbulentSpan (r_turb_pdest, r_turb_pbase,
							    r_turb_s, r_turb_t,
							    r_turb_sstep, r_turb_tstep,
							    r_turb_spancount,
							    r_turb_turb);

			r_turb_s = snext;
			r_turb_t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

//====================

// Enable custom filtering
extern cvar_t	*sw_texture_filtering;
static const int filtering_kernel[2][2][2] = {
	{
		{0x1 << (SHIFT16XYZ-2), 0x0},
		{0xC << (SHIFT16XYZ-4), 0x1 << (SHIFT16XYZ-1)}
	},
	{
		{0x1 << (SHIFT16XYZ-1), 0xC << (SHIFT16XYZ-4)},
		{0x0, 0x1 << (SHIFT16XYZ-2)}
	}
};

/*
=============
D_DrawSpan
=============
*/
static pixel_t *
D_DrawSpan(pixel_t *pdest, const pixel_t *pbase, int cachewidth, int s, int t, int sstep, int tstep, int spancount)
{
	const pixel_t *tdest_max = pdest + spancount;

	// horisontal span (span in same row)
	if (((t + tstep * spancount) >> SHIFT16XYZ) == (t >> SHIFT16XYZ))
	{
		// position in texture
		const pixel_t *tbase = pbase + (t >> SHIFT16XYZ) * cachewidth;

		do
		{
			*pdest++ = *(tbase + (s >> SHIFT16XYZ));
			s += sstep;
		} while (pdest < tdest_max);
	}
	// vertical span (span in same column)
	else if (((s + sstep * spancount) >> SHIFT16XYZ) == (s >> SHIFT16XYZ))
	{
		// position in texture
		const pixel_t *tbase = pbase + (s >> SHIFT16XYZ);

		do
		{
			*pdest++ = *(tbase + (t >> SHIFT16XYZ) * cachewidth);
			t += tstep;
		} while (pdest < tdest_max);
	}
	// diagonal span
	else
	{
		do
		{
			*pdest++ = *(pbase + (s >> SHIFT16XYZ) + (t >> SHIFT16XYZ) * cachewidth);
			s += sstep;
			t += tstep;
		} while (pdest < tdest_max);
	}

	return pdest;
}

/*
=============
D_DrawSpanFiltered
=============
*/
static pixel_t *
D_DrawSpanFiltered(pixel_t *pdest, const pixel_t *pbase, int cachewidth, int s, int t, int sstep, int tstep, int spancount,
	   const espan_t *pspan)
{
	do
	{
		int idiths = s;
		int iditht = t;

		int X = (pspan->u + spancount) & 1;
		int Y = (pspan->v)&1;

		//Using the kernel
		idiths += filtering_kernel[X][Y][0];
		iditht += filtering_kernel[X][Y][1];

		idiths = idiths >> SHIFT16XYZ;
		idiths = idiths ? idiths -1 : idiths;


		iditht = iditht >> SHIFT16XYZ;
		iditht = iditht ? iditht -1 : iditht;


		*pdest++ = *(pbase + idiths + iditht * cachewidth);
		s += sstep;
		t += tstep;
	} while (--spancount > 0);

	return pdest;
}

/*
=============
D_DivSpan

x / n as C has it (truncated), for the step of the last part of a span. The
ARM of the MiSTer has no division instruction: for the parts of up to 16
pixels a multiplication with ceil(2^32 / n) gives the same quotient, as
|x| < 2^28 (s and t are clamped to the surface's extents in 16.16).
=============
*/
#define D_RECIP(n) (0xFFFFFFFFu / (n) + 1)
static const unsigned d_span_recip[17] = {
	0, 0, D_RECIP(2), D_RECIP(3), D_RECIP(4), D_RECIP(5), D_RECIP(6), D_RECIP(7), D_RECIP(8),
	D_RECIP(9), D_RECIP(10), D_RECIP(11), D_RECIP(12), D_RECIP(13), D_RECIP(14), D_RECIP(15), D_RECIP(16)
};

static inline int
D_DivSpan(int x, int n)
{
	if (n == 1)
	{
		return x;
	}
	if (n > 16)
	{
		return x / n;
	}
	if (x >= 0)
	{
		return (int)(((unsigned long long)(unsigned)x * d_span_recip[n]) >> 32);
	}
	return -(int)(((unsigned long long)(unsigned)-x * d_span_recip[n]) >> 32);
}

/*
=============
D_DrawSpansParms

The texture of a list of spans, from what D_CalcGradients and
D_DrawSpanGetStep worked out: on either thread (sw_jobs.c), so nothing here
reads the renderer's globals.
=============
*/
typedef struct
{
	espan_t	*spans;
	const pixel_t	*pbase;
	pixel_t	*viewbuffer;
	int	width, cachewidth;
	float	sdivzorigin, tdivzorigin, ziorigin;
	float	sdivzstepu, tdivzstepu, zistepu;
	float	sdivzstepv, tdivzstepv, zistepv;
	int	sadjust, tadjust, bbextents, bbextentt;
	int	spanstep_shift;
	qboolean	filtering;
} spanparms_t;

static void
D_DrawSpansParms(const spanparms_t *p)
{
	espan_t	*pspan = p->spans;
	int 	spancount;
	const pixel_t	*pbase = p->pbase;
	int	snext, tnext;
	float	spancountminus1;
	float	sdivzpow2stepu, tdivzpow2stepu, zipow2stepu;
	int	spanstep_shift, spanstep_value;
	const int	sadjust = p->sadjust, tadjust = p->tadjust;
	const int	bbextents = p->bbextents, bbextentt = p->bbextentt;

	spanstep_shift = p->spanstep_shift;
	spanstep_value = (1 << spanstep_shift);

	sdivzpow2stepu = p->sdivzstepu * spanstep_value;
	tdivzpow2stepu = p->tdivzstepu * spanstep_value;
	zipow2stepu = p->zistepu * spanstep_value;

	do
	{
		pixel_t	*pdest;
		int	count, s, t;
		float	sdivz, tdivz, zi, z, du, dv;

		pdest = p->viewbuffer + (p->width * pspan->v) + pspan->u;

		count = pspan->count;

		// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = p->sdivzorigin + dv*p->sdivzstepv + du*p->sdivzstepu;
		tdivz = p->tdivzorigin + dv*p->tdivzstepv + du*p->tdivzstepu;
		zi = p->ziorigin + dv*p->zistepv + du*p->zistepu;
		z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
			int	sstep, tstep;

			sstep = 0;	// keep compiler happy
			tstep = 0;	// ditto

			// calculate s and t at the far end of the span
			if (count >= spanstep_value)
				spancount = spanstep_value;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				// calculate s/z, t/z, zi->fixed s and t at far end of span,
				// calculate s and t steps across span by shifting
				sdivz += sdivzpow2stepu;
				tdivz += tdivzpow2stepu;
				zi += zipow2stepu;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				sstep = (snext - s) >> spanstep_shift;
				tstep = (tnext - t) >> spanstep_shift;
			}
			else
			{
				// calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
				// can't step off polygon), clamp, calculate s and t steps across
				// span by division, biasing steps low so we don't run off the
				// texture
				spancountminus1 = (float)(spancount - 1);
				sdivz += p->sdivzstepu * spancountminus1;
				tdivz += p->tdivzstepu * spancountminus1;
				zi += p->zistepu * spancountminus1;
				z = (float)SHIFT16XYZ_MULT / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < spanstep_value)
					// prevent round-off error on <0 steps from
					//  from causing overstepping & running off the
					//  edge of the texture
					snext = spanstep_value;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < spanstep_value)
					// guard against round-off error on <0 steps
					tnext = spanstep_value;

				if (spancount > 1)
				{
					sstep = D_DivSpan(snext - s, spancount - 1);
					tstep = D_DivSpan(tnext - t, spancount - 1);
				}
			}

			// Drawing phrase
			if (!p->filtering)
			{
				pdest = D_DrawSpan(pdest, pbase, p->cachewidth, s, t, sstep, tstep,
						   spancount);
			}
			else
			{
				pdest = D_DrawSpanFiltered(pdest, pbase, p->cachewidth, s, t, sstep, tstep,
						   spancount, pspan);
			}
			s = snext;
			t = tnext;
		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/* The renderer's state for D_DrawSpansParms */
static void
D_GetSpanParms(spanparms_t *p, espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv)
{
	p->spans = pspan;
	p->pbase = (const pixel_t *)cacheblock;
	p->cachewidth = cachewidth;
	p->viewbuffer = d_viewbuffer;
	p->width = vid_buffer_width;
	p->sdivzorigin = d_sdivzorigin;
	p->tdivzorigin = d_tdivzorigin;
	p->ziorigin = d_ziorigin;
	p->sdivzstepu = d_sdivzstepu;
	p->tdivzstepu = d_tdivzstepu;
	p->zistepu = d_zistepu;
	p->sdivzstepv = d_sdivzstepv;
	p->tdivzstepv = d_tdivzstepv;
	p->zistepv = d_zistepv;
	p->sadjust = sadjust;
	p->tadjust = tadjust;
	p->bbextents = bbextents;
	p->bbextentt = bbextentt;
	p->spanstep_shift = D_DrawSpanGetStep(d_zistepu, d_zistepv);
	p->filtering = (int)sw_texture_filtering->value && !fastmoving;
}

/*
=============
D_DrawSpansPow2
=============
*/
void
D_DrawSpansPow2 (espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv)
{
	spanparms_t parms;

	D_GetSpanParms(&parms, pspan, d_ziorigin, d_zistepu, d_zistepv);
	D_DrawSpansParms(&parms);
}

/*
=============
D_MarkZSpans

The part of D_DrawZSpans that keeps track of what was written to the z buffer
(VID_DamageZBuffer): which spans get their z, in the order they come. On this
thread; D_DrawZSpansParms writes them, on either.
=============
*/
static void
D_MarkZSpans (espan_t *pspan)
{
	do
	{
		pspan->zwrite = VID_CheckDamageZBuffer(pspan->u, pspan->v, pspan->count, 0);

		if (pspan->zwrite)
		{
			// solid map walls damage
			VID_DamageZBuffer(pspan->u, pspan->v);
			VID_DamageZBuffer(pspan->u + pspan->count, pspan->v);
		}
	} while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawZSpansParms

D_DrawZSpans for the spans D_MarkZSpans marked
=============
*/
static void
D_DrawZSpansParms (const espan_t *pspan, zvalue_t *zbuffer, int width,
	float d_ziorigin, float d_zistepu, float d_zistepv)
{
	zvalue_t	izistep;
	int		safe_step;

	// FIXME: check for clamping/range problems
	// we count on FP exceptions being turned off to avoid range problems
	izistep = (int)(d_zistepu * 0x8000 * (float)SHIFT16XYZ_MULT);
	safe_step = D_DrawZSpanGetStepValue(izistep);

	do
	{
		int		count;
		zvalue_t	izi;
		zvalue_t	*pdest;
		float		zi;
		float		du, dv;

		if (!pspan->zwrite)
		{
			continue;
		}

		pdest = zbuffer + (width * pspan->v) + pspan->u;

		count = pspan->count;

		// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		// we count on FP exceptions being turned off to avoid range problems
		izi = (int)(zi * 0x8000 * (float)SHIFT16XYZ_MULT);

		if (safe_step > 1)
		{
			const zvalue_t *tdest_max = pdest + count;
			do
			{
				int	step;
				zvalue_t izi_shifted = izi >> SHIFT16XYZ;

				for(step = 0; (step < safe_step) && (pdest < tdest_max); step++)
				{
					*pdest++ = izi_shifted;
				}
				izi += (izistep * safe_step);
			} while (pdest < tdest_max);
		}
		else
		{
			while (count > 0)
			{
				*pdest++ = izi >> SHIFT16XYZ;
				izi += izistep;
				count--;
			}
		}
	} while ((pspan = pspan->pnext) != NULL);
}

typedef struct
{
	spanparms_t	spans;
	zvalue_t	*zbuffer;
	qboolean	build;
	drawsurf_t	drawsurf;
} solidjob_t;

_Static_assert(sizeof(solidjob_t) <= RJOB_ARG_SIZE, "solidjob_t does not fit a job");

static void
D_SolidSpansJob(const void *arg)
{
	solidjob_t job = *(const solidjob_t *)arg;

	if (job.build)
	{
		D_BuildSurface(&job.drawsurf);
	}
	D_DrawSpansParms(&job.spans);
	D_DrawZSpansParms(job.spans.spans, job.zbuffer, job.spans.width,
		job.spans.ziorigin, job.spans.zistepu, job.spans.zistepv);
}

/*
=============
D_DrawSolidSpans

D_DrawSpansPow2 and D_DrawZSpans as a job (sw_jobs.c), with the drawing of
the surface's cache block first if build is not NULL: between R_JobsBegin and R_JobsFinish
this is done at some time before those return, on either thread. The spans
and the surface's cache block must stay as they are until then.
=============
*/
#ifdef MISTER_HYBRID
/*
 * MISTER_HYBRID_SPANCHECK=1, a test: once the jobs are done, before anything
 * changes a cache block (D_CacheSurface) and after the last of a
 * D_DrawSurfaces, every surface is drawn again on this thread into a buffer
 * of its own and compared with what the jobs drew. That would differ if a
 * surface's cache block had changed before its job was done with it. D_DivSpan is compared with the
 * division once, at the start.
 */
#define CHECK_MAX 4096
static int check_on = -1;
static spanparms_t check_list[CHECK_MAX];
static int check_count;
static pixel_t *check_buffer;
static zvalue_t *check_zbuffer;
static int check_size;
static unsigned long long check_pixels, check_wrong, check_surfaces;

static void
D_CheckDivSpan(void)
{
	unsigned long long wrong = 0, n = 0;
	int d, x;

	for (d = 1; d <= 16; d++)
	{
		for (x = -(1 << 28) + 1; x < (1 << 28); x += 997)
		{
			wrong += D_DivSpan(x, d) != x / d;
			wrong += D_DivSpan(x + d - 1, d) != (x + d - 1) / d;
			n += 2;
		}
	}
	Com_Printf("spancheck: D_DivSpan %llu of %llu quotients wrong\n", wrong, n);
}

void
D_CheckSolidSpans(qboolean report)
{
	int i;

	if (check_on <= 0)
	{
		return;
	}

	if (check_size != vid_buffer_width * vid_buffer_height)
	{
		check_size = vid_buffer_width * vid_buffer_height;
		free(check_buffer);
		free(check_zbuffer);
		check_buffer = malloc(check_size);
		check_zbuffer = malloc(check_size * sizeof(zvalue_t));
	}

	for (i = 0; i < check_count; i++)
	{
		spanparms_t p = check_list[i];
		const espan_t *span;

		p.viewbuffer = check_buffer;
		D_DrawSpansParms(&p);
		D_DrawZSpansParms(p.spans, check_zbuffer, p.width, p.ziorigin, p.zistepu, p.zistepv);

		for (span = p.spans; span; span = span->pnext)
		{
			int o = span->v * p.width + span->u, k;

			for (k = 0; k < span->count; k++)
			{
				check_wrong += check_buffer[o + k] != check_list[i].viewbuffer[o + k];
				if (span->zwrite)
				{
					check_wrong += check_zbuffer[o + k] != d_pzbuffer[o + k];
				}
			}
			check_pixels += span->count;
		}
	}

	check_surfaces += check_count;
	check_count = 0;

	if (report && (r_framecount & 255) == 0)
	{
		Com_Printf("spancheck: %llu surfaces, %llu pixels, %llu wrong\n",
			check_surfaces, check_pixels, check_wrong);
	}
}
#endif

void
D_DrawSolidSpans (espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv,
	const drawsurf_t *build)
{
	solidjob_t job;
	spanparms_t parms;

	D_GetSpanParms(&parms, pspan, d_ziorigin, d_zistepu, d_zistepv);
#ifdef MISTER_HYBRID
	if (check_on < 0)
	{
		check_on = getenv("MISTER_HYBRID_SPANCHECK") && atoi(getenv("MISTER_HYBRID_SPANCHECK"));
		if (check_on)
		{
			D_CheckDivSpan();
		}
	}
	if (check_on > 0 && check_count < CHECK_MAX)
	{
		check_list[check_count++] = parms;
	}
#endif
	// which spans write their z is worked out here, in order
	D_MarkZSpans(pspan);

	job.spans = parms;
	job.zbuffer = d_pzbuffer;
	job.build = build != NULL;
	if (build)
	{
		job.drawsurf = *build;
	}
	R_JobAdd(D_SolidSpansJob, &job, sizeof(job));
}

/*
=============
D_DrawZSpans
=============
*/
void
D_DrawZSpans (espan_t *pspan, float d_ziorigin, float d_zistepu, float d_zistepv)
{
	zvalue_t	izistep;
	int		safe_step;

	// FIXME: check for clamping/range problems
	// we count on FP exceptions being turned off to avoid range problems
	izistep = (int)(d_zistepu * 0x8000 * (float)SHIFT16XYZ_MULT);
	safe_step = D_DrawZSpanGetStepValue(izistep);

	do
	{
		int		count;
		zvalue_t	izi;
		zvalue_t	*pdest;
		float		zi;
		float		du, dv;

		if (!VID_CheckDamageZBuffer(pspan->u, pspan->v, pspan->count, 0))
		{
			continue;
		}

		// solid map walls damage
		VID_DamageZBuffer(pspan->u, pspan->v);
		VID_DamageZBuffer(pspan->u + pspan->count, pspan->v);

		pdest = d_pzbuffer + (vid_buffer_width * pspan->v) + pspan->u;

		count = pspan->count;

		// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		// we count on FP exceptions being turned off to avoid range problems
		izi = (int)(zi * 0x8000 * (float)SHIFT16XYZ_MULT);

		if (safe_step > 1)
		{
			const zvalue_t *tdest_max = pdest + count;
			do
			{
				int	step;
				zvalue_t izi_shifted = izi >> SHIFT16XYZ;

				for(step = 0; (step < safe_step) && (pdest < tdest_max); step++)
				{
					*pdest++ = izi_shifted;
				}
				izi += (izistep * safe_step);
			} while (pdest < tdest_max);
		}
		else
		{
			while (count > 0)
			{
				*pdest++ = izi >> SHIFT16XYZ;
				izi += izistep;
				count--;
			}
		}
	} while ((pspan = pspan->pnext) != NULL);
}
