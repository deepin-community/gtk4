/* GDK - The GIMP Drawing Kit
 *
 * gdkglcontext.c: GL context abstraction
 *
 * Copyright © 2014  Emmanuele Bassi
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * GdkGLContext:
 *
 * `GdkGLContext` is an object representing a platform-specific
 * OpenGL draw context.
 *
 * `GdkGLContext`s are created for a surface using
 * [method@Gdk.Surface.create_gl_context], and the context will match
 * the characteristics of the surface.
 *
 * A `GdkGLContext` is not tied to any particular normal framebuffer.
 * For instance, it cannot draw to the surface back buffer. The GDK
 * repaint system is in full control of the painting to that. Instead,
 * you can create render buffers or textures and use [func@cairo_draw_from_gl]
 * in the draw function of your widget to draw them. Then GDK will handle
 * the integration of your rendering with that of other widgets.
 *
 * Support for `GdkGLContext` is platform-specific and context creation
 * can fail, returning %NULL context.
 *
 * A `GdkGLContext` has to be made "current" in order to start using
 * it, otherwise any OpenGL call will be ignored.
 *
 * ## Creating a new OpenGL context
 *
 * In order to create a new `GdkGLContext` instance you need a `GdkSurface`,
 * which you typically get during the realize call of a widget.
 *
 * A `GdkGLContext` is not realized until either [method@Gdk.GLContext.make_current]
 * or [method@Gdk.GLContext.realize] is called. It is possible to specify
 * details of the GL context like the OpenGL version to be used, or whether
 * the GL context should have extra state validation enabled after calling
 * [method@Gdk.Surface.create_gl_context] by calling [method@Gdk.GLContext.realize].
 * If the realization fails you have the option to change the settings of
 * the `GdkGLContext` and try again.
 *
 * ## Using a GdkGLContext
 *
 * You will need to make the `GdkGLContext` the current context before issuing
 * OpenGL calls; the system sends OpenGL commands to whichever context is current.
 * It is possible to have multiple contexts, so you always need to ensure that
 * the one which you want to draw with is the current one before issuing commands:
 *
 * ```c
 * gdk_gl_context_make_current (context);
 * ```
 *
 * You can now perform your drawing using OpenGL commands.
 *
 * You can check which `GdkGLContext` is the current one by using
 * [func@Gdk.GLContext.get_current]; you can also unset any `GdkGLContext`
 * that is currently set by calling [func@Gdk.GLContext.clear_current].
 */

#include "config.h"

#include "gdkglcontextprivate.h"

#include "gdkdebugprivate.h"
#include "gdkdisplayprivate.h"
#include <glib/gi18n-lib.h>
#include "gdkmemoryformatprivate.h"
#include "gdkmemorytextureprivate.h"
#include "gdkprofilerprivate.h"
#include "gdkglversionprivate.h"

#include "gdkprivate.h"

#ifdef GDK_WINDOWING_WIN32
# include "gdk/win32/gdkwin32.h"
#endif

#include <epoxy/gl.h>
#ifdef HAVE_EGL
#include <epoxy/egl.h>
#endif

#include <math.h>

#define DEFAULT_ALLOWED_APIS GDK_GL_API_GL | GDK_GL_API_GLES

typedef struct {
  GdkGLVersion required;
  GdkGLVersion gl_version;

  guint has_khr_debug : 1;
  guint use_khr_debug : 1;
  guint has_half_float : 1;
  guint has_sync : 1;
  guint has_unpack_subimage : 1;
  guint has_debug_output : 1;
  guint extensions_checked : 1;
  guint debug_enabled : 1;
  guint forward_compatible : 1;
  guint is_legacy : 1;

  GdkGLAPI allowed_apis;
  GdkGLAPI api;

  int max_debug_label_length;

#ifdef HAVE_EGL
  EGLContext egl_context;
  EGLBoolean (*eglSwapBuffersWithDamage) (EGLDisplay, EGLSurface, const EGLint *, EGLint);
#endif
} GdkGLContextPrivate;

enum {
  PROP_0,

  PROP_ALLOWED_APIS,
  PROP_API,
  PROP_SHARED_CONTEXT,

  LAST_PROP
};

static GParamSpec *properties[LAST_PROP] = { NULL, };

G_DEFINE_QUARK (gdk-gl-error-quark, gdk_gl_error)

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GdkGLContext, gdk_gl_context, GDK_TYPE_DRAW_CONTEXT)

typedef struct _MaskedContext MaskedContext;

static inline MaskedContext *
mask_context (GdkGLContext *context,
              gboolean      surfaceless)
{
  return (MaskedContext *) GSIZE_TO_POINTER (GPOINTER_TO_SIZE (context) | (surfaceless ? 1 : 0));
}

static inline GdkGLContext *
unmask_context (MaskedContext *mask)
{
  return GDK_GL_CONTEXT (GSIZE_TO_POINTER (GPOINTER_TO_SIZE (mask) & ~(gsize) 1));
}

static inline gboolean
mask_is_surfaceless (MaskedContext *mask)
{
  return GPOINTER_TO_SIZE (mask) & (gsize) 1;
}

static void
unref_unmasked (gpointer data)
{
  g_object_unref (unmask_context (data));
}

static GPrivate thread_current_context = G_PRIVATE_INIT (unref_unmasked);

static void
gdk_gl_context_clear_old_updated_area (GdkGLContext *context)
{
  for (unsigned int i = 0; i < GDK_GL_MAX_TRACKED_BUFFERS; i++)
    g_clear_pointer (&context->old_updated_area[i], cairo_region_destroy);
}

static void
gdk_gl_context_dispose (GObject *gobject)
{
  GdkGLContext *context = GDK_GL_CONTEXT (gobject);
#ifdef HAVE_EGL
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  if (priv->egl_context != NULL)
    {
      GdkDisplay *display = gdk_draw_context_get_display (GDK_DRAW_CONTEXT (context));
      EGLDisplay *egl_display = gdk_display_get_egl_display (display);

      if (eglGetCurrentContext () == priv->egl_context)
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

      GDK_DISPLAY_DEBUG (display, OPENGL, "Destroying EGL context");

      eglDestroyContext (egl_display, priv->egl_context);
      priv->egl_context = NULL;
    }
#endif

  gdk_gl_context_clear_old_updated_area (context);

  G_OBJECT_CLASS (gdk_gl_context_parent_class)->dispose (gobject);
}

