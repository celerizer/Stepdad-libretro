#include <libretro.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "libh8300h/devices/bma150.h"
#include "libh8300h/devices/buttons.h"
#include "libh8300h/devices/buzzer.h"
#include "libh8300h/devices/eeprom.h"
#include "libh8300h/devices/generic_adc.h"
#include "libh8300h/devices/lcd.h"
#include "libh8300h/devices/led.h"
#include "libh8300h/frontend.h"
#include "libh8300h/rtc.h"
#include "libh8300h/system.h"

/* A step is a push of this many frames followed by at least as many at rest */
#define H8LR_STEP_FRAMES 15

/* Accelerometer counts per g: the BMA150's 10-bit, +/-2 g range */
#define H8LR_COUNTS_PER_G 256

#define H8LR_GRAVITY 9.80665f

typedef struct
{
  h8_system_t system;
  h8_lcd_t *lcd;
  h8_led_t *led;
  h8_buttons_t *buttons;
  h8_device_t *eeprom;
  h8_device_t *buzzer;

  h8_u16 screen_buffer[96 * 64];
  h8_s16 audio_buffer[H8_BUZZER_BUFFER_SIZE * 2];

  /* Step button presses not yet turned into accelerometer pulses */
  unsigned steps_queued;

  /* Frames left in the current step pulse and its following rest */
  unsigned step_frames;

  h8_bool step_held;
  h8_bool network_started;

  /* Running average of the squared sensor magnitude, in g */
  float sensor_mag2;
} lr_stepdad_ctx_t;

static lr_stepdad_ctx_t ctx;

/* libretro video options */
static const h8_u16 lr_video_width = 96;
static const h8_u16 lr_video_height = 64;
static const float lr_video_aspect = 96.0 / 64.0;

/* libretro callbacks */
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;

static struct retro_sensor_interface sensor_cb;
static bool sensor_enabled;

static h8_device_t *h8lr_find_device(unsigned type)
{
  unsigned i;

  for (i = 0; i < ctx.system.device_count; i++)
    if (ctx.system.devices[i].type == type)
      return &ctx.system.devices[i];

  return NULL;
}

static void display_message(const char *msg)
{
  struct retro_message rmsg;

  rmsg.frames = 300;
  rmsg.msg = msg;
  environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rmsg);
}

static h8_bool h8lr_input(unsigned id)
{
  return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) ? TRUE : FALSE;
}

/* Returns whether an accelerometer step pulse is being applied this frame */
static h8_bool h8lr_step_pulse(void)
{
  if (ctx.step_frames)
    ctx.step_frames--;
  else if (ctx.steps_queued)
  {
    ctx.steps_queued--;
    ctx.step_frames = H8LR_STEP_FRAMES * 2 - 1;
  }

  return ctx.step_frames >= H8LR_STEP_FRAMES;
}

static int h8lr_clamp(int value, int min, int max)
{
  return value < min ? min : value > max ? max : value;
}

/* Reads the frontend's accelerometer in g, or the device lying at rest */
static void h8lr_read_accel(float accel[3])
{
  unsigned i;

  accel[0] = accel[1] = 0.0f;
  accel[2] = 1.0f;
  if (!sensor_enabled)
    return;

  for (i = 0; i < 3; i++)
    accel[i] = sensor_cb.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_X + i) /
               H8LR_GRAVITY;

  /* Frontends are meant to remove gravity, but some report it. Put it back
   * on Z when it is missing, since the step detectors expect to feel it. */
  ctx.sensor_mag2 = ctx.sensor_mag2 * 0.98f + 0.02f *
    (accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
  if (ctx.sensor_mag2 < 0.25f)
    accel[2] += 1.0f;
}

