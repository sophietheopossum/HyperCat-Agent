/* ui_sprite — implementation of the generic spritesheet + animation engine (see ui_sprite.hpp). INTERNAL.
 *
 * TEXTURE OWNERSHIP & TEARDOWN (the one genuinely subtle thing here, written out in full):
 *   ImGui 1.92 manages GPU textures for the backend. Each frame, ImGui rebuilds GetPlatformIO().Textures
 *   from (font atlases + the context's UserTextures); the renderer backend uploads any with WantCreate and,
 *   at shutdown, frees the GPU side of every entry with RefCount==1. So to keep a texture alive across
 *   frames we register it once with RegisterUserTexture (RefCount->1, added to UserTextures) — pushing
 *   straight into GetPlatformIO().Textures would be wiped at the next frame's end.
 *
 *   Consequences this code relies on:
 *     1. The GPU texture is destroyed by the BACKEND's shutdown sweep (it walks GetPlatformIO().Textures,
 *        which still lists our texture from the final frame, RefCount==1). We never call GL ourselves —
 *        that is what keeps the renderer-isolation / Metal-swap locked decision intact.
 *     2. Therefore ~SpriteSheet must run AFTER the backend is gone, and must NOT access the ImGui CONTEXT
 *        (no GImGui: no UnregisterUserTexture, no widget calls — the context is already destroyed). It
 *        frees ONLY the CPU side (IM_DELETE -> ~ImTextureData -> DestroyPixels). NOTE this still routes
 *        through ImGui's ALLOCATOR (ImGui::MemFree, for both the object and its IM_ALLOC'd pixel buffer),
 *        which is deliberately safe after DestroyContext: ImGui's allocator function pointers are
 *        PROCESS-GLOBAL (set by SetAllocatorFunctions; default = malloc/free), not stored in the context,
 *        so MemFree never dereferences GImGui. UiApp enforces the order in its Impl destructor (it deletes
 *        the Backend before the MascotView member is destroyed — see ui_app.cpp).
 *   Decoding trusted, app-bundled QOI only (libs/hc_image), with every dimension bounded by the decoder —
 *   no network/model bytes ever reach this path in v1. */

#include "ui_sprite.hpp"

#include "hc_image.h" /* hc_image_decode_qoi / hc_image_free — the in-house QOI decoder (libs/hc_image) */

#include "imgui.h"
/* ImGui::RegisterUserTexture lives in imgui_internal.h and is tagged EXPERIMENTAL there. It is the only
 * way in 1.92 to keep a user texture across frames (GetPlatformIO().Textures is rebuilt from UserTextures
 * every frame). We pin imgui to an exact tag, so this cannot shift under us; if a pin bump breaks this
 * call it is a COMPILE error (check imgui_internal.h for a rename or a promotion to imgui.h). */
#include "imgui_internal.h"

#include <cstdint> /* int64_t — overflow-safe grid intermediates */

#include <cmath>   /* std::fmod */
#include <cstring> /* std::memcpy */

