#!/bin/bash -x

#
# Builds a saslauthd with Zimbra authentication support.  We modify
# configure.in which means we have to run autoconf.  I have borrowed
# the steps to run autoconf from the rpm spec.  The cyrus CVS tree's
# SMakefile is probably the source for the rpm spec.
#
BETA=$1

platform=`uname -s`
cyrus_root=`pwd`
p4_root=`cd ${cyrus_root}/../..; pwd`
build_platform=`sh ${p4_root}/ZimbraBuild/rpmconf/Build/get_plat_tag.sh`

if [ x$BETA = "xbeta" ]; then
  source ../beta_versions.sh
else
  source ../versions.sh
fi

rm -fr build
mkdir build
cd build
tar xfz ../src/cyrus-sasl-${cyrus_release}.tar.gz  -C .
chmod -R +w cyrus-sasl-${cyrus_release}
mv cyrus-sasl-${cyrus_release} ${cyrus_src}

cd ${cyrus_src}
patch -g0 -p1 < ../../sasl-link-order.patch
patch -g0 -p1 < ../../sasl-darwin.patch
patch -g0 -p1 < ../../sasl-auth-zimbra.patch
patch -g0 -p1 < ../../gcc-fix.patch
rm config/ltconfig config/libtool.m4
if [ -x /usr/bin/libtoolize ]; then
	LIBTOOLIZE=/usr/bin/libtoolize
else
	if [ -x /opt/local/bin/glibtoolize ]; then
		export CPPFLAGS=-DDARWIN
		LIBTOOLIZE=/opt/local/bin/glibtoolize
	elif [ -x /usr/bin/glibtoolize ]; then
		export CPPFLAGS=-DDARWIN
		LIBTOOLIZE=/usr/bin/glibtoolize
	else
		echo "Where is libtoolize?"
		exit 1
	fi
fi
$LIBTOOLIZE -f -c
aclocal -I config -I cmulocal
automake -a -c -f
autoheader
autoconf -f

cd saslauthd
rm config/ltconfig
$LIBTOOLIZE -f -c
aclocal -I config -I ../cmulocal -I ../config
automake -a -c -f
autoheader
autoconf -f

cd ..
# fix linking against OpenSSL
sed -i.obak -e 's|${with_openssl}/$CMU_LIB_SUBDIR|${with_openssl}/lib|' -e 's|${with_openssl}/lib $andrew_runpath_switch${with_openssl}/$CMU_LIB_SUBDIR|${with_openssl}/lib|' configure
sed -i.obak -e 's|${with_openssl}/$CMU_LIB_SUBDIR|${with_openssl}/lib|' -e 's|${with_openssl}/lib $andrew_runpath_switch${with_openssl}/$CMU_LIB_SUBDIR|${with_openssl}/lib|' saslauthd/configure

sed -i.bak 's/-lRSAglue //' configure
if [ $platform = "Darwin" ]; then
LIBS="/opt/zimbra/libxml2/lib/libxml2.a" CFLAGS="-D_REENTRANT -g -O2 -I/opt/zimbra/libxml2/include/libxml2" ./configure --enable-zimbra --prefix=/opt/zimbra/${cyrus_src} \
            --with-saslauthd=/opt/zimbra/${cyrus_src}/state \
            --with-plugindir=/opt/zimbra/${cyrus_src}/lib/sasl2 \
            --enable-static=no \
            --enable-shared \
            --with-dblib=no \
            --with-openssl=/opt/zimbra/openssl-${openssl_version} \
            --with-libcurl=/opt/zimbra/curl-${curl_version}/bin/curl-config \
            --with-gss_impl=heimdal \
            --enable-gssapi=/opt/zimbra/heimdal-${heimdal_version} \
            --with-libxml2=/opt/zimbra/libxml2-${xml2_version}/bin/xml2-config \
            --enable-login
else 
LIBS="/opt/zimbra/libxml2/lib/libxml2.a" CFLAGS="-D_REENTRANT -g -O2 -I/opt/zimbra/libxml2/include/libxml2" ./configure --enable-zimbra --prefix=/opt/zimbra/${cyrus_src} \
            --with-saslauthd=/opt/zimbra/${cyrus_src}/state \
            --with-plugindir=/opt/zimbra/${cyrus_src}/lib/sasl2 \
            --with-dblib=no \
            --with-openssl=/opt/zimbra/openssl-${openssl_version} \
            --with-libcurl=/opt/zimbra/curl-${curl_version}/bin/curl-config \
            --with-gss_impl=heimdal \
            --enable-gssapi=/opt/zimbra/heimdal-${heimdal_version} \
            --with-libxml2=/opt/zimbra/libxml2-${xml2_version}/bin/xml2-config \
            --enable-login
fi
if [ $platform = "Darwin" ]; then
     sed -i .bak -e 's/\_la_LDFLAGS)/_la_LDFLAGS) $(AM_LDFLAGS)/' plugins/Makefile
     sed -i .bak -e "s|-L/opt/zimbra/libxml2-${xml2_version}/lib -lxml2||" saslauthd/Makefile
elif [ $build_platform = "F7" -o $build_platform -o "DEBIAN4.0" ]; then
     sed -i.bak -e 's/\_la_LDFLAGS)/_la_LDFLAGS) $(AM_LDFLAGS)/' plugins/Makefile
fi
LD_RUN_PATH="${openssl_lib_dir}:${heimdal_lib_dir}:${cyrus_lib_dir}:${curl_lib_dir}" make
