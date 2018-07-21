/* $FreeBSD$ */
/* $NetBSD$ */
/*
 * Copyright (c) 2018 Pierre Pronchery <khorben@defora.org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE DEVELOPERS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE DEVELOPERS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */



#include <sys/endian.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <dev/usb/mbim.h>
#include <dev/usb/if_umbreg.h>



/* constants */
static const struct umb_valdescr _umb_regstate[] =
	MBIM_REGSTATE_DESCRIPTIONS;

static const struct umb_valdescr _umb_dataclass[] =
	MBIM_DATACLASS_DESCRIPTIONS;

static const struct umb_valdescr _umb_state[] =
	UMB_INTERNAL_STATE_DESCRIPTIONS;

static const struct umb_valdescr _umb_regmode[] =
{
	{ MBIM_REGMODE_UNKNOWN, "unknown" },
	{ MBIM_REGMODE_AUTOMATIC, "automatic" },
	{ MBIM_REGMODE_MANUAL, "manual" },
	{ 0, NULL }
};

static const struct umb_valdescr _umb_ber[] =
{
	{ UMB_BER_EXCELLENT, "excellent" },
	{ UMB_BER_VERYGOOD, "very good" },
	{ UMB_BER_GOOD, "good" },
	{ UMB_BER_OK, "ok" },
	{ UMB_BER_MEDIUM, "medium" },
	{ UMB_BER_BAD, "bad" },
	{ UMB_BER_VERYBAD, "very bad" },
	{ UMB_BER_EXTREMELYBAD, "extremely bad" },
	{ 0, NULL }
};


/* prototypes */
static int _char_to_utf16(const char * in, uint16_t * out, size_t outlen);
static int _error(int ret, char const * format, ...);
static int _umbctl(char const * ifname, int verbose, int argc, char * argv[]);
static int _umbctl_file(char const * ifname, char const * filename, int verbose,
		int argc, char * argv[]);
static void _umbctl_info(char const * ifname, struct umb_info * umbi);
static int _umbctl_ioctl(char const * ifname, int fd, unsigned long request,
		struct ifreq * ifr);
static int _umbctl_set(char const * ifname, struct umb_parameter * umbp,
		int argc, char * argv[]);
static int _umbctl_socket(void);
static int _usage(void);
static void _utf16_to_char(uint16_t *in, int inlen, char *out, size_t outlen);


/* functions */
/* this function is from OpenBSD's ifconfig(8) */
static int _char_to_utf16(const char * in, uint16_t * out, size_t outlen)
{
	int	n = 0;
	uint16_t c;

	for (;;) {
		c = *in++;

		if (c == '\0') {
			/*
			 * NUL termination is not required, but zero out the
			 * residual buffer
			 */
			memset(out, 0, outlen);
			return n;
		}
		if (outlen < sizeof(*out))
			return -1;

		*out++ = htole16(c);
		n += sizeof(*out);
		outlen -= sizeof(*out);
	}
}


static int _error(int ret, char const * format, ...)
{
	va_list ap;

	fputs("umbctl: ", stderr);
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputs("\n", stderr);
	return ret;
}


static int _umbctl(char const * ifname, int verbose, int argc, char * argv[])
{
	int fd;
	struct ifreq ifr;
	struct umb_info umbi;
	struct umb_parameter umbp;

	if((fd = _umbctl_socket()) < 0)
		return 2;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if(argc != 0)
	{
		memset(&umbp, 0, sizeof(umbp));
		ifr.ifr_data = &umbp;
		if(_umbctl_ioctl(ifname, fd, SIOCGUMBPARAM, &ifr) != 0
				|| _umbctl_set(ifname, &umbp, argc, argv) != 0
				|| _umbctl_ioctl(ifname, fd, SIOCSUMBPARAM,
					&ifr) != 0)
		{
			close(fd);
			return 2;
		}
	}
	if(argc == 0 || verbose > 0)
	{
		ifr.ifr_data = &umbi;
		if(_umbctl_ioctl(ifname, fd, SIOCGUMBINFO, &ifr) != 0)
		{
			close(fd);
			return 3;
		}
		_umbctl_info(ifname, &umbi);
	}
	if(close(fd) != 0)
		return _error(2, "%s: %s", ifname, strerror(errno));
	return 0;
}


