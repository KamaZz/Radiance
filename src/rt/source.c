/* Copyright (c) 1990 Regents of the University of California */

#ifndef lint
static char SCCSid[] = "$SunId$ LBL";
#endif

/*
 *  source.c - routines dealing with illumination sources.
 *
 *     8/20/85
 */

#include  "ray.h"

#include  "octree.h"

#include  "source.h"

#include  "otypes.h"

#include  "cone.h"

#include  "face.h"

#include  "random.h"


extern double  dstrsrc;			/* source distribution amount */
extern double  shadthresh;		/* relative shadow threshold */
extern double  shadcert;		/* shadow testing certainty */

SRCREC  *source = NULL;			/* our list of sources */
int  nsources = 0;			/* the number of sources */

static CONTRIB  *srccnt;		/* source contributions in direct() */
static CNTPTR  *cntord;			/* source ordering in direct() */


marksources()			/* find and mark source objects */
{
	register OBJREC  *o, *m;
	register int  i;

	for (i = 0; i < nobjects; i++) {
	
		o = objptr(i);

		if (o->omod == OVOID)
			continue;

		m = objptr(o->omod);

		if (!islight(m->otype))
			continue;
	
		if (m->oargs.nfargs != (m->otype == MAT_GLOW ? 4 :
				m->otype == MAT_SPOT ? 7 : 3))
			objerror(m, USER, "bad # arguments");

		if (m->otype == MAT_GLOW &&
				o->otype != OBJ_SOURCE &&
				m->oargs.farg[3] <= FTINY)
			continue;			/* don't bother */

		if (source == NULL)
			source = (SRCREC *)malloc(sizeof(SRCREC));
		else
			source = (SRCREC *)realloc((char *)source,
					(unsigned)(nsources+1)*sizeof(SRCREC));
		if (source == NULL)
			goto memerr;

		newsource(&source[nsources], o);

		if (m->otype == MAT_GLOW) {
			source[nsources].sflags |= SPROX;
			source[nsources].sl.prox = m->oargs.farg[3];
			if (o->otype == OBJ_SOURCE)
				source[nsources].sflags |= SSKIP;
		} else if (m->otype == MAT_SPOT) {
			source[nsources].sflags |= SSPOT;
			source[nsources].sl.s = makespot(m);
		}
		nsources++;
	}
	if (nsources <= 0) {
		error(WARNING, "no light sources found");
		return;
	}
	srccnt = (CONTRIB *)malloc(nsources*sizeof(CONTRIB));
	cntord = (CNTPTR *)malloc(nsources*sizeof(CNTPTR));
	if (srccnt != NULL && cntord != NULL)
		return;
	/* fall through */
memerr:
	error(SYSTEM, "out of memory in marksources");
}


newsource(src, so)			/* add a source to the array */
register SRCREC  *src;
register OBJREC  *so;
{
	double  cos(), tan(), sqrt();
	double  theta;
	FACE  *f;
	CONE  *co;
	int  j;
	register int  i;
	
	src->sflags = 0;
	src->nhits = 1; src->ntests = 2;	/* start probability = 1/2 */
	src->so = so;

	switch (so->otype) {
	case OBJ_SOURCE:
		if (so->oargs.nfargs != 4)
			objerror(so, USER, "bad arguments");
		src->sflags |= SDISTANT;
		VCOPY(src->sloc, so->oargs.farg);
		if (normalize(src->sloc) == 0.0)
			objerror(so, USER, "zero direction");
		theta = PI/180.0/2.0 * so->oargs.farg[3];
		if (theta <= FTINY)
			objerror(so, USER, "zero size");
		src->ss = theta >= PI/4 ? 1.0 : tan(theta);
		src->ss2 = 2.0*PI * (1.0 - cos(theta));
		break;
	case OBJ_SPHERE:
		VCOPY(src->sloc, so->oargs.farg);
		src->ss = so->oargs.farg[3];
		src->ss2 = PI * src->ss * src->ss;
		break;
	case OBJ_FACE:
						/* get the face */
		f = getface(so);
						/* find the center */
		for (j = 0; j < 3; j++) {
			src->sloc[j] = 0.0;
			for (i = 0; i < f->nv; i++)
				src->sloc[j] += VERTEX(f,i)[j];
			src->sloc[j] /= f->nv;
		}
		if (!inface(src->sloc, f))
			objerror(so, USER, "cannot hit center");
		src->ss = sqrt(f->area / PI);
		src->ss2 = f->area;
		break;
	case OBJ_RING:
						/* get the ring */
		co = getcone(so, 0);
		VCOPY(src->sloc, CO_P0(co));
		if (CO_R0(co) > 0.0)
			objerror(so, USER, "cannot hit center");
		src->ss = CO_R1(co);
		src->ss2 = PI * src->ss * src->ss;
		break;
	default:
		objerror(so, USER, "illegal material");
	}
}


