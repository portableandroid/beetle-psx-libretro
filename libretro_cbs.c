#include <boolean.h>
#include "libretro.h"

bool content_is_pal = false;
retro_video_refresh_t video_cb;
retro_environment_t environ_cb;
uint8_t widescreen_hack = 0;
uint8_t psx_gpu_upscale_shift;
int lineRenderMode;
int filter_mode = 0;
bool opaque_check;
bool semitrans_check;
