AM_LIBADD = \
	$(PULSEAUDIO_LIBS) \
	$(HYBRIS_LIBS)
AM_CFLAGS = \
	$(PULSEAUDIO_CFLAGS) \
	$(DROIDHEADERS_CFLAGS) \
	$(HYBRIS_CFLAGS) \
	-I$(top_srcdir)/src/droid


modlibexec_LTLIBRARIES = \
	libdroid-util.la \
	libdroid-sink.la \
	libdroid-source.la \
	module-droid-sink.la \
	module-droid-source.la \
	module-droid-card.la

noinst_HEADERS = module-droid-sink-symdef.h module-droid-card-symdef.h

libdroid_util_la_SOURCES = droid-util.c droid-util.h
libdroid_util_la_LDFLAGS = -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
libdroid_util_la_LIBADD = $(AM_LIBADD)
libdroid_util_la_CFLAGS = $(AM_CFLAGS)

libdroid_sink_la_SOURCES = droid-sink.c droid-sink.h
libdroid_sink_la_LDFLAGS = -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
libdroid_sink_la_LIBADD = $(AM_LIBADD) libdroid-util.la
libdroid_sink_la_CFLAGS = $(AM_CFLAGS)

libdroid_source_la_SOURCES = droid-source.c droid-source.h
libdroid_source_la_LDFLAGS = -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
libdroid_source_la_LIBADD = $(AM_LIBADD) libdroid-util.la
libdroid_source_la_CFLAGS = $(AM_CFLAGS)

module_droid_sink_la_SOURCES = module-droid-sink.c
module_droid_sink_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
module_droid_sink_la_LIBADD = $(AM_LIBADD) -lm libdroid-util.la libdroid-sink.la
module_droid_sink_la_CFLAGS = $(AM_CFLAGS)

module_droid_source_la_SOURCES = module-droid-source.c
module_droid_source_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
module_droid_source_la_LIBADD = $(AM_LIBADD) -lm libdroid-util.la libdroid-source.la
module_droid_source_la_CFLAGS = $(AM_CFLAGS)

module_droid_card_la_SOURCES = module-droid-card.c
module_droid_card_la_LDFLAGS = -module -avoid-version -Wl,-no-undefined -Wl,-z,noexecstack -lhybris-common
module_droid_card_la_LIBADD = $(AM_LIBADD) -lm libdroid-util.la libdroid-sink.la libdroid-source.la
module_droid_card_la_CFLAGS = $(AM_CFLAGS)
