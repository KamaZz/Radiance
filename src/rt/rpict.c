/* Copyright (c) 1986 Regents of the University of California */

#ifndef lint
static char SCCSid[] = "$SunId$ LBL";
#endif

/*
 *  rpict.c - routines and variables for picture generation.
 *
 *     8/14/85
 */

#include  "ray.h"

#ifdef BSD
#include  <sys/time.h>
#include  <sys/resource.h>
#endif

#include  "view.h"

#include  "random.h"

VIEW  ourview = STDVIEW(512);		/* view parameters */

int  psample = 4;			/* pixel sample size */
double  maxdiff = .05;			/* max. difference for interpolation */
double  dstrpix = 0.67;			/* square pixel distribution */

double  dstrsrc = 0.0;			/* square source distribution */
double  shadthresh = .05;		/* shadow threshold */
double  shadcert = .5;			/* shadow certainty */

int  maxdepth = 6;			/* maximum recursion depth */
double  minweight = 5e-3;		/* minimum ray weight */

COLOR  ambval = BLKCOLOR;		/* ambient value */
double  ambacc = 0.2;			/* ambient accuracy */
int  ambres = 32;			/* ambient resolution */
int  ambdiv = 128;			/* ambient divisions */
int  ambssamp = 0;			/* ambient super-samples */
int  ambounce = 0;			/* ambient bounces */
char  *amblist[128];			/* ambient include/exclude list */
int  ambincl = -1;			/* include == 1, exclude == 0 */

int  ralrm = 0;				/* seconds between reports */

double  pctdone = 0.0;			/* percentage done */

extern long  nrays;			/* number of rays traced */

#define  MAXDIV		32		/* maximum sample size */

#define  pixjitter()	(.5+dstrpix*(.5-frandom()))

double  pixvalue();


quit(code)			/* quit program */
int  code;
{
	if (code || ralrm > 0)		/* report status */
		report();

	exit(code);
}


report()		/* report progress */
{
#ifdef BSD
	struct rusage  rubuf;
	double  t;

	getrusage(RUSAGE_SELF, &rubuf);
	t = (rubuf.ru_utime.tv_usec + rubuf.ru_stime.tv_usec) / 1e6;
	t += rubuf.ru_utime.tv_sec + rubuf.ru_stime.tv_sec;
	getrusage(RUSAGE_CHILDREN, &rubuf);
	t += (rubuf.ru_utime.tv_usec + rubuf.ru_stime.tv_usec) / 1e6;
	t += rubuf.ru_utime.tv_sec + rubuf.ru_stime.tv_sec;

	sprintf(errmsg, "%ld rays, %4.2f%% done after %5.4f CPU hours\n",
			nrays, pctdone, t/3600.0);
#else
	sprintf(errmsg, "%ld rays, %4.2f%% done\n", nrays, pctdone);
#endif
	eputs(errmsg);

	if (ralrm > 0)
		alarm(ralrm);
}


