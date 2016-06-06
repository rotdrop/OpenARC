/*
**  Copyright (c) 2009-2016, The Trusted Domain Project.  All rights reserved.
*/

#include "build-config.h"

/* for Solaris */
#ifndef _REENTRANT
# define _REENTRANT
#endif /* ! REENTRANT */

/* system includes */
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#endif /* HAVE_STDBOOL_H */
#include <netdb.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <resolv.h>

#ifdef __STDC__
# include <stdarg.h>
#else /* __STDC__ */
# include <varargs.h>
#endif /* _STDC_ */

/* OpenSSL includes */
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/sha.h>

/* libopenarc includes */
#include "arc-internal.h"
#include "arc-canon.h"
#include "arc-tables.h"
#include "arc-types.h"
#include "arc-util.h"
#include "arc.h"
#include "base64.h"

/* libbsd if found */
#ifdef USE_BSD_H
# include <bsd/string.h>
#endif /* USE_BSD_H */

/* libstrl if needed */
#ifdef USE_STRL_H
# include <strl.h>
#endif /* USE_STRL_H */

/* prototypes */
void arc_error __P((ARC_MESSAGE *, const char *, ...));

/* macros */
#define	ARC_STATE_INIT		0
#define	ARC_STATE_HEADER	1
#define	ARC_STATE_EOH		2
#define	ARC_STATE_BODY		3
#define	ARC_STATE_EOM		4
#define	ARC_STATE_UNUSABLE	99

#define	CRLF			"\r\n"

#define	BUFRSZ			1024
#define	DEFERRLEN		128
#define	DEFTIMEOUT		10

/* local definitions needed for DNS queries */
#define MAXPACKET		8192
#if defined(__RES) && (__RES >= 19940415)
# define RES_UNC_T		char *
#else /* __RES && __RES >= 19940415 */
# define RES_UNC_T		unsigned char *
#endif /* __RES && __RES >= 19940415 */

#ifndef T_AAAA
# define T_AAAA			28
#endif /* ! T_AAAA */

/* macros */
#define ARC_ISLWSP(x)  ((x) == 011 || (x) == 013 || (x) == 014 || (x) == 040)

#define	ARC_PHASH(x)		((x) - 32)

/*
**  ARC_ERROR -- log an error into a DKIM handle
**
**  Parameters:
**  	msg -- ARC message context in which this is performed
**  	format -- format to apply
**  	... -- arguments
**
**  Return value:
**  	None.
*/

void
arc_error(ARC_MESSAGE *msg, const char *format, ...)
{
	int flen;
	int saverr;
	u_char *new;
	va_list va;

	assert(msg != NULL);
	assert(format != NULL);

	saverr = errno;

	if (msg->arc_error == NULL)
	{
		msg->arc_error = malloc(DEFERRLEN);
		if (msg->arc_error == NULL)
		{
			errno = saverr;
			return;
		}
		msg->arc_errorlen = DEFERRLEN;
	}

	for (;;)
	{
		va_start(va, format);
		flen = vsnprintf((char *) msg->arc_error, msg->arc_errorlen,
		                 format, va);
		va_end(va);

		/* compensate for broken vsnprintf() implementations */
		if (flen == -1)
			flen = msg->arc_errorlen * 2;

		if (flen >= msg->arc_errorlen)
		{
			new = malloc(flen + 1);
			if (new == NULL)
			{
				errno = saverr;
				return;
			}

			free(msg->arc_error);
			msg->arc_error = new;
			msg->arc_errorlen = flen + 1;
		}
		else
		{
			break;
		}
	}

	errno = saverr;
}

/*
**  ARC_INIT -- create a library instance
**
**  Parameters:
**  	None.
**
**  Return value:
**  	A new library instance.
*/

ARC_LIB *
arc_init(void)
{
	ARC_LIB *lib;

	lib = (ARC_LIB *) malloc(sizeof *lib);
	if (lib == NULL)
		return lib;

	memset(lib, '\0', sizeof *lib);
	lib->arcl_flags = ARC_LIBFLAGS_DEFAULT;

#define FEATURE_INDEX(x)	((x) / (8 * sizeof(u_int)))
#define FEATURE_OFFSET(x)	((x) % (8 * sizeof(u_int)))
#define FEATURE_ADD(lib,x)	(lib)->arcl_flist[FEATURE_INDEX((x))] |= (1 << FEATURE_OFFSET(x))

	lib->arcl_flsize = (FEATURE_INDEX(ARC_FEATURE_MAX)) + 1;
	lib->arcl_flist = (u_int *) malloc(sizeof(u_int) * lib->arcl_flsize);
	if (lib->arcl_flist == NULL)
	{
		free(lib);
		return NULL;
	}
	memset(lib->arcl_flist, '\0', sizeof(u_int) * lib->arcl_flsize);

#ifdef HAVE_SHA256
	FEATURE_ADD(lib, ARC_FEATURE_SHA256);
#endif /* HAVE_SHA256 */

	return lib;
}

