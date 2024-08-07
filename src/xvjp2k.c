/*
 * xvjp2k.c - I/O subroutines for JPEG 2000 format pictures
 *
 * This module is a "shim" between XV and a JPEG 2000 CODEC in the open-source
 * JasPer Library created by Michael D. Adams; for more information, see the
 * URL "http://www.ece.uvic.ca/~mdadams/jasper".  We don't use most of the
 * other facilities in this library, so it's better to link XV with a UNIX
 * "archive" representation of it, not a DLL.
 *
 * JPEG 2000 files can be represented in either of two general ways:  The
 * simplest representation is a "code stream", which often has a ".jpc" file
 * name suffix and is organized much like a classical JPEG file, except that
 * unfortunately, JPEG 2000 code streams indicate the no. of colors in an image
 * but no longer give any clues about its color space (e.g., RGB or YCbCr).
 * Instead, there is now a semantically higher-level representation, which
 * often has a ".jp2" file name suffix and encapsulates a "code stream" with
 * (possibly a lot of) color-space information, optionally including things
 * like ICC correction tables.
 *
 * Compared to the IJG JPEG Library used in file "xvjpeg.c", one must solve
 * several problems for color images when marrying JasPer to XV.
 *
 * 1. JPEG 2000 files can represent a wide range of image sizes, resolutions,
 *    and color spaces, not all of which can be easily rendered "as is" on a
 * normal "X Windows" display, so we must carefully check a decoded image's
 * parameters in order to reject anything that we can't handle gracefully.
 *
 * 2. JasPer prefers to decode/encode images using color-plane "slices",
 * instead of interleaved pixels needed by "X Windows", so we must
 * (de)interleave copies of the image buffer here.
 *
 * XXX  Things to do:
 *
 * 1. In "LoadJP{2,C}()" try to handle the "quick" option, which requests
 *    faster loading of a reduced-size image for the visual schnauzer.  The
 * schnauzer's icon size is currently 80x60, so the old "xvjpeg.c" module tries
 * to produce a 2X (160x120) image.  Can we do the same?
 *
 * 2. In "StoreJP2K()", JasPer Library Version 1.701 apparently has no API to
 *    let the XV global "picComments" string be inserted in a JPEG 2000 comment
 * record.  If the library ever gets fixed, enhance our code accordingly.
 *
 * --Scott Marovich <marovich@hpl.hp.com>, Hewlett-Packard Laboratories,
 *   January 2005.
 *
 * Michael Aadams <mdadams@ece.uvic.ca>, University of Victoria, January 2022.
 * The original code needed to be almost entirely rewritten due to its
 * insistence on bypassing the JasPer library API and violating many
 * preconditions in the usage of the API (which, of course, caused
 * many problems as the JasPer library evolved over time).
 *
 */
#include "copyright.h"

#define NEEDSARGS
#include "xv.h"

#ifdef HAVE_JP2K

#include <jasper/jasper.h>

#define GIBI (1024ULL * 1024ULL * 1024ULL)

static const char *fbasename, /* File's base name, for error/warning msgs */
	bad_samp[] = "%s:  can't read %d-plane %s file!", fmode[] = "rb",
	full_msg[] = "%s %s. (%ld bytes)", jp2_kind[] = "JP2",
	load_msg[] = "Loading %dx%d %s %s (%ld bytes)...",
	no_mem[] = "%s:  can't read %s file - out of memory",
	pixel_size[] = "%s:  can't display %d-bit pixels!",
	shrt_msg[] = "%dx%d %s %s. ",
	bad_dims[] = "%s:  error in JPEG-2000 header (bad image size)";

static int get_debug_level(void)
{
	int debug_level = 0;
	const char *cp;
	if ((cp = getenv("XV_JASPER_DEBUG_LEVEL"))) {
		debug_level = atoi(cp);
	}
	return debug_level;
}

#if (JAS_VERSION_MAJOR >= 3)
static int print_log(jas_logtype_t type, const char *format, va_list ap) {
	const int buffer_size = 512;
	char buffer[buffer_size];
	int count;

	int log_class = jas_logtype_getclass(type);
	int kind;
	switch (log_class) {
	case JAS_LOGTYPE_CLASS_INFO:
		kind = ISTR_INFO;
		break;
	case JAS_LOGTYPE_CLASS_WARN:
	case JAS_LOGTYPE_CLASS_ERROR:
	default:
		kind = ISTR_WARNING;
		break;
	}

	count = vsnprintf(buffer, buffer_size, format, ap);

	if (log_class == JAS_LOGTYPE_CLASS_WARN ||
		log_class == JAS_LOGTYPE_CLASS_ERROR) {
		if (get_debug_level() >= 1) {
			jas_eprintf("%s", buffer);
		} else {
			int i;
			for (i = 0; i < count; ++i) {
				if (buffer[i] == '\n') {
					buffer[i] = ' ';
				}
			}
			SetISTR(kind, "%s:  %s", fbasename, buffer);
		}
	} else {
		jas_eprintf("%s", buffer);
	}
	return count;
}
#endif

