/*
 * $Id$
 *
 * Passcode verification function (otpd client) for rlm_otp.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 * Copyright 2006,2007 TRI-D Systems, Inc.
 */

RCSID("$Id$")

#define LOG_PREFIX "rlm_otp - "

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>

#include "extern.h"
#include "otp.h"
#include "otp_pw_valid.h"

#include <pthread.h>
#include <sys/un.h>

static int otprc2rlmrc(int);
static int otp_verify(REQUEST *request, rlm_otp_t const *,
		      otp_request_t const *, otp_reply_t *);
static int otp_read(otp_fd_t *, char *, size_t);
static int otp_write(otp_fd_t *, char const *, size_t);
static int otp_connect(char const *);
static otp_fd_t *otp_getfd(rlm_otp_t const *);
static void otp_putfd(otp_fd_t *, int);

/* transform otpd return codes into rlm return codes */
static int otprc2rlmrc(int rc)
{
	switch (rc) {
	case OTP_RC_OK:			return RLM_MODULE_OK;
	case OTP_RC_USER_UNKNOWN:	return RLM_MODULE_REJECT;
	case OTP_RC_AUTHINFO_UNAVAIL:	return RLM_MODULE_REJECT;
	case OTP_RC_AUTH_ERR:		return RLM_MODULE_REJECT;
	case OTP_RC_MAXTRIES:		return RLM_MODULE_USERLOCK;
	case OTP_RC_NEXTPASSCODE:	return RLM_MODULE_USERLOCK;
	case OTP_RC_IPIN:		return RLM_MODULE_REJECT;
	case OTP_RC_SERVICE_ERR:	return RLM_MODULE_FAIL;
	default:			return RLM_MODULE_FAIL;
	}
}

static otp_fd_t *otp_fd_head;
static pthread_mutex_t otp_fd_head_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Test for passcode validity by asking otpd.
 *
 * If challenge is supplied, it is used to generate the card response
 * against which the passcode will be compared.  If challenge is not
 * supplied, or if the comparison fails, synchronous responses are
 * generated and tested.  NOTE: for async authentications, sync mode
 * responses are still considered valid!  (Assuming module configuration
 * allows sync mode.)
 *
 * Returns one of the RLM_MODULE_* codes.  passcode is filled in.
 * NB: The returned passcode will contain the PIN!  DO NOT LOG!
 */