/*
**  ARC_CLOSE -- terminate a library instance
**
**  Parameters:
**  	lib -- library instance to terminate
**
**  Return value:
**  	None.
*/

void
arc_close(ARC_LIB *lib)
{
	free(lib);
}

/*
** 
**  ARC_OPTIONS -- get/set library options
**
**  Parameters:
**  	lib -- library instance of interest
**  	opt -- ARC_OP_GETOPT or ARC_OP_SETOPT
**  	arg -- ARC_OPTS_* constant
**  	val -- pointer to the new value (or NULL)
**  	valsz -- size of the thing at val
**
**  Return value:
**  	An ARC_STAT_* constant.
*/

ARC_STAT
arc_options(ARC_LIB *lib, int op, int arg, void *val, size_t valsz)
{
	assert(lib != NULL);
	assert(op == ARC_OP_GETOPT || op == ARC_OP_SETOPT);

	switch (arg)
	{
	  case ARC_OPTS_FLAGS:
		if (val == NULL)
			return ARC_STAT_INVALID;

		if (valsz != sizeof lib->arcl_flags)
			return ARC_STAT_INVALID;

		if (op == ARC_OP_GETOPT)
			memcpy(val, &lib->arcl_flags, valsz);
		else
			memcpy(&lib->arcl_flags, val, valsz);

		return ARC_STAT_OK;

	  case ARC_OPTS_TMPDIR:
		if (op == ARC_OP_GETOPT)
		{
			strlcpy((char *) val, (char *) lib->arcl_tmpdir,
			        valsz);
		}
		else if (val == NULL)
		{
			strlcpy((char *) lib->arcl_tmpdir, DEFTMPDIR,
			        sizeof lib->arcl_tmpdir);
		}
		else
		{
			strlcpy((char *) lib->arcl_tmpdir, (char *) val,
			        sizeof lib->arcl_tmpdir);
		}
		return ARC_STAT_OK;

	  default:
		assert(0);
	}
}

/*
**  ARC_GETSSLBUF -- retrieve SSL error buffer
**
**  Parameters:
**  	lib -- library handle
**
**  Return value:
**  	Pointer to the SSL buffer in the library handle.
*/

const char *
arc_getsslbuf(ARC_LIB *lib)
{
	return (const char *) arc_dstring_get(lib->arcl_sslerrbuf);
}

/*
**  ARC_CHECK_UINT -- check a parameter for a valid unsigned integer
**
**  Parameters:
**  	value -- value to check
**
**  Return value:
**  	TRUE iff the input value looks like a properly formed unsigned integer.
*/

_Bool
arc_check_uint(u_char *value)
{
	uint64_t tmp = 0;
	char *end;

	assert(value != NULL);

	errno = 0;

	if (value[0] == '-')
	{
		errno = ERANGE;
		tmp = (uint64_t) -1;
	}
	else if (value[0] == '\0')
	{
		errno = EINVAL;
		tmp = (uint64_t) -1;
	}
	else
	{
		tmp = strtoull((char *) value, &end, 10);
	}

	return !(tmp == (uint64_t) -1 || errno != 0 || *end != '\0');
}

/*
**  ARC_PARAM_GET -- get a parameter from a set
**
**  Parameters:
**  	set -- set to search
**  	param -- parameter to find
**
**  Return value:
**  	Pointer to the parameter requested, or NULL if it's not in the set.
*/

static u_char *
arc_param_get(ARC_KVSET *set, u_char *param)
{
	ARC_PLIST *plist;

	assert(set != NULL);
	assert(param != NULL);

	for (plist = set->set_plist[ARC_PHASH(param[0])];
	     plist != NULL;
	     plist = plist->plist_next)
	{
		if (strcmp((char *) plist->plist_param, (char *) param) == 0)
			return plist->plist_value;
	}

	return NULL;
}

/*
**  ARC_SET_FIRST -- return first set in a context
**
**  Parameters:
**  	msg -- ARC message context
**  	type -- type to find, or ARC_KVSETTYPE_ANY
**
**  Return value:
**  	Pointer to the first ARC_KVSET in the context, or NULL if none.
*/

