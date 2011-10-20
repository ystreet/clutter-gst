/*
 * Clutter-GStreamer.
 *
 * GStreamer integration library for Clutter.
 *
 * clutter-gst-util.c - Miscellaneous functions.
 *
 * Authored By Matthew Allum  <mallum@openedhand.com>
 * Authored By Damien Lespiau <damien.lespiau@intel.com>
 *
 * Copyright (C) 2006 OpenedHand
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:clutter-gst-util
 * @short_description: Utility functions for Clutter-Gst.
 *
 * The <application>Clutter-Gst</application> library should be initialized
 * with clutter_gst_init() before it can be used. You should pass pointers to
 * the main argc and argv variables so that GStreamer, Clutter and Clutter-Gst
 * gst can process their own command line options, as shown in the following
 * example:
 *
 * <example>
 * <title>Initializing the Clutter-Gst library</title>
 * <programlisting language="c">
 * int
 * main (int argc, char *argv[])
 * {
 *   // initialize the Clutter-Gst library
 *   clutter_gst_init (&amp;argc, &amp;argv);
 *   ...
 * }
 * </programlisting>
 * </example>
 *
 * It's allowed to pass two NULL pointers to clutter_gst_init() in case you
 * don't want to pass the command line arguments to GStreamer, Clutter and
 * Clutter-Gst.
 *
 * You can also use a #GOptionEntry array to initialize your own parameters
 * as shown in the next code fragment:
 *
 * <example>
 * <title>Initializing the Clutter-Gst library with additional options</title>
 * <programlisting language="c">
 * static GOptionEntry options[] =
 * {
 *   { "framerate", 'f', 0, G_OPTION_ARG_INT, &opt_framerate,
 *     "Number of frames per second", NULL },
 *   { "fourcc", 'o', 0, G_OPTION_ARG_STRING,
 *      &opt_fourcc, "Fourcc of the wanted YUV format", NULL },
 *   { NULL }
 * };
 *
 * int
 * main (int argc, char *argv[])
 * {
 *    GError           *error = NULL;
 *    gboolean          result;
 *
 *    if (!g_thread_supported ())
 *      g_thread_init (NULL);
 *
 *    result = clutter_gst_init_with_args (&argc, &argv,
 *                                         " - Test YUV frames uploading",
 *                                         options, NULL, &error);
 *
 *    if (error)
 *      {
 *        g_print ("%s\n", error->message);
 *        g_error_free (error);
 *        return EXIT_FAILURE;
 *      }
 *   ...
 * }
 * </programlisting>
 * </example>
 */

#include "config.h"

#include <gst/gst.h>
#include <clutter/clutter.h>

#if defined (CLUTTER_WINDOWING_X11)
#include <X11/Xlib.h>
#endif

#include "clutter-gst-private.h"
#include "clutter-gst-debug.h"
#include "clutter-gst-video-sink.h"
#include "clutter-gst-util.h"

static gboolean clutter_gst_is_initialized = FALSE;

/**
 * clutter_gst_init:
 * @argc: (inout): The number of arguments in @argv
 * @argv: (array length=argc) (inout) (allow-none): A pointer to an array
 *
 * Utility function to initialize both Clutter and GStreamer.
 *
 * This function should be called before calling any other GLib functions. If
 * this is not an option, your program must initialise the GLib thread system
 * using g_thread_init() before any other GLib functions are called.
 *
 * Return value: A #ClutterInitError.
 */
ClutterInitError
clutter_gst_init (int    *argc,
                  char ***argv)
{
  ClutterInitError retval;

  if (clutter_gst_is_initialized)
    return CLUTTER_INIT_SUCCESS;

#if defined (CLUTTER_WINDOWING_X11)
  /* Required by some GStreamer element like VA */
  XInitThreads ();
#endif

  gst_init (argc, argv);
  retval = clutter_init (argc, argv);

  /* initialize debugging infrastructure */
#ifdef CLUTTER_GST_ENABLE_DEBUG
  _clutter_gst_debug_init();
#endif
  gst_plugin_register_static (GST_VERSION_MAJOR,
			      GST_VERSION_MINOR,
			      "cluttersink",
			      "Element to render to Clutter textures",
			      _internal_plugin_init,
			      VERSION,
			      "LGPL", /* license */
			      "clutter-gst", PACKAGE,
			      "http://www.clutter-project.org");


  clutter_gst_is_initialized = TRUE;

  return retval;
}

