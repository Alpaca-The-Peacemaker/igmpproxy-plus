PORTNAME=	igmpproxy-plus
DISTVERSIONPREFIX=	v
DISTVERSION=	1.2.4
CATEGORIES=	net

MAINTAINER=	8hx8dfdss2@privaterelay.appleid.com
COMMENT=	IGMP proxy with fast leave and multicast leave timer control
WWW=		https://github.com/Alpaca-The-Peacemaker/igmpproxy-plus

LICENSE=	GPLv2+

USES=		autoreconf
USE_GITHUB=	yes
GH_ACCOUNT=	Alpaca-The-Peacemaker
GNU_CONFIGURE=	yes
USE_RC_SUBR=	igmpproxy_plus

CONFLICTS_INSTALL=	igmpproxy

PLIST_FILES=	sbin/igmpproxy-plus \
		share/man/man5/igmpproxy-plus.conf.5.gz \
		share/man/man8/igmpproxy-plus.8.gz \
		etc/igmpproxy-plus.conf.sample

post-install:
	${MV} ${STAGEDIR}${PREFIX}/sbin/igmpproxy ${STAGEDIR}${PREFIX}/sbin/igmpproxy-plus
	${MV} ${STAGEDIR}${PREFIX}/share/man/man5/igmpproxy.conf.5 ${STAGEDIR}${PREFIX}/share/man/man5/igmpproxy-plus.conf.5
	${MV} ${STAGEDIR}${PREFIX}/share/man/man8/igmpproxy.8 ${STAGEDIR}${PREFIX}/share/man/man8/igmpproxy-plus.8
	${RM} -f ${STAGEDIR}${PREFIX}/etc/igmpproxy.conf
	${INSTALL_DATA} ${WRKSRC}/igmpproxy.conf ${STAGEDIR}${PREFIX}/etc/igmpproxy-plus.conf.sample

.include <bsd.port.mk>
