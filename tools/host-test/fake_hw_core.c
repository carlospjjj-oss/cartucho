/* A minimal hardware-rendered libretro core for testing Cartucho's GLES path:
 * it asks for an OpenGL ES 3 context, renders into the frontend's framebuffer
 * with a bottom-left origin (like the Azahar core), red in the top half and
 * blue in the bottom half, at 320x240 inside a 400x480 maximum. */
#include <string.h>
#include <GLES3/gl3.h>
#include "libretro.h"

static retro_environment_t env;
static retro_video_refresh_t video;
static struct retro_hw_render_callback hw;
static PFNGLCLEARPROC p_clear;
static PFNGLSCISSORPROC p_scissor;
static PFNGLENABLEPROC p_enable;
static PFNGLCLEARCOLORPROC p_clear_color;
static PFNGLBINDFRAMEBUFFERPROC p_bind_fb;
static PFNGLVIEWPORTPROC p_viewport;
static int resets;

void retro_set_environment(retro_environment_t cb) { env = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb) { (void)cb; }
void retro_set_input_state(retro_input_state_t cb) { (void)cb; }
void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_get_system_info(struct retro_system_info* i) {
    memset(i, 0, sizeof(*i));
    i->library_name = "fake-hw";
    i->library_version = "1";
    i->need_fullpath = true;
    i->valid_extensions = "fake";
}
void retro_get_system_av_info(struct retro_system_av_info* i) {
    memset(i, 0, sizeof(*i));
    i->geometry.base_width = 320;
    i->geometry.base_height = 240;
    i->geometry.max_width = 400;
    i->geometry.max_height = 480;
    i->geometry.aspect_ratio = 4.0f / 3.0f;
    i->timing.fps = 60.0;
    i->timing.sample_rate = 48000.0;
}
void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
void retro_reset(void) {}
static void context_reset(void) {
    resets++;
    p_clear = (PFNGLCLEARPROC)hw.get_proc_address("glClear");
    p_scissor = (PFNGLSCISSORPROC)hw.get_proc_address("glScissor");
    p_enable = (PFNGLENABLEPROC)hw.get_proc_address("glEnable");
    p_clear_color = (PFNGLCLEARCOLORPROC)hw.get_proc_address("glClearColor");
    p_bind_fb = (PFNGLBINDFRAMEBUFFERPROC)hw.get_proc_address("glBindFramebuffer");
    p_viewport = (PFNGLVIEWPORTPROC)hw.get_proc_address("glViewport");
}
static void context_destroy(void) {}
void retro_run(void) {
    if (!p_clear) return;
    p_bind_fb(GL_FRAMEBUFFER, (GLuint)hw.get_current_framebuffer());
    p_viewport(0, 0, 320, 240);
    p_enable(GL_SCISSOR_TEST);
    /* GL origin is bottom-left: y 120..240 is the top half of the picture. */
    p_scissor(0, 120, 320, 120);
    p_clear_color(1, 0, 0, 1);
    p_clear(GL_COLOR_BUFFER_BIT);
    p_scissor(0, 0, 320, 120);
    p_clear_color(0, 0, 1, 1);
    p_clear(GL_COLOR_BUFFER_BIT);
    /* Leave state dirty on purpose: the frontend must cope. */
    video(RETRO_HW_FRAME_BUFFER_VALID, 320, 240, 0);
}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void* d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void* d, size_t s) { (void)d; (void)s; return false; }
bool retro_load_game(const struct retro_game_info* g) {
    (void)g;
    memset(&hw, 0, sizeof(hw));
    hw.context_type = RETRO_HW_CONTEXT_OPENGLES3;
    hw.version_major = 3;
    hw.context_reset = context_reset;
    hw.context_destroy = context_destroy;
    hw.depth = true;
    hw.stencil = true;
    hw.bottom_left_origin = true;
    return env(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw);
}
void retro_unload_game(void) {}
void* retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
int fake_core_resets(void) { return resets; }