static int _umbctl_file(char const * ifname, char const * filename, int verbose,
		int argc, char * argv[])
{
	FILE * fp;
	struct umb_parameter umbp;
	char buf[512];
	int eof;
	char * tokens[3];
	char * p;
	char * last;
	size_t i;

	if((fp = fopen(filename, "r")) == NULL)
		return _error(2, "%s: %s", filename, strerror(errno));
	memset(&umbp, 0, sizeof(umbp));
	while(fgets(buf, sizeof(buf), fp) != NULL)
	{
		if(buf[0] == '#')
			continue;
		buf[sizeof(buf) - 1] = '\0';
		for(i = 0, p = strtok_r(buf, " ", &last); i < 3 && p != NULL;
				i++, p = strtok_r(NULL, " ", &last))
			tokens[i] = p;
		tokens[i] = NULL;
		if(_umbctl_set(ifname, &umbp, i, tokens) != 0)
			break;
	}
	eof = feof(fp);
	if(fclose(fp) != 0 || !eof)
		return _error(2, "%s: %s", filename, strerror(errno));
	/* FIXME really apply the parameters */
	_umbctl(ifname, verbose, argc, argv);
	return 0;
}


static void _umbctl_info(char const * ifname, struct umb_info * umbi)
{
	char provider[UMB_PROVIDERNAME_MAXLEN + 1];
	char pn[UMB_PHONENR_MAXLEN + 1];
	char roaming[UMB_ROAMINGTEXT_MAXLEN + 1];
	char apn[UMB_APN_MAXLEN + 1];
	char fwinfo[UMB_FWINFO_MAXLEN + 1];
	char hwinfo[UMB_HWINFO_MAXLEN + 1];

	_utf16_to_char(umbi->provider, UMB_PROVIDERNAME_MAXLEN,
			provider, sizeof(provider));
	_utf16_to_char(umbi->pn, UMB_PHONENR_MAXLEN, pn, sizeof(pn));
	_utf16_to_char(umbi->roamingtxt, UMB_ROAMINGTEXT_MAXLEN,
			roaming, sizeof(roaming));
	_utf16_to_char(umbi->apn, UMB_APN_MAXLEN, apn, sizeof(apn));
	_utf16_to_char(umbi->fwinfo, UMB_FWINFO_MAXLEN, fwinfo, sizeof(fwinfo));
	_utf16_to_char(umbi->hwinfo, UMB_HWINFO_MAXLEN, hwinfo, sizeof(hwinfo));
	printf("%s: state %s, mode %s, registration %s\n"
			"\tprovider \"%s\", dataclass %s, signal %s\n"
			"\tphone number \"%s\", roaming \"%s\" (%s)\n"
			"\tAPN \"%s\", TX %" PRIu64 ", RX %" PRIu64 "\n"
			"\tfirmware \"%s\", hardware \"%s\"\n",
			ifname, umb_val2descr(_umb_state, umbi->state),
			umb_val2descr(_umb_regmode, umbi->regmode),
			umb_val2descr(_umb_regstate, umbi->regstate), provider,
			umb_val2descr(_umb_dataclass, umbi->cellclass),
			umb_val2descr(_umb_ber, umbi->ber), pn, roaming,
			umbi->enable_roaming ? "allowed" : "denied",
			apn, umbi->uplink_speed, umbi->downlink_speed,
			fwinfo, hwinfo);
}


static int _umbctl_ioctl(char const * ifname, int fd, unsigned long request,
		struct ifreq * ifr)
{
	if(ioctl(fd, request, ifr) != 0)
		return _error(-1, "%s: %s", ifname, strerror(errno));
	return 0;
}