static void
gdk_gl_context_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  GdkGLContext *self = GDK_GL_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_ALLOWED_APIS:
      gdk_gl_context_set_allowed_apis (self, g_value_get_flags (value));
      break;

    case PROP_SHARED_CONTEXT:
      g_assert (g_value_get_object (value) == NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gdk_gl_context_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GdkGLContext *self = GDK_GL_CONTEXT (object);
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_ALLOWED_APIS:
      g_value_set_flags (value, priv->allowed_apis);
      break;

    case PROP_API:
      g_value_set_flags (value, priv->api);
      break;

    case PROP_SHARED_CONTEXT:
      g_value_set_object (value, NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

#define N_EGL_ATTRS 16

#ifdef HAVE_EGL
static inline EGLenum
gdk_api_to_egl_api (GdkGLAPI api)
{
  switch (api)
    {
    case GDK_GL_API_GLES:
      return EGL_OPENGL_ES_API;
    case GDK_GL_API_GL:
    default:
      return EGL_OPENGL_API;
    }
}

static GdkGLAPI
gdk_gl_context_create_egl_context (GdkGLContext *context,
                                   GdkGLAPI      api,
                                   gboolean      legacy)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkDisplay *display = gdk_gl_context_get_display (context);
  EGLDisplay egl_display = gdk_display_get_egl_display (display);
  GdkGLContext *share = gdk_display_get_gl_context (display);
  GdkGLContextPrivate *share_priv = gdk_gl_context_get_instance_private (share);
  EGLConfig egl_config;
  EGLContext ctx;
  EGLint context_attribs[N_EGL_ATTRS], i = 0, flags = 0;
  gsize major_idx, minor_idx;
  gboolean debug_bit, forward_bit;
  GdkGLVersion version;
  const GdkGLVersion* supported_versions;
  gsize j;
  G_GNUC_UNUSED gint64 start_time = GDK_PROFILER_CURRENT_TIME;

  if (!gdk_gl_context_is_api_allowed (context, api, NULL))
    return 0;

  /* We will use the default version matching the context status
   * unless the user requested a version which makes sense */
  gdk_gl_context_get_matching_version (context, api, legacy, &version);

  if (!eglBindAPI (gdk_api_to_egl_api (api)))
    return 0;

  debug_bit = gdk_gl_context_get_debug_enabled (context);
  forward_bit = gdk_gl_context_get_forward_compatible (context);

  if (display->have_egl_no_config_context)
    egl_config = NULL;
  else
    egl_config = gdk_display_get_egl_config (display);

  if (debug_bit)
    flags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;
  if (forward_bit)
    flags |= EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;

  if (api == GDK_GL_API_GL)
    {
      /* We want a core profile, unless in legacy mode */
      context_attribs[i++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
      context_attribs[i++] = legacy
        ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT
        : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
    }

  if (legacy || api == GDK_GL_API_GLES)
    flags &= ~EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR;

  context_attribs[i++] = EGL_CONTEXT_MAJOR_VERSION;
  major_idx = i++;
  context_attribs[i++] = EGL_CONTEXT_MINOR_VERSION;
  minor_idx = i++;
  context_attribs[i++] = EGL_CONTEXT_FLAGS_KHR;
  context_attribs[i++] = flags;

  context_attribs[i++] = EGL_NONE;
  g_assert (i < N_EGL_ATTRS);

  GDK_DISPLAY_DEBUG (display, OPENGL,
                     "Creating EGL context version %d.%d (debug:%s, forward:%s, legacy:%s, es:%s)",
                     gdk_gl_version_get_major (&version), gdk_gl_version_get_minor (&version),
                     debug_bit ? "yes" : "no",
                     forward_bit ? "yes" : "no",
                     legacy ? "yes" : "no",
                     api == GDK_GL_API_GLES ? "yes" : "no");

  supported_versions = gdk_gl_versions_get_for_api (api);
  for (j = 0; gdk_gl_version_greater_equal (&supported_versions[j], &version); j++)
    {
      context_attribs [major_idx] = gdk_gl_version_get_major (&supported_versions[j]);
      context_attribs [minor_idx] = gdk_gl_version_get_minor (&supported_versions[j]);

      ctx = eglCreateContext (egl_display,
                              egl_config,
                              share ? share_priv->egl_context : EGL_NO_CONTEXT,
                              context_attribs);
      if (ctx != NULL)
        break;
    }

  if (ctx == NULL)
    return 0;

  GDK_DISPLAY_DEBUG (display, OPENGL, "Created EGL context[%p]", ctx);

  priv->egl_context = ctx;
  gdk_gl_context_set_version (context, &supported_versions[j]);
  gdk_gl_context_set_is_legacy (context, legacy);

  if (epoxy_has_egl_extension (egl_display, "EGL_KHR_swap_buffers_with_damage"))
    priv->eglSwapBuffersWithDamage = (gpointer) epoxy_eglGetProcAddress ("eglSwapBuffersWithDamageKHR");
  else if (epoxy_has_egl_extension (egl_display, "EGL_EXT_swap_buffers_with_damage"))
    priv->eglSwapBuffersWithDamage = (gpointer) epoxy_eglGetProcAddress ("eglSwapBuffersWithDamageEXT");

  gdk_profiler_end_mark (start_time, "realize GdkWaylandGLContext", NULL);

  return api;
}

static GdkGLAPI
gdk_gl_context_realize_egl (GdkGLContext  *context,
                            GError       **error)
{
  GdkDisplay *display = gdk_gl_context_get_display (context);
  GdkGLContext *share = gdk_display_get_gl_context (display);
  GdkGLAPI api, preferred_api;
  gboolean prefer_legacy;

  if (share && gdk_gl_context_is_api_allowed (context,
                                              gdk_gl_context_get_api (share),
                                              NULL))
    preferred_api = gdk_gl_context_get_api (share);
  else if (gdk_gl_context_is_api_allowed (context, GDK_GL_API_GL, NULL))
    preferred_api = GDK_GL_API_GL;
  else if (gdk_gl_context_is_api_allowed (context, GDK_GL_API_GLES, NULL))
    preferred_api = GDK_GL_API_GLES;
  else
    {
      g_set_error_literal (error, GDK_GL_ERROR,
                           GDK_GL_ERROR_NOT_AVAILABLE,
                           _("No GL API allowed."));
      return 0;
    }

  prefer_legacy = (gdk_display_get_debug_flags(display) & GDK_DEBUG_GL_LEGACY) ||
                   (share != NULL && gdk_gl_context_is_legacy (share));

  if (preferred_api == GDK_GL_API_GL)
    {
      if ((api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GL, prefer_legacy)) ||
          (api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GLES, FALSE)) ||
          (api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GL, TRUE)))
        return api;
    }
  else
    {
      if ((api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GLES, FALSE)) ||
          (api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GL, prefer_legacy)) ||
          (api = gdk_gl_context_create_egl_context (context, GDK_GL_API_GL, TRUE)))
        return api;
    }

  g_set_error_literal (error, GDK_GL_ERROR,
                       GDK_GL_ERROR_NOT_AVAILABLE,
                       _("Unable to create a GL context"));
  return 0;
}
#endif /* HAVE_EGL */

static GdkGLAPI
gdk_gl_context_default_realize (GdkGLContext  *context,
                                GError       **error)
{
#ifdef HAVE_EGL
  GdkDisplay *display = gdk_gl_context_get_display (context);

  if (gdk_display_get_egl_display (display))
    return gdk_gl_context_realize_egl (context, error);
#endif

  g_set_error_literal (error, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
                       _("The current backend does not support OpenGL"));
  return 0;
}