static ARC_KVSET *
arc_set_first(ARC_MESSAGE *msg, arc_kvsettype_t type)
{
	ARC_KVSET *set;

	assert(msg != NULL);

	if (type == ARC_KVSETTYPE_ANY)
		return msg->arc_kvsethead;

	for (set = msg->arc_kvsethead; set != NULL; set = set->set_next)
	{
		if (set->set_type == type)
			return set;
	}

	return NULL;
}

/*
**  ARC_SET_NEXT -- return next set in a context
**
**  Parameters:
**  	set -- last set reported (i.e. starting point for this search)
**  	type -- type to find, or ARC_KVSETTYPE_ANY
**
**  Return value:
**  	Pointer to the next ARC_KVSET in the context, or NULL if none.
*/

static ARC_KVSET *
arc_set_next(ARC_KVSET *cur, arc_kvsettype_t type)
{
	ARC_KVSET *set;

	assert(cur != NULL);

	if (type == ARC_KVSETTYPE_ANY)
		return cur->set_next;

	for (set = cur->set_next; set != NULL; set = set->set_next)
	{
		if (set->set_type == type)
			return set;
	}

	return NULL;
}

/*
**  ARC_ADD_PLIST -- add an entry to a parameter-value set
**
**  Parameters:
**  	msg -- ARC message context in which this is performed
**  	set -- set to modify
**   	param -- parameter
**  	value -- value
**  	force -- override existing value, if any
**
**  Return value:
**  	0 on success, -1 on failure.
**
**  Notes:
**  	Data is not copied; a reference to it is stored.
*/

static int
arc_add_plist(ARC_MESSAGE *msg, ARC_KVSET *set, u_char *param, u_char *value,
              _Bool force)
{
	ARC_PLIST *plist;

	assert(msg != NULL);
	assert(set != NULL);
	assert(param != NULL);
	assert(value != NULL);

	if (!isprint(param[0]))
	{
		arc_error(msg, "invalid parameter '%s'", param);
		return -1;
	}

	/* see if we have one already */
	for (plist = set->set_plist[ARC_PHASH(param[0])];
	     plist != NULL;
	     plist = plist->plist_next)
	{
		if (strcasecmp((char *) plist->plist_param,
		               (char *) param) == 0)
			break;
	}

	/* nope; make one and connect it */
	if (plist == NULL)
	{
		int n;

		plist = (ARC_PLIST *) malloc(sizeof(ARC_PLIST));
		if (plist == NULL)
		{
			arc_error(msg, "unable to allocate %d byte(s)",
			          sizeof(ARC_PLIST));
			return -1;
		}
		force = TRUE;
		n = ARC_PHASH(param[0]);
		plist->plist_next = set->set_plist[n];
		set->set_plist[n] = plist;
		plist->plist_param = param;
	}

	/* set the value if "force" was set (or this was a new entry) */
	if (force)
		plist->plist_value = value;

	return 0;
}

/*
**  ARC_PROCESS_SET -- process a parameter set, i.e. a string of the form
**                     param=value[; param=value]*
**
**  Parameters:
**  	msg -- ARC_MESSAGE context in which this is performed
**  	type -- an ARC_KVSETTYPE constant
**  	str -- string to be scanned
**  	len -- number of bytes available at "str"
**
**  Return value:
**  	An ARC_STAT constant.
*/