static int LoadJP2K(char *fname, register PICINFO *pinfo, int quick,
  bool jpc_format) {
	jas_image_t *img = 0;
	jas_stream_t *str = 0;
	FILE *fp;
	const char *s;
	unsigned long filesize;
	long w, h, npixels, bufsize;
	int vstride;
	register int i;
	jas_matrix_t *data = 0;

	int ret = 1;

	int debug_level = get_debug_level();
#if (JAS_VERSION_MAJOR >= 3)
	size_t max_mem = jas_get_total_mem_size();
	if (!max_mem) {
		max_mem = GIBI;
	}
	jas_conf_clear();
	jas_conf_set_max_mem_usage(max_mem);
	jas_init_library();
	jas_init_thread();
	jas_set_vlogmsgf(print_log);
#else
	jas_init();
#endif
#if (JAS_VERSION_MAJOR >= 3)
	jas_set_debug_level(debug_level);
#else
	jas_setdbglevel(debug_level);
#endif

	XV_UNUSED(quick);

	if (!(fp = xv_fopen(fname, fmode))) {
		return 0;
	}
	/* Input file's base name, for message(s) */
	fbasename = BaseName(fname);

	/* Compute file size is portable way. */
	fseek(fp, 0L, 2);
	filesize = ftell(fp);
	fseek(fp, 0L, 0);

#if (JAS_VERSION_MAJOR >= 3)
	/*
	This approach will not work in JasPer prior to 3.0.0 due to a bug in
	the stream code.
	*/
	if (!(str = jas_stream_freopen(fname,fmode,fp))) {
		fclose(fp);
		ret = 0;
		goto done;
	}

	/* It's not clear to me whether the following represents a JasPer Library
	   "bug" but it sure looks goofy:  Unless a stream buffer is marked "read
	   only", which only happens when the stream's "fillbuf" method is called,
	   even though our buffers are always "read only", the library will try to
	   flush out buffer contents when the stream is destroyed, which makes it
	   die a horrible death. So, mark the stream buffer proactively:
	*/
	str->bufmode_ |= JAS_STREAM_RDBUF; /* We will only read the stream buffer */
#else
	{
		if (!(str = jas_stream_memopen(0, 0))) {
			ret = 0;
			goto done;
		}
		const size_t buffer_size = 1024;
		char buffer[buffer_size];
		for (;;) {
			size_t count;
			count = fread(buffer, 1, buffer_size, fp);
			if (!count) {
				if (!feof(fp)) {
					ret = 0;
					goto done;
				}
				break;
			}
			if (jas_stream_write(str, buffer, count) != count) {
				ret = 0;
				goto done;
			}
		}
		jas_stream_rewind(str);
	}
#endif

	const jas_image_fmtinfo_t *fmtinfo = jas_image_lookupfmtbyname(
	  jpc_format ? "jpc" : "jp2");
	assert(fmtinfo);
	if (!(img = jas_image_decode(str, fmtinfo->id, 0))) {
		ret = 0;
		goto done;
	}

	w = jas_image_width(img);
	h = jas_image_height(img);
	vstride = jas_image_numcmpts(img);

	/* avoid buffer overflow */
	npixels = w * h;
	bufsize = vstride * npixels;
	if (w <= 0 || h <= 0 || npixels / w != h || bufsize / vstride != npixels) {
		(void)fclose(fp);
		SetISTR(ISTR_WARNING, bad_dims, fbasename);
		ret = 0;
		goto done;
	}
	pinfo->normw = pinfo->w = w;
	pinfo->normh = pinfo->h = h;

	/* Sanity-check the image's color space and no. of colors.  For now, accept
	   only "generic" color spaces, not files needing image-specific color
	   correction, but fix that someday...
	*/
	switch (vstride) {
		static char color_space[] = {"%s:  invalid color space!"};

	default:
		SetISTR(ISTR_WARNING, bad_samp, fbasename, vstride, jp2_kind);
		ret = 0;
		goto done;
	case 1:
		if (!jas_clrspc_isunknown(i = jas_image_clrspc(img)) &&
			jas_clrspc_fam(i) != JAS_CLRSPC_FAM_GRAY) {
			SetISTR(ISTR_WARNING, color_space, fbasename);
			ret = 0;
			goto done;
		}
		if ((i = jas_image_cmptprec(img, 0)) != 8) /* not 8-bit pixels */
		{
			SetISTR(ISTR_WARNING, pixel_size, fbasename, i);
			ret = 0;
			goto done;
		}
		s = "Greyscale";
		pinfo->type = PIC8;
		pinfo->colType = F_GREYSCALE;
		i = 256; /* Return fake indexed-color "map" */
		while (--i >= 0)
			pinfo->r[i] = pinfo->g[i] = pinfo->b[i] = i;
		break;
	case 3:
		if (jas_clrspc_isunknown(i = jas_image_clrspc(img))) {
			SetISTR(ISTR_WARNING, color_space, fbasename);
			ret = 0;
			goto done;
		}
		if (jas_clrspc_fam(i) != JAS_CLRSPC_FAM_RGB) {
			jas_image_t *oimg;
			jas_cmprof_t *profile;

			/* Here's where the JasPer Library really shines.  The only color
			   space that XV handles is RGB, so if that's not what our image
			   uses, then to quote Capt. Kirk:  "Make it so!"
			*/
			if (!(profile = jas_cmprof_createfromclrspc(JAS_CLRSPC_SRGB))) {
				SetISTR(ISTR_WARNING, "%s:  can't create RGB profile",
						fbasename);
				ret = 0;
				goto done;
			}
			img =
				jas_image_chclrspc(oimg = img, profile, JAS_CMXFORM_INTENT_PER);
			jas_cmprof_destroy(profile);
			if (!img) /* Oops!  We failed, so restore original image */
			{
				img = oimg;
				SetISTR(ISTR_WARNING, "%s:  can't convert to RGB", fbasename);
				ret = 0;
				goto done;
			}
			jas_image_destroy(oimg);
		}

		/* BEWARE OF KLUDGE:  If the image's color space is RGB, assume that the
							  data-sample precision for all color planes is the
		   same.  If the color space is YCbCr, assume the luminance (Y = 0th)
		   component has the greatest precision, although the chrominance
		   (Cr = 1st, Cb = 2nd) components are usually sub-sampled.
		*/
		if ((i = jas_image_cmptprec(img, 0)) != 8) /* not 24-bit pixels */
		{
			SetISTR(ISTR_WARNING, pixel_size, fbasename, i * 3);
			ret = 0;
			goto done;
		}
		s = "Color";
		pinfo->type = PIC24;
		pinfo->colType = F_FULLCOLOR;

		/* XXX  Unlike the IJG JPEG Library, the JasPer Library is apparently
				unable to quantize colors or tell us whether the image's colors
		   were quantized by its creator, so it seems that we can't return a
		   color map for XV to potentially use 8-bit indexed color.  If there
		   *is* an easy way to do it that escapes me, put the code here someday.
		*/
	}

	/* image buffer for XV */
	if (!(pinfo->pic = (byte *)malloc(bufsize)))
	{
		SetISTR(ISTR_WARNING, no_mem, fbasename, jp2_kind);
		ret = 0;
		goto done;
	}
	pinfo->frmType = F_JP2;
	sprintf(pinfo->fullInfo, full_msg, s, jp2_kind, filesize);
	sprintf(pinfo->shrtInfo, shrt_msg, pinfo->w, pinfo->h, s, jp2_kind);
	SetISTR(ISTR_INFO, load_msg, pinfo->normw, pinfo->normh, s, jp2_kind,
			filesize);

	/* Copy the sample data from the JasPer image to the xv image. */
	{
		int num_comps = vstride;
		int width = w;
		int height = h;
		int comp_ind;
		data = jas_matrix_create(height, width);
		assert(data);
		for (comp_ind = 0; comp_ind < num_comps; ++comp_ind) {
			if (jas_image_readcmpt(img, comp_ind, 0, 0, width, height, data)) {
				ret = 0;
				goto done;
			}
			jas_seqent_t *src;
			unsigned char *dst;
			int xx, yy;
			dst = pinfo->pic + comp_ind;
			for (yy = 0; yy < height; ++yy) {
				src = jas_matrix_getvref(data, yy);
				for (xx = 0; xx < width; ++xx) {
					*dst = *src;
					++src;
					dst += num_comps;
				}
			}
		}
	}

	/* Success! */
	ret = 1;

done:
	if (data) {
		jas_matrix_destroy(data);
	}
	if (img) {
		jas_image_destroy(img);
	}
	if (str) {
		jas_stream_close(str);
	}
#if (JAS_VERSION_MAJOR >= 3)
	jas_cleanup_thread();
	jas_cleanup_library();
#else
	jas_cleanup();
#endif
	return ret;
}