#undef N_EGL_ATTRS

static cairo_region_t *
gdk_gl_context_real_get_damage (GdkGLContext *context)
{
  GdkSurface *surface = gdk_draw_context_get_surface (GDK_DRAW_CONTEXT (context));
#ifdef HAVE_EGL
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkDisplay *display = gdk_draw_context_get_display (GDK_DRAW_CONTEXT (context));

  if (priv->egl_context && display->have_egl_buffer_age)
    {
      EGLSurface egl_surface;
      int buffer_age = 0;
      egl_surface = gdk_surface_get_egl_surface (surface);
      gdk_gl_context_make_current (context);
      eglQuerySurface (gdk_display_get_egl_display (display), egl_surface,
                       EGL_BUFFER_AGE_EXT, &buffer_age);

      if (buffer_age > 0 && buffer_age <= GDK_GL_MAX_TRACKED_BUFFERS)
        {
          cairo_region_t *damage = cairo_region_create ();
          int i;

          for (i = 0; i < buffer_age - 1; i++)
            {
              if (context->old_updated_area[i] == NULL)
                {
                  cairo_region_create_rectangle (&(GdkRectangle) {
                                                     0, 0,
                                                     gdk_surface_get_width (surface),
                                                     gdk_surface_get_height (surface)
                                                 });
                  break;
                }
              cairo_region_union (damage, context->old_updated_area[i]);
            }

          return damage;
        }
    }
#endif

  return cairo_region_create_rectangle (&(GdkRectangle) {
                                            0, 0,
                                            gdk_surface_get_width (surface),
                                            gdk_surface_get_height (surface)
                                        });
}

static gboolean
gdk_gl_context_real_is_shared (GdkGLContext *self,
                               GdkGLContext *other)
{
  if (gdk_draw_context_get_display (GDK_DRAW_CONTEXT (self)) != gdk_draw_context_get_display (GDK_DRAW_CONTEXT (other)))
    return FALSE;

  /* XXX: Should we check es or legacy here? */

  return TRUE;
}

static gboolean
gdk_gl_context_real_is_current (GdkGLContext *self)
{
#ifdef HAVE_EGL
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  return priv->egl_context == eglGetCurrentContext ();
#else
  return TRUE;
#endif
}

static gboolean
gdk_gl_context_real_clear_current (GdkGLContext *context)
{
#ifdef HAVE_EGL
  GdkDisplay *display = gdk_gl_context_get_display (context);
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  if (priv->egl_context == NULL)
    return FALSE;

  return eglMakeCurrent (gdk_display_get_egl_display (display),
                         EGL_NO_SURFACE,
                         EGL_NO_SURFACE,
                         EGL_NO_CONTEXT);
#else
  return FALSE;
#endif
}

static gboolean
gdk_gl_context_real_make_current (GdkGLContext *context,
                                  gboolean      surfaceless)
{
#ifdef HAVE_EGL
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkDisplay *display = gdk_gl_context_get_display (context);
  EGLSurface egl_surface;

  if (priv->egl_context == NULL)
    return FALSE;

  if (!surfaceless)
    egl_surface = gdk_surface_get_egl_surface (gdk_gl_context_get_surface (context));
  else
    egl_surface = EGL_NO_SURFACE;

  return eglMakeCurrent (gdk_display_get_egl_display (display),
                         egl_surface,
                         egl_surface,
                         priv->egl_context);
#else
  return FALSE;
#endif
}

double
gdk_gl_context_get_scale (GdkGLContext *self)
{
  GdkDisplay *display;
  GdkSurface *surface;
  double scale;

  surface = gdk_draw_context_get_surface (GDK_DRAW_CONTEXT (self));
  scale = gdk_surface_get_scale (surface);

  display = gdk_gl_context_get_display (self);
  if (!(gdk_display_get_debug_flags (display) & GDK_DEBUG_GL_FRACTIONAL))
    scale = ceil (scale);

  return scale;
}

static void
gdk_gl_context_real_begin_frame (GdkDrawContext *draw_context,
                                 GdkMemoryDepth  depth,
                                 cairo_region_t *region)
{
  GdkGLContext *context = GDK_GL_CONTEXT (draw_context);
  G_GNUC_UNUSED GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkSurface *surface;
  cairo_region_t *damage;
  double scale;
  int ww, wh;
  int i;

  surface = gdk_draw_context_get_surface (draw_context);
  scale = gdk_gl_context_get_scale (context);

#ifdef HAVE_EGL
  if (priv->egl_context)
    gdk_surface_ensure_egl_surface (surface, depth != GDK_MEMORY_U8);
#endif

  damage = GDK_GL_CONTEXT_GET_CLASS (context)->get_damage (context);

  g_clear_pointer (&context->old_updated_area[GDK_GL_MAX_TRACKED_BUFFERS - 1], cairo_region_destroy);
  for (i = GDK_GL_MAX_TRACKED_BUFFERS - 1; i > 0; i--)
    {
      context->old_updated_area[i] = context->old_updated_area[i - 1];
    }
  context->old_updated_area[0] = cairo_region_copy (region);

  cairo_region_union (region, damage);
  cairo_region_destroy (damage);

  ww = (int) ceil (gdk_surface_get_width (surface) * scale);
  wh = (int) ceil (gdk_surface_get_height (surface) * scale);

  gdk_gl_context_make_current (context);

  /* Initial setup */
  glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
  glDisable (GL_DEPTH_TEST);
  glDisable (GL_BLEND);
  glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  glViewport (0, 0, ww, wh);

#ifdef HAVE_EGL
  if (priv->egl_context && gdk_gl_context_check_version (context, NULL, "3.0"))
    glDrawBuffers (1, (GLenum[1]) { gdk_gl_context_get_use_es (context) ? GL_BACK : GL_BACK_LEFT });
#endif
}

static void
gdk_gl_context_real_end_frame (GdkDrawContext *draw_context,
                               cairo_region_t *painted)
{
#ifdef HAVE_EGL
  GdkGLContext *context = GDK_GL_CONTEXT (draw_context);
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkSurface *surface = gdk_gl_context_get_surface (context);
  GdkDisplay *display = gdk_surface_get_display (surface);
  EGLSurface egl_surface;

  if (priv->egl_context == NULL)
    return;

  gdk_gl_context_make_current (context);

  egl_surface = gdk_surface_get_egl_surface (surface);

  gdk_profiler_add_mark (GDK_PROFILER_CURRENT_TIME, 0, "EGL", "swap buffers");

  if (priv->eglSwapBuffersWithDamage)
    {
      EGLint stack_rects[4 * 4]; /* 4 rects */
      EGLint *heap_rects = NULL;
      int i, j, n_rects = cairo_region_num_rectangles (painted);
      int surface_height = gdk_surface_get_height (surface);
      double scale = gdk_gl_context_get_scale (context);
      EGLint *rects;

      if (n_rects < G_N_ELEMENTS (stack_rects) / 4)
        rects = (EGLint *)&stack_rects;
      else
        heap_rects = rects = g_new (EGLint, n_rects * 4);

      for (i = 0, j = 0; i < n_rects; i++)
        {
          cairo_rectangle_int_t rect;

          cairo_region_get_rectangle (painted, i, &rect);
          rects[j++] = (int) floor (rect.x * scale);
          rects[j++] = (int) floor ((surface_height - rect.height - rect.y) * scale);
          rects[j++] = (int) ceil (rect.width * scale);
          rects[j++] = (int) ceil (rect.height * scale);
        }
      priv->eglSwapBuffersWithDamage (gdk_display_get_egl_display (display), egl_surface, rects, n_rects);
      g_free (heap_rects);
    }
  else
    eglSwapBuffers (gdk_display_get_egl_display (display), egl_surface);
#endif
}

