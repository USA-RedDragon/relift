#include "runner.h"
#include "n64.h"

#include <relift/settings.h>

TEST(n64_the_picture_is_the_consoles_unless_the_program_or_the_run_picks_another)
{
    rl_settings_reset();
    rl_n64_declare_picture(NULL);
    rl_n64_picture p;
    rl_n64_picture_get(&p);
    CHECK(p.upscale == 1 && p.downscale_steps == 0 && !p.super_sampled_readback && !p.weave);
    CHECK(p.vi_aa && p.vi_resample && p.vi_serrate && p.dither_filter && p.divot && p.gamma_dither);
    CHECK(p.native_tex_rect && !p.native_texture_lod && p.crop_overscan == 0);

    static const char *const dev[] = {"mode=custom", "upscale=4x", "divot=false", "crop_overscan=8", "vi_aa=maybe",
                                      "nothing=1", NULL};
    rl_settings_reset();
    rl_n64_declare_picture(dev);
    rl_n64_picture_get(&p);
    CHECK(p.upscale == 4 && !p.divot && p.crop_overscan == 8 && p.vi_aa);

    CHECK(rl_settings_set("n64.picture.deinterlace", "weave") == RL_SETTING_OK);
    CHECK(rl_settings_set("n64.picture.upscale", "8x") == RL_SETTING_OK);
    rl_n64_picture_get(&p);
    CHECK(p.upscale == 8 && p.weave);

    CHECK(rl_settings_set("n64.picture.mode", "console") == RL_SETTING_OK);
    rl_n64_picture_get(&p);
    CHECK(p.upscale == 1 && p.divot && !p.weave && p.crop_overscan == 0);
    rl_settings_reset();
}
