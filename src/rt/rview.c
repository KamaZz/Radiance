/* Copyright (c) 1987 Regents of the University of California */

#ifndef lint
static char SCCSid[] = "$SunId$ LBL";
#endif

/*
 *  rview.c - routines and variables for interactive view generation.
 *
 *     3/24/87
 */

#include  "standard.h"

#include  "color.h"

#include  "rpaint.h"

#include  <signal.h>

#include  <ctype.h>

VIEW  ourview = STDVIEW(470);		/* viewing parameters */

int  psample = 8;			/* pixel sample size */
double  maxdiff = .15;			/* max. sample difference */

double  exposure = 1.0;			/* exposure for scene */

double  dstrsrc = 0.0;			/* square source distribution */
double  shadthresh = .1;		/* shadow threshold */
double  shadcert = .25;			/* shadow certainty */

int  maxdepth = 4;			/* maximum recursion depth */
double  minweight = 1e-2;		/* minimum ray weight */

COLOR  ambval = BLKCOLOR;		/* ambient value */
double  ambacc = 0.2;			/* ambient accuracy */
int  ambres = 8;			/* ambient resolution */
int  ambdiv = 32;			/* ambient divisions */
int  ambssamp = 0;			/* ambient super-samples */
int  ambounce = 0;			/* ambient bounces */
char  *amblist[128];			/* ambient include/exclude list */
int  ambincl = -1;			/* include == 1, exclude == 0 */

int  greyscale = 0;			/* map colors to brightness? */
char  *devname = "X";			/* output device name */

struct driver  *dev = NULL;		/* driver functions */

VIEW  oldview;				/* previous view parameters */

PNODE  ptrunk;				/* the base of our image */
RECT  pframe;				/* current frame boundaries */
int  pdepth;				/* image depth in current frame */

#define  CTRL(c)	('c'-'@')


quit(code)			/* quit program */
int  code;
{
	devclose();
	exit(code);
}


devopen(dname)				/* open device driver */
char  *dname;
{
	extern char  *progname, *octname;
	char  *id;
	register int  i;

	id = octname!=NULL ? octname : progname;
						/* check device table */
	for (i = 0; devtable[i].name; i++)
		if (!strcmp(dname, devtable[i].name))
			if ((dev = (*devtable[i].init)(dname, id)) == NULL) {
				sprintf(errmsg, "cannot initialize %s", dname);
				error(USER, errmsg);
			} else
				return;
						/* not there, try exec */
	if ((dev = comm_init(dname, id)) == NULL) {
		sprintf(errmsg, "cannot start device \"%s\"", dname);
		error(USER, errmsg);
	}
}


devclose()				/* close our device */
{
	if (dev != NULL)
		(*dev->close)();
	dev = NULL;
}


printdevices()				/* print list of output devices */
{
	register int  i;

	for (i = 0; devtable[i].name; i++)
		printf("%-16s # %s\n", devtable[i].name, devtable[i].descrip);
}


rview()				/* do a view */
{
	char  buf[32];

	devopen(devname);		/* open device */
	newimage();			/* set up image */

	for ( ; ; ) {			/* quit in command() */
		while (ourview.hresolu <= 1<<pdepth &&
				ourview.vresolu <= 1<<pdepth)
			command("done: ");

		if (ourview.hresolu <= psample<<pdepth &&
				ourview.vresolu <= psample<<pdepth) {
			sprintf(buf, "%d sampling...\n", 1<<pdepth);
			(*dev->comout)(buf);
			rsample();
		} else {
			sprintf(buf, "%d refining...\n", 1<<pdepth);
			(*dev->comout)(buf);
			refine(&ptrunk, 0, 0, ourview.hresolu,
					ourview.vresolu, pdepth+1);
		}
		if (dev->inpready)
			command(": ");
		else
			pdepth++;
	}
}


