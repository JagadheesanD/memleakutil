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
AC_PREREQ([2.65])
AC_INIT([libmemfnswrap], [1.0], [duraisamy.jagadheesan@gmail.com])
AM_INIT_AUTOMAKE([foreign subdir-objects])
LT_INIT

AC_PREFIX_DEFAULT(`pwd`)
AC_ENABLE_SHARED
AC_DISABLE_STATIC
#AC_PROG_AWK
AC_PROG_LN_S
AC_PROG_MAKE_SET

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
#AC_PROG_CXX
AC_PROG_INSTALL
AM_PROG_CC_C_O
AM_PROG_LIBTOOL(libtool)

TRACE_CFLAGS=" "
TRACE_LIBS=" "

# Checks for header files.
AC_CHECK_HEADERS([stdio.h stdlib.h string.h unistd.h stdarg.h limits.h])
AC_CHECK_HEADERS([mqueue.h stdbool.h dlfcn.h stddef.h errno.h pthread.h fcntl.h sys/stat.h sys/types.h time.h])
# For SELF_TEST
AC_CHECK_HEADERS([semaphore.h sys/mman.h])
AC_CHECK_FUNCS([memset])
#AC_FUNC_MKTIME
AC_FUNC_MALLOC
#AC_PROG_RANLIB
#AC_TYPE_SIZE_T
#AC_TYPE_UINT32_T
# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE

BUILD_FLAVOR="generic"
AC_ARG_WITH([build-flavor],
[  --with-build-flavor  build flavor],
[case "${withval}" in
  generic) BUILD_FLAVOR=generic;;
  rdk) BUILD_FLAVOR=rdk;;
  *) AC_MSG_ERROR([bad value ${withval} for --with-build-flavor]);;
esac],[RDK_FLAVOR="generic"])
#AC_SUBST([BUILD_FLAVOR], [$build-flavor])
AM_CONDITIONAL([generic], [test "x$BUILD_FLAVOR" = xgeneric])
AM_COND_IF([generic], [AC_DEFINE([__BUILDGENERIC__], [1], [Generic platform selection macro])])
#AM_COND_IF([generic], [PKG_CHECK_MODULES([PTHREAD], [libpthread >= 0.0])])

AM_CONDITIONAL([rdk], [test "x$BUILD_FLAVOR" = xrdk])
AM_COND_IF([rdk], [AC_DEFINE([__BUILDRDK__], [1], [rdk platform selection macro])])

# Check if self test enabled
AC_ARG_ENABLE([self-test],
    [AS_HELP_STRING([--enable-self-test], [Enable generating selftest binary support])],
    [USE_SELF_TEST="$enableval"],
    [USE_SELF_TEST=no])

AM_CONDITIONAL([USE_SELF_TEST], [test "$USE_SELF_TEST" = "yes"])

if test "$USE_SELF_TEST" = "yes"; then
    AC_DEFINE([USE_SELF_TEST], [1], [Define to enable generating selftest binary support])
fi

# Sample to default enable certain options
AC_ARG_ENABLE([example],
    [AS_HELP_STRING([--enable-example], [Enable some example support])],
    [USE_EXAMPLE="$enableval"],
    [USE_EXAMPLE=no])
AM_CONDITIONAL([USE_EXAMPLE], [test "$USE_EXAMPLE" = "yes"])

if test "$USE_EXAMPLE" = "yes"; then
    AC_DEFINE([USE_EXAMPLE], [1], [Define to enable some example support])
fi

# Another example for yes/no type OEMAKE option
AC_ARG_ENABLE([comcast-build],
              AS_HELP_STRING([--enable-comcast-build],[enable comcast build (default is yes)]),
              [
                case "${enableval}" in
                 yes) ENABLE_COMCAST_BUILD=true 
                      ENABLE_COMCAST_OFFSET_FLAG="-DCOMCAST_OFFSET" ;;
                  no) [echo "comcast build is disabled"] ;;
                   *) AC_MSG_ERROR([bad value ${enableval} for --enable-comcast-build]) ;;
                esac
              ],
              [
                ENABLE_COMCAST_BUILD=true
                [echo "comcast build is enabled"]
              ])
AM_CONDITIONAL([ENABLE_COMCAST_BUILD], [test x$ENABLE_COMCAST_BUILD = xtrue])

AC_SUBST(TRACE_CFLAGS)
AC_SUBST(TRACE_LIBS)
AC_CONFIG_FILES(
    Makefile
    )
AC_OUTPUT
