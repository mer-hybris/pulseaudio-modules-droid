AC_PREREQ(2.60)

AC_INIT([pulseaudio-modules-droid], [m4_esyscmd(./git-version-gen .tarball-version)], [mer-general@lists.merproject.org])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign -Wall silent-rules])
AC_CONFIG_MACRO_DIR(m4)
AM_SILENT_RULES([yes])

AS_IF([! test -n "$VERSION"], [
   AC_MSG_ERROR([git-version-gen failed])
])

if type -p stow > /dev/null && test -d /usr/local/stow ; then
   AC_MSG_NOTICE([*** Found /usr/local/stow: default install prefix set to /usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION} ***])
   ac_default_prefix="/usr/local/stow/${PACKAGE_NAME}-${PACKAGE_VERSION}"
fi

AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O
AC_PROG_GCC_TRADITIONAL

m4_define(pa_major, `echo $VERSION | cut -d. -f1 | cut -d- -f1`)
m4_define(pa_minor, `echo $VERSION | cut -d. -f2 | cut -d- -f1`)
m4_define(pa_module_version, `echo $VERSION | cut -d. -f3 | cut -d- -f1`)

AC_SUBST(PA_MAJOR, pa_major)
AC_SUBST(PA_MAJORMINOR, pa_major.pa_minor)
AC_SUBST(PA_MODULE_VERSION, pa_module_version)

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

PKG_CHECK_MODULES([PULSEAUDIO], [libpulse >= 5.0 pulsecore >= 5.0])
AC_SUBST(PULSEAUDIO_CFLAGS)
AC_SUBST(PULSEAUDIO_LIBS)

# Check if building droid parts
AC_ARG_ENABLE([droid],
    AS_HELP_STRING([--disable-droid], [Disable droid specific plugins]))

AM_CONDITIONAL([ENABLE_DROID], [test "ENABLE_DROID" = x1])
AS_IF([test "ENABLE_DROID" = "x1"], AC_DEFINE([ENABLE_DROID], 1, [Enable droid modules?]))
    
if test "x$enable_droid" != "xno" ; then    
    #PKG_CHECK_MODULES([DROIDHEADERS], [android-headers >= 0.0.6])
    # android-headers.pc has broken version field
    PKG_CHECK_MODULES([DROIDHEADERS], [android-headers])
    AC_SUBST(DROIDHEADERS_CFLAGS)

    PKG_CHECK_MODULES([HYBRIS], [libhardware >= 0.1.0])
    AC_SUBST(HYBRIS_CFLAGS)
    AC_SUBST(HYBRIS_LIBS)
fi

PKG_CHECK_MODULES([DBUS], [dbus-1 >= 1.2])
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

#### expat (for xml config format parsing) (optional) ####

AC_ARG_ENABLE([xml],
    AS_HELP_STRING([--disable-xml],[Disable optional xml config support]))

AS_IF([test "x$enable_xml" != "xno"],
    [PKG_CHECK_MODULES(EXPAT, [ expat >= 2.1 ], HAVE_EXPAT=1, HAVE_EXPAT=0)],
    HAVE_EXPAT=0)

AS_IF([test "x$enable_xml" = "xyes" && test "x$HAVE_EXPAT" = "x0"],
    [AC_MSG_ERROR([*** expat not found])])

AC_SUBST(HAVE_EXPAT)
AC_SUBST(EXPAT_CFLAGS)
AC_SUBST(EXPAT_LIBS)
AM_CONDITIONAL([HAVE_EXPAT], [test "x$HAVE_EXPAT" = x1])
AS_IF([test "x$HAVE_EXPAT" = "x1"], AC_DEFINE([HAVE_EXPAT], 1, [Have expat?]))
AS_IF([test "x$HAVE_EXPAT" = "x1"], ENABLE_XML="yes (expat)", ENABLE_XML=no)

# Output devices
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_HDMI])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_HDMI_ARC])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_TELEPHONY_TX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_LINE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_SPDIF])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_AUX_LINE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_SPEAKER_SAFE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_FM])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_FM_TX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_ANC_HEADSET])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_ANC_HEADPHONE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_PROXY])
# Added in 6.0
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_OUT_IP])

# Input devices
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_HDMI])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_TELEPHONY_RX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_FM_TUNER])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_TV_TUNER])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_LINE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_SPDIF])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_BLUETOOTH_A2DP])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_LOOPBACK])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_PROXY])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_FM_RX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_FM_RX_A2DP])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_ALL_USB])
# Added in 6.0
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_DEVICE_IN_IP])

# Audio sources
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_SOURCE_FM_TUNER])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_SOURCE_FM_RX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_SOURCE_FM_RX_A2DP])

# Output flags
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_NON_BLOCKING])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_HW_AV_SYNC])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_VOIP_RX])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_INCALL_MUSIC])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_COMPRESS_PASSTHROUGH])
# Added in 6.0
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_TTS])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_RAW])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_SYNC])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_OUTPUT_FLAG_IEC958_NONAUDIO])

# Input flags
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_INPUT_FLAG_NONE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_INPUT_FLAG_FAST])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_INPUT_FLAG_HW_HOTWORD])
# Added in 6.0
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_INPUT_FLAG_RAW])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_INPUT_FLAG_SYNC])

# Channels
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_OUT_SURROUND])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_OUT_5POINT1_BACK])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_OUT_5POINT1_SIDE])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_IN_VOICE_CALL_MONO])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_IN_VOICE_UPLINK])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_CHANNEL_IN_VOICE_DNLINK])

# Formats
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_FORMAT_PCM_OFFLOAD])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_FORMAT_FLAC])
CC_CHECK_DROID_ENUM([${DROIDHEADERS_CFLAGS}], [AUDIO_FORMAT_OPUS])

AC_ARG_WITH(
        [module-dir],
        AS_HELP_STRING([--with-module-dir],[Directory where to install the modules to (defaults to ${libdir}/pulse-${PA_MAJORMINOR}/modules]),
        [modlibexecdir=$withval], [modlibexecdir="${libdir}/pulse-${PA_MAJORMINOR}/modules"])

AC_SUBST(modlibexecdir)

############################################
# Droid device type
droiddevice="generic"
AC_ARG_WITH([droid-device],
        AS_HELP_STRING([--with-droid-device], [Droid device type for possible specific quirks (defaults to generic).]),
        [droiddevice=$withval], [droiddevice="generic"]
        )
if test "x$droiddevice" != x ; then
    DROID_DEVICE_CFLAGS="-DDROID_DEVICE_`echo $droiddevice | tr '[a-z]' '[A-Z]'`=1 -DDROID_DEVICE_STRING=\"\\\"$droiddevice\\\"\""
    AC_SUBST([DROID_DEVICE_CFLAGS])
fi

# Workaround for SBJ HAL headers
if test "x$droiddevice" = xsbj ; then
    SBJ_DEVICE_LDFLAGS="-Wl,--allow-multiple-definition"
    AC_SUBST([SBJ_DEVICE_LDFLAGS])
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
src/common/Makefile
src/common/libdroid-util.pc
src/droid/Makefile
])

AC_OUTPUT

echo "

 ---{ $PACKAGE_NAME $VERSION }---

    compiler:               ${CC}
    CFLAGS:                 ${CFLAGS}

    prefix:                 ${prefix}
    modules directory:      ${modlibexecdir}

    Droid device:           ${droiddevice}
    XML config support:     ${ENABLE_XML}
    Android module support: ${ENABLE_DROID}
    "
