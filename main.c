#include <libretro.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "libh8300h/devices/buttons.h"
#include "libh8300h/devices/lcd.h"
#include "libh8300h/system.h"

static h8_system_t lr_system;

static h8_u16 screen_buffer[96 * 64];
static char system_dir[1024];

/* libretro video options */
static h8_u8 (*lr_video_draw)(h8_u8 *vram, h8_u16 *buffer);
static h8_u16 lr_video_width = 96;
static h8_u16 lr_video_height = 64;
static float lr_video_aspect = 96.0 / 64.0;

/* libretro callbacks */
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_led_interface led_cb = { NULL };
static retro_log_printf_t log_cb;

static struct retro_rumble_interface rumble;
static retro_video_refresh_t video_cb;

static h8_device_t *h8lr_find_device(unsigned type)
{
  unsigned i;

  for (i = 0; i < lr_system.device_count; i++)
    if (lr_system.devices[i].type == type)
      return &lr_system.devices[i];

  return NULL;
}

static void display_message(const char *msg)
{
  char *str = (char*)calloc(4096, sizeof(char));
  struct retro_message rmsg;

  snprintf(str, 4096, "%s", msg);
  rmsg.frames = 300;
  rmsg.msg = str;
  environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rmsg);
}

void handle_input(void)
{
  h8_device_t *device = h8lr_find_device(H8_DEVICE_3BUTTON);

  if (!device)
    device = h8lr_find_device(H8_DEVICE_1BUTTON);
  if (!device)
    return;
  else
  {
    h8_buttons_t *buttons = (h8_buttons_t*)device->device;

    input_poll_cb();
    buttons->buttons[0] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
                                         RETRO_DEVICE_ID_JOYPAD_A) ? 1 : 0;
    if (buttons->button_count > 1)
    {
      buttons->buttons[1] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
                                           RETRO_DEVICE_ID_JOYPAD_LEFT) ? 1 : 0;
      buttons->buttons[2] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
                                           RETRO_DEVICE_ID_JOYPAD_LEFT) ? 1 : 0;
    }
  }
}

/* libretro API */

void retro_init(void)
{
  char *dir = NULL;

  if (!environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
    log_cb = NULL;
}

void retro_reset(void)
{
}

bool retro_load_game(const struct retro_game_info *info)
{
  if (info && info->data && info->size)
  {
    memcpy(lr_system.vmem.raw, info->data, info->size);
    h8_init(&lr_system);
    h8_system_init(&lr_system, H8_SYSTEM_NTR_032);
    return true;
  }

  return false;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info)
{
  return false;
}

void retro_unload_game(void)
{
}

static h8_u16 h8lr_colors[4] =
{
  0xB5B6,
  0x8410,
  0x632C,
  0x18E3
};

void retro_run(void)
{
  h8_device_t *screen = h8lr_find_device(H8_DEVICE_LCD);
  unsigned i;

  handle_input();

  for (i = 0; i < 2000; i++)
    h8_step(&lr_system);

  if (screen)
  {
    h8_lcd_t *lcd = (h8_lcd_t*)screen->device;

    for (int y = 0; y < 64; y++)
    {
      for (int x = 0; x < 96; x++)
      {
        int yo  = y + lcd->start_line;
        int pg  = (yo / 8) % 21;
        int b   = yo % 8;
        int col = x;
        h8_u16 byte = (lcd->vram[pg*0x100 + col*2] << 8) | lcd->vram[pg*0x100 + col*2+1];
        byte >>= b;
        uint8_t color = ((byte & 0x100) >> 7) | (byte & 1);

        screen_buffer[y * 96 + x] = h8lr_colors[color];
      }
    }
    video_cb(screen_buffer, lr_video_width, lr_video_height, lr_video_width * 2);
  }
}

void retro_get_system_info(struct retro_system_info *info)
{
  memset(info, 0, sizeof(*info));
  info->library_name = "Stepdad";
  info->library_version = "GIT_VERSION";
  info->need_fullpath = false;
  info->valid_extensions = "rom|bin";
  info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
  memset(info, 0, sizeof(*info));
  info->geometry.base_width = lr_video_width;
  info->geometry.base_height = lr_video_height;
  info->geometry.max_width = lr_video_width;
  info->geometry.max_height = lr_video_height;
  info->geometry.aspect_ratio = lr_video_aspect;
  info->timing.fps = 60;
  info->timing.sample_rate = 0;
}

void retro_deinit(void)
{
}

unsigned retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
  return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
}

void retro_set_environment(retro_environment_t cb)
{
  static const struct retro_variable vars[] = {
    { NULL, NULL },
  };
  static const struct retro_controller_description port[] = {
    { "Buttons", RETRO_DEVICE_JOYPAD },
    { NULL, 0 },
  };
  static const struct retro_controller_info ports[] = {
    { port, 2 },
    { NULL, 0 },
  };
  struct retro_input_descriptor desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },

    { 0 },
  };
  enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;

  environ_cb = cb;
  cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
  cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
  cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565);
  cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
  audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
  input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
  input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
  video_cb = cb;
}

size_t retro_serialize_size(void)
{
  return 0;
}

bool retro_serialize(void *data, size_t size)
{
  return false;
}

bool retro_unserialize(const void *data, size_t size)
{
  return false;
}

void *retro_get_memory_data(unsigned type)
{
  switch (type)
  {
  case RETRO_MEMORY_SYSTEM_RAM:
    return lr_system.vmem.raw;
  case RETRO_MEMORY_SAVE_RAM:
    return lr_system.devices[2].data;
  default:
    return NULL;
  }
}

size_t retro_get_memory_size(unsigned type)
{
  switch (type)
  {
  case RETRO_MEMORY_SYSTEM_RAM:
    return 0x10000;
  case RETRO_MEMORY_SAVE_RAM:
    return lr_system.devices[2].size;
  default:
    return 0;
  }
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned a, bool b, const char *c)
{
}
