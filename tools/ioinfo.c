/*
 *  ioinfo - A program for connecting gensios.
 *  Copyright (C) 2019  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 */


#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "ioinfo.h"

struct ioinfo {
    struct gensio *io;
    struct ioinfo *otherio;
    struct gensio_os_funcs *o;
    bool ready;

    int escape_char;
    bool in_escape;
    char escape_data[11];
    unsigned int escape_pos;

    struct ioinfo_sub_handlers *sh;
    void *subdata;

    struct ioinfo_user_handlers *uh;
    void *userdata;

    struct ioinfo_oob *oob_head;
    struct ioinfo_oob *oob_tail;
};

void
ioinfo_out(struct ioinfo *ioinfo, char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ioinfo->uh->out(ioinfo, fmt, ap);
    va_end(ap);
}

void
ioinfo_err(struct ioinfo *ioinfo, char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ioinfo->uh->err(ioinfo, fmt, ap);
    va_end(ap);
}

struct gensio *
ioinfo_otherio(struct ioinfo *ioinfo)
{
    return ioinfo->otherio->io;
}

void *
ioinfo_subdata(struct ioinfo *ioinfo)
{
    return ioinfo->subdata;
}

void *
ioinfo_othersubdata(struct ioinfo *ioinfo)
{
    return ioinfo->otherio->subdata;
}

void *
ioinfo_userdata(struct ioinfo *ioinfo)
{
    return ioinfo->userdata;
}

struct ioinfo *
ioinfo_otherioinfo(struct ioinfo *ioinfo)
{
    return ioinfo->otherio;
}

void
ioinfo_sendoob(struct ioinfo *ioinfo, struct ioinfo_oob *oobinfo)
{
    oobinfo->next = NULL;
    if (ioinfo->oob_tail)
	ioinfo->oob_tail->next = oobinfo;
    else
	ioinfo->oob_head = oobinfo;
    ioinfo->oob_tail = oobinfo;
    gensio_set_write_callback_enable(ioinfo->io, true);
}

static bool
handle_escapechar(struct ioinfo *ioinfo, char c)
{
    bool rv = false;

    if (ioinfo->escape_pos > 0) {
	/* We are getting a multichar escape from the input. */
	if (c == '\r' || c == '\n') {
	    ioinfo->escape_data[ioinfo->escape_pos++] = '\0';
	    if (ioinfo->escape_pos > 1 && ioinfo->sh)
		ioinfo->sh->handle_multichar_escape(ioinfo,
						    ioinfo->escape_data);
	    ioinfo_out(ioinfo, ">", 1);
	    ioinfo->escape_pos = 0;
	    return false;
	}
	if (c == '\b' || c == 0x7f) {
	    if (ioinfo->escape_pos > 1) {
		ioinfo->escape_pos--;
		ioinfo_out(ioinfo, "\b \b", 3);
	    }
	    return true;
	}

	ioinfo_out(ioinfo, &c, 1);
	if (ioinfo->escape_pos < sizeof(ioinfo->escape_data) - 1)
	    ioinfo->escape_data[ioinfo->escape_pos++] = c;
	return true;
    }

    c = tolower(c);

    if (c == 'q') {
	ioinfo->uh->shutdown(ioinfo, true);
	return false;
    }

    if (!ioinfo->otherio->ready)
	return false;

    if (c == 'b') { /* Send a break */
	gensio_control(ioinfo->otherio->io, 0, false,
		       GENSIO_CONTROL_SEND_BREAK, NULL, NULL);
	return false;
    }

    if (ioinfo->sh) {
	rv = ioinfo->sh->handle_escape(ioinfo, c);
	if (rv) {
	    ioinfo_out(ioinfo, "<", 1);
	    ioinfo->escape_data[0] = c;
	    ioinfo->escape_pos = 1;
	}
    }
    return rv;
}

