/* $FreeBSD */

#ifndef _SOCKIO_H_
#define _SOCKIO_H_

#include <sys/ioccom.h>

#define SIOCGUMBINFO	_IOWR('i', 190, struct ifreq)	/* get MBIM info */
#define SIOCSUMBPARAM	 _IOW('i', 191, struct ifreq)	/* set MBIM param */
#define SIOCGUMBPARAM	_IOWR('i', 192, struct ifreq)	/* get MBIM param */

#endif /* _SOCKIO_H_ */