int LoadJP2(char *fname, register PICINFO *pinfo, int quick) {
	return LoadJP2K(fname, pinfo, quick, false);
}

int LoadJPC(char *fname, register PICINFO *pinfo, int quick) {
	return LoadJP2K(fname, pinfo, quick, true);
}

/* The following variables and subroutines are used when writing a JPEG 2000
   file, which is done mainly using call-backs from "X Windows" widgets.  The
   most complicated part of this interface is: managing interactions with a
   window to request the boat-loads of options that the JasPer Library
   supports. Start by defining subwindow sizes, plus indices into several
   arrays of corresponding widget-state variables.

   IMPLEMENTATION NOTES:  The following dimensions create a tall, thin window
						  which appears to have considerable empty space at the
   bottom.  Before you complain, click the Precinct Height menu button in order
   to the tall pop-up subwindow that it generates.  If the parent window is
   made shorter, then this pop-up will be clipped, which is an ugly nuisance. I
   don't know how to make the pop-up visible outside its parent's borders; do
   you?  If there's some way to make "X Windows" do this, then we might
   consider making the parent shorter.

   Note that there is currently no mechanism to program the no. of intermediate
   layers used by the encoder, or their rates.  This is potentially a large and
   complicated data-entry problem, and perhaps someday we can invent a clever
   solution using the rest of the parent window's space.
*/
#define JP2KW (275*dpiMult) /* Window width, in pixels */
#define JP2KH (400*dpiMult) /* Window height, in pixels */
#define BUTTW (51*dpiMult)  /* Button width, in pixels (odd for half-toning) */
#define BUTTH (20*dpiMult)  /* Button height, in pixels */
#define MENUW (75*dpiMult)  /* Menu-button width, in pixels (odd for half-toning) */
#define MENUH (24*dpiMult)  /* Menu-button height, in pixels */
#define RBUTH (20*dpiMult)  /* Radio button height, in pixels */
#define RBUTW (51*dpiMult)  /* Radio button width, in pixels (odd for half-toning) */
#define TEXTH (LINEHIGH + 5*dpiMult) /* Text subwindow height, in pixels */
#define TEXTW (75*dpiMult)  /* Text subwindow width, in pixels */

#define J_BOK 0	  /* Boolean "Ok" button */
#define J_BCANC 1 /* Boolean "Cancel" button */
#define J_NBUTT 2 /* No. of regular button widgets */

#define J_CSOP 0 /* Boolean encoding-style option buttons */
#define J_CEPH 1
#define J_CLAZY 2
#define J_CTERM 3
#define J_CSEGS 4
#define J_CVCAU 5
#define J_CPTRM 6
#define J_CRSTP 7
#define J_NCHKB 8 /* No. of check-box button widgets */

#define J_MCBXW 0 /* 1-of-N menu-selection buttons */
#define J_MCBXH 1
#define J_MPREW 2
#define J_MPREH 3
#define J_MPROG 4
#define J_NMENU 5 /* No. of menu-button widgets */

#define J_TGBIT 0 /* (Unsigned numeric) string subwindows */
#define J_TRES 1
#define J_TRATE 2
#define J_NTEXT 3 /* No. of text subwindows */

static BUTT button[J_NBUTT];
static CBUTT chkbut[J_NCHKB];
static MBUTT menu[J_NMENU];
static RBUTT *radio;
static Window text[J_NTEXT];
static int colorType, format, textval[J_NTEXT];
static const char *ProgList[] = {"lrcp", "rlcp", "rpcl", "pcrl", "cprl"};

