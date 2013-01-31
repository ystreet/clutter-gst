include $(top_srcdir)/build/autotools/Makefile.am.silent

NULL = #

SUBDIRS = shaders

CLEANFILES = #
DISTCLEANFILES = #
EXTRA_DIST = #
BUILT_SOURCES = #

# glib-mkenums
glib_enum_h = clutter-gst-enum-types.h
glib_enum_c = clutter-gst-enum-types.c
glib_enum_headers = $(source_h)
include $(top_srcdir)/build/autotools/Makefile.am.enums

# glib-genmarshal
glib_marshal_list = clutter-gst-marshal.list
glib_marshal_prefix = _clutter_gst_marshal
include $(top_srcdir)/build/autotools/Makefile.am.marshal

source_h = 					\
	$(srcdir)/clutter-gst.h			\
	$(srcdir)/clutter-gst-types.h		\
	$(srcdir)/clutter-gst-util.h		\
	$(srcdir)/clutter-gst-version.h		\
	$(srcdir)/clutter-gst-video-sink.h	\
	$(srcdir)/clutter-gst-video-texture.h 	\
	$(srcdir)/clutter-gst-player.h		\
	$(NULL)

source_priv_h =					\
	$(srcdir)/clutter-gst-debug.h		\
	$(srcdir)/clutter-gst-marshal.h		\
	$(srcdir)/clutter-gst-private.h		\
	$(NULL)

source_c = 					\
	$(srcdir)/clutter-gst-debug.c		\
	$(srcdir)/clutter-gst-marshal.c		\
	$(srcdir)/clutter-gst-player.c		\
	$(srcdir)/clutter-gst-video-sink.c	\
	$(srcdir)/clutter-gst-video-texture.c	\
        $(srcdir)/clutter-gst-util.c		\
	$(glib_enum_c)				\
	$(NULL)

libclutter_gst_@CLUTTER_GST_MAJORMINOR@_la_SOURCES =	\
	$(MARSHALFILES)  				\
	$(source_c)      				\
	$(source_priv_h) 				\
	$(source_h)					\
	$(glib_enum_h)					\
	$(NULL)

INCLUDES =					\
	-I$(top_srcdir)				\
	-I$(top_builddir)/clutter-gst/shaders	\
	-I$(top_srcdir)/clutter-gst/shaders	\
	$(NULL)

AM_CPPFLAGS =					\
	-DCLUTTER_GST_COMPILATION		\
	-DG_DISABLE_SINGLE_INCLUDES		\
	-DCOGL_ENABLE_EXPERIMENTAL_API		\
	-DCLUTTER_ENABLE_EXPERIMENTAL_API	\
	-DG_LOG_DOMAIN=\"Clutter-Gst\"		\
	$(NULL)

AM_CFLAGS = 				\
	$(MAINTAINER_CFLAGS)		\
	$(CLUTTER_GST_DEBUG_CFLAGS)	\
	$(CLUTTER_GST_CFLAGS)		\
	$(GST_CFLAGS)			\
	$(HW_CFLAGS)			\
	$(NULL)

lib_LTLIBRARIES = libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la

libclutter_gst_@CLUTTER_GST_MAJORMINOR@_la_LIBADD  = @CLUTTER_GST_LIBS@ $(GST_LIBS) $(HW_LIBS)
libclutter_gst_@CLUTTER_GST_MAJORMINOR@_la_LDFLAGS =	\
	$(CLUTTER_GST_LT_LDFLAGS)			\
	$(GL_LDFLAGS)					\
	-export-dynamic					\
	-export-symbols-regex "^clutter_gst.*"		\
	-rpath $(libdir)
	$(NULL)

cluttergstheadersdir = $(includedir)/clutter-gst-@CLUTTER_GST_API_VERSION@/clutter-gst
cluttergstheaders_HEADERS = $(source_h) $(glib_enum_h)

#
# GStreamer plugin
#

plugin_source_c = 				\
	$(srcdir)/clutter-gst-plugin.c	\
	$(srcdir)/clutter-gst-video-sink.c	\
	$(srcdir)/clutter-gst-auto-video-sink.c	\
	$(srcdir)/clutter-gst-auto-video-sink.h	\
	$(NULL)

libgstclutter_la_SOURCES =	\
	$(plugin_source_c)      \
	$(NULL)

plugin_LTLIBRARIES = libgstclutter.la

libgstclutter_la_LIBADD  = $(PLUGIN_LIBS) $(HW_LIBS)
libgstclutter_la_LDFLAGS =	\
	$(GL_LDFLAGS)		\
	-module -avoid-version	\
	-no-undefined		\
	$(NULL)

#
# Introspection data
#

if HAVE_INTROSPECTION
BUILT_GIRSOURCES =

ClutterGst-@CLUTTER_GST_API_VERSION@.gir: $(INTROSPECTION_SCANNER) libclutter-gst-@CLUTTER_GST_API_VERSION@.la
	$(QUIET_GEN)$(INTROSPECTION_SCANNER) -v \
		--namespace ClutterGst --nsversion=@CLUTTER_GST_API_VERSION@ \
		$(INCLUDES) \
		$(AM_CPPFLAGS) \
		-D GST_USE_UNSTABLE_API \
		--quiet --warn-all \
		--add-include-path=$(srcdir) --add-include=path=. \
		--c-include="clutter-gst/clutter-gst.h" \
		--include=GObject-2.0 \
		--include=Clutter-1.0 \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir gstreamer-1.0` \
		--include=Gst-1.0 \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir gstreamer-base-1.0` \
		--include=GstBase-1.0 \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir gstreamer-video-1.0` \
		--include=GstVideo-1.0 \
		--add-include-path=`$(PKG_CONFIG) --variable=girdir gstreamer-audio-1.0` \
		--include=GstAudio-1.0 \
		--add-init-section="clutter_gst_init(0,NULL);" \
		--library=libclutter-gst-@CLUTTER_GST_API_VERSION@.la \
		--libtool="$(top_builddir)/libtool" \
		--output $@ \
		--pkg gobject-2.0 \
		--pkg clutter-1.0 \
		--pkg gstreamer-1.0 \
		--pkg gstreamer-base-1.0 \
		--pkg-export clutter-gst-@CLUTTER_GST_API_VERSION@ \
		$(cluttergstheaders_HEADERS) \
		$(source_c)

BUILT_GIRSOURCES += ClutterGst-@CLUTTER_GST_API_VERSION@.gir

# INTROSPECTION_GIRDIR/INTROSPECTION_TYPELIBDIR aren't the right place to
# install anything - we need to install inside our prefix.
girdir = $(datadir)/gir-1.0
gir_DATA = $(BUILT_GIRSOURCES)

typelibsdir = $(libdir)/girepository-1.0/
typelibs_DATA = $(BUILT_GIRSOURCES:.gir=.typelib)

%.typelib: %.gir $(INTROSPECTION_COMPILER)
	$(QUIET_GEN)$(INTROSPECTION_COMPILER) \
		--includedir=$(srcdir) \
		--includedir=. \
		--includedir=`$(PKG_CONFIG) --variable=girdir gstreamer-1.0` \
		--includedir=`$(PKG_CONFIG) --variable=girdir gstreamer-base-1.0` \
		--includedir=`$(PKG_CONFIG) --variable=girdir gstreamer-video-1.0` \
		--includedir=`$(PKG_CONFIG) --variable=girdir gstreamer-audio-1.0` \
		$(INTROSPECTION_COMPILER_OPTS) $< -o $(builddir)/$(@F)

CLEANFILES += $(BUILT_GIRSOURCES) $(typelibs_DATA)
endif