int otp_pw_valid(REQUEST *request, int pwe, char const *challenge,
		 rlm_otp_t const *opt,
		 char passcode[OTP_MAX_PASSCODE_LEN + 1])
{
	otp_request_t	otp_request;
	otp_reply_t	otp_reply;
	VALUE_PAIR	*cvp, *rvp;
	char const	*username = request->username->vp_strvalue;
	int		rc;

	otp_request.version = 2;

	if (strlcpy(otp_request.username, username, sizeof(otp_request.username)) >= sizeof(otp_request.username)) {
		REDEBUG("Username \"%s\" too long", username);
		return RLM_MODULE_REJECT;
	}
	if (strlcpy(otp_request.challenge, challenge, sizeof(otp_request.challenge)) >= sizeof(otp_request.challenge)) {
		REDEBUG("Challenge too long");
		return RLM_MODULE_REJECT;
	}

	otp_request.pwe.pwe = pwe;

	/*
	 *	otp_pwe_present() (done by caller) guarantees that both of
	 *	these exist
	 */
	cvp = fr_pair_find_by_num(request->packet->vps, pwattr[pwe - 1]->vendor, pwattr[pwe - 1]->attr, TAG_ANY);

	rvp = fr_pair_find_by_num(request->packet->vps, pwattr[pwe]->vendor, pwattr[pwe]->attr, TAG_ANY);

	/* this is just to quiet Coverity */
	if (!rvp || !cvp) {
		return RLM_MODULE_REJECT;
	}

	/*
	 *	Validate available vps based on pwe type.
	 *	Unfortunately (?) otpd must do this also.
	 */
	switch (otp_request.pwe.pwe) {
	case PWE_NONE:
		return RLM_MODULE_NOOP;

	case PWE_PAP:
		if (strlcpy(otp_request.pwe.u.pap.passcode, rvp->vp_strvalue, sizeof(otp_request.pwe.u.pap.passcode)) >=
		    sizeof(otp_request.pwe.u.pap.passcode)) {
			REDEBUG("Passcode too long");

			return RLM_MODULE_REJECT;
		}
		break;

	case PWE_CHAP:
		if (cvp->vp_length > 16) {
			REDEBUG("CHAP challenge too long");

			return RLM_MODULE_INVALID;
		}

		if (rvp->vp_length != 17) {
			REDEBUG("CHAP response wrong size");

			return RLM_MODULE_INVALID;
		}

		(void)memcpy(otp_request.pwe.u.chap.challenge, cvp->vp_octets, cvp->vp_length);

		otp_request.pwe.u.chap.clen = cvp->vp_length;
		(void)memcpy(otp_request.pwe.u.chap.response, rvp->vp_octets, rvp->vp_length);

		otp_request.pwe.u.chap.rlen = rvp->vp_length;
		break;

	case PWE_MSCHAP:
		if (cvp->vp_length != 8) {
			REDEBUG("MS-CHAP challenge wrong size");

			return RLM_MODULE_INVALID;
		}

		if (rvp->vp_length != 50) {
			REDEBUG("MS-CHAP response wrong size");

			return RLM_MODULE_INVALID;
		}
		(void) memcpy(otp_request.pwe.u.chap.challenge,
			      cvp->vp_octets, cvp->vp_length);

		otp_request.pwe.u.chap.clen = cvp->vp_length;

		(void) memcpy(otp_request.pwe.u.chap.response,
			      rvp->vp_octets, rvp->vp_length);

		otp_request.pwe.u.chap.rlen = rvp->vp_length;
		break;

	case PWE_MSCHAP2:
		if (cvp->vp_length != 16) {
			REDEBUG("MS-CHAP2 challenge wrong size");

			return RLM_MODULE_INVALID;
		}

		if (rvp->vp_length != 50) {
			REDEBUG("MS-CHAP2 response for wrong size");

			return RLM_MODULE_INVALID;
		}

		(void)memcpy(otp_request.pwe.u.chap.challenge, cvp->vp_octets, cvp->vp_length);

		otp_request.pwe.u.chap.clen = cvp->vp_length;

		(void) memcpy(otp_request.pwe.u.chap.response, rvp->vp_octets, rvp->vp_length);
		otp_request.pwe.u.chap.rlen = rvp->vp_length;
		break;
	} /* switch (otp_request.pwe.pwe) */

	/*
	 *	last byte must also be a terminator so otpd can verify
	 *	length easily.
	 */
	otp_request.username[OTP_MAX_USERNAME_LEN] = '\0';
	otp_request.challenge[OTP_MAX_CHALLENGE_LEN] = '\0';

	if (otp_request.pwe.pwe == PWE_PAP) {
		otp_request.pwe.u.pap.passcode[OTP_MAX_PASSCODE_LEN] = '\0';
	}

	otp_request.allow_sync = opt->allow_sync;
	otp_request.allow_async = opt->allow_async;
	otp_request.challenge_delay = opt->challenge_delay;
	otp_request.resync = 1;


	rc = otp_verify(request, opt, &otp_request, &otp_reply);
	if (rc == OTP_RC_OK) {
		(void) strcpy(passcode, otp_reply.passcode);
	}

	return otprc2rlmrc(rc);
}

/*
 * Verify an otp by asking otpd.
 * Returns an OTP_* code, or -1 on system failure.
 * Fills in reply.
 */
static int otp_verify(REQUEST *request, rlm_otp_t const *opt,
		      otp_request_t const *otp_request, otp_reply_t *reply)
{
	otp_fd_t *fdp;
	int rc;
	int tryagain = 2;

	retry:
	if (!tryagain--) {
		return -1;
	}

	fdp = otp_getfd(opt);
	if (!fdp || fdp->fd == -1) {
		return -1;
	}

	rc = otp_write(fdp, (char const *) otp_request, sizeof(*otp_request));
	if (rc != sizeof(*otp_request)) {
		if (rc == 0) {
			goto retry;	/* otpd disconnect */	/*TODO: pause */
		} else {
			return -1;
		}
	}

	rc = otp_read(fdp, (char *) reply, sizeof(*reply));
	if (rc != sizeof(*reply)) {
		if (rc == 0) {
			goto retry;	/* otpd disconnect */	/*TODO: pause */
		} else {
			return -1;
		}
	}

	/* validate the reply */
	if (reply->version != 1) {
		REDEBUG("Invalid (version %d != 1)", reply->version);
		otp_putfd(fdp, 1);

		return -1;
	}

	if (reply->passcode[OTP_MAX_PASSCODE_LEN] != '\0') {
		REDEBUG("Invalid (passcode)");
		otp_putfd(fdp, 1);

		return -1;
	}

	otp_putfd(fdp, 0);
	return reply->rc;
}