command(prompt)			/* get/execute command */
char  *prompt;
{
#define  badcom(s)	strncmp(s, inpbuf, args-inpbuf-1)
	double  atof();
	char  inpbuf[256];
	char  *args;
again:
	(*dev->comout)(prompt);			/* get command + arguments */
	(*dev->comin)(inpbuf);
	for (args = inpbuf; *args && *args != ' '; args++)
		;
	if (*args) *args++ = '\0';
	else *++args = '\0';
	
	switch (inpbuf[0]) {
	case 'f':				/* new frame */
		if (badcom("frame"))
			goto commerr;
		getframe(args);
		break;
	case 'v':				/* view */
		if (badcom("view"))
			goto commerr;
		getview(args);
		break;
	case 'l':				/* last view */
		if (badcom("last"))
			goto commerr;
		lastview(args);
		break;
	case 'e':				/* exposure */
		if (badcom("exposure"))
			goto commerr;
		getexposure(args);
		break;
	case 's':				/* set a parameter */
		if (badcom("set"))
			goto commerr;
		setparam(args);
		break;
	case 'n':				/* new picture */
		if (badcom("new"))
			goto commerr;
		newimage();
		break;
	case 't':				/* trace a ray */
		if (badcom("trace"))
			goto commerr;
		traceray(args);
		break;
	case 'a':				/* aim camera */
		if (badcom("aim"))
			goto commerr;
		getaim(args);
		break;
	case 'm':				/* move camera */
		if (badcom("move"))
			goto commerr;
		getmove(args);
		break;
	case 'r':				/* rotate camera */
		if (badcom("rotate"))
			goto commerr;
		getrotate(args);
		break;
	case 'p':				/* pivot view */
		if (badcom("pivot"))
			goto commerr;
		getpivot(args);
		break;
	case CTRL(R):				/* redraw */
		redraw();
		break;
	case 'w':				/* write */
		if (badcom("write"))
			goto commerr;
		writepict(args);
		break;
	case 'q':				/* quit */
		if (badcom("quit"))
			goto commerr;
		quit(0);
	case CTRL(C):				/* interrupt */
		goto again;
#ifdef  SIGTSTP
	case CTRL(Z):				/* stop */
		devclose();
		kill(0, SIGTSTP);
		/* pc stops here */
		devopen(devname);
		redraw();
		break;
#endif
	case '\0':				/* continue */
		break;
	default:;
commerr:
		if (iscntrl(inpbuf[0]))
			sprintf(errmsg, "^%c: unknown control",
					inpbuf[0]|0100);
		else
			sprintf(errmsg, "%s: unknown command", inpbuf);
		error(COMMAND, errmsg);
		break;
	}
#undef  badcom
}


