/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2019 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM && !SDL_VIDEO_OPENGL_EGL

#include <sys/mman.h>
#include <errno.h>
#include <string.h>

#include "SDL_log.h"
#include "SDL_kmsdrmdyn.h"
#include "SDL_kmsdrmvideo.h"

#include "SDL_kmsdrmframebuffer.h"


static SDL_bool KMSDRM_CreateDrmFB(_THIS, SDL_Window *window, int fb_num) {
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   SDL_VideoData *vdata = (SDL_VideoData *)_this->driverdata;
   KMSDRM_DrmFB *drm_fb = &wdata->drm_fbs[fb_num];
   struct drm_mode_create_dumb creq;
   struct drm_mode_map_dumb mreq;
   uint32_t fb_id = 0;
   int err;

   memset(&creq, 0, sizeof(creq));
   creq.width = (uint32_t)window->w;
   creq.height = (uint32_t)window->h;
   creq.bpp = 32;
   err = KMSDRM_drmIoctl(vdata->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
   if (err) {
      SDL_SetError("Could not create dumb buffer: %d", errno);
      return SDL_FALSE;
   }
   drm_fb->stride = creq.pitch;
   drm_fb->size = creq.size;
   drm_fb->handle = creq.handle;
   err = KMSDRM_drmModeAddFB(vdata->drm_fd, creq.width, creq.height, 24, 32, drm_fb->stride,
                             drm_fb->handle, &fb_id);
   if (err) {
      SDL_SetError("Could not create framebuffer: %d", errno);
      return SDL_FALSE;
   }
   drm_fb->id = fb_id;

   memset(&mreq, 0, sizeof(mreq));
   mreq.handle = drm_fb->handle;
   err = KMSDRM_drmIoctl(vdata->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
   if (err) {
      SDL_SetError("Could not map dumb buffer: %d", errno);
      return SDL_FALSE;
   }

   drm_fb->map = mmap(NULL, (size_t)drm_fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      vdata->drm_fd, mreq.offset);
   if (drm_fb->map == MAP_FAILED) {
      drm_fb->map = NULL;
      SDL_SetError("Could not mmap dumb buffer: %d", errno);
      return SDL_FALSE;
   }

   memset(drm_fb->map, 0, (size_t)drm_fb->size);
   return SDL_TRUE;
}

static void KMSDRM_DestroyDrmFB(_THIS, SDL_Window *window, int fb_num) {
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   SDL_VideoData *vdata = (SDL_VideoData *)_this->driverdata;
   KMSDRM_DrmFB *drm_fb = &wdata->drm_fbs[fb_num];

   if (drm_fb->id != 0) {
      KMSDRM_drmModeRmFB(vdata->drm_fd, drm_fb->id);
   }
   
   if (drm_fb->handle != 0) {
      struct drm_mode_destroy_dumb dreq;
      memset(&dreq, 0, sizeof(dreq));
      KMSDRM_drmIoctl(vdata->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
   }

   memset(drm_fb, 0, sizeof(*drm_fb));
}

static SDL_bool KMSDRM_CreateDrmFBs(_THIS, SDL_Window *window) {
   int fb_num;
   for (fb_num = 0; fb_num < KMSDRM_DRMFB_COUNT; ++fb_num) {
      if (!KMSDRM_CreateDrmFB(_this, window, fb_num))
         return SDL_FALSE;
   }
   return SDL_TRUE;
}

static void KMSDRM_DestroyDrmFBs(_THIS, SDL_Window *window) {
   int fb_num;
   for (fb_num = 0; fb_num < KMSDRM_DRMFB_COUNT; ++fb_num) {
      KMSDRM_DestroyDrmFB(_this, window, fb_num);
   }
}

static SDL_bool KMSDRM_InitCrtc(_THIS, SDL_Window *window) {
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   SDL_VideoData *vdata = (SDL_VideoData *)_this->driverdata;
   SDL_DisplayData *displaydata = (SDL_DisplayData *) SDL_GetDisplayForWindow(window)->driverdata;
   KMSDRM_DrmFB *drm_fb = NULL;
   int err = -1;
   SDL_bool ret = SDL_FALSE;

   if (wdata->crtc_ready) {
      ret = SDL_TRUE;
      goto exit;
   }

   if (!KMSDRM_CreateDrmFBs(_this, window))
      goto exit;

   wdata->front_drm_fb = 0;
   drm_fb = &wdata->drm_fbs[wdata->front_drm_fb];
   err = KMSDRM_drmModeSetCrtc(vdata->drm_fd, vdata->crtc_id, drm_fb->id,
                               0, 0, &vdata->saved_conn_id, 1, &displaydata->cur_mode);
   if (err) {
      SDL_SetError("Could not set up CRTC: %d", errno);
      goto exit;
   }

   wdata->front_drm_fb = (wdata->front_drm_fb + 1) % KMSDRM_DRMFB_COUNT;
   wdata->crtc_ready = SDL_TRUE;
   ret = SDL_TRUE;

exit:
   if (!ret)
      KMSDRM_DestroyWindowFramebuffer(_this, window);
   return ret;
}


int KMSDRM_CreateWindowFramebuffer(_THIS, SDL_Window * window,
                                   Uint32 * format,
                                   void ** pixels, int *pitch)
{
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   KMSDRM_DrmFB *drm_fb = NULL;

   if (!wdata->crtc_ready) {
      if (!KMSDRM_InitCrtc(_this, window))
         return -1;
   }

   drm_fb = &wdata->drm_fbs[wdata->front_drm_fb];
   *format = SDL_PIXELFORMAT_RGB888;
   *pixels = drm_fb->map;
   *pitch = (int)drm_fb->stride;
   return 0;
}

int KMSDRM_UpdateWindowFramebuffer(_THIS, SDL_Window * window,
                                   const SDL_Rect * rects, int numrects)
{
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   SDL_VideoData *vdata = (SDL_VideoData *)_this->driverdata;
   KMSDRM_DrmFB *drm_fb = &wdata->drm_fbs[wdata->front_drm_fb];
   int err;

   if (!KMSDRM_WaitPageFlip(_this, wdata, -1)) {
        return 0;
   }

   wdata->waiting_for_flip = SDL_TRUE;
   err = KMSDRM_drmModePageFlip(vdata->drm_fd, vdata->crtc_id, drm_fb->id,
                                DRM_MODE_PAGE_FLIP_EVENT, &wdata->waiting_for_flip);
   if (err) {
      wdata->waiting_for_flip = SDL_FALSE;
      SDL_SetError("Could not queue pageflip: %d", errno);
      return -1;
   }

   wdata->front_drm_fb = (wdata->front_drm_fb + 1) % KMSDRM_DRMFB_COUNT;
   window->surface_valid = SDL_FALSE;
   return 0;
}

void KMSDRM_DestroyWindowFramebuffer(_THIS, SDL_Window * window) {
   SDL_WindowData *wdata = (SDL_WindowData *) window->driverdata;
   KMSDRM_DestroyDrmFBs(_this, window);
   wdata->crtc_ready = SDL_TRUE;
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM && !SDL_VIDEO_OPENGL_EGL */

/* vi: set ts=4 sw=4 expandtab: */
