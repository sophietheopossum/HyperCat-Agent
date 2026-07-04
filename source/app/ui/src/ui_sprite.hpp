#ifndef HC_UI_SPRITE_HPP
#define HC_UI_SPRITE_HPP

/* ui_sprite — a tiny, reusable pixel-art spritesheet + animation engine for the ImGui UI (WI-5). INTERNAL
 * to hc_ui. This is the GENERIC layer (no HyperCat-specific knowledge — the mascot lives in ui_mascot);
 * its single job is "a grid of frames on a texture, played over time."
 *
 * Three pieces, smallest-responsibility first:
 *   - SpriteSheet : one backend-managed GPU texture sliced into a uniform cols×rows grid. uv_for(frame)
 *                   is a PURE function (row/col/margin math, no GL, no ImGui) and is the offline test
 *                   surface. from_qoi() decodes a trusted app-bundled QOI blob (libs/hc_image) and uploads
 *                   it via ImGui 1.92's backend-managed texture path — never direct GL, so the locked
 *                   renderer-isolation / Metal-swap decision is preserved.
 *   - Animation   : pure data — an ordered list of (frame, duration) steps + a LoopMode. No GL.
 *   - SpritePlayer: a playhead over an Animation; advance(dt) resolves the current frame; draw() blits it
 *                   crisp (the backend's NEAREST sampler) via ImGui::Image. No persistent GPU state.
 *
 * Ownership / lifetime (the one subtle part — documented in full in ui_sprite.cpp):
 *   A SpriteSheet owns exactly one ImTextureData, registered with the ImGui context (RegisterUserTexture)
 *   so it survives across frames. The GPU side is freed by the renderer backend's shutdown sweep; the
 *   SpriteSheet destructor frees only the CPU-side ImTextureData object. THEREFORE a SpriteSheet MUST be
 *   destroyed AFTER the backend is torn down (UiApp guarantees this: it owns the sheets in its Impl, which
 *   is destroyed after the Backend). A SpriteSheet is non-copyable (it owns a unique texture).
 * Threading: single-threaded — construct, advance, and draw on the UI/GL thread only. from_qoi requires a
 *   live ImGui context (it registers the texture). */

#include "imgui.h" /* ImVec2, ImTextureRef, ImU32, ImTextureData (fwd via imgui.h) */

#include <cstddef> /* size_t */
#include <vector>

namespace hc::ui {

/* How a SpritePlayer treats the end of an Animation's step list. */
enum class LoopMode { Once, Loop, PingPong };

/* The normalized (uv0,uv1) sub-rectangle of a SpriteSheet for one frame — the pure output of uv_for. */
struct FrameUV {
    ImVec2 uv0, uv1;
};

/* PURE grid math: the (uv0,uv1) for frame `i` of a tex_w×tex_h sheet tiled into a cols×rows grid of
 * frame_w×frame_h frames with an outer `margin` and inter-frame `spacing` (pixels). Frames are row-major
 * (left-to-right, then top-to-bottom); `i` is clamped to [0, cols*rows). A degenerate sheet returns the
 * full [0,0]..[1,1] rect. Exposed (free function) so the math is unit-tested without a GPU texture —
 * SpriteSheet::uv_for simply forwards here. No GL, no ImGui. */
FrameUV sprite_uv(int tex_w, int tex_h, int cols, int rows, int frame_w, int frame_h, int margin,
                  int spacing, int i);

/* One backend-managed texture sliced into a uniform cols×rows grid of frames, addressed row-major
 * (left-to-right, then top-to-bottom). Optional outer `margin` and inter-frame `spacing` (pixels) support
 * padded atlases; both default 0 for a contiguous grid (what the mascot strips use). */
class SpriteSheet {
public:
    /* Decode a COMPLETE QOI byte stream (trusted, app-bundled — never network/model bytes) into a
     * backend-managed RGBA texture sliced into cols×rows frames. Requires a live ImGui context. Returns
     * nullptr on any decode/validation/oversize/upload failure or a degenerate grid (caller shows a
     * fallback — never a crash). Caller owns the returned pointer; see the file banner for teardown. */
    static SpriteSheet *from_qoi(const unsigned char *qoi, size_t len, int cols, int rows, int margin = 0,
                                 int spacing = 0);
    ~SpriteSheet();