static int _umbctl_set(char const * ifname, struct umb_parameter * umbp,
		int argc, char * argv[])
{
	int i;

	for(i = 0; i < argc; i++)
	{
		if(strcmp(argv[i], "apn") == 0 && i + 1 < argc)
		{
			umbp->apnlen = _char_to_utf16(argv[i + 1],
					umbp->apn, sizeof(umbp->apn));
			if(umbp->apnlen < 0 || (size_t)umbp->apnlen
					> sizeof(umbp->apn))
				return _error(-1, "%s: %s", ifname,
						"APN too long");
			i++;
		}
		else if(strcmp(argv[i], "username") == 0 && i + 1 < argc)
		{
			umbp->usernamelen = _char_to_utf16(argv[i + 1],
					umbp->username, sizeof(umbp->username));
			if(umbp->usernamelen < 0 || (size_t)umbp->usernamelen
					> sizeof(umbp->username))
				return _error(-1, "%s: %s", ifname,
						"Username too long");
			i++;
		}
		else if(strcmp(argv[i], "password") == 0 && i + 1 < argc)
		{
			umbp->passwordlen = _char_to_utf16(argv[i + 1],
					umbp->password, sizeof(umbp->password));
			if(umbp->passwordlen < 0 || (size_t)umbp->passwordlen
					> sizeof(umbp->password))
				return _error(-1, "%s: %s", ifname,
						"Password too long");
			i++;
		}
		else if(strcmp(argv[i], "pin") == 0 && i + 1 < argc)
		{
			umbp->is_puk = 0;
			umbp->op = MBIM_PIN_OP_ENTER;
			umbp->pinlen = _char_to_utf16(argv[i + 1], umbp->pin,
					sizeof(umbp->pin));
			if(umbp->pinlen < 0 || (size_t)umbp->pinlen
					> sizeof(umbp->pin))
				return _error(-1, "%s: %s", ifname,
						"PUK code too long");
			i++;
		}
		else if(strcmp(argv[i], "puk") == 0 && i + 1 < argc)
		{
			umbp->is_puk = 1;
			umbp->op = MBIM_PIN_OP_ENTER;
			umbp->pinlen = _char_to_utf16(argv[i + 1], umbp->pin,
					sizeof(umbp->pin));
			if(umbp->pinlen < 0 || (size_t)umbp->pinlen
					> sizeof(umbp->pin))
				return _error(-1, "%s: %s", ifname,
						"PIN code too long");
			i++;
		}
		else
			return _error(-1, "%s: Unknown or incomplete parameter",
					argv[i]);
	}
	return 0;
}


static int _umbctl_socket(void)
{
	int fd;

	if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return _error(-1, "socket: %s", strerror(errno));
	return fd;
}


static int _usage(void)
{
	fputs("Usage: umbctl [-v] ifname [parameter[=value]] [...]\n"
"       umbctl -f config-file ifname [...]\n",
			stderr);
	return 1;
}


static void _utf16_to_char(uint16_t *in, int inlen, char *out, size_t outlen)
{
	uint16_t c;

	while (outlen > 0) {
		c = inlen > 0 ? htole16(*in) : 0;
		if (c == 0 || --outlen == 0) {
			/* always NUL terminate result */
			*out = '\0';
			break;
		}
		*out++ = isascii(c) ? (char)c : '?';
		in++;
		inlen--;
	}
}


int main(int argc, char * argv[])
{
	int o;
	char const * filename = NULL;
	int verbose = 0;

	while((o = getopt(argc, argv, "f:v")) != -1)
		switch(o)
		{
			case 'f':
				filename = optarg;
				break;
			case 'v':
				verbose++;
				break;
			default:
				return _usage();
		}
	if(optind == argc)
		return _usage();
	if(filename != NULL)
		return _umbctl_file(argv[optind], filename, verbose,
				argc - optind - 1, &argv[optind + 1]);
	return _umbctl(argv[optind], verbose, argc - optind - 1,
			&argv[optind + 1]);
}