ARC_STAT
arc_process_set(ARC_MESSAGE *msg, arc_kvsettype_t type, u_char *str, size_t len)
{
	_Bool spaced;
	int state;
	int status;
	u_char *p;
	u_char *param;
	u_char *value;
	u_char *hcopy;
	char *ctx;
	ARC_KVSET *set;
	const char *settype;

	assert(msg != NULL);
	assert(str != NULL);
	assert(type == ARC_KVSETTYPE_SEAL ||
	       type == ARC_KVSETTYPE_SIGNATURE ||
	       type == ARC_KVSETTYPE_AR ||
	       type == ARC_KVSETTYPE_KEY);

	param = NULL;
	value = NULL;
	state = 0;
	spaced = FALSE;

	hcopy = (u_char *) malloc(len + 1);
	if (hcopy == NULL)
	{
		arc_error(msg, "unable to allocate %d byte(s)", len + 1);
		return ARC_STAT_INTERNAL;
	}
	strlcpy((char *) hcopy, (char *) str, len + 1);

	set = (ARC_KVSET *) malloc(sizeof(ARC_KVSET));
	if (set == NULL)
	{
		free(hcopy);
		arc_error(msg, "unable to allocate %d byte(s)",
		          sizeof(ARC_KVSET));
		return ARC_STAT_INTERNAL;
	}

	set->set_type = type;
	settype = arc_code_to_name(settypes, type);

	if (msg->arc_kvsethead == NULL)
		msg->arc_kvsethead = set;
	else
		msg->arc_kvsettail->set_next = set;

	msg->arc_kvsettail = set;

	set->set_next = NULL;
	memset(&set->set_plist, '\0', sizeof set->set_plist);
	set->set_data = hcopy;
	set->set_bad = FALSE;

	for (p = hcopy; *p != '\0'; p++)
	{
		if (!isascii(*p) || (!isprint(*p) && !isspace(*p)))
		{
			arc_error(msg,
			          "invalid character (ASCII 0x%02x at offset %d) in %s data",
			          *p, p - hcopy, settype);
			set->set_bad = TRUE;
			return ARC_STAT_SYNTAX;
		}

		switch (state)
		{
		  case 0:				/* before param */
			if (isspace(*p))
			{
				continue;
			}
			else if (isalnum(*p))
			{
				param = p;
				state = 1;
			}
			else
			{
				arc_error(msg,
				          "syntax error in %s data (ASCII 0x%02x at offset %d)",
				          settype, *p, p - hcopy);
				set->set_bad = TRUE;
				return ARC_STAT_SYNTAX;
			}
			break;

		  case 1:				/* in param */
			if (isspace(*p))
			{
				spaced = TRUE;
			}
			else if (*p == '=')
			{
				*p = '\0';
				state = 2;
				spaced = FALSE;
			}
			else if (*p == ';' || spaced)
			{
				arc_error(msg,
				          "syntax error in %s data (ASCII 0x%02x at offset %d)",
				          settype, *p, p - hcopy);
				set->set_bad = TRUE;
				return ARC_STAT_SYNTAX;
			}
			break;

		  case 2:				/* before value */
			if (isspace(*p))
			{
				continue;
			}
			else if (*p == ';')		/* empty value */
			{
				*p = '\0';
				value = p;

				/* collapse the parameter */
				arc_collapse(param);

				/* create the ARC_PLIST entry */
				status = arc_add_plist(msg, set, param,
				                       value, TRUE);
				if (status == -1)
				{
					set->set_bad = TRUE;
					return ARC_STAT_INTERNAL;
				}

				/* reset */
				param = NULL;
				value = NULL;
				state = 0;
			}
			else
			{
				value = p;
				state = 3;
			}
			break;

		  case 3:				/* in value */
			if (*p == ';')
			{
				*p = '\0';

				/* collapse the parameter and value */
				arc_collapse(param);
				arc_collapse(value);

				/* create the ARC_PLIST entry */
				status = arc_add_plist(msg, set, param,
				                       value, TRUE);
				if (status == -1)
				{
					set->set_bad = TRUE;
					return ARC_STAT_INTERNAL;
				}

				/* reset */
				param = NULL;
				value = NULL;
				state = 0;
			}
			break;

		  default:				/* shouldn't happen */
			assert(0);
		}
	}

	switch (state)
	{
	  case 0:					/* before param */
	  case 3:					/* in value */
		/* parse the data found, if any */
		if (value != NULL)
		{
			/* collapse the parameter and value */
			arc_collapse(param);
			arc_collapse(value);

			/* create the ARC_PLIST entry */
			status = arc_add_plist(msg, set, param, value, TRUE);
			if (status == -1)
			{
				set->set_bad = TRUE;
				return ARC_STAT_INTERNAL;
			}
		}
		break;

	  case 2:					/* before value */
		/* create an empty ARC_PLIST entry */
		status = arc_add_plist(msg, set, param, (u_char *) "", TRUE);
		if (status == -1)
		{
			set->set_bad = TRUE;
			return ARC_STAT_INTERNAL;
		}
		break;

	  case 1:					/* after param */
		arc_error(msg, "tag without value at end of %s data",
		          settype);
		set->set_bad = TRUE;
		return ARC_STAT_SYNTAX;

	  default:					/* shouldn't happen */
		assert(0);
	}

	/* load up defaults, assert requirements */
	switch (set->set_type)
	{
	  case ARC_KVSETTYPE_SIGNATURE:
		/* make sure required stuff is here */
		if (arc_param_get(set, (u_char *) "s") == NULL ||
		    arc_param_get(set, (u_char *) "h") == NULL ||
		    arc_param_get(set, (u_char *) "d") == NULL ||
		    arc_param_get(set, (u_char *) "b") == NULL ||
		    arc_param_get(set, (u_char *) "v") == NULL ||
		    arc_param_get(set, (u_char *) "i") == NULL ||
		    arc_param_get(set, (u_char *) "a") == NULL)
		{
			arc_error(msg, "missing parameter(s) in %s data",
			          settype);
			set->set_bad = TRUE;
			return ARC_STAT_SYNTAX;
		}

		/* make sure nothing got signed that shouldn't be */
		p = arc_param_get(set, (u_char *) "h");
		hcopy = strdup(p);
		if (hcopy == NULL)
		{
			len = strlen(p);
			arc_error(msg, "unable to allocate %d byte(s)",
			          len + 1);
			set->set_bad = TRUE;
			return ARC_STAT_INTERNAL;
		}
		for (p = strtok_r(hcopy, ":", &ctx);
		     p != NULL;
		     p = strtok_r(NULL, ":", &ctx))
		{
			if (strcasecmp(p, ARC_AR_HDRNAME) == 0 ||
			    strcasecmp(p, ARC_MSGSIG_HDRNAME) == 0 ||
			    strcasecmp(p, ARC_SEAL_HDRNAME) == 0)
			{
				arc_error(msg, "ARC-Message-Signature signs %s",
				          p);
				set->set_bad = TRUE;
				return ARC_STAT_INTERNAL;
			}
		}

		/* test validity of "t", "x", and "i" */
		if (!arc_check_uint(arc_param_get(set, (u_char *) "t")))
		{
			arc_error(msg,
			          "invalid \"t\" value in %s data",
			          settype);
			set->set_bad = TRUE;
			return ARC_STAT_SYNTAX;
		}

		if (!arc_check_uint(arc_param_get(set, (u_char *) "x")))
		{
			arc_error(msg,
			          "invalid \"x\" value in %s data",
			          settype);
			set->set_bad = TRUE;
			return ARC_STAT_SYNTAX;
		}

		if (!arc_check_uint(arc_param_get(set, (u_char *) "i")))
		{
			arc_error(msg,
			          "invalid \"i\" value in %s data",
			          settype);
			set->set_bad = TRUE;
			return ARC_STAT_SYNTAX;
		}

		/* default for "q" */
		status = arc_add_plist(msg, set, (u_char *) "q",
		                       (u_char *) "dns/txt", FALSE);
		if (status == -1)
		{
			set->set_bad = TRUE;
			return ARC_STAT_INTERNAL;
		}

  		break;

	  case ARC_KVSETTYPE_KEY:
		status = arc_add_plist(msg, set, (u_char *) "k",
		                       (u_char *) "rsa", FALSE);
		if (status == -1)
		{
			set->set_bad = TRUE;
			return ARC_STAT_INTERNAL;
		}

		break;
			
	  default:
		assert(0);
	}

	return ARC_STAT_OK;
}

