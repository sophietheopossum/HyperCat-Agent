#ifndef HC_UI_SPRITE_REGISTRY_HPP
#define HC_UI_SPRITE_REGISTRY_HPP

/* ui_sprite_registry — the UiApp-owned catalog of sprite sheets (Conductor P5-S0). INTERNAL to hc_ui.
 *
 * The ui_sprite ENGINE is already reusable; what wasn't was OWNERSHIP — every consumer (just the mascot, so
 * far) privately decoded its own sheets and the GPU-teardown order was hand-enforced per-consumer. As the
 * conductor chat avatar + tool cue join, that would mean duplicated QOI decodes and N copies of the
 * "backend-before-sprites" invariant. The registry fixes that: it OWNS every SpriteSheet (decode-once, keyed
 * by the manifest name) + a default Animation per sheet (from the manifest fps/loop); consumers BORROW a sheet
 * and hold only a cheap, GPU-free SpritePlayer.
 *
 * Owns:      one SpriteSheet per catalog entry (each a backend-managed GPU texture). The catalog is the
 *            GENERATED app/ui/assets/sprite_assets.h (hc_sprite_assets[], packed by tools/qoi_pack from
 *            app/ui/assets/sprites.manifest). The named Animations are pure data.
 * Teardown:  the registry is the SINGLE holder of the ui_sprite "destroy sheets AFTER the renderer backend"
 *            invariant — UiApp::Impl declares it BEFORE the borrowers (`mascot`, `chat_view`) so it outlives
 *            them, and after `delete backend` runs in the dtor body; ~SpriteSheet only frees the CPU-side
 *            ImTextureData (no ImGui calls, safe post-DestroyContext).
 * Threading: single-threaded (UI/GL thread). ensure_loaded() needs a live ImGui context (it uploads textures);
 *            animation() is pure and works without one. A per-sheet decode failure is recorded as a null sheet
 *            and skipped — one bad asset never blocks the rest, and sheet() returns nullptr (consumers fall
 *            back to text, never a crash). */

#include "ui_sprite.hpp" /* SpriteSheet, Animation */

#include <memory>
#include <string>
#include <unordered_map>

namespace hc::ui {

class SpriteRegistry {
public:
    SpriteRegistry();  /* registers the named Animations from the catalog (pure; no GL/context) */
    ~SpriteRegistry() = default; /* the unique_ptr<SpriteSheet> members free CPU textures — see the banner */

    SpriteRegistry(const SpriteRegistry &) = delete;
    SpriteRegistry &operator=(const SpriteRegistry &) = delete;

    /* Decode + upload the whole catalog. Idempotent (a second call is a no-op). Needs a live ImGui context.
     * One bad asset -> a recorded null sheet, skipped; the rest still load. */
    void ensure_loaded();

    /* Borrow a decoded sheet by catalog name, or nullptr if unknown / not-yet-loaded / failed-to-decode. */
    const SpriteSheet *sheet(const char *name) const;

    /* The catalog's default Animation for `name` (a strip at the manifest fps/loop, or a still for fps<=0 /
     * a single frame). An unknown name returns Animation::still(0) so a caller's player is always valid. */
    Animation animation(const char *name) const;

    bool loaded() const { return loaded_; }

private:
    bool                                                          loaded_ = false;
    std::unordered_map<std::string, std::unique_ptr<SpriteSheet>> sheets_; /* owns the GPU textures */
    std::unordered_map<std::string, Animation>                    anims_;  /* pure data            */
};

} // namespace hc::ui

#endif /* HC_UI_SPRITE_REGISTRY_HPP */