rsample()			/* sample the image */
{
	int  xsiz, ysiz, y;
	RECT  r;
	PNODE  *p;
	register RECT  *rl;
	register PNODE  **pl;
	register int  x;
	/*
	 *     We initialize the bottom row in the image at our current
	 * resolution.  During sampling, we check super-pixels to the
	 * right and above by calling bigdiff().  If there is a significant
	 * difference, we subsample the super-pixels.  The testing process
	 * includes initialization of the next row.
	 */
	xsiz = (((pframe.r-pframe.l)<<pdepth)+ourview.hresolu-1) /
			ourview.hresolu;
	ysiz = (((pframe.u-pframe.d)<<pdepth)+ourview.vresolu-1) /
			ourview.vresolu;
	rl = (RECT *)malloc(xsiz*sizeof(RECT));
	pl = (PNODE **)malloc(xsiz*sizeof(PNODE *));
	if (rl == NULL || pl == NULL)
		error(SYSTEM, "out of memory in rsample");
	/*
	 * Initialize the bottom row.
	 */
	rl[0].l = rl[0].d = 0;
	rl[0].r = ourview.hresolu; rl[0].u = ourview.vresolu;
	pl[0] = findrect(pframe.l, pframe.d, &ptrunk, rl, pdepth);
	for (x = 1; x < xsiz; x++) {
		rl[x].l = rl[x].d = 0;
		rl[x].r = ourview.hresolu; rl[x].u = ourview.vresolu;
		pl[x] = findrect(pframe.l+((x*ourview.hresolu)>>pdepth),
				pframe.d, &ptrunk, rl+x, pdepth);
	}
						/* sample the image */
	for (y = 0; /* y < ysiz */ ; y++) {
		for (x = 0; x < xsiz-1; x++) {
			if (dev->inpready)
				goto escape;
			/*
			 * Test super-pixel to the right.
			 */
			if (pl[x] != pl[x+1] && bigdiff(pl[x]->v,
					pl[x+1]->v, maxdiff)) {
				refine(pl[x], rl[x].l, rl[x].d,
						rl[x].r, rl[x].u, 1);
				refine(pl[x+1], rl[x+1].l, rl[x+1].d,
						rl[x+1].r, rl[x+1].u, 1);
			}
		}
		if (y >= ysiz-1)
			break;
		for (x = 0; x < xsiz; x++) {
			if (dev->inpready)
				goto escape;
			/*
			 * Find super-pixel at this position in next row.
			 */
			r.l = r.d = 0;
			r.r = ourview.hresolu; r.u = ourview.vresolu;
			p = findrect(pframe.l+((x*ourview.hresolu)>>pdepth),
				pframe.d+(((y+1)*ourview.vresolu)>>pdepth),
					&ptrunk, &r, pdepth);
			/*
			 * Test super-pixel in next row.
			 */
			if (pl[x] != p && bigdiff(pl[x]->v, p->v, maxdiff)) {
				refine(pl[x], rl[x].l, rl[x].d,
						rl[x].r, rl[x].u, 1);
				refine(p, r.l, r.d, r.r, r.u, 1);
			}
			/*
			 * Copy into super-pixel array.
			 */
			rl[x].l = r.l; rl[x].d = r.d;
			rl[x].r = r.r; rl[x].u = r.u;
			pl[x] = p;
		}
	}
escape:
	free((char *)rl);
	free((char *)pl);
}


int
refine(p, xmin, ymin, xmax, ymax, pd)		/* refine a node */
register PNODE  *p;
int  xmin, ymin, xmax, ymax;
int  pd;
{
	int  growth;
	int  mx, my;
	int  i;

	if (dev->inpready)			/* quit for input */
		return(0);

	if (pd <= 0)				/* depth limit */
		return(0);

	mx = (xmin + xmax) >> 1;
	my = (ymin + ymax) >> 1;
	growth = 0;

	if (p->kid == NULL) {			/* subdivide */

		if ((p->kid = newptree()) == NULL)
			error(SYSTEM, "out of memory in refine");
		/*
		 *  The following paint order can leave a black pixel
		 *  when redraw() is called in (*dev->paintr)().
		 */
		if (p->x >= mx && p->y >= my)
			pcopy(p, p->kid+UR);
		else
			paint(p->kid+UR, mx, my, xmax, ymax);
		if (p->x < mx && p->y >= my)
			pcopy(p, p->kid+UL);
		else
			paint(p->kid+UL, xmin, my, mx, ymax);
		if (p->x >= mx && p->y < my)
			pcopy(p, p->kid+DR);
		else
			paint(p->kid+DR, mx, ymin, xmax, my);
		if (p->x < mx && p->y < my)
			pcopy(p, p->kid+DL);
		else
			paint(p->kid+DL, xmin, ymin, mx, my);

		growth++;
	}
						/* do children */
	if (mx > pframe.l) {
		if (my > pframe.d)
			growth += refine(p->kid+DL, xmin, ymin, mx, my, pd-1);
		if (my < pframe.u)
			growth += refine(p->kid+UL, xmin, my, mx, ymax, pd-1);
	}
	if (mx < pframe.r) {
		if (my > pframe.d)
			growth += refine(p->kid+DR, mx, ymin, xmax, my, pd-1);
		if (my < pframe.u)
			growth += refine(p->kid+UR, mx, my, xmax, ymax, pd-1);
	}
						/* recompute sum */
	if (growth) {
		setcolor(p->v, 0.0, 0.0, 0.0);
		for (i = 0; i < 4; i++)
			addcolor(p->v, p->kid[i].v);
		scalecolor(p->v, 0.25);
	}
	return(growth);
}