static void
gdk_gl_context_surface_resized (GdkDrawContext *draw_context)
{
  GdkGLContext *context = GDK_GL_CONTEXT (draw_context);

  gdk_gl_context_clear_old_updated_area (context);
}

static guint
gdk_gl_context_real_get_default_framebuffer (GdkGLContext *self)
{
  return 0;
}

static void
gdk_gl_context_class_init (GdkGLContextClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GdkDrawContextClass *draw_context_class = GDK_DRAW_CONTEXT_CLASS (klass);

  klass->realize = gdk_gl_context_default_realize;
  klass->get_damage = gdk_gl_context_real_get_damage;
  klass->is_shared = gdk_gl_context_real_is_shared;
  klass->make_current = gdk_gl_context_real_make_current;
  klass->clear_current = gdk_gl_context_real_clear_current;
  klass->is_current = gdk_gl_context_real_is_current;
  klass->get_default_framebuffer = gdk_gl_context_real_get_default_framebuffer;

  draw_context_class->begin_frame = gdk_gl_context_real_begin_frame;
  draw_context_class->end_frame = gdk_gl_context_real_end_frame;
  draw_context_class->surface_resized = gdk_gl_context_surface_resized;

  /**
   * GdkGLContext:shared-context: (attributes org.gtk.Property.get=gdk_gl_context_get_shared_context)
   *
   * Always %NULL
   *
   * As many contexts can share data now and no single shared context exists
   * anymore, this function has been deprecated and now always returns %NULL.
   *
   * Deprecated: 4.4: Use [method@Gdk.GLContext.is_shared] to check if contexts
   *   can be shared.
   */
  properties[PROP_SHARED_CONTEXT] =
    g_param_spec_object ("shared-context", NULL, NULL,
                         GDK_TYPE_GL_CONTEXT,
                         G_PARAM_READWRITE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS |
                         G_PARAM_DEPRECATED);

  /**
   * GdkGLContext:allowed-apis: (attributes org.gtk.Property.get=gdk_gl_context_get_allowed_apis org.gtk.Property.gdk_gl_context_set_allowed_apis)
   *
   * The allowed APIs.
   *
   * Since: 4.6
   */
  properties[PROP_ALLOWED_APIS] =
    g_param_spec_flags ("allowed-apis", NULL, NULL,
                        GDK_TYPE_GL_API,
                        DEFAULT_ALLOWED_APIS,
                        G_PARAM_READWRITE |
                        G_PARAM_STATIC_STRINGS |
                        G_PARAM_EXPLICIT_NOTIFY);

  /**
   * GdkGLContext:api: (attributes org.gtk.Property.get=gdk_gl_context_get_api)
   *
   * The API currently in use.
   *
   * Since: 4.6
   */
  properties[PROP_API] =
    g_param_spec_flags ("api", NULL, NULL,
                        GDK_TYPE_GL_API,
                        0,
                        G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS |
                        G_PARAM_EXPLICIT_NOTIFY);

  gobject_class->set_property = gdk_gl_context_set_property;
  gobject_class->get_property = gdk_gl_context_get_property;
  gobject_class->dispose = gdk_gl_context_dispose;

  g_object_class_install_properties (gobject_class, LAST_PROP, properties);
}

static void
gdk_gl_context_init (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  priv->allowed_apis = DEFAULT_ALLOWED_APIS;
}

/* Must have called gdk_display_prepare_gl() before */
GdkGLContext *
gdk_gl_context_new (GdkDisplay *display,
                    GdkSurface *surface)
{
  GdkGLContext *shared;

  g_assert (surface == NULL || display == gdk_surface_get_display (surface));

  /* assert gdk_display_prepare_gl() had been called */
  shared = gdk_display_get_gl_context (display);
  g_assert (shared);

  return g_object_new (G_OBJECT_TYPE (shared),
                       "display", display,
                       "surface", surface,
                       NULL);
}

void
gdk_gl_context_push_debug_group (GdkGLContext *context,
                                 const char   *message)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  if (priv->use_khr_debug)
    glPushDebugGroupKHR (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message);
}

void
gdk_gl_context_push_debug_group_printf (GdkGLContext *context,
                                        const char   *format,
                                        ...)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  char *message;
  va_list args;

  if (priv->use_khr_debug)
    {
      int msg_len;

      va_start (args, format);
      message = g_strdup_vprintf (format, args);
      va_end (args);

      msg_len = MIN (priv->max_debug_label_length, strlen (message) - 1);
      glPushDebugGroupKHR (GL_DEBUG_SOURCE_APPLICATION, 0, msg_len, message);
      g_free (message);
    }
}

void
gdk_gl_context_pop_debug_group (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  if (priv->use_khr_debug)
    glPopDebugGroupKHR ();
}

void
gdk_gl_context_label_object (GdkGLContext *context,
                             guint         identifier,
                             guint         name,
                             const char   *label)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  if (priv->use_khr_debug)
    glObjectLabel (identifier, name, -1, label);
}

void
gdk_gl_context_label_object_printf  (GdkGLContext *context,
                                     guint         identifier,
                                     guint         name,
                                     const char   *format,
                                     ...)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  char *message;
  va_list args;

  if (priv->use_khr_debug)
    {
      int msg_len;

      va_start (args, format);
      message = g_strdup_vprintf (format, args);
      va_end (args);

      msg_len = MIN (priv->max_debug_label_length, strlen (message) - 1);

      glObjectLabel (identifier, name, msg_len, message);
      g_free (message);
    }
}


gboolean
gdk_gl_context_has_unpack_subimage (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  return priv->has_unpack_subimage;
}

static gboolean
gdk_gl_context_is_realized (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  return priv->api != 0;
}

/**
 * gdk_gl_context_set_debug_enabled:
 * @context: a `GdkGLContext`
 * @enabled: whether to enable debugging in the context
 *
 * Sets whether the `GdkGLContext` should perform extra validations and
 * runtime checking.
 *
 * This is useful during development, but has additional overhead.
 *
 * The `GdkGLContext` must not be realized or made current prior to
 * calling this function.
 */