/*
**  ARC_MESSAGE -- create a new message handle
**
**  Parameters:
**  	lib -- containing library instance
**  	err -- error string (returned)
**
**  Return value:
**  	A new message instance, or NULL on failure (and "err" is updated).
*/

ARC_MESSAGE *
arc_message(ARC_LIB *lib, const u_char **err)
{
	ARC_MESSAGE *msg;

	msg = (ARC_MESSAGE *) malloc(sizeof *msg);
	if (msg == NULL)
	{
		*err = strerror(errno);
	}
	else
	{
		memset(msg, '\0', sizeof *msg);

		msg->arc_library = lib;
	}

	return msg;
}

/*
**  ARC_FREE -- deallocate a message object
**
**  Parameters:
**  	msg -- message object to be destroyed
**
**  Return value:
**  	None.
*/

void
arc_free(ARC_MESSAGE *msg)
{
	struct arc_hdrfield *h;
	struct arc_hdrfield *tmp;

	h = msg->arc_hhead;
	while (h != NULL)
	{
		tmp = h->hdr_next;
		free(h->hdr_text);
		free(h);
		h = tmp;
	}

	free(msg);
}

/*
**  ARC_HDRFIELD -- consume a header field
**
**  Parameters:
**  	msg -- message handle
**  	hdr -- full text of the header field
**  	hlen -- bytes to use at hname
**
**  Return value:
**  	An ARC_STAT_* constant.
*/

