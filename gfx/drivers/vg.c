/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2012-2015 - Michael Lelli
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <VG/openvg.h>
#include <VG/vgext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "../video_context_driver.h"
#include <gfx/math/matrix_3x3.h>
#include <retro_inline.h>
#include "../../libretro.h"
#include "../../general.h"
#include "../../retroarch.h"
#include "../../driver.h"
#include "../../performance.h"
#include "../font_renderer_driver.h"
#include "../../content.h"
#include "../../runloop.h"
#include "../video_viewport.h"

typedef struct
{
   bool should_resize;
   float mScreenAspect;
   bool mKeepAspect;
   bool mEglImageBuf;
   unsigned mTextureWidth;
   unsigned mTextureHeight;
   unsigned mRenderWidth;
   unsigned mRenderHeight;
   unsigned x1, y1, x2, y2;
   VGImageFormat mTexType;
   VGImage mImage;
   math_matrix_3x3 mTransformMatrix;
   VGint scissor[4];
   EGLImageKHR last_egl_image;

   char *mLastMsg;
   uint32_t mFontHeight;
   VGFont mFont;
   void *mFontRenderer;
   const font_renderer_driver_t *font_driver;
   bool mFontsOn;
   VGuint mMsgLength;
   VGuint mGlyphIndices[1024];
   VGPaint mPaintFg;
   VGPaint mPaintBg;
} vg_t;

static PFNVGCREATEEGLIMAGETARGETKHRPROC pvgCreateEGLImageTargetKHR;

static void vg_set_nonblock_state(void *data, bool state)
{
   gfx_ctx_swap_interval(data, state ? 0 : 1);
}

static INLINE bool vg_query_extension(const char *ext)
{
   const char *str = (const char*)vgGetString(VG_EXTENSIONS);
   bool ret = str && strstr(str, ext);
   RARCH_LOG("Querying VG extension: %s => %s\n",
         ext, ret ? "exists" : "doesn't exist");

   return ret;
}

