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



#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <dev/usb/mbim.h>
#include <dev/usb/if_umbreg.h>




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


static int _char_to_utf16(const char *in, uint16_t *out, size_t outlen);
static int _error(int ret, char const * message);
static int _print_info(char const * ifname, struct umb_parameter * mp);
static char const * _print_stars(size_t count);
static int _umbctl(char const * ifname, char const * apn,
		char const * username, char const * password, int is_puk,
		char const * pin);
static int _umbctl_info(char const * ifname);
static int _umbctl_socket(void);
static int _usage(void);
static void _utf16_to_char(uint16_t *in, int inlen, char *out, size_t outlen);

/* this function is from OpenBSD's ifconfig(8) */
static int _char_to_utf16(const char *in, uint16_t *out, size_t outlen)
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

static int _error(int ret, char const * message)
{
	fputs("umbctl: ", stderr);
	perror(message);
	return ret;
}

static int _print_info(char const * ifname, struct umb_parameter * mp)
{
	printf("%s: %s %s, roaming %d, classes %u\n", ifname,
			mp->is_puk ? "PUK" : "PIN",
			_print_stars(mp->pinlen), mp->roaming,
			mp->preferredclasses);
	return 0;
}

static char const * _print_stars(size_t count)
{
	static char buf[MBIM_PIN_MAXLEN + 1];
	size_t i;

	for(i = 0; i < count && i < sizeof(buf) - 1; i++)
		buf[i] = '*';
	buf[i] = '\0';
	return buf;
}

static int _umbctl(char const * ifname, char const * apn,
		char const * username, char const * password, int is_puk,
		char const * pin)
{
	int fd;
	struct ifreq ifr;
	struct umb_parameter mp;

	if((fd = _umbctl_socket()) < 0)
		return 2;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	memset(&mp, 0, sizeof(mp));
	ifr.ifr_data = &mp;
	if(ioctl(fd, SIOCGUMBPARAM, &ifr) != 0)
		return (errno == ENXIO)
			? _error(2, ifname) : _error(2, "SIOCGUMBPARAM");
	if(apn == NULL && username == NULL && password == NULL && is_puk < 0)
		_print_info(ifname, &mp);
	else
	{
		if(apn != NULL && (mp.apnlen = _char_to_utf16(apn, mp.apn,
						sizeof(mp.apn))) < 0)
		{
			fprintf(stderr, "%s: %s: APN too long\n", "umbctl",
					apn);
			return 3;
		}
		if(username != NULL && (mp.usernamelen = _char_to_utf16(
						username, mp.username,
						sizeof(mp.username))) < 0)
		{
			fprintf(stderr, "%s: %s: username too long\n", "umbctl",
					username);
			return 3;
		}
		if(password != NULL && (mp.passwordlen = _char_to_utf16(
						password, mp.password,
						sizeof(mp.password))) < 0)
		{
			fprintf(stderr, "%s: %s: password too long\n", "umbctl",
					_print_stars(strlen(password)));
			return 3;
		}
		if(is_puk >= 0)
		{
			mp.is_puk = is_puk;
			mp.op = MBIM_PIN_OP_ENTER;
			if((mp.pinlen = _char_to_utf16(pin, mp.pin,
							sizeof(mp.pin))) < 0)
			{
				fprintf(stderr, "%s: %s: %s code too long\n",
						"umbctl",
						is_puk ? "PIN" : "PUK",
						_print_stars(strlen(pin)));
				return 4;
			}
		}
		if(ioctl(fd, SIOCSUMBPARAM, &ifr) != 0)
			return _error(2, ifname);
	}
	if(close(fd) != 0)
		return _error(2, "close");
	return 0;
}

static int _umbctl_info(char const * ifname)
{
	int fd;
	struct ifreq ifr;
	struct umb_info mi;
	char provider[UMB_PROVIDERNAME_MAXLEN + 1];
	char pn[UMB_PHONENR_MAXLEN + 1];
	char roaming[UMB_ROAMINGTEXT_MAXLEN + 1];
	char apn[UMB_APN_MAXLEN + 1];
	char fwinfo[UMB_FWINFO_MAXLEN + 1];
	char hwinfo[UMB_HWINFO_MAXLEN + 1];

	if((fd = _umbctl_socket()) < 0)
		return 2;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	memset(&mi, 0, sizeof(mi));
	ifr.ifr_data = &mi;
	if(ioctl(fd, SIOCGUMBINFO, &ifr) != 0)
		return (errno == ENXIO)
			? _error(2, ifname) : _error(2, "SIOCGUMBINFO");
	_utf16_to_char(mi.provider, UMB_PROVIDERNAME_MAXLEN,
			provider, sizeof(provider));
	_utf16_to_char(mi.pn, UMB_PHONENR_MAXLEN, pn, sizeof(pn));
	_utf16_to_char(mi.roamingtxt, UMB_ROAMINGTEXT_MAXLEN,
			roaming, sizeof(roaming));
	_utf16_to_char(mi.apn, UMB_APN_MAXLEN, apn, sizeof(apn));
	_utf16_to_char(mi.fwinfo, UMB_FWINFO_MAXLEN, fwinfo, sizeof(fwinfo));
	_utf16_to_char(mi.hwinfo, UMB_HWINFO_MAXLEN, hwinfo, sizeof(hwinfo));
	printf("%s: state %s, mode %s, registration %s\n"
			"\tprovider \"%s\", dataclass %s, signal %s\n"
			"\tphone number \"%s\", roaming \"%s\" (%s)\n"
			"\tAPN \"%s\", TX %" PRIu64 ", RX %" PRIu64 "\n"
			"\tfirmware \"%s\", hardware \"%s\"\n",
			ifname, umb_val2descr(_umb_state, mi.state),
			umb_val2descr(_umb_regmode, mi.regmode),
			umb_val2descr(_umb_regstate, mi.regstate),
			provider, umb_val2descr(_umb_dataclass, mi.cellclass),
			umb_val2descr(_umb_ber, mi.ber), pn,
			roaming, mi.enable_roaming ? "allowed" : "denied",
			apn, mi.uplink_speed, mi.downlink_speed,
			fwinfo, hwinfo);
	return 0;
}

static int _umbctl_socket(void)
{
	int fd;

	if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return _error(-1, "socket");
	return fd;
}

static int _usage(void)
{
	fputs("Usage: umbctl interface\n"
"       umbctl -I interface\n"
"       umbctl -a apn|-u username|-p password|-s PIN|-S PUK interface\n",
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
	int info = 0;
	char const * apn = NULL;
	char const * username = NULL;
	char const * password = NULL;
	int is_puk = -1;
	char const * pin = NULL;

	while((o = getopt(argc, argv, "a:Ip:S:s:u:")) != -1)
		switch(o)
		{
			case 'a':
				apn = optarg;
				break;
			case 'I':
				info = 1;
				break;
			case 'p':
				password = optarg;
				break;
			case 's':
				pin = optarg;
				is_puk = 0;
				break;
			case 'S':
				pin = optarg;
				is_puk = 1;
				break;
			case 'u':
				username = optarg;
				break;
			default:
				return _usage();
		}
	if(optind + 1 != argc)
		return _usage();
	return (info != 0)
		? _umbctl_info(argv[optind])
		: _umbctl(argv[optind], apn, username, password, is_puk, pin);
}
