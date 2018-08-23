AM_LIBADD = \
	$(PULSEAUDIO_LIBS) \
	$(HYBRIS_LIBS) \
	$(EXPAT_LIBS)
AM_CFLAGS = \
	$(DROID_DEVICE_CFLAGS) \
	$(PULSEAUDIO_CFLAGS) \
	$(DROIDHEADERS_CFLAGS) \
	$(HYBRIS_CFLAGS) \
	$(EXPAT_CFLAGS) \
	-DPULSEAUDIO_VERSION=@PA_MAJOR@ \
	-I$(top_srcdir)/src/common \
	-I$(top_srcdir)/src/common/include


modlibexec_LTLIBRARIES = libdroid-util.la
includedir = @includedir@/pulsecore/modules/droid
include_HEADERS = include/droid/version.h           \
                  include/droid/conversion.h        \
                  include/droid/droid-config.h      \
                  include/droid/droid-util.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libdroid-util.pc

libdroid_util_la_SOURCES = droid-util.c             \
                           droid-config.c           \
                           conversion.c             \
                           config-parser-legacy.c   \
                           config-parser-xml.c      \
                           droid-util-audio.h       \
                           droid-util-41qc.h
libdroid_util_la_LDFLAGS = -avoid-version -Wl,-z,noexecstack -lhybris-common $(SBJ_DEVICE_LDFLAGS)
libdroid_util_la_LIBADD = $(AM_LIBADD)
libdroid_util_la_CFLAGS = $(AM_CFLAGS)
