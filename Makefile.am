# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#

if generic

ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = $(TRACE_CFLAGS) $(CFLAGS) -I${top_srcdir}/inc
AM_LDFLAGS = -L$(PKG_CONFIG_SYSROOT_DIR)/$(libdir) $(LDFLAGS)
AM_LDFLAGS += -lrt -ldl

lib_LTLIBRARIES = libmemfnswrap.la
libmemfnswrap_la_SOURCES = ${top_srcdir}/lib/memfns_wrap.c
libmemfnswrap_la_LDFLAGS = -shared -fPIC -Wl,-soname,libmemfnswrap.so.0
libmemfnswrap_la_includedir = $(includedir)
libmemfnswrap_la_include_HEADERS = inc/memfns_wrap.h

bin_PROGRAMS = memleakutil
memleakutil_SOURCES = \
    ${top_srcdir}/uty/memleakutil.c
memleakutil_includedir = ${top_srcdir}/inc
memleakutil_CFLAGS = $(AM_CFLAGS)
memleakutil_CPPFLAGS = $(AM_CFLAGS) 
memleakutil_LDFLAGS = $(AM_LDFLAGS) -lpthread

if USE_SELF_TEST
bin_PROGRAMS += memleakutil_test
memleakutil_test_DEPENDENCIES = libmemfnswrap.la
memleakutil_test_SOURCES = ${top_srcdir}/uty/memleakutil.c \
			   ${top_srcdir}/uty/memleakutil_test.c \
			   ${top_srcdir}/lib/memfns_wrap.c
memleakutil_test_CFLAGS = $(AM_CFLAGS) -DSELF_TEST
memleakutil_test_CPPFLAGS = $(AM_CFLAGS) -DSELF_TEST
memleakutil_test_LDFLAGS = $(AM_LDFLAGS) -lpthread
endif

#if USE_EXAMPLE
#memleakutil_SOURCES += \
#    ${top_srcdir}/example1.c \
#    ${top_srcdir}/example2.c
#else
#memleakutil_SOURCES += \
#    ${top_srcdir}/noexample1.c \
#endif

endif

if rdk
bin_PROGRAMS =
endif