void
gdk_gl_context_set_debug_enabled (GdkGLContext *context,
                                  gboolean      enabled)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));
  g_return_if_fail (!gdk_gl_context_is_realized (context));

  enabled = !!enabled;

  priv->debug_enabled = enabled;
}

/**
 * gdk_gl_context_get_debug_enabled:
 * @context: a `GdkGLContext`
 *
 * Retrieves whether the context is doing extra validations and runtime checking.
 *
 * See [method@Gdk.GLContext.set_debug_enabled].
 *
 * Returns: %TRUE if debugging is enabled
 */
gboolean
gdk_gl_context_get_debug_enabled (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), FALSE);

  return priv->debug_enabled;
}

/**
 * gdk_gl_context_set_forward_compatible:
 * @context: a `GdkGLContext`
 * @compatible: whether the context should be forward-compatible
 *
 * Sets whether the `GdkGLContext` should be forward-compatible.
 *
 * Forward-compatible contexts must not support OpenGL functionality that
 * has been marked as deprecated in the requested version; non-forward
 * compatible contexts, on the other hand, must support both deprecated and
 * non deprecated functionality.
 *
 * The `GdkGLContext` must not be realized or made current prior to calling
 * this function.
 */
void
gdk_gl_context_set_forward_compatible (GdkGLContext *context,
                                       gboolean      compatible)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));
  g_return_if_fail (!gdk_gl_context_is_realized (context));

  compatible = !!compatible;

  priv->forward_compatible = compatible;
}

/**
 * gdk_gl_context_get_forward_compatible:
 * @context: a `GdkGLContext`
 *
 * Retrieves whether the context is forward-compatible.
 *
 * See [method@Gdk.GLContext.set_forward_compatible].
 *
 * Returns: %TRUE if the context should be forward-compatible
 */
gboolean
gdk_gl_context_get_forward_compatible (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), FALSE);

  return priv->forward_compatible;
}

void
gdk_gl_context_get_matching_version (GdkGLContext *context,
                                     GdkGLAPI      api,
                                     gboolean      legacy,
                                     GdkGLVersion *out_version)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  GdkGLVersion min_version;

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));

  if (api == GDK_GL_API_GL)
    {
      if (legacy)
        min_version = GDK_GL_MIN_GL_LEGACY_VERSION;
      else
        min_version = GDK_GL_MIN_GL_VERSION;
    }
  else
    {
      min_version = GDK_GL_MIN_GLES_VERSION;
    }

  if (gdk_gl_version_greater_equal (&priv->required, &min_version))
    *out_version = priv->required;
  else
    *out_version = min_version;

}

/**
 * gdk_gl_context_set_required_version:
 * @context: a `GdkGLContext`
 * @major: the major version to request
 * @minor: the minor version to request
 *
 * Sets the major and minor version of OpenGL to request.
 *
 * Setting @major and @minor to zero will use the default values.
 *
 * Setting @major and @minor lower than the minimum versions required
 * by GTK will result in the context choosing the minimum version.
 *
 * The @context must not be realized or made current prior to calling
 * this function.
 */
void
gdk_gl_context_set_required_version (GdkGLContext *context,
                                     int           major,
                                     int           minor)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));
  g_return_if_fail (!gdk_gl_context_is_realized (context));

  priv->required = GDK_GL_VERSION_INIT (major, minor);
}

gboolean
gdk_gl_context_check_gl_version (GdkGLContext       *self,
                                 const GdkGLVersion *required_gl,
                                 const GdkGLVersion *required_gles)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (self), FALSE);

  if (!gdk_gl_context_is_realized (self))
    return FALSE;

  switch (priv->api)
    {
    case GDK_GL_API_GL:
      return required_gl == NULL || gdk_gl_version_greater_equal (&priv->gl_version, required_gl);

    case GDK_GL_API_GLES:
      return required_gles == NULL || gdk_gl_version_greater_equal (&priv->gl_version, required_gles);

    default:
      g_return_val_if_reached (FALSE);

    }
}

/**
 * gdk_gl_context_get_required_version:
 * @context: a `GdkGLContext`
 * @major: (out) (nullable): return location for the major version to request
 * @minor: (out) (nullable): return location for the minor version to request
 *
 * Retrieves required OpenGL version set as a requirement for the @context
 * realization. It will not change even if a greater OpenGL version is supported
 * and used after the @context is realized. See
 * [method@Gdk.GLContext.get_version] for the real version in use.
 *
 * See [method@Gdk.GLContext.set_required_version].
 */
void
gdk_gl_context_get_required_version (GdkGLContext *context,
                                     int          *major,
                                     int          *minor)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));

  if (major != NULL)
    *major = gdk_gl_version_get_major (&priv->required);
  if (minor != NULL)
    *minor = gdk_gl_version_get_minor (&priv->required);
}

/**
 * gdk_gl_context_is_legacy:
 * @context: a `GdkGLContext`
 *
 * Whether the `GdkGLContext` is in legacy mode or not.
 *
 * The `GdkGLContext` must be realized before calling this function.
 *
 * When realizing a GL context, GDK will try to use the OpenGL 3.2 core
 * profile; this profile removes all the OpenGL API that was deprecated
 * prior to the 3.2 version of the specification. If the realization is
 * successful, this function will return %FALSE.
 *
 * If the underlying OpenGL implementation does not support core profiles,
 * GDK will fall back to a pre-3.2 compatibility profile, and this function
 * will return %TRUE.
 *
 * You can use the value returned by this function to decide which kind
 * of OpenGL API to use, or whether to do extension discovery, or what
 * kind of shader programs to load.
 *
 * Returns: %TRUE if the GL context is in legacy mode
 */
gboolean
gdk_gl_context_is_legacy (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), FALSE);
  g_return_val_if_fail (gdk_gl_context_is_realized (context), FALSE);

  return priv->is_legacy;
}

void
gdk_gl_context_set_version (GdkGLContext       *context,
                            const GdkGLVersion *version)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  priv->gl_version = *version;
}

void
gdk_gl_context_set_is_legacy (GdkGLContext *context,
                              gboolean      is_legacy)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  priv->is_legacy = !!is_legacy;
}

/**
 * gdk_gl_context_is_shared:
 * @self: a `GdkGLContext`
 * @other: the `GdkGLContext` that should be compatible with @self
 *
 * Checks if the two GL contexts can share resources.
 *
 * When they can, the texture IDs from @other can be used in @self. This
 * is particularly useful when passing `GdkGLTexture` objects between
 * different contexts.
 *
 * Contexts created for the same display with the same properties will
 * always be compatible, even if they are created for different surfaces.
 * For other contexts it depends on the GL backend.
 *
 * Both contexts must be realized for this check to succeed. If either one
 * is not, this function will return %FALSE.
 *
 * Returns: %TRUE if the two GL contexts are compatible.
 *
 * Since: 4.4
 */