static int
io_event(struct gensio *io, void *user_data, int event, int err,
	 unsigned char *buf, gensiods *buflen,
	 const char *const *auxdata)
{
    struct ioinfo *ioinfo = user_data;
    struct ioinfo *rioinfo = ioinfo->otherio;
    int rv, escapepos = -1;
    gensiods count = 0;
    static const char *oobaux[2] = { "oob", NULL };

    if (err) {
	if (err != GE_REMCLOSE) {
	    ioinfo_err(ioinfo, "read error: %s", gensio_err_to_str(err));
	    ioinfo->uh->shutdown(ioinfo, false);
	} else {
	    ioinfo->uh->shutdown(ioinfo, true);
	}
	return 0;
    }

    switch(event) {
    case GENSIO_EVENT_READ:
	if (*buflen == 0)
	    return 0;

	if (gensio_str_in_auxdata(auxdata, "oob")) {
	    if (ioinfo->uh->oobdata)
		ioinfo->uh->oobdata(ioinfo, buf, buflen);
	    return 0;
	}

	if (ioinfo->escape_char >= 0) {
	    unsigned int i;

	    if (ioinfo->in_escape) {
		if (ioinfo->escape_pos == 0 && buf[0] == ioinfo->escape_char) {
		    /* double escape means send one escape char. */
		    ioinfo->in_escape = false;
		} else {
		    ioinfo->in_escape = handle_escapechar(ioinfo, buf[0]);
		    *buflen = 1;
		    return 0;
		}
	    } else {
		for (i = 0; i < *buflen; i++) {
		    if (buf[i] == ioinfo->escape_char) {
			escapepos = i;
			*buflen = i;
			break;
		    }
		}
	    }
	}
	if (rioinfo->ready) {
	    rv = gensio_write(rioinfo->io, &count, buf, *buflen, NULL);
	    if (rv) {
		if (rv != GE_REMCLOSE)
		    ioinfo_err(rioinfo, "write error: %s",
			       gensio_err_to_str(rv));
		ioinfo->uh->shutdown(ioinfo, rv == GE_REMCLOSE);
		return 0;
	    }
	} else {
	    /*
	     * The remote end isn't ready, cause the read to be
	     * disabled for now.  The remote gensio coming ready will
	     * enable read again.
	     */
	    count = 0;
	}
	if (count < *buflen) {
	    *buflen = count;
	    gensio_set_read_callback_enable(ioinfo->io, false);
	    if (rioinfo->ready)
		gensio_set_write_callback_enable(rioinfo->io, true);
	} else if (escapepos >= 0) {
	    /*
	     * Don't do this if we didn't handle all the characters, get
	     * it the next time characters are handled.
	     */
	    (*buflen)++;
	    ioinfo->in_escape = true;
	    ioinfo->escape_pos = 0;
	}
	return 0;

    case GENSIO_EVENT_WRITE_READY:
	if (ioinfo->oob_head) {
	    struct ioinfo_oob *oob = ioinfo->oob_head;
	    gensiods count;

	    rv = gensio_write(ioinfo->io, &count, oob->buf, oob->len, oobaux);
	    if (rv) {
		ioinfo_err(rioinfo, "write error: %s", gensio_err_to_str(rv));
		ioinfo->uh->shutdown(ioinfo, false);
		return 0;
	    }
	    if (count >= oob->len) {
		if (oob->send_done)
		    oob->send_done(oob->cb_data);
		ioinfo->oob_head = oob->next;
		if (ioinfo->oob_head == NULL)
		    /* No more OOB data. */
		    ioinfo->oob_tail = NULL;
	    } else {
		oob->buf += count;
		oob->len -= count;
	    }
	    return 0;
	}

	gensio_set_read_callback_enable(rioinfo->io, true);
	gensio_set_write_callback_enable(ioinfo->io, false);
	return 0;

    default:
	break;
    }

    if (!rioinfo->ready)
	return 0;

    rv = GE_NOTSUP;
    if (ioinfo->sh)
	rv = ioinfo->sh->handle_event(io, event, buf, buflen);

    if (rv == GE_NOTSUP && ioinfo->uh->event)
	rv = ioinfo->uh->event(ioinfo, io, event, err, buf, buflen, auxdata);

    return rv;
}

void
ioinfo_set_ready(struct ioinfo *ioinfo, struct gensio *io)
{
    struct ioinfo *rioinfo = ioinfo->otherio;

    ioinfo->io = io;
    gensio_set_callback(io, io_event, ioinfo);
    gensio_set_read_callback_enable(ioinfo->io, true);
    ioinfo->ready = true;
    if (rioinfo->ready)
	gensio_set_read_callback_enable(rioinfo->io, true);
}

void
ioinfo_set_otherioinfo(struct ioinfo *ioinfo, struct ioinfo *otherioinfo)
{
    ioinfo->otherio = otherioinfo;
    otherioinfo->otherio = ioinfo;
}

struct ioinfo *
alloc_ioinfo(struct gensio_os_funcs *o,
	     int escape_char,
	     struct ioinfo_sub_handlers *sh, void *subdata,
	     struct ioinfo_user_handlers *uh, void *userdata)
{
    struct ioinfo *ioinfo;

    ioinfo = malloc(sizeof(*ioinfo));
    if (ioinfo) {
	memset(ioinfo, 0, sizeof(*ioinfo));
	ioinfo->escape_char = escape_char;
	ioinfo->o = o;
	ioinfo->sh = sh;
	ioinfo->subdata = subdata;
	ioinfo->uh = uh;
	ioinfo->userdata = userdata;
    }
    return ioinfo;
}

void
free_ioinfo(struct ioinfo *ioinfo)
{
    free(ioinfo);
}