ARC_STAT
arc_header_field(ARC_MESSAGE *msg, u_char *hdr, size_t hlen)
{
	u_char *colon;
	u_char *semicolon;
	u_char *end = NULL;
	size_t c;
	struct arc_hdrfield *h;

	assert(msg != NULL);
	assert(hdr != NULL);
	assert(hlen != 0);

	if (msg->arc_state > ARC_STATE_HEADER)
		return ARC_STAT_INVALID;
	msg->arc_state = ARC_STATE_HEADER;

	/* enforce RFC 5322, Section 2.2 */
	colon = NULL;
	for (c = 0; c < hlen; c++)
	{
		if (colon == NULL)
		{
			/*
			**  Field names are printable ASCII; also tolerate
			**  plain whitespace.
			*/

			if (hdr[c] < 32 || hdr[c] > 126)
				return ARC_STAT_SYNTAX;

			/* the colon is special */
			if (hdr[c] == ':')
				colon = &hdr[c];
		}
		else
		{
			/* field bodies are printable ASCII, SP, HT, CR, LF */
			if (!(hdr[c] != 9 ||  /* HT */
			      hdr[c] != 10 || /* LF */
			      hdr[c] != 13 || /* CR */
			      (hdr[c] >= 32 && hdr[c] <= 126) /* SP, print */ ))
				return ARC_STAT_SYNTAX;
		}
	}

	if (colon == NULL)
		return ARC_STAT_SYNTAX;

	end = colon;

	while (end > hdr && isascii(*(end - 1)) && isspace(*(end - 1)))
		end--;

	/* don't allow a field name containing a semicolon */
	semicolon = memchr(hdr, ';', hlen);
	if (semicolon != NULL && colon != NULL && semicolon < colon)
		return ARC_STAT_SYNTAX;

	h = malloc(sizeof *h);
	if (h == NULL)
	{
		arc_error(msg, "unable to allocate %d byte(s)", sizeof *h);
		return ARC_STAT_NORESOURCE;
	}

	if ((msg->arc_library->arcl_flags & ARC_LIBFLAGS_FIXCRLF) != 0)
	{
		u_char prev = '\0';
		u_char *p;
		u_char *q;
		struct arc_dstring *tmphdr;

		tmphdr = arc_dstring_new(msg, BUFRSZ, MAXBUFRSZ);
		if (tmphdr == NULL)
		{
			free(h);
			return ARC_STAT_NORESOURCE;
		}

		q = hdr + hlen;

		for (p = hdr; p < q && *p != '\0'; p++)
		{
			if (*p == '\n' && prev != '\r')		/* bare LF */
			{
				arc_dstring_catn(tmphdr, CRLF, 2);
			}
			else if (prev == '\r' && *p != '\n')	/* bare CR */
			{
				arc_dstring_cat1(tmphdr, '\n');
				arc_dstring_cat1(tmphdr, *p);
			}
			else					/* other */
			{
				arc_dstring_cat1(tmphdr, *p);
			}

			prev = *p;
		}

		if (prev == '\r')				/* end CR */
			arc_dstring_cat1(tmphdr, '\n');

		h->hdr_text = arc_strndup(arc_dstring_get(tmphdr),
		                          arc_dstring_len(tmphdr));

		arc_dstring_free(tmphdr);
	}
	else
	{
		h->hdr_text = arc_strndup(hdr, hlen);
	}

	if (h->hdr_text == NULL)
	{
		free(h);
		return ARC_STAT_NORESOURCE;
	}

	h->hdr_namelen = end != NULL ? end - hdr : hlen;
	h->hdr_textlen = hlen;
	if (colon == NULL)
		h->hdr_colon = NULL;
	else
		h->hdr_colon = h->hdr_text + (colon - hdr);
	h->hdr_flags = 0;
	h->hdr_next = NULL;

	if (msg->arc_hhead == NULL)
	{
		msg->arc_hhead = h;
		msg->arc_htail = h;
	}
	else
	{
		msg->arc_htail->hdr_next = h;
		msg->arc_htail = h;
	}

	msg->arc_hdrcnt++;

	return ARC_STAT_OK;
}

/*
**  ARC_EOH -- declare no more header fields are coming
**
**  Parameters:
**  	msg -- message handle
**
**  Return value:
**  	An ARC_STAT_* constant.
*/

