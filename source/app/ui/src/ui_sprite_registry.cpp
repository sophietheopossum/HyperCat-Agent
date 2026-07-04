/* ui_sprite_registry — implementation. See ui_sprite_registry.hpp.
 *
 * The catalog is the GENERATED sprite_assets.h: hc_sprite_assets[] (name + QOI bytes + grid + fps/loop),
 * packed by tools/qoi_pack from app/ui/assets/sprites.manifest. This TU is the ONLY consumer of that header,
 * so "asset bytes -> named, grid-sliced, animated sprite" lives in exactly one place; nothing in C++ hardcodes
 * a grid. Only trusted, app-bundled QOI is decoded (libs/hc_image), never network/model bytes. */

#include "ui_sprite_registry.hpp"

#include "sprite_assets.h" /* GENERATED: hc_sprite_asset, hc_sprite_assets[], hc_sprite_assets_count */

namespace hc::ui {

namespace {
/* The manifest fps/loop -> the default Animation: a uniform strip over all frames, or a still for a single
 * frame / fps<=0. loop codes match qoi_pack: 0=Once, 1=Loop, 2=PingPong. */
Animation anim_from(const hc_sprite_asset &a)
{
    const int frames = a.cols * a.rows;
    if (a.fps <= 0.0f || frames <= 1) return Animation::still(0);
    const LoopMode lm = a.loop == 1 ? LoopMode::Loop : a.loop == 2 ? LoopMode::PingPong : LoopMode::Once;
    return Animation::strip(frames, a.fps, lm);
}
} // namespace

SpriteRegistry::SpriteRegistry()
{
    /* Pure: register every catalog Animation up front (no GL) so animation() is valid before ensure_loaded()
     * (and offline-testable). The sheets themselves are decoded lazily in ensure_loaded(). */
    for (std::size_t i = 0; i < hc_sprite_assets_count; i++)
        anims_[hc_sprite_assets[i].name] = anim_from(hc_sprite_assets[i]);
}

void SpriteRegistry::ensure_loaded()
{
    if (loaded_) return;
    loaded_ = true; /* one-shot regardless of per-asset outcome — never re-decode a bad asset every frame */
    for (std::size_t i = 0; i < hc_sprite_assets_count; i++) {
        const hc_sprite_asset &a = hc_sprite_assets[i];
        /* from_qoi returns nullptr on any decode/upload failure -> recorded as a null sheet, skipped by
         * sheet(); the rest of the catalog still loads. */
        sheets_[a.name].reset(SpriteSheet::from_qoi(a.qoi, a.qoi_len, a.cols, a.rows));
    }
}

const SpriteSheet *SpriteRegistry::sheet(const char *name) const
{
    auto it = sheets_.find(name);
    return it != sheets_.end() ? it->second.get() : nullptr;
}

Animation SpriteRegistry::animation(const char *name) const
{
    auto it = anims_.find(name);
    return it != anims_.end() ? it->second : Animation::still(0);
}

} // namespace hc::ui