gboolean
gdk_gl_context_is_shared (GdkGLContext *self,
                          GdkGLContext *other)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (self), FALSE);
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (other), FALSE);

  if (!gdk_gl_context_is_realized (self) ||
      !gdk_gl_context_is_realized (other))
    return FALSE;

  return GDK_GL_CONTEXT_GET_CLASS (self)->is_shared (self, other);
}

/**
 * gdk_gl_context_set_allowed_apis: (attributes org.gtk.Method.set_property=allowed-apis)
 * @self: a GL context
 * @apis: the allowed APIs
 *
 * Sets the allowed APIs. When gdk_gl_context_realize() is called, only the
 * allowed APIs will be tried. If you set this to 0, realizing will always fail.
 *
 * If you set it on a realized context, the property will not have any effect.
 * It is only relevant during gdk_gl_context_realize().
 *
 * By default, all APIs are allowed.
 *
 * Since: 4.6
 **/
void
gdk_gl_context_set_allowed_apis (GdkGLContext *self,
                                 GdkGLAPI      apis)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  g_return_if_fail (GDK_IS_GL_CONTEXT (self));

  if (priv->allowed_apis == apis)
    return;

  priv->allowed_apis = apis;

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ALLOWED_APIS]);
}

/**
 * gdk_gl_context_get_allowed_apis: (attributes org.gtk.Method.get_property=allowed-apis)
 * @self: a GL context
 *
 * Gets the allowed APIs set via gdk_gl_context_set_allowed_apis().
 *
 * Returns: the allowed APIs
 *
 * Since: 4.6
 **/
GdkGLAPI
gdk_gl_context_get_allowed_apis (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (self), 0);

  return priv->allowed_apis;
}

/**
 * gdk_gl_context_get_api: (attributes org.gtk.Method.get_property=api)
 * @self: a GL context
 *
 * Gets the API currently in use.
 *
 * If the renderer has not been realized yet, 0 is returned.
 *
 * Returns: the currently used API
 *
 * Since: 4.6
 **/
GdkGLAPI
gdk_gl_context_get_api (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (self), 0);

  return priv->api;
}

gboolean
gdk_gl_context_is_api_allowed (GdkGLContext  *self,
                               GdkGLAPI       api,
                               GError       **error)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  if (gdk_display_get_debug_flags (gdk_gl_context_get_display (self)) & GDK_DEBUG_GL_GLES)
    {
      if (!(api & GDK_GL_API_GLES))
        {
          g_set_error_literal (error, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
                               _("Anything but OpenGL ES disabled via GDK_DEBUG"));
          return FALSE;
        }
    }

  if (priv->allowed_apis & api)
    return TRUE;

  g_set_error (error, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
               _("Application does not support %s API"),
               api == GDK_GL_API_GL ? "OpenGL" : "OpenGL ES");

  return FALSE;
}

/**
 * gdk_gl_context_set_use_es:
 * @context: a `GdkGLContext`
 * @use_es: whether the context should use OpenGL ES instead of OpenGL,
 *   or -1 to allow auto-detection
 *
 * Requests that GDK create an OpenGL ES context instead of an OpenGL one.
 *
 * Not all platforms support OpenGL ES.
 *
 * The @context must not have been realized.
 *
 * By default, GDK will attempt to automatically detect whether the
 * underlying GL implementation is OpenGL or OpenGL ES once the @context
 * is realized.
 *
 * You should check the return value of [method@Gdk.GLContext.get_use_es]
 * after calling [method@Gdk.GLContext.realize] to decide whether to use
 * the OpenGL or OpenGL ES API, extensions, or shaders.
 */
void
gdk_gl_context_set_use_es (GdkGLContext *context,
                           int           use_es)
{
  g_return_if_fail (GDK_IS_GL_CONTEXT (context));
  g_return_if_fail (!gdk_gl_context_is_realized (context));

  switch (use_es)
  {
    case -1:
      gdk_gl_context_set_allowed_apis (context, DEFAULT_ALLOWED_APIS);
      break;
    case 0:
      gdk_gl_context_set_allowed_apis (context, GDK_GL_API_GL);
      break;
    case 1:
      gdk_gl_context_set_allowed_apis (context, GDK_GL_API_GLES);
      break;
    default:
      /* Just ignore the call */
      break;
  }
}

/**
 * gdk_gl_context_get_use_es:
 * @context: a `GdkGLContext`
 *
 * Checks whether the @context is using an OpenGL or OpenGL ES profile.
 *
 * Returns: %TRUE if the `GdkGLContext` is using an OpenGL ES profile;
 * %FALSE if other profile is in use of if the @context has not yet
 * been realized.
 */
gboolean
gdk_gl_context_get_use_es (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), FALSE);

  return priv->api == GDK_GL_API_GLES;
}

static void APIENTRY
gl_debug_message_callback (GLenum        source,
                           GLenum        type,
                           GLuint        id,
                           GLenum        severity,
                           GLsizei       length,
                           const GLchar *message,
                           const void   *user_data)
{
  const char *message_source;
  const char *message_type;
  const char *message_severity;
  GLogLevelFlags log_level;

  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
    return;

  switch (source)
    {
    case GL_DEBUG_SOURCE_API:
      message_source = "API";
      break;
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
      message_source = "Window System";
      break;
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
      message_source = "Shader Compiler";
      break;
    case GL_DEBUG_SOURCE_THIRD_PARTY:
      message_source = "Third Party";
      break;
    case GL_DEBUG_SOURCE_APPLICATION:
      message_source = "Application";
      break;
    case GL_DEBUG_SOURCE_OTHER:
    default:
      message_source = "Other";
    }

  switch (type)
    {
    case GL_DEBUG_TYPE_ERROR:
      message_type = "Error";
      break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
      message_type = "Deprecated Behavior";
      break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
      message_type = "Undefined Behavior";
      break;
    case GL_DEBUG_TYPE_PORTABILITY:
      message_type = "Portability";
      break;
    case GL_DEBUG_TYPE_PERFORMANCE:
      message_type = "Performance";
      break;
    case GL_DEBUG_TYPE_MARKER:
      message_type = "Marker";
      break;
    case GL_DEBUG_TYPE_PUSH_GROUP:
      message_type = "Push Group";
      break;
    case GL_DEBUG_TYPE_POP_GROUP:
      message_type = "Pop Group";
      break;
    case GL_DEBUG_TYPE_OTHER:
    default:
      message_type = "Other";
    }

  switch (severity)
    {
    case GL_DEBUG_SEVERITY_HIGH:
      message_severity = "High";
      log_level = G_LOG_LEVEL_CRITICAL;
      break;
    case GL_DEBUG_SEVERITY_MEDIUM:
      message_severity = "Medium";
      log_level = G_LOG_LEVEL_WARNING;
      break;
    case GL_DEBUG_SEVERITY_LOW:
      message_severity = "Low";
      log_level = G_LOG_LEVEL_MESSAGE;
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      message_severity = "Notification";
      log_level = G_LOG_LEVEL_INFO;
      break;
    default:
      message_severity = "Unknown";
      log_level = G_LOG_LEVEL_MESSAGE;
    }

  /* There's no higher level function taking a log level argument... */
  g_log_structured_standard (G_LOG_DOMAIN, log_level,
                             __FILE__, G_STRINGIFY (__LINE__),
                             G_STRFUNC,
                             "OPENGL:\n    Source: %s\n    Type: %s\n    Severity: %s\n    Message: %s",
                             message_source, message_type, message_severity, message);
}