/*
 * Full read with logging, and close on failure.
 * Returns nread on success, 0 on EOF, -1 on other failures.
 */
static int otp_read(otp_fd_t *fdp, char *buf, size_t len)
{
	ssize_t n;
	size_t nread = 0;	/* bytes read into buf */

	while (nread < len) {
		n = read(fdp->fd, &buf[nread], len - nread);
		if (n == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				ERROR("%s: read from otpd: %s", __func__, fr_syserror(errno));
				otp_putfd(fdp, 1);

				return -1;
			}
		}

		if (!n) {
			ERROR("%s: otpd disconnect", __func__);
			otp_putfd(fdp, 1);

			return 0;
		}

		nread += n;
	} /* while (more to read) */

	return nread;
}

/*
 *	Full write with logging, and close on failure.
 *	Returns number of bytes written on success, errno on failure.
 */
static int otp_write(otp_fd_t *fdp, char const *buf, size_t len)
{
	size_t nleft = len;
	ssize_t nwrote;

	while (nleft) {
		nwrote = write(fdp->fd, &buf[len - nleft], nleft);
		if (nwrote == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				ERROR("%s: write to otpd: %s", __func__, fr_syserror(errno));
				otp_putfd(fdp, 1);

				return errno;

			}
		}

		nleft -= nwrote;
	}

	return len - nleft;
}

/* connect to otpd and return fd */
static int otp_connect(char const *path)
{
	int fd;
	struct sockaddr_un sa;
	size_t sp_len;		/* sun_path length (strlen) */

	/* setup for unix domain socket */
	sp_len = strlen(path);
	if (sp_len > sizeof(sa.sun_path) - 1) {
		ERROR("%s: rendezvous point name too long", __func__);

		return -1;
	}
	sa.sun_family = AF_UNIX;
	(void) strcpy(sa.sun_path, path);

	/* connect to otpd */
	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		ERROR("%s: socket: %s", __func__, fr_syserror(errno));

		return -1;
	}

	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa.sun_family) + sp_len) == -1) {
		ERROR("%s: connect(%s): %s", __func__, path, fr_syserror(errno));
		(void)close(fd);

		return -1;
	}

	return fd;
}

/*
 * Retrieve an fd (from pool) to use for otpd connection.
 * It'd be simpler to use TLS but FR can have lots of threads
 * and we don't want to waste fd's that way.
 * We can't have a global fd because we'd then be pipelining
 * requests to otpd and we have no way to demultiplex
 * the responses.
 */
static otp_fd_t * otp_getfd(rlm_otp_t const *opt)
{
	int rc;
	otp_fd_t *fdp;

	/* walk the connection pool looking for an available fd */
	for (fdp = otp_fd_head; fdp; fdp = fdp->next) {
		rc = otp_pthread_mutex_trylock(&fdp->mutex);
		if (!rc) {
			if (!strcmp(fdp->path, opt->otpd_rp)) {	/* could just use == */
				break;
			}
		}
	}

	if (!fdp) {
		/* no fd was available, add a new one */
		fdp = rad_malloc(sizeof(*fdp));
		otp_pthread_mutex_init(&fdp->mutex, NULL);
		otp_pthread_mutex_lock(&fdp->mutex);

		/* insert new fd at head */
		otp_pthread_mutex_lock(&otp_fd_head_mutex);
		fdp->next = otp_fd_head;
		otp_fd_head = fdp;
		otp_pthread_mutex_unlock(&otp_fd_head_mutex);

		/* initialize */
		fdp->path = opt->otpd_rp;
		fdp->fd = -1;
	}

	/* establish connection */
	if (fdp->fd == -1) {
		fdp->fd = otp_connect(fdp->path);
	}

	return fdp;
}

/* release fd, and optionally disconnect from otpd */
static void otp_putfd(otp_fd_t *fdp, int disconnect)
{
	if (disconnect) {
		(void) close(fdp->fd);
		fdp->fd = -1;
	}

	/* make connection available to another thread */
	otp_pthread_mutex_unlock(&fdp->mutex);
}
