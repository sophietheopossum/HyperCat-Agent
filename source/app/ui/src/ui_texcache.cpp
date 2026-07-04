/* ui_texcache — implementation of the OperatorTextureCache + its pure LruBudget core (see ui_texcache.hpp).
 *
 * MID-RUN TEXTURE DESTRUCTION (the one genuinely subtle thing — written out in full).
 *   ImGui 1.92 manages GPU textures for the renderer backend. A user texture is kept alive across frames by
 *   RegisterUserTexture (RefCount->1, added to g.UserTextures, which UpdateTexturesEndFrame republishes into
 *   GetPlatformIO().Textures every frame). For its GPU side to be FREED while the app is still running, the
 *   backend must see it in the per-frame texture list with Status==WantDestroy && UnusedFrames>0 — at which
 *   point ImGui_ImplOpenGL3_UpdateTexture() calls glDeleteTextures + SetStatus(Destroyed). ImGui runs this
 *   exact lifecycle automatically for its OWN font-atlas textures (imgui_draw.cpp), but NOT for user textures
 *   — so we reproduce the same state machine here, once per frame, in new_frame():
 *
 *     frame N   : retire(key) — move the ImTextureData* to retiring_ and set WantDestroyNextFrame=true. It is
 *                 NO LONGER drawn (dropped from map_), but stays REGISTERED so the backend can still find it.
 *     frame N+1 : new_frame() flips WantDestroyNextFrame -> Status=WantDestroy and UnusedFrames++ (=1). The
 *                 backend's RenderDrawData (end_frame) then sees WantDestroy && UnusedFrames>0 and destroys the
 *                 GPU texture, SetStatus(Destroyed). The resurrection guard in ImTextureData::SetStatus
 *                 (`!WantDestroyNextFrame`) does NOT fire BECAUSE we set WantDestroyNextFrame — so a still-
 *                 populated Pixels buffer is not re-uploaded. (That flag is why WantDestroy alone is not
 *                 enough.)
 *     frame N+2 : new_frame() sees Status==Destroyed (a destroy WE scheduled) -> UnregisterUserTexture (drop
 *                 from g.UserTextures) + IM_DELETE (free the CPU side). Reaped.
 *
 *   The texture is never referenced by draw data after frame N (we drop it from map_ at retire), and the GL id
 *   was already deleted by frame N+1, so there is no use-after-free across the handshake.
 *
 * SHUTDOWN.  ~OperatorTextureCache runs AFTER `delete backend` (UiApp::Impl declares the cache so its dtor runs
 *   in the Impl body's member-teardown, which the explicit `delete backend` in ~Impl precedes). By then the
 *   backend's DestroyDeviceObjects sweep has freed the GPU side of every still-registered texture (RefCount==1)
 *   and the ImGui context is gone — so the dtor frees ONLY the CPU side (IM_DELETE) and calls NO ImGui
 *   function, exactly like ~SpriteSheet. IM_DELETE routes pixel frees through ImGui's PROCESS-GLOBAL allocator
 *   (safe post-DestroyContext; see the ui_sprite.cpp banner). */

#include "ui_texcache.hpp"

#include "hc_image.h" /* hc_image_decode_auto / _qoi / _free — the bounded, fuzzed decoders (libs/hc_image) */

#include "imgui.h"
#include "imgui_internal.h" /* Register/UnregisterUserTexture — EXPERIMENTAL, pinned by our exact imgui tag */

#include <cstring> /* std::memcpy */

