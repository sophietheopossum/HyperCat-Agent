#ifndef HC_UI_TEXCACHE_HPP
#define HC_UI_TEXCACHE_HPP

/* ui_texcache — the OperatorTextureCache: decoded RGBA8 of OPERATOR-opened on-disk images, uploaded to
 * backend-managed GPU textures and cached LRU under a byte+count budget. INTERNAL to hc_ui (W4 P4.2).
 *
 * WHY this exists separately from SpriteRegistry/SpriteSheet: the sprite layer holds a fixed catalog of
 * TRUSTED bundled assets for the whole app lifetime, freed once at shutdown. The media Viewer (and later the
 * IDE) instead shows a CHANGING set of UNTRUSTED operator images — open one, open another, open a third — so
 * its textures must be created AND destroyed MID-RUN, bounded so a session of opening big images can't grow
 * GPU memory without limit. That mid-run destroy is the one subtle thing here (see the .cpp banner): ImGui
 * 1.92 frees a user texture's GPU side only via the renderer backend across a 2-frame handshake, so the cache
 * drives that handshake itself in new_frame().
 *
 * Two pieces, smallest-responsibility first:
 *   - LruBudget   : PURE bookkeeping (keys + byte costs + access order under a byte/count budget). No GL, no
 *                   ImGui — the offline-testable policy core (mirrors how ui_sprite's uv math is pure).
 *   - OperatorTextureCache : decodes (hc_image), uploads via the ImGui-backend texture path, caches by a
 *                   CONTENT key, evicts LRU using LruBudget, and runs the deferred-destroy lifecycle.
 *
 * Ownership / teardown (the ui_sprite contract, same reasoning): each cached texture is an ImTextureData
 * registered with the ImGui context (survives across frames). Its GPU side is freed either (a) mid-run by the
 * backend after the cache schedules WantDestroy, or (b) at shutdown by the backend's DestroyDeviceObjects
 * sweep. THEREFORE ~OperatorTextureCache (which runs AFTER the Backend is deleted — UiApp::Impl guarantees the
 * order) frees ONLY the CPU side (IM_DELETE) and calls NO ImGui function (the context is gone). Non-copyable.
 * Threading: single-threaded — UI/GL thread only; new_frame()/get() need a live ImGui context. */

