QUIET_GEN = $(Q:@=@echo '  GEN   '$@;)

SUBDIRS = build scripts clutter-gst tests examples

if BUILD_GTK_DOC
SUBDIRS += doc
endif

DIST_SUBDIRS = build scripts clutter-gst tests examples doc

ACLOCAL_AMFLAGS = -I build/autotools ${ACLOCAL_FLAGS}

clutter-gst-$(CLUTTER_GST_API_VERSION).pc: clutter-gst.pc
	$(AM_V_GEN)cp -f clutter-gst.pc $@

pkgconfig_DATA = clutter-gst-$(CLUTTER_GST_API_VERSION).pc
pkgconfigdir   = $(libdir)/pkgconfig

EXTRA_DIST = clutter-gst.pc.in ChangeLog.pre-git-import

CLEANFILES = clutter-gst.pc clutter-gst-$(CLUTTER_GST_API_VERSION).pc

DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc

# Extra clean files so that maintainer-clean removes *everything*
MAINTAINERCLEANFILES = aclocal.m4 compile config.guess config.sub \
                       configure depcomp install-sh ltmain.sh     \
                       Makefile.in missing config.h.in

include $(top_srcdir)/build/autotools/Makefile.am.release