namespace hc::ui {

// ---- LruBudget (pure) ----------------------------------------------------------------------------------

LruBudget::LruBudget(std::size_t budget_bytes, std::size_t max_count)
    : budget_(budget_bytes), max_count_(max_count == 0 ? 1 : max_count)
{
}

void LruBudget::touch(std::uint64_t key)
{
    auto it = idx_.find(key);
    if (it == idx_.end()) return;
    order_.splice(order_.begin(), order_, it->second.first); /* move its node to the front (MRU) */
}

void LruBudget::insert(std::uint64_t key, std::size_t bytes)
{
    if (idx_.find(key) != idx_.end()) return; /* caller contract: NEW keys only; ignore a double-insert */
    order_.push_front(key);
    idx_.emplace(key, std::make_pair(order_.begin(), bytes));
    bytes_ += bytes;
}

std::size_t LruBudget::remove(std::uint64_t key)
{
    auto it = idx_.find(key);
    if (it == idx_.end()) return 0;
    std::size_t b = it->second.second;
    order_.erase(it->second.first);
    idx_.erase(it);
    bytes_ -= b;
    return b;
}

std::vector<std::uint64_t> LruBudget::victims_for(std::size_t incoming) const
{
    std::vector<std::uint64_t> victims;
    /* Evict LRU-first until BOTH constraints would hold after the new entry is inserted:
     *   (a) bytes_ - freed + incoming <= budget_     (b) (count - victims) + 1 <= max_count_
     * Walk from the back (LRU). A single over-budget `incoming` simply evicts everything (soft budget). */
    std::size_t freed = 0;
    std::size_t kept  = order_.size(); /* entries that would remain if we stop now */
    auto        rit   = order_.rbegin();
    while (rit != order_.rend()) {
        const bool bytes_ok = (bytes_ - freed + incoming) <= budget_;
        const bool count_ok = (kept + 1) <= max_count_;
        if (bytes_ok && count_ok) break;
        std::uint64_t key = *rit;
        victims.push_back(key);
        auto fit = idx_.find(key);
        freed += (fit != idx_.end()) ? fit->second.second : 0;
        --kept;
        ++rit;
    }
    return victims;
}

// ---- OperatorTextureCache ------------------------------------------------------------------------------

namespace {

/* The byte/count budget for operator-image textures. 64 MiB of RGBA8 ~= one 4096x4096 image or a handful of
 * full-HD ones — plenty for a viewer/IDE that shows one image at a time, while bounding a session of opening
 * many large images. The count cap bounds the map against many tiny/failed entries. */
constexpr std::size_t kBudgetBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxEntries  = 64u;

/* Upload a tightly-packed RGBA8 buffer (w*h*4 bytes) to a backend-managed texture, registered to survive
 * across frames. Mirrors SpriteSheet::from_qoi's upload block (the deliberate, reviewed ImGui-1.92 path: no
 * direct GL, so the renderer-isolation / Metal-swap decision is preserved). Returns the owned ImTextureData*
 * (caller registers ownership), or nullptr on a degenerate size. */
ImTextureData *register_rgba_texture(const unsigned char *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return nullptr;
    ImTextureData *tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(tex->GetPixels(), rgba, (std::size_t)w * (std::size_t)h * 4u);
    ImGui::RegisterUserTexture(tex);
    return tex;
}

} // namespace

OperatorTextureCache::OperatorTextureCache() : lru_(kBudgetBytes, kMaxEntries) {}

OperatorTextureCache::~OperatorTextureCache()
{
    /* CPU side only — the GPU textures were freed by the backend's shutdown sweep and the ImGui context is
     * already gone, so we must NOT call any ImGui function (no UnregisterUserTexture). See the file banner. */
    for (auto &kv : map_)
        if (kv.second.tex) IM_DELETE(kv.second.tex);
    for (ImTextureData *tex : retiring_)
        if (tex) IM_DELETE(tex);
}

void OperatorTextureCache::retire(std::uint64_t key)
{
    auto it = map_.find(key);
    if (it == map_.end()) return;
    if (it->second.tex) {
        it->second.tex->WantDestroyNextFrame = true; /* schedule the 2-frame destroy handshake (new_frame) */
        retiring_.push_back(it->second.tex);
    }
    map_.erase(it);
    lru_.remove(key);
}

void OperatorTextureCache::new_frame()
{
    /* Reproduce ImGui's per-frame texture-status transitions (imgui_draw.cpp's atlas update) for OUR user
     * textures, then reap any the backend has finished destroying. Runs before this frame's draw/Render, so
     * the backend's RenderDrawData (in end_frame) sees a consistent state. */
    for (std::size_t i = 0; i < retiring_.size();) {
        ImTextureData *tex  = retiring_[i];
        bool           reap = false;
        if (tex->WantDestroyNextFrame && tex->Status != ImTextureStatus_Destroyed &&
            tex->Status != ImTextureStatus_WantDestroy)
            tex->Status = ImTextureStatus_WantDestroy;
        /* A texture that never reached the backend (retired the same frame it was created) is destroyed here. */
        if (tex->Status == ImTextureStatus_WantDestroy && tex->TexID == ImTextureID_Invalid &&
            tex->BackendUserData == nullptr)
            tex->Status = ImTextureStatus_Destroyed;
        if (tex->Status == ImTextureStatus_Destroyed)
            reap = true; /* WantDestroyNextFrame is set -> this is OUR scheduled destroy, safe to free */
        if (tex->Status == ImTextureStatus_WantDestroy)
            tex->UnusedFrames++; /* the backend frees the GPU side once this is > 0 */
        if (reap) {
            ImGui::UnregisterUserTexture(tex); /* drop from g.UserTextures (RefCount -> 0) */
            IM_DELETE(tex);                     /* free the CPU side */
            retiring_.erase(retiring_.begin() + (std::ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

const OperatorTexture *OperatorTextureCache::get(std::uint64_t key, const unsigned char *bytes, std::size_t len,
                                                 ImageCodec codec)
{
    auto it = map_.find(key);
    if (it != map_.end()) { /* hit (positive OR cached failure) — no re-decode */
        lru_.touch(key);
        return it->second.tex ? &it->second.view : nullptr;
    }

    /* Miss: decode the untrusted bytes through the bounded, fuzzed decoders. */
    unsigned char *rgba = nullptr;
    int            w = 0, h = 0;
    const int      rc = (codec == ImageCodec::Qoi) ? hc_image_decode_qoi(bytes, len, &rgba, &w, &h)
                                                   : hc_image_decode_auto(bytes, len, &rgba, &w, &h);

    Entry e;
    if (rc == 0 && rgba && w > 0 && h > 0) {
        const std::size_t tex_bytes = (std::size_t)w * (std::size_t)h * 4u;
        for (std::uint64_t v : lru_.victims_for(tex_bytes)) retire(v); /* make room BEFORE registering */
        ImTextureData *tex = register_rgba_texture(rgba, w, h);
        if (tex) {
            e.tex   = tex;
            e.bytes = tex_bytes;
            e.view  = OperatorTexture{tex->GetTexRef(), w, h};
        }
        /* tex == nullptr (degenerate) falls through as a negative entry. */
    }
    if (rgba) hc_image_free(rgba);

    lru_.insert(key, e.bytes);
    auto ins = map_.emplace(key, std::move(e)).first;
    return ins->second.tex ? &ins->second.view : nullptr;
}

} // namespace hc::ui