namespace hc::ui {

// ---- SpriteSheet ---------------------------------------------------------------------------------------

SpriteSheet *SpriteSheet::from_qoi(const unsigned char *qoi, size_t len, int cols, int rows, int margin,
                                   int spacing)
{
    /* Reject non-positive AND absurdly-large grid params up front. The decoder caps any image at
     * HC_IMAGE_MAX_DIM per axis, so a grid that tiles a valid image cannot need more cols/rows — nor a
     * margin/spacing — beyond that cap. Bounding all four here keeps the int arithmetic below
     * (2*margin, (cols-1)*spacing) far under INT_MAX, so it cannot overflow (UB) for ANY caller, not just
     * today's tiny compile-time constants. */
    const int kMaxGrid = (int)HC_IMAGE_MAX_DIM;
    if (!qoi || cols <= 0 || rows <= 0 || margin < 0 || spacing < 0 || cols > kMaxGrid ||
        rows > kMaxGrid || margin > kMaxGrid || spacing > kMaxGrid)
        return nullptr;

    unsigned char *rgba = nullptr;
    int            w = 0, h = 0;
    if (hc_image_decode_qoi(qoi, len, &rgba, &w, &h) != 0) return nullptr; /* malformed/oversize -> -1 */

    /* The grid must tile the image exactly — a mismatch is a programming/asset error, surfaced as a null
     * (the caller then shows a fallback) rather than a silently-misaligned sprite. Computed in 64-bit as
     * belt-and-suspenders over the bounds above. */
    const int64_t usable_w = (int64_t)w - 2LL * margin - (int64_t)(cols - 1) * spacing;
    const int64_t usable_h = (int64_t)h - 2LL * margin - (int64_t)(rows - 1) * spacing;
    if (usable_w <= 0 || usable_h <= 0 || usable_w % cols != 0 || usable_h % rows != 0) {
        hc_image_free(rgba);
        return nullptr;
    }
    const int fw = (int)(usable_w / cols), fh = (int)(usable_h / rows);

    /* Upload via the backend-managed texture path (no direct GL). Create() allocates Pixels (w*h*4) and
     * sets Status=WantCreate; we fill it and register so the backend uploads it next frame. */
    ImTextureData *tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(tex->GetPixels(), rgba, (size_t)w * (size_t)h * 4u);
    hc_image_free(rgba);
    ImGui::RegisterUserTexture(tex);

    SpriteSheet *s = new SpriteSheet();
    s->tex_ = tex;
    s->tex_w_ = w;
    s->tex_h_ = h;
    s->cols_ = cols;
    s->rows_ = rows;
    s->frame_w_ = fw;
    s->frame_h_ = fh;
    s->margin_ = margin;
    s->spacing_ = spacing;
    return s;
}

SpriteSheet::~SpriteSheet()
{
    /* CPU side only — see the file banner. The GPU texture was freed by the backend's shutdown sweep; the
     * ImGui context no longer exists by the time we run, so we must not call any ImGui function here. */
    if (tex_) IM_DELETE(tex_);
}

ImTextureRef SpriteSheet::tex_ref() const { return tex_ ? tex_->GetTexRef() : ImTextureRef(); }

FrameUV sprite_uv(int tex_w, int tex_h, int cols, int rows, int frame_w, int frame_h, int margin,
                  int spacing, int i)
{
    /* All grid arithmetic is 64-bit so a hostile/large (cols,rows,margin,spacing,frame_*) from any caller
     * cannot overflow signed int (UB) — the products narrow to float at the end anyway. */
    const int64_t count = (int64_t)cols * rows;
    if (tex_w <= 0 || tex_h <= 0 || count <= 0) return {ImVec2(0, 0), ImVec2(1, 1)};
    if (i < 0) i = 0;
    if (i >= count) i = (int)(count - 1);

    const int     col = i % cols;
    const int     row = i / cols;
    const int64_t x0 = (int64_t)margin + (int64_t)col * ((int64_t)frame_w + spacing);
    const int64_t y0 = (int64_t)margin + (int64_t)row * ((int64_t)frame_h + spacing);

    const float iw = (float)tex_w, ih = (float)tex_h;
    FrameUV     uv;
    uv.uv0 = ImVec2((float)x0 / iw, (float)y0 / ih);
    uv.uv1 = ImVec2((float)(x0 + frame_w) / iw, (float)(y0 + frame_h) / ih);
    return uv;
}

FrameUV SpriteSheet::uv_for(int i) const
{
    return sprite_uv(tex_w_, tex_h_, cols_, rows_, frame_w_, frame_h_, margin_, spacing_, i);
}

// ---- Animation -----------------------------------------------------------------------------------------

Animation Animation::strip(int count, float fps, LoopMode loop)
{
    Animation a;
    a.loop = loop;
    if (count <= 0) return a;
    const float dur = (fps > 0.0f) ? (1000.0f / fps) : 100.0f;
    a.steps.reserve((size_t)count);
    for (int i = 0; i < count; ++i) a.steps.push_back({i, dur});
    return a;
}

Animation Animation::still(int frame)
{
    Animation a;
    a.loop = LoopMode::Once;
    a.steps.push_back({frame, 0.0f}); /* total_ms()==0 -> the player holds this single frame */
    return a;
}

float Animation::total_ms() const
{
    float t = 0.0f;
    for (const auto &s : steps) t += s.dur_ms;
    return t;
}

// ---- SpritePlayer --------------------------------------------------------------------------------------

void SpritePlayer::set_animation(const Animation &a)
{
    anim_ = a;
    t_ms_ = 0.0f;
    step_ = 0;
    playing_ = true;
}

void SpritePlayer::stop()
{
    playing_ = false;
    t_ms_ = 0.0f;
    step_ = resolve_step();
}

void SpritePlayer::set_speed(float s) { speed_ = (s < 0.0f) ? 0.0f : s; }

void SpritePlayer::seek_time(float ms)
{
    t_ms_ = (ms < 0.0f) ? 0.0f : ms;
    step_ = resolve_step();
}

void SpritePlayer::seek_step(int step_index)
{
    if (anim_.steps.empty()) {
        step_ = 0;
        t_ms_ = 0.0f;
        return;
    }
    if (step_index < 0) step_index = 0;
    if (step_index >= (int)anim_.steps.size()) step_index = (int)anim_.steps.size() - 1;
    float t = 0.0f;
    for (int i = 0; i < step_index; ++i) t += anim_.steps[(size_t)i].dur_ms;
    t_ms_ = t;
    step_ = step_index;
}

void SpritePlayer::advance(float dt_ms)
{
    if (!playing_ || anim_.empty()) return;
    const float total = anim_.total_ms();
    if (total <= 0.0f) { /* a still (one zero-duration step) — nothing to advance */
        step_ = 0;
        return;
    }
    t_ms_ += dt_ms * speed_;
    switch (anim_.loop) {
        case LoopMode::Once:
            if (t_ms_ >= total) {
                t_ms_ = total;
                playing_ = false; /* hold the final frame */
            }
            break;
        case LoopMode::Loop:
            t_ms_ = std::fmod(t_ms_, total);
            if (t_ms_ < 0.0f) t_ms_ += total;
            break;
        case LoopMode::PingPong: {
            const float period = 2.0f * total;
            t_ms_ = std::fmod(t_ms_, period);
            if (t_ms_ < 0.0f) t_ms_ += period;
            break;
        }
    }
    step_ = resolve_step();
}

int SpritePlayer::resolve_step() const
{
    if (anim_.steps.empty()) return 0;
    const float total = anim_.total_ms();
    if (total <= 0.0f) return 0; /* a still -> the single step */

    float eff = t_ms_;
    if (anim_.loop == LoopMode::PingPong && eff > total) eff = 2.0f * total - eff; /* mirror 2nd half */
    if (eff < 0.0f) eff = 0.0f;
    if (eff >= total) eff = total;

    float acc = 0.0f;
    for (size_t i = 0; i < anim_.steps.size(); ++i) {
        acc += anim_.steps[i].dur_ms;
        if (eff < acc) return (int)i;
    }
    return (int)anim_.steps.size() - 1;
}

int SpritePlayer::current_frame() const
{
    if (anim_.steps.empty()) return 0;
    int s = step_;
    if (s < 0) s = 0;
    if (s >= (int)anim_.steps.size()) s = (int)anim_.steps.size() - 1;
    return anim_.steps[(size_t)s].frame;
}

void SpritePlayer::draw(const SpriteSheet &sheet, ImVec2 size, ImU32 tint) const
{
    if (sheet.frame_count() <= 0 || anim_.empty()) {
        ImGui::Dummy(size); /* keep the layout stable even with no art */
        return;
    }
    const FrameUV uv = sheet.uv_for(current_frame());

    /* Crisp pixels: bracket the image with the backend's NEAREST sampler callback, then restore LINEAR for
     * everything drawn after (text, borders). The standard callbacks live in the platform IO; if a backend
     * doesn't provide them we just draw with its default (linear) sampler — soft, but never broken. */
    ImDrawList            *dl = ImGui::GetWindowDrawList();
    const ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
    const bool nearest = pio.DrawCallback_SetSamplerNearest && pio.DrawCallback_SetSamplerLinear;
    if (nearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
    if (tint == IM_COL32_WHITE) {
        ImGui::Image(sheet.tex_ref(), size, uv.uv0, uv.uv1);
    } else {
        ImGui::ImageWithBg(sheet.tex_ref(), size, uv.uv0, uv.uv1, ImVec4(0, 0, 0, 0),
                           ImGui::ColorConvertU32ToFloat4(tint));
    }
    if (nearest) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
}

} // namespace hc::ui