static void *vg_init(const video_info_t *video, const input_driver_t **input, void **input_data)
{
   VGfloat clearColor[4] = {0, 0, 0, 1};
   settings_t        *settings = config_get_ptr();
   driver_t            *driver = driver_get_ptr();
   global_t            *global = global_get_ptr();
   const gfx_ctx_driver_t *ctx = NULL;
   vg_t                    *vg = (vg_t*)calloc(1, sizeof(vg_t));

   if (!vg)
      goto error;

   ctx = gfx_ctx_init_first(vg, settings->video.context_driver,
         GFX_CTX_OPENVG_API, 0, 0, false);

   if (!ctx)
      goto error;

   driver->video_context = ctx;

   gfx_ctx_get_video_size(vg, &global->video_data.width, &global->video_data.height);
   RARCH_LOG("Detecting screen resolution %ux%u.\n", global->video_data.width, global->video_data.height);

   gfx_ctx_swap_interval(vg, video->vsync ? 1 : 0);

   gfx_ctx_update_window_title(vg);

   vg->mTexType = video->rgb32 ? VG_sXRGB_8888 : VG_sRGB_565;
   vg->mKeepAspect = video->force_aspect;

   unsigned win_width  = video->width;
   unsigned win_height = video->height;
   if (video->fullscreen && (win_width == 0) && (win_height == 0))
   {
      win_width  = global->video_data.width;
      win_height = global->video_data.height;
   }

   if (!gfx_ctx_set_video_mode(vg, win_width, win_height, video->fullscreen))
      goto error;

   gfx_ctx_get_video_size(vg, &global->video_data.width, &global->video_data.height);
   RARCH_LOG("Verified window resolution %ux%u.\n", global->video_data.width, global->video_data.height);
   vg->should_resize = true;

   vg->mScreenAspect = (float)global->video_data.width / global->video_data.height;

   gfx_ctx_translate_aspect(vg, &vg->mScreenAspect, global->video_data.width, global->video_data.height);

   vgSetfv(VG_CLEAR_COLOR, 4, clearColor);

   vg->mTextureWidth = vg->mTextureHeight = video->input_scale * RARCH_SCALE_BASE;
   vg->mImage = vgCreateImage(vg->mTexType, vg->mTextureWidth, vg->mTextureHeight,
         video->smooth ? VG_IMAGE_QUALITY_BETTER : VG_IMAGE_QUALITY_NONANTIALIASED);
   vg_set_nonblock_state(vg, !video->vsync);

   gfx_ctx_input_driver(vg, input, input_data);

   if (settings->video.font_enable && font_renderer_create_default(&vg->font_driver, &vg->mFontRenderer,
            *settings->video.font_path ? settings->video.font_path : NULL, settings->video.font_size))
   {
      vg->mFont            = vgCreateFont(0);

      if (vg->mFont != VG_INVALID_HANDLE)
      {
         vg->mFontsOn      = true;
         vg->mFontHeight   = settings->video.font_size;
         vg->mPaintFg      = vgCreatePaint();
         vg->mPaintBg      = vgCreatePaint();
         VGfloat paintFg[] = { settings->video.msg_color_r, settings->video.msg_color_g, settings->video.msg_color_b, 1.0f };
         VGfloat paintBg[] = { settings->video.msg_color_r / 2.0f, settings->video.msg_color_g / 2.0f, settings->video.msg_color_b / 2.0f, 0.5f };

         vgSetParameteri(vg->mPaintFg, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
         vgSetParameterfv(vg->mPaintFg, VG_PAINT_COLOR, 4, paintFg);

         vgSetParameteri(vg->mPaintBg, VG_PAINT_TYPE, VG_PAINT_TYPE_COLOR);
         vgSetParameterfv(vg->mPaintBg, VG_PAINT_COLOR, 4, paintBg);
      }
   }

   if (vg_query_extension("KHR_EGL_image") && gfx_ctx_image_buffer_init(vg, video))
   {
      pvgCreateEGLImageTargetKHR = (PFNVGCREATEEGLIMAGETARGETKHRPROC)gfx_ctx_get_proc_address("vgCreateEGLImageTargetKHR");

      if (pvgCreateEGLImageTargetKHR)
      {
         RARCH_LOG("[VG] Using EGLImage buffer\n");
         vg->mEglImageBuf = true;
      }
   }

#if 0
   const char *ext = (const char*)vgGetString(VG_EXTENSIONS);
   if (ext)
      RARCH_LOG("[VG] Supported extensions: %s\n", ext);
#endif

   return vg;

error:
   if (vg)
      free(vg);
   if (driver)
      driver->video_context = NULL;
   return NULL;
}

static void vg_free(void *data)
{
   vg_t                    *vg = (vg_t*)data;

   if (!vg)
      return;

   vgDestroyImage(vg->mImage);

   if (vg->mFontsOn)
   {
      vgDestroyFont(vg->mFont);
      vg->font_driver->free(vg->mFontRenderer);
      vgDestroyPaint(vg->mPaintFg);
      vgDestroyPaint(vg->mPaintBg);
   }

   free(vg);
}

#if 0
static void vg_render_message(vg_t *vg, const char *msg)
{
   free(vg->mLastMsg);
   vg->mLastMsg = strdup(msg);

   if (vg->mMsgLength)
   {
      while (--vg->mMsgLength)
         vgClearGlyph(vg->mFont, vg->mMsgLength);

      vgClearGlyph(vg->mFont, 0);
   }

   struct font_output_list out;
   vg->font_driver->render_msg(vg->mFontRenderer, msg, &out);
   struct font_output *head = out.head;

   while (head)
   {
      if (vg->mMsgLength >= 1024)
         break;

      VGfloat origin[2], escapement[2];
      VGImage img;

      escapement[0] = head->advance_x;
      escapement[1] = head->advance_y;
      origin[0] = -head->char_off_x;
      origin[1] = -head->char_off_y;

      img = vgCreateImage(VG_A_8, head->width, head->height, VG_IMAGE_QUALITY_NONANTIALIASED);

      // flip it
      for (unsigned i = 0; i < head->height; i++)
         vgImageSubData(img, head->output + head->pitch * i, head->pitch, VG_A_8, 0, head->height - i - 1, head->width, 1);

      vgSetGlyphToImage(vg->mFont, vg->mMsgLength, img, origin, escapement);
      vgDestroyImage(img);

      vg->mMsgLength++;
      head = head->next;
   }

   vg->font_driver->free_output(vg->mFontRenderer, &out);

   for (unsigned i = 0; i < vg->mMsgLength; i++)
      vg->mGlyphIndices[i] = i;
}

static void vg_draw_message(vg_t *vg, const char *msg)
{
   settings_t *settings = config_get_ptr();

   if (!vg->mLastMsg || strcmp(vg->mLastMsg, msg))
      vg_render_message(vg, msg);

   vgSeti(VG_SCISSORING, VG_FALSE);
   vgSeti(VG_IMAGE_MODE, VG_DRAW_IMAGE_STENCIL);

   VGfloat origins[] = {
      global->video_data.width  * settings->video.msg_pos_x - 2.0f,
      global->video_data.height * settings->video.msg_pos_y - 2.0f,
   };

   vgSetfv(VG_GLYPH_ORIGIN, 2, origins);
   vgSetPaint(vg->mPaintBg, VG_FILL_PATH);
   vgDrawGlyphs(vg->mFont, vg->mMsgLength, vg->mGlyphIndices, NULL, NULL, VG_FILL_PATH, VG_TRUE);
   origins[0] += 2.0f;
   origins[1] += 2.0f;
   vgSetfv(VG_GLYPH_ORIGIN, 2, origins);
   vgSetPaint(vg->mPaintFg, VG_FILL_PATH);
   vgDrawGlyphs(vg->mFont, vg->mMsgLength, vg->mGlyphIndices, NULL, NULL, VG_FILL_PATH, VG_TRUE);

   vgSeti(VG_SCISSORING, VG_TRUE);
   vgSeti(VG_IMAGE_MODE, VG_DRAW_IMAGE_NORMAL);
}
#endif

static void vg_calculate_quad(vg_t *vg)
{
   global_t *global     = global_get_ptr();

   /* set viewport for aspect ratio, taken from the OpenGL driver. */
   if (vg->mKeepAspect)
   {
      float desired_aspect = global->system.aspect_ratio;

      /* If the aspect ratios of screen and desired aspect ratio 
       * are sufficiently equal (floating point stuff),
       * assume they are actually equal. */
      if (fabs(vg->mScreenAspect - desired_aspect) < 0.0001)
      {
         vg->x1 = 0;
         vg->y1 = 0;
         vg->x2 = global->video_data.width;
         vg->y2 = global->video_data.height;
      }
      else if (vg->mScreenAspect > desired_aspect)
      {
         float delta = (desired_aspect / vg->mScreenAspect - 1.0) / 2.0 + 0.5;
         vg->x1 = global->video_data.width * (0.5 - delta);
         vg->y1 = 0;
         vg->x2 = 2.0 * global->video_data.width * delta + vg->x1;
         vg->y2 = global->video_data.height + vg->y1;
      }
      else
      {
         float delta = (vg->mScreenAspect / desired_aspect - 1.0) / 2.0 + 0.5;
         vg->x1 = 0;
         vg->y1 = global->video_data.height * (0.5 - delta);
         vg->x2 = global->video_data.width + vg->x1;
         vg->y2 = 2.0 * global->video_data.height * delta + vg->y1;
      }
   }
   else
   {
      vg->x1 = 0;
      vg->y1 = 0;
      vg->x2 = global->video_data.width;
      vg->y2 = global->video_data.height;
   }

   vg->scissor[0] = vg->x1;
   vg->scissor[1] = vg->y1;
   vg->scissor[2] = vg->x2 - vg->x1;
   vg->scissor[3] = vg->y2 - vg->y1;

   vgSetiv(VG_SCISSOR_RECTS, 4, vg->scissor);
}

static void vg_copy_frame(void *data, const void *frame,
      unsigned width, unsigned height, unsigned pitch)
{
   vg_t *vg = (vg_t*)data;

   if (vg->mEglImageBuf)
   {
      EGLImageKHR img = 0;
      bool                new_egl = gfx_ctx_image_buffer_write(vg,
            frame, width, height, pitch, (vg->mTexType == VG_sXRGB_8888), 0, &img);

      rarch_assert(img != EGL_NO_IMAGE_KHR);

      if (new_egl)
      {
         vgDestroyImage(vg->mImage);
         vg->mImage = pvgCreateEGLImageTargetKHR((VGeglImageKHR) img);
         if (!vg->mImage)
         {
            RARCH_ERR("[VG:EGLImage] Error creating image: %08x\n", vgGetError());
            exit(2);
         }
         vg->last_egl_image = img;
      }
   }
   else
   {
      vgImageSubData(vg->mImage, frame, pitch, vg->mTexType, 0, 0, width, height);
   }
}

static bool vg_frame(void *data, const void *frame,
      unsigned width, unsigned height, unsigned pitch, const char *msg)
{
   vg_t                    *vg = (vg_t*)data;
   global_t            *global = global_get_ptr();

   RARCH_PERFORMANCE_INIT(vg_fr);
   RARCH_PERFORMANCE_START(vg_fr);

   if (width != vg->mRenderWidth || height != vg->mRenderHeight || vg->should_resize)
   {
      vg->mRenderWidth = width;
      vg->mRenderHeight = height;
      vg_calculate_quad(vg);
      matrix_3x3_quad_to_quad(
         vg->x1, vg->y1, vg->x2, vg->y1, vg->x2, vg->y2, vg->x1, vg->y2,
         /* needs to be flipped, Khronos loves their bottom-left origin */
         0, height, width, height, width, 0, 0, 0,
         &vg->mTransformMatrix);
      vgSeti(VG_MATRIX_MODE, VG_MATRIX_IMAGE_USER_TO_SURFACE);
      vgLoadMatrix(vg->mTransformMatrix.data);

      vg->should_resize = false;
   }
   vgSeti(VG_SCISSORING, VG_FALSE);
   vgClear(0, 0, global->video_data.width, global->video_data.height);
   vgSeti(VG_SCISSORING, VG_TRUE);

   RARCH_PERFORMANCE_INIT(vg_image);
   RARCH_PERFORMANCE_START(vg_image);
   vg_copy_frame(vg, frame, width, height, pitch);
   RARCH_PERFORMANCE_STOP(vg_image);

   vgDrawImage(vg->mImage);

#if 0
   if (msg && vg->mFontsOn)
      vg_draw_message(vg, msg);
#endif

   gfx_ctx_update_window_title(vg);

   RARCH_PERFORMANCE_STOP(vg_fr);

   gfx_ctx_swap_buffers(vg);

   return true;
}

static bool vg_alive(void *data)
{
   bool quit;
   vg_t         *vg = (vg_t*)data;
   global_t *global = global_get_ptr();

   gfx_ctx_check_window(data, &quit,
         &vg->should_resize, &global->video_data.width, &global->video_data.height);
   return !quit;
}

static bool vg_focus(void *data)
{
   return gfx_ctx_focus(data);
}

static bool vg_suppress_screensaver(void *data, bool enable)
{
   return gfx_ctx_suppress_screensaver(data, enable);
}

static bool vg_has_windowed(void *data)
{
   return gfx_ctx_has_windowed(data);
}

static bool vg_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   (void)data;
   (void)type;
   (void)path;

   return false; 
}

static void vg_set_rotation(void *data, unsigned rotation)
{
   (void)data;
   (void)rotation;
}

static void vg_viewport_info(void *data,
      struct video_viewport *vp)
{
   (void)data;
   (void)vp;
}

static bool vg_read_viewport(void *data, uint8_t *buffer)
{
   (void)data;
   (void)buffer;

   return true;
}

static void vg_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   (void)data;
   (void)iface;
}

video_driver_t video_vg = {
   vg_init,
   vg_frame,
   vg_set_nonblock_state,
   vg_alive,
   vg_focus,
   vg_suppress_screensaver,
   vg_has_windowed,
   vg_set_shader,
   vg_free,
   "vg",
   NULL, /* set_viewport */
   vg_set_rotation,
   vg_viewport_info,
   vg_read_viewport,
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
  NULL, /* overlay_interface */
#endif
  vg_get_poke_interface
};