/**
 * clutter_gst_init_with_args:
 * @argc: (inout): The number of arguments in @argv
 * @argv: (array length=argc) (inout) (allow-none): A pointer to an array
 * @parameter_string: a string which is displayed in
 *    the first line of <option>--help</option> output, after
 *    <literal><replaceable>programname</replaceable> [OPTION...]</literal>
 * @entries: a %NULL-terminated array of #GOptionEntry<!-- -->s
 *    describing the options of your program
 * @translation_domain: a translation domain to use for translating
 *    the <option>--help</option> output for the options in @entries
 *    with gettext(), or %NULL
 * @error: a return location for errors
 *
 * This function does the same work as clutter_gst_init(). Additionally, it
 * allows you to add your own command line options, and it automatically
 * generates nicely formatted --help output. Clutter's and GStreamer's
 * #GOptionGroup<!-- -->s are added to the set of available options.
 *
 * Your program must initialise the GLib thread system using g_thread_init()
 * before any other GLib functions are called.
 *
 * Return value: %CLUTTER_INIT_SUCCESS on success, a negative integer
 *   on failure.
 *
 * Since: 1.0
 */
ClutterInitError
clutter_gst_init_with_args (int            *argc,
                            char         ***argv,
                            const char     *parameter_string,
                            GOptionEntry   *entries,
                            const char     *translation_domain,
                            GError        **error)
{
  GOptionContext *context;
  gboolean res;

  if (clutter_gst_is_initialized)
    return CLUTTER_INIT_SUCCESS;

#if defined (CLUTTER_WINDOWING_X11)
  /* Required by some gstreamer element like VA */
  XInitThreads ();
#endif

  context = g_option_context_new (parameter_string);

  g_option_context_add_group (context, gst_init_get_option_group ());
  g_option_context_add_group (context, clutter_get_option_group ());

  if (entries)
    g_option_context_add_main_entries (context, entries, translation_domain);

  res = g_option_context_parse (context, argc, argv, error);
  g_option_context_free (context);

  if (!res)
        return CLUTTER_INIT_ERROR_INTERNAL;

  /* initialize debugging infrastructure */
#ifdef CLUTTER_GST_ENABLE_DEBUG
  _clutter_gst_debug_init ();
#endif
  gst_plugin_register_static (GST_VERSION_MAJOR,
			      GST_VERSION_MINOR,
			      "cluttersink",
			      "Element to render to Clutter textures",
			      _internal_plugin_init,
			      VERSION,
			      "LGPL", /* license */
			      "clutter-gst", PACKAGE,
			      "http://www.clutter-project.org");


  clutter_gst_is_initialized = TRUE;

  return CLUTTER_INIT_SUCCESS;
}

/**
 * clutter_gst_video_sink_new:
 * @texture: a #ClutterTexture
 *
 * Creates a new GStreamer video sink which uses @texture as the target
 * for sinking a video stream from GStreamer.
 *
 * <note>This function has to be called from Clutter's main thread. While
 * GStreamer will spawn threads to do its work, we want all the GL calls to
 * happen in the same thread. Clutter-gst knows which thread it is by
 * assuming this constructor is called from the Clutter thread.</note>
 *
 * Return value: a #GstElement for the newly created video sink
 *
 * Deprecated: 1.6: Use gst_element_factory_make ("cluttersink", ...) and the
 * "texture" GObject property instead.
 */
GstElement *
clutter_gst_video_sink_new (ClutterTexture *texture)
{
  return g_object_new (CLUTTER_GST_TYPE_VIDEO_SINK,
                       "texture", texture,
                       NULL);
}