render(zfile, oldfile)				/* render the scene */
char  *zfile, *oldfile;
{
	COLOR  *scanbar[MAXDIV+1];	/* scanline arrays of pixel values */
	float  *zbar[MAXDIV+1];		/* z values */
	int  ypos;			/* current scanline */
	FILE  *zfp;
	COLOR  *colptr;
	float  *zptr;
	register int  i;
					/* check sampling */
	if (psample < 1)
		psample = 1;
	else if (psample > MAXDIV)
		psample = MAXDIV;
					/* allocate scanlines */
	for (i = 0; i <= psample; i++) {
		scanbar[i] = (COLOR *)malloc(ourview.hresolu*sizeof(COLOR));
		if (scanbar[i] == NULL)
			goto memerr;
	}
					/* open z file */
	if (zfile != NULL) {
		if ((zfp = fopen(zfile, "a+")) == NULL) {
			sprintf(errmsg, "cannot open z file \"%s\"", zfile);
			error(SYSTEM, errmsg);
		}
		for (i = 0; i <= psample; i++) {
			zbar[i] = (float *)malloc(ourview.hresolu*sizeof(float));
			if (zbar[i] == NULL)
				goto memerr;
		}
	} else {
		zfp = NULL;
		for (i = 0; i <= psample; i++)
			zbar[i] = NULL;
	}
					/* write out boundaries */
	fputresolu(YMAJOR|YDECR, ourview.hresolu, ourview.vresolu, stdout);
					/* recover file and compute first */
	i = salvage(oldfile);
	if (zfp != NULL && fseek(zfp, (long)i*ourview.hresolu*sizeof(float), 0) == EOF)
		error(SYSTEM, "z file seek error in render");
	ypos = ourview.vresolu-1 - i;
	fillscanline(scanbar[0], zbar[0], ourview.hresolu, ypos, psample);
						/* compute scanlines */
	for (ypos -= psample; ypos >= 0; ypos -= psample) {
	
		pctdone = 100.0*(ourview.vresolu-ypos-psample)/ourview.vresolu;

		colptr = scanbar[psample];		/* move base to top */
		scanbar[psample] = scanbar[0];
		scanbar[0] = colptr;
		zptr = zbar[psample];
		zbar[psample] = zbar[0];
		zbar[0] = zptr;
							/* fill base line */
		fillscanline(scanbar[0], zbar[0], ourview.hresolu, ypos, psample);
							/* fill bar */
		fillscanbar(scanbar, zbar, ourview.hresolu, ypos, psample);
							/* write it out */
		for (i = psample; i > 0; i--) {
			if (zfp != NULL && fwrite(zbar[i],sizeof(float),ourview.hresolu,zfp) != ourview.hresolu)
				goto writerr;
			if (fwritescan(scanbar[i],ourview.hresolu,stdout) < 0)
				goto writerr;
		}
		if (zfp != NULL && fflush(zfp) == EOF)
			goto writerr;
		if (fflush(stdout) == EOF)
			goto writerr;
	}
						/* compute residual */
	colptr = scanbar[psample];
	scanbar[psample] = scanbar[0];
	scanbar[0] = colptr;
	zptr = zbar[psample];
	zbar[psample] = zbar[0];
	zbar[0] = zptr;
	if (ypos > -psample) {
		fillscanline(scanbar[-ypos], zbar[-ypos], ourview.hresolu,
				0, psample);
		fillscanbar(scanbar-ypos, zbar-ypos, ourview.hresolu,
				0, psample+ypos);
	}
	for (i = psample; i+ypos >= 0; i--) {
		if (zfp != NULL && fwrite(zbar[i],sizeof(float),ourview.hresolu,zfp) != ourview.hresolu)
			goto writerr;
		if (fwritescan(scanbar[i], ourview.hresolu, stdout) < 0)
			goto writerr;
	}
						/* clean up */
	if (zfp != NULL) {
		if (fclose(zfp) == EOF)
			goto writerr;
		for (i = 0; i <= psample; i++)
			free((char *)zbar[i]);
	}
	if (fflush(stdout) == EOF)
		goto writerr;
	for (i = 0; i <= psample; i++)
		free((char *)scanbar[i]);
	pctdone = 100.0;
	return;
writerr:
	error(SYSTEM, "write error in render");
memerr:
	error(SYSTEM, "out of memory in render");
}


fillscanline(scanline, zline, xres, y, xstep)	/* fill scan line at y */
register COLOR  *scanline;
register float  *zline;
int  xres, y, xstep;
{
	int  b = xstep;
	double  z;
	register int  i;
	
	z = pixvalue(scanline[0], 0, y);
	if (zline) zline[0] = z;

	for (i = xstep; i < xres; i += xstep) {
	
		z = pixvalue(scanline[i], i, y);
		if (zline) zline[i] = z;
		
		b = fillsample(scanline+i-xstep, zline ? zline+i-xstep : NULL,
				i-xstep, y, xstep, 0, b/2);
	}
	if (i-xstep < xres-1) {
		z = pixvalue(scanline[xres-1], xres-1, y);
		if (zline) zline[xres-1] = z;
		fillsample(scanline+i-xstep, zline ? zline+i-xstep : NULL,
				i-xstep, y, xres-1-(i-xstep), 0, b/2);
	}
}


fillscanbar(scanbar, zbar, xres, y, ysize)	/* fill interior */
register COLOR  *scanbar[];
register float  *zbar[];
int  xres, y, ysize;
{
	COLOR  vline[MAXDIV+1];
	float  zline[MAXDIV+1];
	int  b = ysize;
	double  z;
	register int  i, j;
	
	for (i = 0; i < xres; i++) {
		
		copycolor(vline[0], scanbar[0][i]);
		copycolor(vline[ysize], scanbar[ysize][i]);
		if (zbar[0]) {
			zline[0] = zbar[0][i];
			zline[ysize] = zbar[ysize][i];
		}
		
		b = fillsample(vline, zbar[0] ? zline : NULL,
				i, y, 0, ysize, b/2);
		
		for (j = 1; j < ysize; j++)
			copycolor(scanbar[j][i], vline[j]);
		if (zbar[0])
			for (j = 1; j < ysize; j++)
				zbar[j][i] = zline[j];
	}
}