/**
 * gdk_gl_context_realize:
 * @context: a `GdkGLContext`
 * @error: return location for a `GError`
 *
 * Realizes the given `GdkGLContext`.
 *
 * It is safe to call this function on a realized `GdkGLContext`.
 *
 * Returns: %TRUE if the context is realized
 */
gboolean
gdk_gl_context_realize (GdkGLContext  *context,
                        GError       **error)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), FALSE);

  if (priv->api)
    return TRUE;

  priv->api = GDK_GL_CONTEXT_GET_CLASS (context)->realize (context, error);

  if (priv->api)
    {
      g_assert (gdk_gl_version_greater_equal (&priv->gl_version, &GDK_GL_VERSION_INIT (0, 0)));

      g_object_notify_by_pspec (G_OBJECT (context), properties[PROP_API]);
    }

  return priv->api;
}

void
gdk_gl_version_init_epoxy (GdkGLVersion *version)
{
  int epoxy_version = epoxy_gl_version ();

  *version = GDK_GL_VERSION_INIT (epoxy_version / 10, epoxy_version % 10);
}

static void
gdk_gl_context_check_extensions (GdkGLContext *context)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);
  gboolean gl_debug = FALSE;
  GdkDisplay *display;

  if (!gdk_gl_context_is_realized (context))
    return;

  if (priv->extensions_checked)
    return;

  priv->has_debug_output = epoxy_has_gl_extension ("GL_ARB_debug_output") ||
                           epoxy_has_gl_extension ("GL_KHR_debug");

  display = gdk_draw_context_get_display (GDK_DRAW_CONTEXT (context));
  gl_debug = (gdk_display_get_debug_flags (display) & GDK_DEBUG_GL_DEBUG) != 0;

  if (priv->has_debug_output && gl_debug)
    {
      gdk_gl_context_make_current (context);
      glEnable (GL_DEBUG_OUTPUT);
      glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS);
      glDebugMessageCallback (gl_debug_message_callback, NULL);
    }

  if (gdk_gl_context_get_use_es (context))
    {
      priv->has_unpack_subimage = gdk_gl_version_greater_equal (&priv->gl_version, &GDK_GL_VERSION_INIT (3, 0)) ||
                                  epoxy_has_gl_extension ("GL_EXT_unpack_subimage");
      priv->has_khr_debug = epoxy_has_gl_extension ("GL_KHR_debug");
    }
  else
    {
      priv->has_unpack_subimage = TRUE;
      priv->has_khr_debug = epoxy_has_gl_extension ("GL_KHR_debug");

      /* We asked for a core profile, but we didn't get one, so we're in legacy mode */
      if (!gdk_gl_version_greater_equal (&priv->gl_version, &GDK_GL_VERSION_INIT (3, 2)))
        priv->is_legacy = TRUE;
    }

  if (priv->has_khr_debug && gl_debug)
    {
      priv->use_khr_debug = TRUE;
      glGetIntegerv (GL_MAX_LABEL_LENGTH, &priv->max_debug_label_length);
    }

  priv->has_half_float = gdk_gl_context_check_version (context, "3.0", "3.0") ||
                         epoxy_has_gl_extension ("OES_vertex_half_float");

  priv->has_sync = gdk_gl_context_check_version (context, "3.2", "3.0") ||
                   epoxy_has_gl_extension ("GL_ARB_sync") ||
                   epoxy_has_gl_extension ("GL_APPLE_sync");

#ifdef G_ENABLE_DEBUG
  {
    int max_texture_size;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &max_texture_size);
    GDK_DISPLAY_DEBUG (gdk_draw_context_get_display (GDK_DRAW_CONTEXT (context)), OPENGL,
                       "%s version: %d.%d (%s)\n"
                       "* GLSL version: %s\n"
                       "* Max texture size: %d\n"
                       "* Extensions checked:\n"
                       " - GL_KHR_debug: %s\n"
                       " - GL_EXT_unpack_subimage: %s\n"
                       " - half float: %s\n"
                       " - sync: %s",
                       gdk_gl_context_get_use_es (context) ? "OpenGL ES" : "OpenGL",
                       gdk_gl_version_get_major (&priv->gl_version), gdk_gl_version_get_minor (&priv->gl_version),
                       priv->is_legacy ? "legacy" : "core",
                       glGetString (GL_SHADING_LANGUAGE_VERSION),
                       max_texture_size,
                       priv->has_khr_debug ? "yes" : "no",
                       priv->has_unpack_subimage ? "yes" : "no",
                       priv->has_half_float ? "yes" : "no",
                       priv->has_sync ? "yes" : "no");
  }
#endif

  priv->extensions_checked = TRUE;
}

static gboolean
gdk_gl_context_check_is_current (GdkGLContext *context)
{
  return GDK_GL_CONTEXT_GET_CLASS (context)->is_current (context);
}

/**
 * gdk_gl_context_make_current:
 * @context: a `GdkGLContext`
 *
 * Makes the @context the current one.
 */
void
gdk_gl_context_make_current (GdkGLContext *context)
{
  MaskedContext *current, *masked_context;
  gboolean surfaceless;

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));

  surfaceless = !gdk_draw_context_is_in_frame (GDK_DRAW_CONTEXT (context));
  masked_context = mask_context (context, surfaceless);

  current = g_private_get (&thread_current_context);
  if (current == masked_context && gdk_gl_context_check_is_current (context))
    return;

  /* we need to realize the GdkGLContext if it wasn't explicitly realized */
  if (!gdk_gl_context_is_realized (context))
    {
      GError *error = NULL;

      gdk_gl_context_realize (context, &error);
      if (error != NULL)
        {
          g_critical ("Could not realize the GL context: %s", error->message);
          g_error_free (error);
          return;
        }
    }

  if (!GDK_GL_CONTEXT_GET_CLASS (context)->make_current (context, surfaceless))
    {
      g_warning ("gdk_gl_context_make_current() failed");
      return;
    }

  g_object_ref (context);
  g_private_replace (&thread_current_context, masked_context);
  gdk_gl_context_check_extensions (context);
}

/**
 * gdk_gl_context_get_display:
 * @context: a `GdkGLContext`
 *
 * Retrieves the display the @context is created for
 *
 * Returns: (nullable) (transfer none): a `GdkDisplay`
 */
GdkDisplay *
gdk_gl_context_get_display (GdkGLContext *context)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  return gdk_draw_context_get_display (GDK_DRAW_CONTEXT (context));
}