    SpriteSheet(const SpriteSheet &) = delete; /* owns a unique GPU texture — non-copyable */
    SpriteSheet &operator=(const SpriteSheet &) = delete;

    int frame_count() const { return cols_ * rows_; }
    int frame_w() const { return frame_w_; }
    int frame_h() const { return frame_h_; }

    /* The texture reference for ImGui::Image/ImageWithBg. Valid for the SpriteSheet's lifetime. */
    ImTextureRef tex_ref() const;

    /* PURE: the normalized (uv0,uv1) for frame `i`, clamped to [0, frame_count()). No GL / no ImGui — the
     * offline-testable core. A degenerate sheet (no frames) returns the full [0,0]..[1,1] rect. */
    FrameUV uv_for(int i) const;

private:
    SpriteSheet() = default;

    ImTextureData *tex_ = nullptr; /* owned; freed in the dtor (GPU side via the backend shutdown sweep) */
    int            tex_w_ = 0, tex_h_ = 0;
    int            cols_ = 1, rows_ = 1;
    int            frame_w_ = 0, frame_h_ = 0;
    int            margin_ = 0, spacing_ = 0;
};

/* One step of an animation: which sheet frame to show, for how long (ms). */
struct AnimStep {
    int   frame = 0;
    float dur_ms = 100.0f;
};

/* An animation: an ordered list of steps + a loop mode. Pure data, no GL — copyable/movable. */
struct Animation {
    std::vector<AnimStep> steps;
    LoopMode              loop = LoopMode::Loop;

    /* A uniform animation over frames [0, count) at `fps` frames/second. count<=0 -> empty. */
    static Animation strip(int count, float fps, LoopMode loop = LoopMode::Loop);
    /* A single static frame (e.g. an idle pose). total_ms()==0 -> the player simply holds it. */
    static Animation still(int frame);

    float total_ms() const; /* sum of step durations (0 for a still / empty) */
    bool  empty() const { return steps.empty(); }
};

/* A playhead over an Animation: advance(dt) walks time, resolves the current step/frame, draw() blits it.
 * Holds no GPU state — the SpriteSheet does. Single-threaded (UI thread). */
class SpritePlayer {
public:
    void set_animation(const Animation &a); /* adopt `a`, reset the playhead to t=0, resume playing */
    void play() { playing_ = true; }
    void pause() { playing_ = false; }
    void stop(); /* pause + seek to t=0 */
    void set_speed(float s); /* playback-rate multiplier, clamped >= 0 */
    void seek_time(float ms);
    void seek_step(int step_index);
    void advance(float dt_ms); /* typically ImGui::GetIO().DeltaTime * 1000.0f */

    int current_step() const { return step_; }
    int current_frame() const; /* the sheet frame for the current step (0 if empty) */

    /* Draw the current frame from `sheet` at `size` px, crisp (NEAREST), multiplied by `tint`. Reserves
     * `size` with a Dummy and returns if the sheet has no frames / no animation is set. Call inside a
     * window, on the UI thread. */
    void draw(const SpriteSheet &sheet, ImVec2 size, ImU32 tint = IM_COL32_WHITE) const;

private:
    int resolve_step() const; /* fold t_ms_ to the loop domain and walk cumulative durations -> step */

    Animation anim_;
    float     t_ms_ = 0.0f;  /* the raw accumulated playhead (folded per loop mode at resolve time) */
    int       step_ = 0;     /* the resolved step index for t_ms_ */
    float     speed_ = 1.0f;
    bool      playing_ = true;
};

} // namespace hc::ui

#endif /* HC_UI_SPRITE_HPP */