static void h8lr_handle_input(void)
{
  h8_bool step_held;
  h8_bool step;
  float accel[3];
  unsigned i;

  input_poll_cb();

  if (ctx.buttons)
  {
    ctx.buttons->buttons[H8_BUTTON_MAIN] = h8lr_input(RETRO_DEVICE_ID_JOYPAD_A);
    if (ctx.buttons->button_count > 1)
    {
      ctx.buttons->buttons[H8_BUTTON_LEFT] = h8lr_input(RETRO_DEVICE_ID_JOYPAD_LEFT);
      ctx.buttons->buttons[H8_BUTTON_RIGHT] = h8lr_input(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    }
  }

  /* One step per press; holding the button does not repeat it */
  step_held = h8lr_input(RETRO_DEVICE_ID_JOYPAD_B);
  if (step_held && !ctx.step_held && ctx.steps_queued < 8)
    ctx.steps_queued++;
  ctx.step_held = step_held;
  step = h8lr_step_pulse();
  h8lr_read_accel(accel);

  for (i = 0; i < ctx.system.device_count; i++)
  {
    h8_device_t *device = &ctx.system.devices[i];

    if (device->type == H8_DEVICE_BMA150)
    {
      /* 10-bit two's complement; a step pushes Z up to about 2.3 g */
      int x = (int)(accel[0] * H8LR_COUNTS_PER_G);
      int y = (int)(accel[1] * H8LR_COUNTS_PER_G);
      int z = (int)(accel[2] * H8LR_COUNTS_PER_G) + (step ? 344 : 0);

      h8_bma150_set_axis(device, h8lr_clamp(x, -512, 511) & 0x3FF,
                         h8lr_clamp(y, -512, 511) & 0x3FF,
                         h8lr_clamp(z, -512, 511) & 0x3FF);
    }
    else if (device->type == H8_DEVICE_ACCELEROMETER_X ||
             device->type == H8_DEVICE_ACCELEROMETER_Y)
    {
      /* 10-bit, centered at rest; a step pushes it up by 192 */
      float g = accel[device->type == H8_DEVICE_ACCELEROMETER_X ? 0 : 1];
      h8_word_t analog;

      analog.u = h8lr_clamp(512 + (int)(g * H8LR_COUNTS_PER_G) +
                            (step ? 192 : 0), 0, 1023) << 6;
      h8_generic_adrr_set(device, analog);
    }
  }
}

static void h8lr_set_sensor(bool enable)
{
  if (enable && !sensor_enabled &&
      environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensor_cb) &&
      sensor_cb.set_sensor_state && sensor_cb.get_sensor_input)
    sensor_enabled = sensor_cb.set_sensor_state(0, RETRO_SENSOR_ACCELEROMETER_ENABLE, 60);
  else if (!enable && sensor_enabled)
  {
    sensor_cb.set_sensor_state(0, RETRO_SENSOR_ACCELEROMETER_DISABLE, 60);
    sensor_enabled = false;
  }
}

static void h8lr_start_network(void)
{
  struct retro_variable var = { "stepdad_ir_link", NULL };
  h8_network_ctx_t network;

  if (ctx.network_started ||
      !environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value ||
      string_is_equal(var.value, "disabled"))
    return;

  memset(&network, 0, sizeof(network));
  snprintf(network.ip, sizeof(network.ip), "127.0.0.1");
  network.port = 0xAAAA;
  network.server = string_is_equal(var.value, "server");
  if (h8_fe_network_init(&network))
    ctx.network_started = TRUE;
  else
  {
    if (log_cb)
      log_cb(RETRO_LOG_WARN, "IR link failed: %s\n", network.error_message);
    display_message("Failed to start IR link.");
  }
}

/* libretro API */