void CreateJP2KW(void) {
	static const char EXP2_0[] = {"1"}, /* Successive powers of 2 */
		EXP2_1[] = {"2"}, EXP2_2[] = {"4"}, EXP2_3[] = {"8"}, EXP2_4[] = {"16"},
					  EXP2_5[] = {"32"}, EXP2_6[] = {"64"}, EXP2_7[] = {"128"},
					  EXP2_8[] = {"256"}, EXP2_9[] = {"512"},
					  EXP2_10[] = {"1024"}, EXP2_11[] = {"2048"},
					  EXP2_12[] = {"4096"}, EXP2_13[] = {"8192"},
					  EXP2_14[] = {"16384"}, EXP2_15[] = {"32768"};
	static const char *CBoxList[] = {EXP2_1, EXP2_2,  EXP2_3, EXP2_4,
									 EXP2_5, EXP2_6,  EXP2_7, EXP2_8,
									 EXP2_9, EXP2_10, EXP2_11};
	static const char *PrecList[] = {
		EXP2_0, EXP2_1, EXP2_2,	 EXP2_3,  EXP2_4,  EXP2_5,	EXP2_6,	 EXP2_7,
		EXP2_8, EXP2_9, EXP2_10, EXP2_11, EXP2_12, EXP2_13, EXP2_14, EXP2_15};
	static const char hstr[] = {"Height"}, wstr[] = {"Width"};
	int dx, dy;

	if (!(jp2kW = CreateWindow("xvjp2k", "XVjp2k", 0, JP2KW, JP2KH, infofg,
							   infobg, 0)))
		FatalError("can't create JPEG 2000 window!");
	XSelectInput(theDisp, jp2kW, ExposureMask | ButtonPressMask | KeyPressMask);

	/* Create a row of 2 boolean-valued, regular buttons ("Ok" and "Cancel") in
	   the window's bottom right corner.
	*/
	dy = JP2KH - 10*dpiMult - BUTTH - 1*dpiMult;
	BTCreate(&button[J_BOK], jp2kW, JP2KW - 2 * BUTTW - 20*dpiMult,
			 dy, BUTTW, BUTTH, "Ok", infofg, infobg, hicol,
			 locol);
	BTCreate(&button[J_BCANC], jp2kW, JP2KW - BUTTW - 10*dpiMult,
			 dy, BUTTW, BUTTH, "Cancel", infofg, infobg,
			 hicol, locol);

	/* Create a vertical column of 8 boolean-valued, check-box buttons (for
	   encoding-style options) down the window's left side.
	*/
	dx = 10*dpiMult;
	dy = 10*dpiMult + ASCENT + SPACING + 2 * LINEHIGH;
	CBCreate(&chkbut[J_CSOP], jp2kW, dx, dy + 0 * BUTTH, "sop", infofg,
			 infobg, hicol, locol);
	CBCreate(&chkbut[J_CEPH], jp2kW, dx, dy + 1 * BUTTH, "eph", infofg,
			 infobg, hicol, locol);
	CBCreate(&chkbut[J_CLAZY], jp2kW, dx, dy + 2 * BUTTH, "lazy", infofg,
			 infobg, hicol, locol);
	CBCreate(&chkbut[J_CTERM], jp2kW, dx, dy + 3 * BUTTH, "termall",
			 infofg, infobg, hicol, locol);
	CBCreate(&chkbut[J_CSEGS], jp2kW, dx, dy + 4 * BUTTH, "segsym", infofg,
			 infobg, hicol, locol);
	CBCreate(&chkbut[J_CVCAU], jp2kW, dx, dy + 5 * BUTTH, "vcausal",
			 infofg, infobg, hicol, locol);
	CBCreate(&chkbut[J_CPTRM], jp2kW, dx, dy + 6 * BUTTH, "pterm", infofg,
			 infobg, hicol, locol);
	CBCreate(&chkbut[J_CRSTP], jp2kW, dx, dy + 7 * BUTTH, "resetprob",
			 infofg, infobg, hicol, locol);
	/* Create text subwindows for unsigned decimal integer values. */

	dx = JP2KW - TEXTW - 10*dpiMult;
	dy = 10*dpiMult + ASCENT + SPACING + 2 * LINEHIGH + 3 * MENUH;
	text[J_TGBIT] = XCreateSimpleWindow(theDisp, jp2kW, dx, dy + 0 * TEXTH,
										TEXTW, TEXTH, 1, infofg, infobg);
	XSelectInput(theDisp, text[J_TGBIT], ExposureMask | KeyPressMask);
	text[J_TRES] = XCreateSimpleWindow(theDisp, jp2kW, dx, dy + 1 * TEXTH,
									   TEXTW, TEXTH, 1, infofg, infobg);
	XSelectInput(theDisp, text[J_TRES], ExposureMask | KeyPressMask);
	text[J_TRATE] = XCreateSimpleWindow(theDisp, jp2kW, dx, dy + 2 * TEXTH,
										TEXTW, TEXTH, 1, infofg, infobg);
	XSelectInput(theDisp, text[J_TRATE], ExposureMask | KeyPressMask);

	/* Create a row of 2 boolean-valued radio buttons (for the "Rate" subwindow
	   value's unit of measure).  The 1st button is "selected" by default.
	*/
	radio = RBCreate(0, jp2kW, JP2KW - 19 * RBUTW / 8 - 10*dpiMult, dy + 3 * TEXTH + 4*dpiMult,
					 "Percent", infofg, infobg, hicol, locol);
	(void)RBCreate(radio, jp2kW, JP2KW - 1 * RBUTW - 10 * dpiMult, dy + 3 * TEXTH + 4*dpiMult,
				   "Bytes", infofg, infobg, hicol, locol);

	/* Create pop-up menu-selection buttons after mapping the above subwindows,
	   since we don't want the pop-up menus mapped unless the corresponding
	   button is selected.
	*/
	dx = JP2KW - 2 * MENUW - 10*dpiMult;
	dy = 10*dpiMult + ASCENT + SPACING + 2 * LINEHIGH;
	XMapSubwindows(theDisp, jp2kW);
	MBCreate(&menu[J_MCBXW], jp2kW, dx, dy + 0 * MENUH, MENUW, MENUH,
			 wstr, CBoxList, sizeof CBoxList / sizeof *CBoxList, infofg, infobg,
			 hicol, locol);
	MBCreate(&menu[J_MCBXH], jp2kW, dx, dy + 1 * MENUH, MENUW, MENUH,
			 hstr, CBoxList, sizeof CBoxList / sizeof *CBoxList, infofg, infobg,
			 hicol, locol);
	dx = JP2KW - 1 * MENUW - 10*dpiMult;
	MBCreate(&menu[J_MPREW], jp2kW, dx, dy + 0 * MENUH, MENUW, MENUH,
			 wstr, PrecList, sizeof PrecList / sizeof *PrecList, infofg, infobg,
			 hicol, locol);
	MBCreate(&menu[J_MPREH], jp2kW, dx, dy + 1 * MENUH, MENUW, MENUH,
			 hstr, PrecList, sizeof PrecList / sizeof *PrecList, infofg, infobg,
			 hicol, locol);
	MBCreate(&menu[J_MPROG], jp2kW, dx, dy + 2 * MENUH, MENUW, MENUH,
			 "Order", ProgList, sizeof ProgList / sizeof *ProgList, infofg,
			 infobg, hicol, locol);

	/* Initialize values represented by widgets, which should correspond to
	   default compiled into the JasPer Library.  Unfortunately, as of
	   Version 1.701 there is no easy way for an application to extract these
	   from the library, so the following code might get out of sync over time:
	*/
	menu[J_MCBXW].hascheck = 1;
	menu[J_MCBXW].flags[5] = 1;
	menu[J_MCBXH].hascheck = 1;
	menu[J_MCBXH].flags[5] = 1;
	menu[J_MPREW].hascheck = 1;
	menu[J_MPREW].flags[15] = 1;
	menu[J_MPREH].hascheck = 1;
	menu[J_MPREH].flags[15] = 1;
	menu[J_MPROG].hascheck = 1;
	menu[J_MPROG].flags[0] = 1;
	textval[J_TGBIT] = 2;	/* No. of guard bits */
	textval[J_TRES] = 6;	/* Max. no. of resolution levels */
	textval[J_TRATE] = 100; /* Rate = 100% */
}