SPOT *
makespot(m)			/* make a spotlight */
register OBJREC  *m;
{
	extern double  cos();
	register SPOT  *ns;

	if ((ns = (SPOT *)malloc(sizeof(SPOT))) == NULL)
		error(SYSTEM, "out of memory in makespot");
	ns->siz = 2.0*PI * (1.0 - cos(PI/180.0/2.0 * m->oargs.farg[3]));
	VCOPY(ns->aim, m->oargs.farg+4);
	if ((ns->flen = normalize(ns->aim)) == 0.0)
		objerror(m, USER, "zero focus vector");
	return(ns);
}


double
srcray(sr, r, sn)		/* send a ray to a source, return domega */
register RAY  *sr;		/* returned source ray */
RAY  *r;			/* ray which hit object */
register int  sn;		/* source number */
{
	register double  *norm = NULL;	/* plane normal */
	double  ddot;			/* (distance times) cosine */
	FVECT  vd;
	double  d;
	register int  i;

	if (source[sn].sflags & SSKIP)
		return(0.0);			/* skip this source */

	rayorigin(sr, r, SHADOW, 1.0);		/* ignore limits */

	sr->rsrc = sn;				/* remember source */
						/* get source direction */
	if (source[sn].sflags & SDISTANT)
						/* constant direction */
		VCOPY(sr->rdir, source[sn].sloc);
	else {					/* compute direction */
		for (i = 0; i < 3; i++)
			sr->rdir[i] = source[sn].sloc[i] - sr->rorg[i];

		if (source[sn].so->otype == OBJ_FACE)
 			norm = getface(source[sn].so)->norm;
		else if (source[sn].so->otype == OBJ_RING)
			norm = getcone(source[sn].so,0)->ad;

		if (norm != NULL && (ddot = -DOT(sr->rdir, norm)) <= FTINY)
			return(0.0);		/* behind surface! */
	}
	if (dstrsrc > FTINY) {
					/* distribute source direction */
		for (i = 0; i < 3; i++)
			vd[i] = dstrsrc * source[sn].ss * (1.0 - 2.0*frandom());

		if (norm != NULL) {		/* project offset */
			d = DOT(vd, norm);
			for (i = 0; i < 3; i++)
				vd[i] -= d * norm[i];
		}
		for (i = 0; i < 3; i++)		/* offset source direction */
			sr->rdir[i] += vd[i];

	} else if (source[sn].sflags & SDISTANT)
						/* already normalized */
		return(source[sn].ss2);

	if ((d = normalize(sr->rdir)) == 0.0)
						/* at source! */
		return(0.0);
	
	if (source[sn].sflags & SDISTANT)
						/* domega constant */
		return(source[sn].ss2);

						/* check proximity */
	if (source[sn].sflags & SPROX &&
			d > source[sn].sl.prox)
		return(0.0);
						/* compute dot product */
	if (norm != NULL)
		ddot /= d;
	else
		ddot = 1.0;
						/* check angle */
	if (source[sn].sflags & SSPOT) {
		if (source[sn].sl.s->siz < 2.0*PI *
				(1.0 + DOT(source[sn].sl.s->aim,sr->rdir)))
			return(0.0);
		d += source[sn].sl.s->flen;	/* adjust length */
	}
						/* compute domega */
	return(ddot*source[sn].ss2/(d*d));
}


sourcehit(r)			/* check to see if ray hit distant source */
register RAY  *r;
{
	int  first, last;
	register int  i;

	if (r->rsrc >= 0) {		/* check only one if aimed */
		first = last = r->rsrc;
	} else {			/* otherwise check all */
		first = 0; last = nsources-1;
	}
	for (i = first; i <= last; i++)
		if (source[i].sflags & SDISTANT)
			/*
			 * Check to see if ray is within
			 * solid angle of source.
			 */
			if (2.0*PI * (1.0 - DOT(source[i].sloc,r->rdir))
					<= source[i].ss2) {
				r->ro = source[i].so;
				if (!(source[i].sflags & SSKIP))
					break;
			}

	if (r->ro != NULL) {
		for (i = 0; i < 3; i++)
			r->ron[i] = -r->rdir[i];
		r->rod = 1.0;
		r->rox = NULL;
		return(1);
	}
	return(0);
}


static int
cntcmp(sc1, sc2)			/* contribution compare (descending) */
register CNTPTR  *sc1, *sc2;
{
	if (sc1->brt > sc2->brt)
		return(-1);
	if (sc1->brt < sc2->brt)
		return(1);
	return(0);
}