void retro_init(void)
{
  if (!environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
    log_cb = NULL;
}

void retro_reset(void)
{
  h8_init(&ctx.system);
}

bool retro_load_game(const struct retro_game_info *info)
{
  h8_system_id id;
  h8_device_t *device;
  h8_bool network_started = ctx.network_started;

  if (!info || !info->data || !info->size)
    return false;

  memset(&ctx, 0, sizeof(ctx));
  ctx.network_started = network_started;

  id = h8_system_identify((const h8_u8*)info->data, info->size);
  if (id == H8_SYSTEM_INVALID)
    id = info->size > 0x4000 ? H8_SYSTEM_NTR_032 : H8_SYSTEM_NTR_027;

  h8_rtc_set_current(&ctx.system.vmem.parts.io1.rtc, 0);
  memcpy(ctx.system.vmem.raw, info->data,
         info->size < sizeof(ctx.system.vmem.raw) ?
         info->size : sizeof(ctx.system.vmem.raw));
  h8_init(&ctx.system);
  if (!h8_system_init(&ctx.system, id))
    return false;

  device = h8lr_find_device(H8_DEVICE_LCD);
  if (device)
    ctx.lcd = (h8_lcd_t*)device->device;

  device = h8lr_find_device(H8_DEVICE_LED);
  if (device)
    ctx.led = (h8_led_t*)device->device;

  device = h8lr_find_device(H8_DEVICE_3BUTTON);
  if (!device)
    device = h8lr_find_device(H8_DEVICE_1BUTTON);
  if (device)
    ctx.buttons = (h8_buttons_t*)device->device;

  ctx.eeprom = h8lr_find_device(H8_DEVICE_EEPROM_64K);
  if (!ctx.eeprom)
    ctx.eeprom = h8lr_find_device(H8_DEVICE_EEPROM_8K);
  if (ctx.eeprom)
    /* Blank until the frontend loads a save over it */
    memset(ctx.eeprom->data, 0xFF, ctx.eeprom->size);

  ctx.buzzer = h8lr_find_device(H8_DEVICE_BUZZER);
  if (ctx.buzzer)
    h8_buzzer_set_rate(ctx.buzzer, H8_BUZZER_DEFAULT_RATE);

  h8lr_start_network();
  h8lr_set_sensor(h8lr_find_device(H8_DEVICE_BMA150) ||
                  h8lr_find_device(H8_DEVICE_ACCELEROMETER_X));

  return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info)
{
  return false;
}

void retro_unload_game(void)
{
  h8lr_set_sensor(false);
}

static const h8_u8 h8lr_lcd_colors[4][3] =
{
  { 180, 180, 170 },
  { 130, 130, 120 },
  { 100, 100, 90 },
  { 30, 30, 20 }
};

static const h8_u16 h8lr_led_colors[H8_LED_STATE_SIZE] =
{
  0x0000, /* Invalid */
  0x0000, /* Off */
  0xF800, /* Red */
  0x07E0, /* Green */
  0xFD00  /* Red and green together */
};

static h8_u16 h8lr_rgb565(unsigned r, unsigned g, unsigned b)
{
  return (h8_u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void h8lr_draw_lcd(void)
{
  h8_u16 colors[4];
  float contrast = 1.0f - ((ctx.lcd->contrast - 22) / 18.0f);
  unsigned i, x, y;

  if (contrast < 0.0f)
    contrast = 0.0f;
  else if (contrast > 1.0f)
    contrast = 1.0f;

  /* Contrast darkens every shade but the blank background */
  colors[0] = h8lr_rgb565(h8lr_lcd_colors[0][0], h8lr_lcd_colors[0][1],
                          h8lr_lcd_colors[0][2]);
  for (i = 1; i < 4; i++)
    colors[i] = h8lr_rgb565((unsigned)(h8lr_lcd_colors[i][0] * contrast),
                            (unsigned)(h8lr_lcd_colors[i][1] * contrast),
                            (unsigned)(h8lr_lcd_colors[i][2] * contrast));

  for (y = 0; y < lr_video_height; y++)
  {
    unsigned line = (ctx.lcd->start_line + y) & 127;
    unsigned page = (line >> 3) << 8;
    unsigned bit = line & 7;

    for (x = 0; x < lr_video_width; x++)
    {
      h8_u16 color = 0;

      if (x < 128u - ctx.lcd->display_offset)
      {
        h8_u8 hi = ctx.lcd->vram[page + x * 2];
        h8_u8 lo = ctx.lcd->vram[page + x * 2 + 1];

        color = colors[((hi >> bit) & 1) << 1 | ((lo >> bit) & 1)];
      }
      ctx.screen_buffer[y * lr_video_width + x] = color;
    }
  }
}

static void h8lr_draw_led(void)
{
  unsigned i;

  for (i = 0; i < 96 * 64; i++)
    ctx.screen_buffer[i] = h8lr_led_colors[ctx.led->state];
}

static void h8lr_push_audio(void)
{
  h8_s16 samples[H8_BUZZER_BUFFER_SIZE];
  unsigned count, i;

  if (!ctx.buzzer)
    return;

  /* The buzzer is mono; libretro wants interleaved stereo */
  count = h8_buzzer_read(ctx.buzzer, samples, H8_BUZZER_BUFFER_SIZE);
  for (i = 0; i < count; i++)
    ctx.audio_buffer[i * 2] = ctx.audio_buffer[i * 2 + 1] = samples[i];
  if (count)
    audio_batch_cb(ctx.audio_buffer, count);
}

void retro_run(void)
{
  bool updated = false;

  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    h8lr_start_network();

  h8lr_handle_input();
  h8_run(&ctx.system);
  h8lr_push_audio();

  if (ctx.lcd)
    h8lr_draw_lcd();
  else if (ctx.led)
    h8lr_draw_led();
  video_cb(ctx.screen_buffer, lr_video_width, lr_video_height, lr_video_width * 2);
}

void retro_get_system_info(struct retro_system_info *info)
{
  memset(info, 0, sizeof(*info));
  info->library_name = "Stepdad";
  info->library_version = GIT_VERSION;
  info->need_fullpath = false;
  info->valid_extensions = "rom|bin|payload";
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
  info->timing.sample_rate = H8_BUZZER_DEFAULT_RATE;
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
    { "stepdad_ir_link", "IR link (127.0.0.1:43690); server|client|disabled" },
    { NULL, NULL },
  };
  static const struct retro_controller_description port[] = {
    { "Buttons", RETRO_DEVICE_JOYPAD },
    { NULL, 0 },
  };
  static const struct retro_controller_info ports[] = {
    { port, 1 },
    { NULL, 0 },
  };
  struct retro_input_descriptor desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Step" },
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
    return ctx.system.vmem.raw;
  case RETRO_MEMORY_SAVE_RAM:
    if (ctx.eeprom)
      return ctx.eeprom->data;
    else
      return NULL;
  case RETRO_MEMORY_VIDEO_RAM:
    if (ctx.lcd)
      return ctx.lcd->vram;
    else
      return NULL;
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
    if (ctx.eeprom)
      return ctx.eeprom->size;
    else
      return 0;
  case RETRO_MEMORY_VIDEO_RAM:
    if (ctx.lcd)
      return sizeof(ctx.lcd->vram);
    else
      return 0;
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
