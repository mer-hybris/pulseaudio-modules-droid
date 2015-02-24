AC_PREREQ(2.60)

m4_define(PA_MAJOR, [5])
m4_define(PA_MINOR, [0])
m4_define(NEMO_MICRO, [32])

AC_INIT([pulseaudio-modules-droid], [PA_MAJOR.PA_MINOR.NEMO_MICRO], [mer-general@lists.merproject.org])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign -Wall silent-rules])
AC_CONFIG_MACRO_DIR(m4)
AM_SILENT_RULES([yes])

if type -p stow > /dev/null && test -d /usr/local/stow ; then
   AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
   ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
fi

AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O
AC_PROG_GCC_TRADITIONAL

AC_SUBST(PA_MAJORMINOR, PA_MAJOR.PA_MINOR)

DESIRED_FLAGS="-Wall -W -Wextra -pedantic -pipe -Wno-long-long -Winline -Wvla -Wno-overlength-strings -Wunsafe-loop-optimizations -Wundef -Wformat=2 -Wlogical-op -Wsign-compare -Wpacked -Wformat-security -Wmissing-include-dirs -Wformat-nonliteral -Wold-style-definition -Wpointer-arith -Winit-self -Wfloat-equal -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls -Wmissing-declarations -Wmissing-noreturn -Wshadow -Wendif-labels -Wstrict-aliasing=2 -Wwrite-strings -Wno-unused-parameter -ffast-math -Wp,-D_FORTIFY_SOURCE=2 -fno-common -fdiagnostics-show-option" # PulseAudio 0.9.15 usess same + -Wcast-align -Wdeclaration-after-statement

for flag in $DESIRED_FLAGS ; do
  CC_CHECK_CFLAGS([$flag], [CFLAGS="$CFLAGS $flag"])
done

# FIXME: we should push that as .m4 macro installed by pulseaudio-dev package
# Native atomic operation support
AC_ARG_ENABLE([atomic-arm-linux-helpers],
    AS_HELP_STRING([--disable-atomic-arm-linux-helpers],[use inline asm or libatomic_ops instead]),
        [
            case "${enableval}" in
                yes) atomic_arm_linux_helpers=yes ;;
                no) atomic_arm_linux_helpers=no ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-atomic-arm-linux-helpers) ;;
            esac
        ],
        [atomic_arm_linux_helpers=auto])

AC_ARG_ENABLE([atomic-arm-memory-barrier],
    AS_HELP_STRING([--enable-atomic-arm-memory-barrier],[only really needed in SMP arm systems]),
        [
            case "${enableval}" in
                yes) AC_DEFINE_UNQUOTED(ATOMIC_ARM_MEMORY_BARRIER_ENABLED, 1, [Enable memory barriers]) ;;
                no) ;;
                *) AC_MSG_ERROR(bad value ${enableval} for --disable-atomic-arm-linux-helpers) ;;
            esac
        ],)

AC_CANONICAL_HOST
AC_DEFINE_UNQUOTED([CANONICAL_HOST], "$host", [Canonical host string.])

AC_MSG_CHECKING([target operating system])

case $host in
    	*-*-linux*)
	    AC_MSG_RESULT([linux])
	    pulse_target_os=linux
    	;;
	*)
	    AC_MSG_RESULT([unknown])
	    pulse_target_os=unknown
	;;
esac

# If everything else fails use libatomic_ops
need_libatomic_ops=yes

AC_MSG_CHECKING([whether $CC knows __sync_bool_compare_and_swap()])
AC_LANG_CONFTEST([int main() { int a = 4; __sync_bool_compare_and_swap(&a, 4, 5); }])
$CC conftest.c $CFLAGS -o conftest > /dev/null 2> /dev/null
ret=$?
rm -f conftest.o conftest
if test $ret -eq 0 ; then
    AC_DEFINE([HAVE_ATOMIC_BUILTINS], 1, [Have __sync_bool_compare_and_swap() and friends.])
    AC_MSG_RESULT([yes])
    need_libatomic_ops=no
else
    AC_MSG_RESULT([no])
    # HW specific atomic ops stuff
    AC_MSG_CHECKING([architecture for native atomic operations])
    case $host_cpu in
        arm*)
	    AC_MSG_RESULT([arm])
	    AC_MSG_CHECKING([compiler support for arm inline asm atomic operations])
	    AC_LANG_CONFTEST([[int main()
	    {
                  volatile int a=0;
	          int o=0, n=1, r;
	          asm volatile ("ldrex	%0, [%1]\n"
		     	 	"subs	%0, %0, %2\n"
			 	 "strexeq %0, %3, [%1]\n"
			 	 : "=&r" (r)
			 	 : "r" (&a), "Ir" (o), "r" (n)
			 	 : "cc");
                  return (a==1 ? 0 : -1);
	    }]])
	    $CC conftest.c $CFLAGS -o conftest > /dev/null 2>&1
	    ret=$?
	    rm -f conftest.o conftest
	    if test $ret -eq 0 ; then
	       AC_DEFINE([ATOMIC_ARM_INLINE_ASM], 1, [Have ARMv6 instructions.])
	       AC_MSG_RESULT([yes])
	       need_libatomic_ops=no
	    else
		# We really want inline asm atomic operations!!
		echo "ERROR No ARMv6 instructions available check your compiler!!!"
		exit 1

		AC_MSG_CHECKING([whether we can use Linux kernel helpers])
	    	# The Linux kernel helper functions have been there since 2.6.16. However
  	    	# compile time checking for kernel version in cross compile environment
	    	# (which is usually the case for arm cpu) is tricky (or impossible).
	    	if test "x$pulse_target_os" = "xlinux" && test "x$atomic_arm_linux_helpers" != "xno"; then
	           AC_MSG_RESULT([yes])
        	   AC_DEFINE_UNQUOTED(ATOMIC_ARM_LINUX_HELPERS, 1, [special arm linux implementation])
    		   need_libatomic_ops=no
		else
		   AC_MSG_RESULT([no])
               fi
	   fi
      	;;
        *)
	    AC_MSG_RESULT([unknown])
        ;;
    esac