void JP2KSaveParams(int fmt, char *fname, int col) /* Save output-file parms */
{
	format = fmt;	   /* Desired file format: F_JPC|F_JP2 */
	fbasename = fname; /* ->Output file path */
	colorType = col;   /* Desired color space: F_GREYSCALE|... */
}

static void StoreJP2K(char *options) {
	static jas_image_cmptparm_t parm[3] = /* Image parameters */
		{{0, 0, 1, 1, 0, 0, 8, 0},
		 {0, 0, 1, 1, 0, 0, 8, 0},
		 {0, 0, 1, 1, 0, 0, 8, 0}};
	static const char write[] = {"w"};
	jas_image_t *img = 0;
	jas_stream_t *str = 0;
	FILE *fp = 0;
	FILE *fp2 = 0;
	byte *pic, *r, *g, *b;
	const char *filename;
	unsigned long imagesize;
	jas_clrspc_t color_space;
	int nc, w, h;
	int pfree, ptype;
	int error = 1;
	register int i;
	jas_matrix_t *data = 0;
	int debug_level = get_debug_level();

#if (JAS_VERSION_MAJOR >= 3)
	size_t max_mem = jas_get_total_mem_size();
	if (!max_mem) {
		max_mem = GIBI;
	}
	jas_conf_clear();
	jas_conf_set_max_mem_usage(max_mem);
	jas_init_library();
	jas_init_thread();
	jas_set_vlogmsgf(print_log);
	jas_set_debug_level(debug_level);
#else
	jas_init();
	jas_setdbglevel(debug_level);
#endif

	/* This is a generic subroutine for writing JPEG 2000 image files using the
	   JasPer Library.  Our argument is an ASCII string, containing a Space ("
	   ")- separated sequence of encoder options that currently aren't well
	   documented. Most of the work is identical for both ".jpc" and ".jp2"
	   files. Start by verifying that the output file can be opened, then get an
	   image buffer from XV and begin crunching it into a suitable form for the
	   JasPer Library.
	*/
	if (!(fp = OpenOutFile(filename = fbasename)))
		return; /* Oops! */
	setbuf(fp,
		   0); /* We don't really use this file pointer for I/O; see below */
	fbasename = BaseName(filename);
	WaitCursor();
	pic = GenSavePic(&ptype, &w, &h, &pfree, &nc, &r, &g, &b);
	assert(ptype == PIC8 || ptype == PIC24);
	imagesize = w * h;
	if (ptype == PIC24)
		imagesize *= 3;

	assert(colorType == F_GREYSCALE || colorType == F_FULLCOLOR);
#if 1
	/* As an optimization to save file space, even if our user didn't ask to
	   store a gray-scale image, check whether we could and, if so, do it
	   anyway.
	*/
	if (colorType != F_GREYSCALE) /* can we force a gray-scale image? */
	{
		if (ptype == PIC8) {
			i = nc;
			while (--i >= 0 && r[i] == g[i] && r[i] == b[i]) {}
		} else {
			assert(ptype == PIC24);
			/* PIC24 */
			byte *p = pic + imagesize;
			while ((p -= 3) >= pic && p[0] == p[1] && p[0] == p[2]) {}
			i = p - pic;
		};
		if (i < 0) {
			colorType = F_GREYSCALE; /* We made it all the way through */
		}
	};
#endif

	/* Now comes a egregious hack:  The JasPer Library will eventually want to
	   close the output file that it writes, but since XV opened the file, XV
	   also thinks it has the right to close the file!  In order to pacify them
	   both, we duplicate the file pointer and let the JasPer Library have it,
	   while we retain the original for XV.

	   XXX  This code is very UNIX-specific; what's an equivalent hack for
	   Windows?
	*/
	{
		int fd = dup(fileno(fp));
		if (fd < 0) {
			FatalError("StoreJP2K: can't duplicate output file pointer\n");
		}
		if (!(fp2 = fdopen(fd, write))) {
			FatalError("StoreJP2K: can't duplicate output file pointer\n");
		}
	}

	/* Hand our output file to the JasPer Library and create an image object.
	   "jas_stream_close()" will eventually close our output file, so only do it
	   explicitly if no stream can be created.  If everything looks copacetic,
	   then write our buffer contents to the image components' streams.
	*/
	if (!(str = jas_stream_freopen(filename, write, fp2))) {
		(void)fclose(fp2);
		FatalError("StoreJP2K: can't open output stream\n");
	};
	fp2 = 0;

	int num_comps = (colorType == F_GREYSCALE) ? 1 : 3;
	int comp_ind;
	if (num_comps == 3) {
		color_space = JAS_CLRSPC_SRGB;
	} else {
		color_space = JAS_CLRSPC_SGRAY;
	}
	for (comp_ind = 0; comp_ind < num_comps; ++comp_ind) {
		parm[comp_ind].tlx = 0;
		parm[comp_ind].tly = 0;
		parm[comp_ind].hstep = 1;
		parm[comp_ind].vstep = 1;
		parm[comp_ind].width = w;
		parm[comp_ind].height = h;
		parm[comp_ind].prec = 8;
		parm[comp_ind].sgnd = 0;
	}
	if (!(img = jas_image_create(num_comps, parm, color_space))) {
		goto L2;
	}
	if (get_debug_level() >= 1) {
		fprintf(stderr, "num_comps %d width %d height %d\n", num_comps, w, h);
		fprintf(stderr, "colorType %d\n", colorType);
		fprintf(stderr, "ptype %d\n", ptype);
	}

	{
		int comp_ind;
		if (!(data = jas_matrix_create(h, w))) {
			goto done;
		}
		if (num_comps == 3) {
			for (comp_ind = 0; comp_ind < num_comps; ++comp_ind) {
				int comp_type;
				switch (comp_ind) {
				case 0:
					comp_type = JAS_IMAGE_CT_RGB_R;
					break;
				case 1:
					comp_type = JAS_IMAGE_CT_RGB_G;
					break;
				case 2:
					comp_type = JAS_IMAGE_CT_RGB_B;
					break;
				}
				jas_image_setcmpttype(img, comp_ind, comp_type);
				unsigned char *src;
				jas_seqent_t *dst;
				int x, y;
				src = pic + comp_ind;
				for (y = 0; y < h; ++y) {
					dst = jas_matrix_getvref(data, y);
					for (x = 0; x < w; ++x) {
						*dst = *src;
						++dst;
						src += num_comps;
					}
				}
				if (jas_image_writecmpt(img, comp_ind, 0, 0, w, h, data)) {
					goto L1;
				}
			}
		} else {
			int mode = (ptype == PIC8) ? 0 : 1;
			assert(num_comps == 1);
			jas_image_setcmpttype(img, 0, JAS_IMAGE_CT_GRAY_Y);
			unsigned char *src;
			jas_seqent_t *dst;
			int x, y;
			src = pic;
			for (y = 0; y < h; ++y) {
				dst = jas_matrix_getvref(data, y);
				for (x = 0; x < w; ++x) {
					int v;
					if (mode == 0) {
						v = *src;
						v = MONO(r[v], g[v], b[v]);
					} else /* if (mode == 1) */ {
						v = MONO(src[0], src[1], src[2]);
					}
					*dst = v;
					++dst;
					++src;
				}
			}
			if (jas_image_writecmpt(img, 0, 0, 0, w, h, data)) {
				goto L1;
			}
		}
	}

	const jas_image_fmtinfo_t *fmtinfo =
		jas_image_lookupfmtbyname(((format == F_JPC) ? "jpc" : "jp2"));
	if (!jas_image_encode(img, str, fmtinfo->id, options) &&
		jas_stream_flush(str) >= 0) {
		error = 0; /* Success! */
	}

done:
	if (data) {
		jas_matrix_destroy(data);
	}
L1:
	if (img) {
		jas_image_destroy(img);
	}
L2:
	if (str) {
		jas_stream_close(str);
	}
	if (pfree) {
		free(pic);
	}
	if (!CloseOutFile(fp, filename, error)) {
		DirBox(0);
	}
	SetCursors(-1);

#if (JAS_VERSION_MAJOR >= 3)
	jas_cleanup_thread();
	jas_cleanup_library();
#else
	jas_cleanup();
#endif
}