int
fillsample(colline, zline, x, y, xlen, ylen, b)	/* fill interior points */
register COLOR  *colline;
register float  *zline;
int  x, y;
int  xlen, ylen;
int  b;
{
	extern double  fabs();
	double  ratio;
	double  z;
	COLOR  ctmp;
	int  ncut;
	register int  len;
	
	if (xlen > 0)			/* x or y length is zero */
		len = xlen;
	else
		len = ylen;
		
	if (len <= 1)			/* limit recursion */
		return(0);
	
	if (b > 0
	|| (zline && 2.*fabs(zline[0]-zline[len]) > maxdiff*(zline[0]+zline[len]))
			|| bigdiff(colline[0], colline[len], maxdiff)) {
	
		z = pixvalue(colline[len>>1], x + (xlen>>1), y + (ylen>>1));
		if (zline) zline[len>>1] = z;
		ncut = 1;
		
	} else {					/* interpolate */
	
		copycolor(colline[len>>1], colline[len]);
		ratio = (double)(len>>1) / len;
		scalecolor(colline[len>>1], ratio);
		if (zline) zline[len>>1] = zline[len] * ratio;
		ratio = 1.0 - ratio;
		copycolor(ctmp, colline[0]);
		scalecolor(ctmp, ratio);
		addcolor(colline[len>>1], ctmp);
		if (zline) zline[len>>1] += zline[0] * ratio;
		ncut = 0;
	}
							/* recurse */
	ncut += fillsample(colline, zline, x, y, xlen>>1, ylen>>1, (b-1)/2);
	
	ncut += fillsample(colline+(len>>1), zline ? zline+(len>>1) : NULL,
			x+(xlen>>1), y+(ylen>>1),
			xlen-(xlen>>1), ylen-(ylen>>1), b/2);

	return(ncut);
}


double
pixvalue(col, x, y)		/* compute pixel value */
COLOR  col;			/* returned color */
int  x, y;			/* pixel position */
{
	static RAY  thisray;	/* our ray for this pixel */

	rayview(thisray.rorg, thisray.rdir, &ourview,
			x + pixjitter(), y + pixjitter());

	rayorigin(&thisray, NULL, PRIMARY, 1.0);
	
	rayvalue(&thisray);			/* trace ray */

	copycolor(col, thisray.rcol);		/* return color */
	
	return(thisray.rot);			/* return distance */
}


int
salvage(oldfile)		/* salvage scanlines from killed program */
char  *oldfile;
{
	COLR  *scanline;
	FILE  *fp;
	int  x, y;

	if (oldfile == NULL)
		return(0);
	
	if ((fp = fopen(oldfile, "r")) == NULL) {
		sprintf(errmsg, "cannot open recover file \"%s\"", oldfile);
		error(WARNING, errmsg);
		return(0);
	}
				/* discard header */
	getheader(fp, NULL);
				/* get picture size */
	if (fgetresolu(&x, &y, fp) != (YMAJOR|YDECR)) {
		sprintf(errmsg, "bad recover file \"%s\"", oldfile);
		error(WARNING, errmsg);
		fclose(fp);
		return(0);
	}

	if (x != ourview.hresolu || y != ourview.vresolu) {
		sprintf(errmsg, "resolution mismatch in recover file \"%s\"",
				oldfile);
		error(USER, errmsg);
	}

	scanline = (COLR *)malloc(ourview.hresolu*sizeof(COLR));
	if (scanline == NULL)
		error(SYSTEM, "out of memory in salvage");
	for (y = 0; y < ourview.vresolu; y++) {
		if (freadcolrs(scanline, ourview.hresolu, fp) < 0)
			break;
		if (fwritecolrs(scanline, ourview.hresolu, stdout) < 0)
			goto writerr;
	}
	if (fflush(stdout) == EOF)
		goto writerr;
	free((char *)scanline);
	fclose(fp);
	unlink(oldfile);
	return(y);
writerr:
	error(SYSTEM, "write error in salvage");
}