fi

AC_MSG_CHECKING([whether we need libatomic_ops])
if test "x$need_libatomic_ops" = "xyes"; then
   AC_MSG_RESULT([yes])
   AC_CHECK_HEADERS([atomic_ops.h], [], [
   AC_MSG_ERROR([*** libatomic-ops headers not found])
   ])

   # Win32 does not need the lib and breaks horribly if we try to include it
   if test "x$os_is_win32" != "x1" ; then
       LIBS="$LIBS -latomic_ops"
   fi
else
   AC_MSG_RESULT([no])
fi

CC_CHECK_TLS

AC_CACHE_CHECK([whether $CC knows _Bool],
  pulseaudio_cv__Bool,
  [AC_COMPILE_IFELSE(
     AC_LANG_PROGRAM([], [[_Bool b;]]),
     [pulseaudio_cv__Bool=yes],
     [pulseaudio_cv__Bool=no])
  ])

AS_IF([test "$pulseaudio_cv__Bool" = "yes"], [
    AC_DEFINE([HAVE_STD_BOOL], 1, [Have _Bool.])
  ])

#### libtool stuff (FIXME: when sbox will have 2.2 )####
#LT_PREREQ(2.2)
#LT_INIT([dlopen win32-dll disable-static])
AC_PROG_LIBTOOL

PKG_CHECK_MODULES([PULSEAUDIO], [libpulse >= 2.1 pulsecore >= 2.1 ])
AC_SUBST(PULSEAUDIO_CFLAGS)
AC_SUBST(PULSEAUDIO_LIBS)

pulseaudiodir=`pkg-config --variable=prefix pulsecore`

#PKG_CHECK_MODULES([DROIDHEADERS], [android-headers >= 0.0.6])
# android-headers.pc has broken version field
PKG_CHECK_MODULES([DROIDHEADERS], [android-headers])
AC_SUBST(DROIDHEADERS_CFLAGS)

PKG_CHECK_MODULES([HYBRIS], [libhardware >= 0.1.0])
AC_SUBST(HYBRIS_CFLAGS)
AC_SUBST(HYBRIS_LIBS)

PKG_CHECK_MODULES([DBUS], [dbus-1 >= 1.2])
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

AC_ARG_WITH([module-dir],
        AS_HELP_STRING([--with-module-dir],[Directory where to install the modules to (defaults to ${pulseaudiodir}/lib/pulse-${PA_MAJORMINOR}/modules/]),
        [modlibexecdir=$withval], [modlibexecdir="${pulseaudiodir}/lib/pulse-${PA_MAJORMINOR}/modules"])

AC_SUBST(modlibexecdir)

############################################
# Droid device type
droiddevice="generic"
AC_ARG_WITH([droid-device],
        AS_HELP_STRING([--with-droid-device], [Droid device type for possible specific quirks (defaults to generic).]),
        [droiddevice=$withval], [droiddevice="generic"]
        )
if test "x$droiddevice" != x ; then
    DROID_DEVICE_CFLAGS="-DDROID_DEVICE_`echo $droiddevice | tr '[a-z]' '[A-Z]'`=1"
    AC_SUBST([DROID_DEVICE_CFLAGS])
fi

AC_MSG_CHECKING([If we are using hardfp tool chain])
case `echo | gcc -v -xc -o - - 2>&1 | grep COLLECT_GCC_OPTIONS | tail -1` in
     *float-abi=hard*) hardfp=yes; AC_MSG_RESULT([yes]) ;;
     *) hardfp=no; AC_MSG_RESULT([no]) ;;
esac
AM_CONDITIONAL([ARM_HARDFP], [test "x$hardfp" = xyes])
ENABLE_ARM_HARDFP=no
if test "x$hardfp" = xyes ; then
    AC_DEFINE([ARM_HARDFP], 1, [ARM Hard FP toolchain used.])
    ENABLE_ARM_HARDFP=yes
fi

AC_CONFIG_FILES([
Makefile
src/Makefile
src/droid/Makefile
])

AC_OUTPUT

echo "

 ---{ $PACKAGE_NAME $VERSION }---

    compiler:               ${CC}
    CFLAGS:                 ${CFLAGS}

    prefix:                 ${prefix}
    PulseAudio prefix:      ${pulseaudiodir}
    modules directory:      ${modlibexecdir}

    Droid device            ${droiddevice}
    "
