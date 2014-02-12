NULL = #

noinst_PROGRAMS = 				\
	test-alpha				\
	test-rgb-upload				\
	test-start-stop				\
	test-yuv-upload				\
	test-video-texture-new-unref-loop	\
	$(NULL)

AM_CPPFLAGS = -I$(top_srcdir)      \
		$(MAINTAINER_CFLAGS) \
		$(NULL)

test_alpha_SOURCES = test-alpha.c
test_alpha_CFLAGS  = $(CLUTTER_GST_CFLAGS) $(GST_CFLAGS)
test_alpha_LDADD =		\
	$(CLUTTER_GST_LIBS)	\
	$(GST_LIBS)		\
	$(top_builddir)/clutter-gst/libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la

test_rgb_upload_SOURCES = test-rgb-upload.c
test_rgb_upload_CFLAGS  = $(CLUTTER_GST_CFLAGS) $(GST_CFLAGS)
test_rgb_upload_LDADD =	\
	$(CLUTTER_GST_LIBS)	\
	$(GST_LIBS)		\
	$(top_builddir)/clutter-gst/libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la

test_start_stop_SOURCES = test-start-stop.c
test_start_stop_CFLAGS  = $(CLUTTER_GST_CFLAGS) $(GST_CFLAGS)
test_start_stop_LDADD =	\
	$(CLUTTER_GST_LIBS)	\
	$(GST_LIBS)		\
	$(top_builddir)/clutter-gst/libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la

test_yuv_upload_SOURCES = test-yuv-upload.c
test_yuv_upload_CFLAGS  = $(CLUTTER_GST_CFLAGS) $(GST_CFLAGS)
test_yuv_upload_LDADD =	\
	$(CLUTTER_GST_LIBS)	\
	$(GST_LIBS)		\
	$(top_builddir)/clutter-gst/libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la

test_video_texture_new_unref_loop_SOURCES = test-video-texture-new-unref-loop.c
test_video_texture_new_unref_loop_CFLAGS  = $(CLUTTER_GST_CFLAGS) $(GST_CFLAGS)
test_video_texture_new_unref_loop_LDADD =	\
	$(CLUTTER_GST_LIBS)			\
	$(GST_LIBS)				\
	$(top_builddir)/clutter-gst/libclutter-gst-@CLUTTER_GST_MAJORMINOR@.la