direct(r, f, p)				/* add direct component */
RAY  *r;			/* ray that hit surface */
int  (*f)();			/* direct component coefficient function */
char  *p;			/* data for f */
{
	extern double  pow();
	register int  sn;
	int  nshadcheck, ncnts;
	double  prob, ourthresh, hwt, test2, hit2;
	RAY  sr;
			/* NOTE: srccnt and cntord global so no recursion */
	if (nsources <= 0)
		return;		/* no sources?! */
						/* compute number to check */
	nshadcheck = pow((double)nsources, shadcert) + .5;
						/* modify threshold */
	ourthresh = shadthresh / r->rweight;
						/* potential contributions */
	for (sn = 0; sn < nsources; sn++) {
		cntord[sn].sno = sn;
		cntord[sn].brt = 0.0;
						/* get source ray */
		if ((srccnt[sn].dom = srcray(&sr, r, sn)) == 0.0)
			continue;
		VCOPY(srccnt[sn].dir, sr.rdir);
						/* compute coefficient */
		(*f)(srccnt[sn].val, p, srccnt[sn].dir, srccnt[sn].dom);
		cntord[sn].brt = bright(srccnt[sn].val);
		if (cntord[sn].brt <= 0.0)
			continue;
						/* compute intersection */
		if (!( source[sn].sflags & SDISTANT ?
				sourcehit(&sr) :
				(*ofun[source[sn].so->otype].funp)
				(source[sn].so, &sr) )) {
			sprintf(errmsg,
				"aiming failure for light source \"%s\"",
					source[sn].so->oname);
			error(WARNING, errmsg);
			continue;
		}
						/* compute contribution */
		raycont(&sr);
		multcolor(srccnt[sn].val, sr.rcol);
		cntord[sn].brt = bright(srccnt[sn].val);
	}
						/* sort contributions */
	qsort(cntord, nsources, sizeof(CNTPTR), cntcmp);
	{					/* find last */
		register int  l, m;

		sn = 0; ncnts = l = nsources;
		while ((m = (sn + ncnts) >> 1) != l) {
			if (cntord[m].brt > 0.0)
				sn = m;
			else
				ncnts = m;
			l = m;
		}
	}
                                                /* accumulate tail */
        for (sn = ncnts-1; sn > 0; sn--)
                cntord[sn-1].brt += cntord[sn].brt;
						/* start with prob=.5 */
	hit2 = 0.5; test2 = 1.0;
						/* test for shadows */
	for (sn = 0; sn < ncnts; sn++) {
						/* check threshold */
		if ((sn+nshadcheck>=ncnts ? cntord[sn].brt :
				cntord[sn].brt-cntord[sn+nshadcheck].brt)
				< ourthresh*bright(r->rcol))
			break;
						/* get statistics */
		hwt = (double)source[cntord[sn].sno].nhits /
				(double)source[cntord[sn].sno].ntests;
		test2 += hwt;
		source[cntord[sn].sno].ntests++;
						/* test for hit */
		rayorigin(&sr, r, SHADOW, 1.0);
		VCOPY(sr.rdir, srccnt[cntord[sn].sno].dir);
		sr.rsrc = cntord[sn].sno;
		if (localhit(&sr, &thescene) &&
				sr.ro != source[cntord[sn].sno].so) {
						/* check for transmission */
			raycont(&sr);
			if (bright(sr.rcol) <= FTINY)
				continue;	/* missed! */
			(*f)(srccnt[cntord[sn].sno].val, p,
					srccnt[cntord[sn].sno].dir,
					srccnt[cntord[sn].sno].dom);
			multcolor(srccnt[cntord[sn].sno].val, sr.rcol);
		}
						/* add contribution if hit */
		addcolor(r->rcol, srccnt[cntord[sn].sno].val);
		hit2 += hwt;
		source[cntord[sn].sno].nhits++;
	}
					/* weighted hit rate */
	hwt = hit2 / test2;
#ifdef DEBUG
	sprintf(errmsg, "%d tested, %d untested, %f hit rate\n",
			sn, ncnts-sn, hwt);
	eputs(errmsg);
#endif
					/* add in untested sources */
	for ( ; sn < ncnts; sn++) {
		prob = hwt * (double)source[cntord[sn].sno].nhits /
				(double)source[cntord[sn].sno].ntests;
		scalecolor(srccnt[cntord[sn].sno].val, prob);
		addcolor(r->rcol, srccnt[cntord[sn].sno].val);
	}
}


#define  wrongsource(m, r)	(m->otype!=MAT_ILLUM && \
				r->rsrc>=0 && \
				source[r->rsrc].so!=r->ro)

#define  badambient(m, r)	((r->crtype&(AMBIENT|SHADOW))==AMBIENT && \
				!(r->rtype&REFLECTED) && 	/* hack! */\
				!(m->otype==MAT_GLOW&&r->rot>m->oargs.farg[3]))

#define  passillum(m, r)	(m->otype==MAT_ILLUM && \
				!(r->rsrc>=0&&source[r->rsrc].so==r->ro))


m_light(m, r)			/* ray hit a light source */
register OBJREC  *m;
register RAY  *r;
{
						/* check for over-counting */
	if (wrongsource(m, r) || badambient(m, r))
		return;
						/* check for passed illum */
	if (passillum(m, r)) {

		if (m->oargs.nsargs < 1 || !strcmp(m->oargs.sarg[0], VOIDID))
			raytrans(r);
		else
			rayshade(r, modifier(m->oargs.sarg[0]));

						/* otherwise treat as source */
	} else {
						/* check for behind */
		if (r->rod < 0.0)
			return;
						/* get distribution pattern */
		raytexture(r, m->omod);
						/* get source color */
		setcolor(r->rcol, m->oargs.farg[0],
				  m->oargs.farg[1],
				  m->oargs.farg[2]);
						/* modify value */
		multcolor(r->rcol, r->pcol);
						/* assign distance */
		r->rt = r->rot;
	}
}