void JP2KDialog(int vis) {
	if ((jp2kUp = vis))
		CenterMapWindow(jp2kW, 0, 0, JP2KW, JP2KH);
	else
		XUnmapWindow(theDisp, jp2kW);
}

static void TWRedraw(Window w, unsigned int val) {
	char buf[11];
	register int i;

	/* Draw a 1-line numeric text string in the specified window, representing
	   the argument value as a left-justified unsigned decimal integer, followed
	   by a "cursor" icon.
	*/
	sprintf(buf, "%u", val);
	if (ctrlColor)
		XClearArea(theDisp, w, 2*dpiMult, 2*dpiMult, TEXTW - 4*dpiMult, TEXTH - 4*dpiMult, False);
	else
		XClearWindow(theDisp, w);
	Draw3dRect(w, 0, 0, TEXTW - 1*dpiMult, TEXTH - 1*dpiMult, R3D_IN, 2, hicol, locol, infobg);
	XSetForeground(theDisp, theGC, infofg);
	XDrawString(theDisp, w, theGC, 3*dpiMult, ASCENT + 3*dpiMult, buf, i = strlen(buf));

	/* Draw a "cursor" icon after the numeric string. */

	i = XTextWidth(mfinfo, buf, i);
	XDrawLine(theDisp, w, theGC, i + 3*dpiMult, 2*dpiMult,                     i + 3*dpiMult, 2*dpiMult + CHIGH + 1*dpiMult);
	XDrawLine(theDisp, w, theGC, i + 3*dpiMult, 2*dpiMult + CHIGH + 1*dpiMult, i + 5*dpiMult, 2*dpiMult + CHIGH + 3*dpiMult);
	XDrawLine(theDisp, w, theGC, i + 3*dpiMult, 2*dpiMult + CHIGH + 1*dpiMult, i + 1*dpiMult, 2*dpiMult + CHIGH + 3*dpiMult);
}