#include "imgui.h" /* ImTextureRef — the handle handed back for ImGui::Image */

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace hc::ui {

/* PURE LRU-by-bytes bookkeeping (no GL/ImGui). Tracks a set of uint64 keys, each with a byte cost, in
 * most-recently-used order, under a fixed byte budget AND a max entry count. The texture cache asks it which
 * keys to evict before inserting a new one; isolated here so the eviction policy + byte math are unit-tested
 * without a GPU. Soft budget: a single entry larger than the whole budget is still admitted (it becomes the
 * lone resident) so an over-cap image still displays — victims_for then returns "everything evictable". */
class LruBudget {
public:
    LruBudget(std::size_t budget_bytes, std::size_t max_count);

    bool        contains(std::uint64_t key) const { return idx_.find(key) != idx_.end(); }
    void        touch(std::uint64_t key);                       /* move an existing key to MRU; no-op if absent */
    void        insert(std::uint64_t key, std::size_t bytes);   /* add a NEW key at MRU (caller ensures absent) */
    std::size_t remove(std::uint64_t key);                      /* drop a key; returns its bytes (0 if absent)  */

    /* The LRU-first list of keys to evict so that, after also inserting one new entry of `incoming` bytes, the
     * cache holds <= budget bytes AND <= max_count entries. Pure (does not mutate) — the caller reaps each
     * victim's texture then calls remove(). The about-to-insert key is NOT yet present, so it can never be a
     * victim. Stops once the constraints are met or nothing evictable remains. */
    std::vector<std::uint64_t> victims_for(std::size_t incoming) const;

    std::size_t bytes() const { return bytes_; }
    std::size_t count() const { return order_.size(); }

private:
    std::size_t         budget_;
    std::size_t         max_count_;
    std::size_t         bytes_ = 0;
    std::list<std::uint64_t> order_; /* front = MRU, back = LRU */
    std::unordered_map<std::uint64_t, std::pair<std::list<std::uint64_t>::iterator, std::size_t>> idx_;
};

/* Which decoder a get() call should use for the bytes — chosen by the caller from the already-classified
 * DocKind, so the cache stays decoupled from ui_docview's type. */
enum class ImageCodec { Auto, Qoi }; /* Auto = PNG/JPEG via hc_image_decode_auto; Qoi = hc_image_decode_qoi */

/* A borrowed view of a cached texture, valid for the CURRENT frame only (the next get()/new_frame() may evict
 * it). The caller passes tex_ref to ImGui::Image and uses w/h to lay it out. */
struct OperatorTexture {
    ImTextureRef tex_ref;
    int          w = 0;
    int          h = 0;
};

class OperatorTextureCache {
public:
    OperatorTextureCache();
    ~OperatorTextureCache(); /* CPU-side free only (post-backend) — see the .cpp banner. No ImGui calls. */

    OperatorTextureCache(const OperatorTextureCache &)            = delete;
    OperatorTextureCache &operator=(const OperatorTextureCache &) = delete;

    /* Drive the deferred-destroy lifecycle for retiring textures. Call ONCE per frame at the TOP (after
     * NewFrame, before drawing), mirroring ImGui's own per-frame atlas texture update but for user textures.
     * Needs a live ImGui context (it may UnregisterUserTexture + free reaped entries). */
    void new_frame();

    /* CONTENT-ADDRESSED decode+cache: `key` is a content fingerprint (the host's open_hash) so identical bytes
     * hit and changed bytes miss+re-decode. On a hit returns the cached texture with no decode. On a miss
     * decodes `bytes` via `codec`, uploads, evicts LRU to stay within budget, and caches. Returns a borrowed
     * OperatorTexture valid THIS frame, or nullptr on a decode/upload failure (the result — including the
     * failure — is cached so it is not retried every frame).
     *
     * SINGLE-CONSUMER-PER-FRAME contract: this cache is designed for ONE get() per frame (the Viewer shows one
     * opened file). A miss may evict + retire LRU textures, which sets WantDestroyNextFrame on them — so a
     * caller MUST NOT, within the same frame, ImGui::Image() a texture returned by an earlier get() and THEN
     * call get() again with a key whose miss could evict that just-drawn texture (that would mark an in-draw-
     * list texture for destroy, tripping ImGui's PushTexture assertion). Each CONSUMER therefore gets its OWN
     * instance (the Viewer and the conductor chat panel each own one), so the invariant holds per instance. The
     * chat panel, which draws several attachment images per frame, additionally caps its per-frame DECODED bytes
     * (kChatFrameImgBudget) BELOW this budget so its drawn set always fits — evictions then only ever retire
     * PRIOR-frame textures (the budget acts as the "pin"). A second consumer of the SAME instance must pin its
     * prior result or defer eviction. */
    const OperatorTexture *get(std::uint64_t key, const unsigned char *bytes, std::size_t len, ImageCodec codec);

    std::size_t bytes_used() const { return lru_.bytes(); }
    std::size_t live_count() const { return map_.size(); }

private:
    struct Entry {
        struct ImTextureData *tex = nullptr; /* null => decode/upload failed (a negative cache entry) */
        OperatorTexture       view;          /* stable storage so get() can return &entry.view        */
        std::size_t           bytes = 0;     /* w*h*4 charged to the budget (0 for a failed entry)     */
    };

    void retire(std::uint64_t key); /* move a cached texture into retiring_ + schedule its destroy */

    std::unordered_map<std::uint64_t, Entry> map_;
    LruBudget                                lru_;
    std::vector<struct ImTextureData *>      retiring_; /* textures mid-handshake toward GPU+CPU free */
};

} // namespace hc::ui

#endif /* HC_UI_TEXCACHE_HPP */