ARC_STAT
arc_eoh(ARC_MESSAGE *msg)
{
	u_int c;
	u_int n;
	u_int nsets = 0;
	ARC_STAT status;
	struct arc_hdrfield *h;
	ARC_KVSET *set;
	u_int *sets = NULL;
	u_char *inst;

	/*
	**  Process all the header fields that make up ARC sets.
	*/

	for (h = msg->arc_hhead; h != NULL; h = h->hdr_next)
	{
		char hnbuf[ARC_MAXHEADER + 1];

		memset(hnbuf, '\0', sizeof hnbuf);
		strncpy(hnbuf, h->hdr_text, h->hdr_namelen);
		if (strcasecmp(hnbuf, ARC_AR_HDRNAME) == 0 ||
		    strcasecmp(hnbuf, ARC_MSGSIG_HDRNAME) == 0 ||
		    strcasecmp(hnbuf, ARC_SEAL_HDRNAME) == 0)
		{
			arc_kvsettype_t kvtype;

			kvtype = arc_name_to_code(archdrnames, hnbuf);
			status = arc_process_set(msg, kvtype,
			                         h->hdr_colon + 1,
			                         h->hdr_textlen - h->hdr_namelen - 1);
			if (status != ARC_STAT_OK)
				return status;
		}
	}

	/*
	**  Ensure all sets are complete.
	*/

	/* walk the seals */
	for (set = arc_set_first(msg, ARC_KVSETTYPE_SEAL);
             set != NULL;
             set = arc_set_next(set, ARC_KVSETTYPE_SEAL))
	{
		inst = arc_param_get(set, "i");
		n = strtoul(inst, NULL, 10);
		if (n >= nsets)
		{
			u_int *newsets;

			if (nsets == 0)
			{
				newsets = (u_int *) malloc(n * sizeof(u_int));
			}
			else
			{
				newsets = (u_int *) realloc(sets,
				                            n * sizeof(u_int));
			}

			if (newsets == NULL)
			{
				arc_error(msg,
				          "unable to allocate %d byte(s)",
				          n * sizeof(u_int));
				if (sets != NULL)
					free(sets);
				return ARC_STAT_NORESOURCE;
			}

			memset(&newsets[n], '\0', (n + 1) * sizeof(u_int));
			nsets = n;
			sets = newsets;
			sets[n - 1] = 1;
		}
		else
		{
			if (sets[n - 1] != 0)
			{
				arc_error(msg,
				          "duplicate ARC seal at instance %u",
				          n);
				msg->arc_sigerror = ARC_SIGERROR_DUPINSTANCE;
				free(sets);
				return ARC_STAT_SYNTAX;
			}

			sets[n - 1] = 1;
		}
	}

	/* ensure there's a complete sequence */
	for (c = 0; c < n; c++)
	{
		if (sets[c] == 0)
		{
			arc_error(msg, "ARC seal gap at instance %u", c + 1);
			free(sets);
			return ARC_STAT_SYNTAX;
		}
	}

	/* make sure all the seals have signatures */
	memset(sets, '\0', nsets * sizeof(u_int));
	for (set = arc_set_first(msg, ARC_KVSETTYPE_SIGNATURE);
             set != NULL;
             set = arc_set_next(set, ARC_KVSETTYPE_SIGNATURE))
	{
		inst = arc_param_get(set, "i");
		n = strtoul(inst, NULL, 10);
		if (n > nsets)
		{
			arc_error(msg,
			          "ARC signature instance %u out of range",
			          n);
			free(sets);
			return ARC_STAT_SYNTAX;
		}

		if (sets[n] == 1)
		{
			arc_error(msg,
			          "duplicate ARC signature at instance %u",
			          n);
			free(sets);
			return ARC_STAT_SYNTAX;
		}

		sets[n] = 1;
	}

	for (c = 0; c < n; c++)
	{
		if (sets[c] == 0)
		{
			arc_error(msg, "ARC signature gap at instance %u",
			          c + 1);
			free(sets);
			return ARC_STAT_SYNTAX;
		}
	}

	/* make sure all the seals have A-Rs */
	memset(sets, '\0', nsets * sizeof(u_int));
	for (set = arc_set_first(msg, ARC_KVSETTYPE_AR);
             set != NULL;
             set = arc_set_next(set, ARC_KVSETTYPE_AR))
	{
		inst = arc_param_get(set, "i");
		n = strtoul(inst, NULL, 10);
		if (n > nsets)
		{
			arc_error(msg,
			          "ARC authentication results instance %u out of range",
			          n);
			free(sets);
			return ARC_STAT_SYNTAX;
		}

		if (sets[n] == 1)
		{
			arc_error(msg,
			          "duplicate ARC authentication results at instance %u",
			          n);
			free(sets);
			return ARC_STAT_SYNTAX;
		}

		sets[n] = 1;
	}

	for (c = 0; c < n; c++)
	{
		if (sets[c] == 0)
		{
			arc_error(msg,
			          "ARC authentication results gap at instance %u",
			          c + 1);
			free(sets);
			return ARC_STAT_SYNTAX;
		}
	}

	/* ...finally! */
	msg->arc_nsets = nsets;
	return ARC_STAT_OK;
}

/*
**  ARC_BODY -- process a body chunk
**
**  Parameters:
**  	msg -- an ARC message handle
**  	buf -- the body chunk to be processed, in canonical format
**  	len -- number of bytes to process starting at "buf"
**
**  Return value:
**  	A ARC_STAT_* constant.
*/

ARC_STAT
arc_body (ARC_MESSAGE *msg, u_char *buf, size_t len)
{
	assert(msg != NULL);
	assert(buf != NULL);

	if (msg->arc_state > ARC_STATE_BODY ||
	    msg->arc_state < ARC_STATE_EOH)
		return ARC_STAT_INVALID;
	msg->arc_state = ARC_STATE_BODY;

	return arc_canon_bodychunk(msg, buf, len);
}