int JP2KCheckEvent(register XEvent *xev) {
	int ok = 0;

	/* Check whether the argument "X Windows" event is for one of our
	   subwindows. If it is, handle the event and return 1; otherwise, return 0.
	*/
	if (!jp2kUp)
		return 0;
	switch (xev->type) {
		KeySym ks;
		int len;
		char buf[128];
		register int i;

		/* Throw away excess "expose" events for "dumb" windows. */

	case Expose:
#define E ((XExposeEvent *)xev)
		if (E->window == jp2kW) {
			XRectangle rect;

			rect.x = E->x;
			rect.y = E->y;
			rect.width = E->width;
			rect.height = E->height;
			XSetClipRectangles(theDisp, theGC, 0, 0, &rect, 1, Unsorted);
			XSetForeground(theDisp, theGC, infofg);
			XSetBackground(theDisp, theGC, infobg);
			i = sizeof button / sizeof *button;
			while (--i >= 0)
				BTRedraw(&button[i]);
			i = sizeof chkbut / sizeof *chkbut;
			while (--i >= 0)
				CBRedraw(&chkbut[i]);
			i = sizeof menu / sizeof *menu;
			while (--i >= 0)
				MBRedraw(&menu[i]);
			RBRedraw(radio, -1);
			DrawString(jp2kW, 10*dpiMult, 10*dpiMult + ASCENT, "Save JPEG 2000 File...");
			DrawString(jp2kW, 10*dpiMult, 10*dpiMult + ASCENT + 2 * LINEHIGH, "Style options:");
			DrawString(jp2kW, JP2KW - 2 * MENUW - 10*dpiMult,
					   10*dpiMult + ASCENT + 2 * LINEHIGH, "Coding Blk");
			DrawString(jp2kW, JP2KW - 1 * MENUW - 10*dpiMult,
					   10*dpiMult + ASCENT + 2 * LINEHIGH, "  Precinct");
			DrawString(jp2kW, JP2KW - 2 * MENUW - 10*dpiMult,
					   10*dpiMult + ASCENT + SPACING + 3 * LINEHIGH + 2 * MENUH,
					   "Progression:");
			DrawString(jp2kW, JP2KW - 2 * TEXTW - 10*dpiMult,
					   10*dpiMult + ASCENT + SPACING + 3 * LINEHIGH + 3 * MENUH +
						   0 * TEXTH,
					   "Guard bits:");
			DrawString(jp2kW, JP2KW - 2 * TEXTW - 10*dpiMult,
					   10*dpiMult + ASCENT + SPACING + 3 * LINEHIGH + 3 * MENUH +
						   1 * TEXTH,
					   "Res levels:");
			DrawString(jp2kW, JP2KW - 2 * TEXTW - 10*dpiMult,
					   10*dpiMult + ASCENT + SPACING + 3 * LINEHIGH + 3 * MENUH +
						   2 * TEXTH,
					   "Rate:");
			XSetClipMask(theDisp, theGC, None);
			ok = 1;
			break;
		};
		i = sizeof text / sizeof *text;
		while (--i >= 0 && E->window != text[i])
			;
		if (i >= 0) {
			TWRedraw(E->window, textval[i]);
			ok = 1;
		};
		break;
#undef E

		/* Check whether the user pressed one of our buttons. */

	case ButtonPress:
#define E ((XButtonEvent *)xev)
		if (E->button == Button1 && E->window == jp2kW) {
			register int j;

			ok = 1; /* Check whether a regular button was pressed */
			i = sizeof button / sizeof *button;
			while (--i >= 0 && !PTINRECT(E->x, E->y, button[i].x, button[i].y,
										 button[i].w, button[i].h))
				;
			if (i >= 0) /* our button was pressed */
			{
				char options[1024];
				register char *p;
				register const char *q;

				if (!BTTrack(&button[i]))
					break;		/* Ignore it */
				if (i != J_BOK) /* must be "Cancel" button */
				{
					JP2KDialog(0);
					break;
				};

				/* Our user hit the "Ok" button.  At this point, we have an ugly
				   job to do:  JasPer Library encoder options must be specified
				   as an ASCII string of Space (" ")-separated <tag>[=<value>]
				   entries, so we must collect the values of all of our widgets
				   and subwindows, determine whether our user has requested any
				   non-default values, then build the string only for the JasPer
				   Library to immediately tear it apart again.  Yechh!
				*/
				if ((unsigned)textval[J_TGBIT] - 1 > 7) {
					OpenAlert("No. of guard bits must be 1-8");
					CloseAlert();
					break;
				};
				if ((unsigned)textval[J_TRES] <= 0) {
					OpenAlert("Maximum resolution levels must be >= 1");
					CloseAlert();
					break;
				};

				/* XXX  Should we check and complain if the rate is zero?
						JasPer Library Version 1.701 apparently accepts that
				   value, even though it seems kinda weird.
				*/
				p = options;
				i = 0;
				do
					if ((j = MBWhich(&menu[i])) != 5) {
						static const char *parm[2] = {"cblkwidth=%u",
													  "cblkheight=%u"};

						if (p > options)
							*p++ = ' ';
						sprintf(p, parm[i - J_MCBXW], ++j);
						while (*++p)
							;
					}
				while (++i <= J_MCBXH);
				do
					if ((j = MBWhich(&menu[i])) < 15) {
						static const char *parm[2] = {"prcwidth=%u",
													  "prcheight=%u"};

						if (p > options)
							*p++ = ' ';
						sprintf(p, parm[i - J_MPREW], j);
						while (*++p)
							;
					}
				while (++i <= J_MPREH);
				if ((j = MBWhich(&menu[i]))) {
					if (p > options)
						*p++ = ' ';
					*p++ = 'p';
					*p++ = 'r';
					*p++ = 'g';
					*p = '=';
					q = ProgList[j];
					while ((*++p = *q++))
						;
				};
				if ((i = textval[J_TRES]) != 6) {
					if (p > options)
						*p++ = ' ';
					sprintf(p, "numrlvls=%u", i);
					while (*++p)
						;
				};
				i = 0;
				do
					if (chkbut[i].val) /* append this encoding option */
					{
						if (p > options)
							*p++ = ' ';
						q = chkbut[i].str;
						while ((*p++ = *q++))
							;
						*--p = '\000';
					}
				while (++i < sizeof chkbut / sizeof *chkbut);
				if ((i = textval[J_TGBIT]) != 2) {
					if (p > options)
						*p++ = ' ';
					sprintf(p, "numgbits=%u", i);
					while (*++p)
						;
				};
				if ((i = textval[J_TRATE]) != 100) {
					if (p > options)
						*p++ = ' ';
					*p++ = 'r';
					*p++ = 'a';
					*p++ = 't';
					*p++ = 'e';
					*p++ = '=';
					if (i) /* non-zero rate */
					{
						if (RBWhich(radio))
							sprintf(p, "%uB", i); /* # Bytes */
						else /* the value is a percentage */
						{
							if (i > 100)
								i = 200; /* => Raw size + 1 */
							sprintf(p, "%u.%.2u", i / 100, i % 100);
						}
						while (*++p)
							;
					} else /* rate = 0 */
					{

						/* XXX  This is apparently valid, since JasPer Library
								Version 1.701 doesn't complain about it.
						*/
						*p++ = '0';
						*p = '\000';
					}
				};
				StoreJP2K(options); /* Finally, do the *real* work! */
				JP2KDialog(0);
				p = GetDirFullName();
				if (!ISPIPE(*p)) {
					XVCreatedFile(p);
					StickInCtrlList(0);
				};
				break;
			};

			/* See whether a check-box button was pressed. */

			i = sizeof chkbut / sizeof *chkbut;
			while (--i >= 0 && !CBClick(&chkbut[i], E->x, E->y))
				;
			if (i >= 0) /* our button was pressed */
			{
				(void)CBTrack(&chkbut[i]);
				break;
			};

			/* See whether a menu button was pressed. */

			i = sizeof menu / sizeof *menu;
			while (--i >= 0 && !MBClick(&menu[i], E->x, E->y))
				;
			if (i >= 0) /* our button was pressed */
			{
				register int j;

				if ((j = MBTrack(&menu[i])) >= 0) {
					switch (i) {

						/* The JasPer Library constrains a coding box's area
						   (width x height) to be <= 4096 pixels, so if this
						   button-press would violate that condition, then
						   quietly limit the box's orthogonal dimension by just
						   enough to compensate.
						*/
					case J_MCBXH:
					case J_MCBXW:
						if (MBWhich(&menu[!i]) + j >= 10)
							MBSelect(&menu[!i], 10 - j);
					};
					MBSelect(&menu[i], j);
					MBSetActive(&menu[i], 1);
				};
				break;
			};

			/* See whether a radio button was pressed. */

			if ((i = RBClick(radio, E->x, E->y)) >= 0 /* button pressed */
				&& RBTrack(radio, i)) {
				RBSelect(radio, i);
				RBSetActive(radio, i, 1);
			};
			break;
		};
		XBell(theDisp, 50);
		break;
#undef E

		/* Translate a few key-press events into simulated button events. */

	case KeyPress:
#define E ((XKeyEvent *)xev)
		buf[len = XLookupString(E, buf, sizeof buf, &ks, 0)] = '\000';
		RemapKeyCheck(ks, buf, &len);
		if (E->window == jp2kW) {
			ok = 1;
			if (len > 0)
				switch (buf[0]) {
				case '\r':
				case '\n':
					FakeButtonPress(&button[J_BOK]);
					break;
				case '\033':
					FakeButtonPress(&button[J_BCANC]);
				};
			break;
		};
		i = sizeof text / sizeof *text;
		while (--i >= 0 && E->window != text[i])
			;
		if (i >= 0) /* a key was pressed in our text window */
		{
			int oldval = textval[i]; /* Save this subwindow's old value */
			register int c, j = 0;

			/* This is a *very* simple text-input editing loop that assembles an
			   an unsigned integer from successive ASCII decimal digits, typed
			   into one of our 1-line subwindows.  For convenience, the only
			   character recognized is Backspace.
			*/
			ok = 1;
		L:
			if (j >= len)
				break;
			if ((unsigned)(c = buf[j++] - '0') <= 9) {
				TWRedraw(text[i], textval[i] = textval[i] * 10 + c);
				goto L;
			};
			if ((c += '0') == '\b') {
				TWRedraw(text[i], textval[i] = textval[i] / 10);
				goto L;
			};
			textval[i] = oldval;
		};
		XBell(theDisp, 50);
#undef E
	};
	return ok;
}

/*******************************************/
void VersionInfoJP2K(void) /* GRR 20070304 */
{
	fprintf(stderr, "   Compiled with libjasper %s; using libjasper %s.\n",
			JAS_VERSION, jas_getversion());
}

#endif /* HAVE_JP2K */