/**
 * gdk_gl_context_get_surface:
 * @context: a `GdkGLContext`
 *
 * Retrieves the surface used by the @context.
 *
 * Returns: (nullable) (transfer none): a `GdkSurface`
 */
GdkSurface *
gdk_gl_context_get_surface (GdkGLContext *context)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  return gdk_draw_context_get_surface (GDK_DRAW_CONTEXT (context));
}

/**
 * gdk_gl_context_get_shared_context: (attributes org.gtk.Method.get_property=shared-context)
 * @context: a `GdkGLContext`
 *
 * Used to retrieves the `GdkGLContext` that this @context share data with.
 *
 * As many contexts can share data now and no single shared context exists
 * anymore, this function has been deprecated and now always returns %NULL.
 *
 * Returns: (nullable) (transfer none): %NULL
 *
 * Deprecated: 4.4: Use [method@Gdk.GLContext.is_shared] to check if contexts
 *   can be shared.
 */
GdkGLContext *
gdk_gl_context_get_shared_context (GdkGLContext *context)
{
  g_return_val_if_fail (GDK_IS_GL_CONTEXT (context), NULL);

  return NULL;
}

/**
 * gdk_gl_context_get_version:
 * @context: a `GdkGLContext`
 * @major: (out): return location for the major version
 * @minor: (out): return location for the minor version
 *
 * Retrieves the OpenGL version of the @context.
 *
 * The @context must be realized prior to calling this function.
 */
void
gdk_gl_context_get_version (GdkGLContext *context,
                            int          *major,
                            int          *minor)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (context);

  g_return_if_fail (GDK_IS_GL_CONTEXT (context));
  g_return_if_fail (gdk_gl_context_is_realized (context));

  if (major != NULL)
    *major = gdk_gl_version_get_major (&priv->gl_version);
  if (minor != NULL)
    *minor = gdk_gl_version_get_minor (&priv->gl_version);
}

/**
 * gdk_gl_context_clear_current:
 *
 * Clears the current `GdkGLContext`.
 *
 * Any OpenGL call after this function returns will be ignored
 * until [method@Gdk.GLContext.make_current] is called.
 */
void
gdk_gl_context_clear_current (void)
{
  MaskedContext *current;

  current = g_private_get (&thread_current_context);
  if (current != NULL)
    {
      GdkGLContext *context = unmask_context (current);

      if (GDK_GL_CONTEXT_GET_CLASS (context)->clear_current (context))
        g_private_replace (&thread_current_context, NULL);
    }
}

/*<private>
 * gdk_gl_context_clear_current_if_surface:
 * @surface: surface to clear for
 *
 * Does a gdk_gl_context_clear_current() if the current context is attached
 * to @surface, leaves the current context alone otherwise.
 **/
void
gdk_gl_context_clear_current_if_surface (GdkSurface *surface)
{
  MaskedContext *current;

  current = g_private_get (&thread_current_context);
  if (current != NULL && !mask_is_surfaceless (current))
    {
      GdkGLContext *context = unmask_context (current);

      if (gdk_gl_context_get_surface (context) != surface)
        return;

      if (GDK_GL_CONTEXT_GET_CLASS (context)->clear_current (context))
        g_private_replace (&thread_current_context, NULL);
    }
}

/**
 * gdk_gl_context_get_current:
 *
 * Retrieves the current `GdkGLContext`.
 *
 * Returns: (nullable) (transfer none): the current `GdkGLContext`
 */
GdkGLContext *
gdk_gl_context_get_current (void)
{
  MaskedContext *current;
  GdkGLContext *context;

  current = g_private_get (&thread_current_context);
  context = unmask_context (current);

  if (context && !gdk_gl_context_check_is_current (context))
    {
      g_private_replace (&thread_current_context, NULL);
      context = NULL;
    }

  return context;
}

gboolean
gdk_gl_context_has_debug (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  return priv->debug_enabled || priv->use_khr_debug;
}

gboolean
gdk_gl_context_has_vertex_half_float (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  return priv->has_half_float;
}

gboolean
gdk_gl_context_has_sync (GdkGLContext *self)
{
  GdkGLContextPrivate *priv = gdk_gl_context_get_instance_private (self);

  return priv->has_sync;
}

/* This is currently private! */
/* When using GL/ES, don't flip the 'R' and 'B' bits on Windows/ANGLE for glReadPixels() */
gboolean
gdk_gl_context_use_es_bgra (GdkGLContext *context)
{
  if (!gdk_gl_context_get_use_es (context))
    return FALSE;

#ifdef GDK_WINDOWING_WIN32
  if (GDK_WIN32_IS_GL_CONTEXT (context))
    return TRUE;
#endif

  return FALSE;
}

static GdkGLBackend the_gl_backend_type = GDK_GL_NONE;

static const char *gl_backend_names[] = {
  [GDK_GL_NONE] = "No GL (You should never read this)",
  [GDK_GL_EGL] = "EGL",
  [GDK_GL_GLX] = "X11 GLX",
  [GDK_GL_WGL] = "Windows WGL",
  [GDK_GL_CGL] = "Apple CGL"
};

/*<private>
 * gdk_gl_backend_can_be_used:
 * @backend_type: Type of backend to check
 * @error: Return location for an error
 *
 * Checks if this backend type can be used. When multiple displays
 * are opened that use different GL backends, conflicts can arise,
 * so this function checks that all displays use compatible GL
 * backends.
 *
 * Returns: %TRUE if the backend can still be used
 */
gboolean
gdk_gl_backend_can_be_used (GdkGLBackend   backend_type,
                            GError       **error)
{
  if (the_gl_backend_type == GDK_GL_NONE ||
      the_gl_backend_type == backend_type)
    return TRUE;

  g_set_error (error, GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE,
               /* translators: This is about OpenGL backend names, like
                * "Trying to use X11 GLX, but EGL is already in use" */
               _("Trying to use %s, but %s is already in use"),
               gl_backend_names[backend_type],
               gl_backend_names[the_gl_backend_type]);
  return FALSE;
}

/*<private>
 * gdk_gl_backend_use:
 * @backend_type: Type of backend
 *
 * Ensures that the backend in use is the given one. If another backend
 * is already in use, this function will abort the program. It should
 * have previously checked via gdk_gl_backend_can_be_used().
 **/
void
gdk_gl_backend_use (GdkGLBackend backend_type)
{
  /* Check that the context class is properly initializing its backend type */
  g_assert (backend_type != GDK_GL_NONE);

  if (the_gl_backend_type == GDK_GL_NONE)
    {
      the_gl_backend_type = backend_type;
      /* This is important!!!11eleven
       * (But really: How do I print a message in 2 categories?) */
      GDK_DEBUG (OPENGL, "Using OpenGL backend %s", gl_backend_names[the_gl_backend_type]);
      GDK_DEBUG (MISC, "Using OpenGL backend %s", gl_backend_names[the_gl_backend_type]);
    }

  g_assert (the_gl_backend_type == backend_type);
}