/*
**  ARC_EOM -- declare end of message
**
**  Parameters:
**  	msg -- message handle
**
**  Return value:
**  	An ARC_STAT_* constant.
*/

ARC_STAT
arc_eom(ARC_MESSAGE *msg)
{
	ARC_CHAIN cstate = ARC_CHAIN_UNKNOWN;

	/* verify */
	if (msg->arc_nsets == 0)
	{
		cstate = ARC_CHAIN_NONE;
	}
	else
	{
		u_int set;

		if (arc_validate(msg, msg->arc_nsets - 1) == ARC_STAT_BADSIG)
		{
			cstate = ARC_CHAIN_FAIL;
		}
		else
		{
			u_char *inst;
			u_char *cv;
			ARC_KVSET *kvset;

			for (set = msg->arc_nsets - 2; set >= 0; set++)
			{
				for (kvset = arc_set_first(msg,
				                           ARC_KVSETTYPE_SEAL);
				    kvset != NULL;
				    kvset = arc_set_next(kvset,
				                         ARC_KVSETTYPE_SEAL))
				{
					inst = arc_param_get(kvset, "i");
					if (atoi(inst) == set)
						break;
				}

				cv = arc_param_get(kvset, "cv");
				if ((set == 0 && strcasecmp(cv, "none") == 0) ||
				    (set != 0 && strcasecmp(cv, "pass") == 0))
				{
					ARC_STAT status;

					status = arc_validate(msg, set);
					if (status == ARC_STAT_BADSIG)
					{
						cstate = ARC_CHAIN_FAIL;
						break;
					}
					else if (status != ARC_STAT_OK)
					{
						return status;
					}
				}
			}
		}

		cstate = ARC_CHAIN_PASS;
	}

	/* sign */
	return ARC_STAT_OK;
}

/*
**  ARC_GETSEAL -- get the "seal" to apply to this message
**
**  Parameters:
**  	msg -- ARC_MESSAGE object
**  	seal -- seal to apply (returned)
**      selector -- selector name
**      domain -- domain name
**      key -- secret key, printable
**      keylen -- key length
**
**  Return value:
**  	An ARC_STAT_* constant.
*/

ARC_STAT
arc_getseal(ARC_MESSAGE *msg, ARC_HDRFIELD **seal, char *selector,
            char *domain, u_char *key, size_t keylen)
{
	return ARC_STAT_OK;
}

/*
**  ARC_HDR_NAME -- extract name from an ARC_HDRFIELD
**
**  Parameters:
**  	hdr -- ARC_HDRFIELD object
**
**  Return value:
**  	Header field name stored in the object.
*/

u_char *
arc_hdr_name(ARC_HDRFIELD *hdr, size_t *len)
{
	if (len != NULL)
		*len = hdr->hdr_namelen;
	return hdr->hdr_text;
}

/*
**  ARC_HDR_VALUE -- extract value from an ARC_HDRFIELD
**
**  Parameters:
**  	hdr -- ARC_HDRFIELD object
**
**  Return value:
**  	Header field value stored in the object.
*/

u_char *
arc_hdr_value(ARC_HDRFIELD *hdr)
{
	return hdr->hdr_colon + 1;
}

/*
**  ARC_HDR_NEXT -- return pointer to next ARC_HDRFIELD
**
**  Parameters:
**  	hdr -- ARC_HDRFIELD object
**
**  Return value:
**  	Pointer to the next ARC_HDRFIELD in the sequence.
*/

ARC_HDRFIELD *
arc_hdr_next(ARC_HDRFIELD *hdr)
{
	return hdr->hdr_next;
}

/*
**  ARC_SSL_VERSION -- report the version of the crypto library against which
**  	the library was compiled, so the caller can ensure it matches
**
**  Parameters:
**  	None.
**
**  Return value:
**  	SSL library version, expressed as a uint64_t.
*/

uint64_t
arc_ssl_version(void)
{
	return 0;
}

/*
**  ARC_LIBFEATURE -- determine whether or not a particular library feature
**                    is actually available
**
**  Parameters:
**  	lib -- library handle
**  	fc -- feature code to check
**
**  Return value:
**  	TRUE iff the specified feature was compiled in
*/

_Bool
arc_libfeature(ARC_LIB *lib, u_int fc)
{
	u_int idx;
	u_int offset;

	idx = fc / (8 * sizeof(int));
	offset = fc % (8 * sizeof(int));

	if (idx > lib->arcl_flsize)
		return FALSE;
	return ((lib->arcl_flist[idx] & (1 << offset)) != 0);
}