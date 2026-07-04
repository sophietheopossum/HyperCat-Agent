/* ui_conductor_chat — the Conductor chat panel (see ui_conductor_chat.hpp). INTERNAL. */

#include "ui_conductor_chat.hpp"

#include "ui_copy.hpp"            /* copy_block_menu / copy_region_menu — right-click to copy a turn */
#include "ui_markdown.hpp"        /* render_markdown_cached — assistant turns are markdown */
#include "ui_sprite_registry.hpp" /* SpriteRegistry — the borrowed sheet catalog */
#include "ui_texcache.hpp"        /* A: OperatorTextureCache / ImageCodec — inline operator-attached images */
#include "ui_theme.hpp"           /* accent_v4 / muted_v4 / WrappedText */

#include "imgui.h"

#include <cfloat> /* FLT_MIN — the "fill width" sentinel for the input box */
#include <cstdio> /* snprintf — the HH:MM timestamp label (D) */
#include <ctime>  /* localtime_r — render a settled-turn timestamp in local time (D) */

namespace hc::ui {

/* A: a per-frame budget on DECODED inline-image bytes drawn in the chat scrollback. Kept under the dedicated chat
 * texture cache's budget so this panel's drawn set always fits — a later get() then only ever evicts a PRIOR-frame
 * texture, never one already in this frame's draw list (the cache's single-consumer caveat, ui_texcache.hpp). */
constexpr std::size_t kChatFrameImgBudget = 48u * 1024u * 1024u;

/* ---- pure phase / cue classifiers (the offline-testable surface) ----------------------------------- */

ChatAvatar chat_avatar_for(const ChatAvatarIn &prev, const ChatAvatarIn &cur)
{
    if (cur.active_tool == "run_agenda") return ChatAvatar::Dispatch; /* dispatching the fleet — top phase */
    if (cur.busy || cur.streaming) return ChatAvatar::Received;       /* a turn in flight — "processing"  */
    if (cur.assistant_msgs > prev.assistant_msgs) return ChatAvatar::Sent; /* a reply just settled         */
    return ChatAvatar::Base;                                          /* at rest                          */
}

ToolCue chat_tool_cue_for(const std::string &t)
{
    if (t.empty()) return ToolCue::None;
    if (t == "recall_memory" || t == "agenda_status" || t == "plan_goal" || t == "deep_reason")
        return ToolCue::Read; /* read / explore */
    if (t == "write_memory") return ToolCue::Write; /* the one durable write */
    return ToolCue::Generic;  /* run_agenda / set_goal / steer_or_cancel / ask_user / unknown — the placeholder */
}

const char *chat_avatar_sheet(ChatAvatar a)
{
    switch (a) {
        case ChatAvatar::Received: return "conductor.messagereceived";
        case ChatAvatar::Sent:     return "conductor.messagesent";
        case ChatAvatar::Dispatch: return "conductor.taskagendadispatch";
        case ChatAvatar::Base:     break;
    }
    return "conductor.base";
}

const char *chat_tool_sheet(ToolCue c)
{
    switch (c) {
        case ToolCue::Read:    return "conductor.tool.read";
        case ToolCue::Write:   return "conductor.tool.write";
        case ToolCue::Generic: return "conductor.tool.generic";
        case ToolCue::None:    break;
    }
    return nullptr;
}

/* ---- the view ------------------------------------------------------------------------------------- */

void ConductorChatView::retarget_avatar(ChatAvatar want, const SpriteRegistry &sprites)
{
    avatar_ = want;
    avatar_player_.set_animation(sprites.animation(chat_avatar_sheet(want)));
}

namespace {

/* Extract the avatar-relevant slice of the snapshot (the pure classifier input). */
ChatAvatarIn avatar_input(const UiSnapshot &s)
{
    ChatAvatarIn in;
    in.online = s.conductor_online;
    in.busy = s.conductor_busy;
    in.streaming = !s.conductor_streaming.empty();
    in.active_tool = s.conductor_active_tool;
    for (const auto &m : s.conductor_chat)
        if (m.role == "assistant") in.assistant_msgs++;
    return in;
}

const char *avatar_status_word(ChatAvatar a, bool online)
{
    if (!online) return "offline";
    switch (a) {
        case ChatAvatar::Received: return "thinking";
        case ChatAvatar::Sent:     return "replied";
        case ChatAvatar::Dispatch: return "dispatching";
        case ChatAvatar::Base:     break;
    }
    return "idle";
}

/* The multiline-input callback state + handler. ImGui's InputText callback is a C function pointer, so the view's
 * input string + the send flag ride through UserData. One callback serves two flags:
 *   - CallbackResize  : grow the std::string in place (the imgui_stdlib idiom — text isn't capped by a fixed buffer).
 *   - CallbackCharFilter : the Enter key feeds the filter a '\n'. Enter WITHOUT Shift means SEND, so discard the
 *     newline (return 1) and raise the send flag; Shift+Enter falls through (return 0) and inserts a real newline. */
struct ChatInputState {
    std::string *str;
    bool        *send;
    bool        *clear;
};

int chat_input_cb(ImGuiInputTextCallbackData *data)
{
    auto *st = static_cast<ChatInputState *>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        st->str->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = st->str->data();
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (data->EventChar == '\n' && !ImGui::GetIO().KeyShift) {
            *st->send = true;
            return 1; /* discard the newline — Enter sends, it does not insert a line */
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        /* a send last frame asked to clear the box: an ACTIVE InputText owns its edit state and ignores an outside
         * buffer clear, so wipe it HERE where DeleteChars drives that internal state (then sync back to the string). */
        if (*st->clear) {
            data->DeleteChars(0, data->BufTextLen);
            *st->clear = false;
        }
    }
    return 0;
}

} // namespace

void ConductorChatView::draw_scrollback(const UiSnapshot &s, const SpriteRegistry &sprites,
                                        OperatorTextureCache &tex, float dt)
{
    /* the scrollback — reserve the (variable) input box height + the hint/Send row below it (+ the staged-
     * attachment chip row when present), so as the multiline input grows 1..4 lines the scrollback shrinks and the
     * input's top edge rises (it "expands upward"). */
    const float chips_h = s.conductor_staged.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing();
    const float footer =
        input_height_ + ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() + chips_h;
    if (ImGui::BeginChild("conductor_scroll", ImVec2(0, -footer), 0, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (s.conductor_chat.empty() && s.conductor_streaming.empty() && !s.conductor_busy)
            ImGui::TextColored(muted_v4(), "say something to HyperCat\xE2\x80\xA6"); /* ellipsis */

        /* one Copy-all string for the whole exchange (role-prefixed), built once per frame (a frame-local,
         * caller-owned temporary consumed synchronously by the copy menus below); each turn is also grouped so a
         * right-click offers Copy (just that message) / Copy all. The full pre-pass is needed because an early
         * turn's "Copy all" must see the COMPLETE string; at a very long turn count it could be memoized / built
         * lazily inside the popup, but it's well under the rendering cost at realistic sizes. */
        std::string all;
        for (const auto &m : s.conductor_chat) {
            all += (m.role == "assistant") ? "HyperCat: " : "you: ";
            all += m.text;
            all += "\n\n";
        }
        std::size_t frame_img_bytes = 0; /* A: decoded image bytes drawn THIS frame (caps the chat texcache load) */
        int         idx = 0;
        for (const auto &m : s.conductor_chat) {
            ImGui::PushID(idx++);
            ImGui::BeginGroup();
            const bool assistant = (m.role == "assistant");
            ImGui::TextColored(assistant ? accent_v4() : muted_v4(), "%s", assistant ? "HyperCat" : "you");
            if (m.at_ms != 0) { /* D: a muted local HH:MM beside the name (0 = a resumed turn with no stored time) */
                const std::time_t t = (std::time_t)(m.at_ms / 1000u);
                std::tm            tmv{};
                char               ts[8] = "";
                if (::localtime_r(&t, &tmv)) std::snprintf(ts, sizeof ts, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
                if (ts[0]) {
                    ImGui::SameLine();
                    ImGui::TextColored(muted_v4(), "%s", ts);
                }
            }
            if (assistant)
                render_markdown_cached(m.text); /* the FINISHED assistant turn, formatted */
            else
                WrappedText("%s", m.text.c_str()); /* untrusted operator text, verbatim */
            /* A: operator-attached images, shown inline (the text-only conductor never sees them). The per-frame
             * byte budget keeps this panel's DRAWN set within the dedicated chat cache, so a later get() only ever
             * evicts a PRIOR-frame texture — never one already in this frame's draw list. */
            for (const auto &att : m.attachments) {
                if (!att.bytes) continue;
                const ImageCodec       codec = att.qoi ? ImageCodec::Qoi : ImageCodec::Auto;
                const OperatorTexture *t =
                    tex.get(att.hash, (const unsigned char *)att.bytes->data(), att.bytes->size(), codec);
                if (!t) {
                    ImGui::TextColored(muted_v4(), "[image: %s \xE2\x80\x94 could not decode]", att.name.c_str());
                    continue;
                }
                const std::size_t b = (std::size_t)t->w * (std::size_t)t->h * 4u;
                if (frame_img_bytes + b > kChatFrameImgBudget) {
                    ImGui::TextColored(muted_v4(), "[image: %s \xE2\x80\x94 too large to preview here]",
                                       att.name.c_str());
                    continue; /* cached but deliberately NOT drawn -> safe for a later get() to evict */
                }
                frame_img_bytes += b;
                const float maxw  = 360.0f;
                const float scale = (t->w > maxw) ? maxw / (float)t->w : 1.0f;
                ImGui::Image(t->tex_ref, ImVec2((float)t->w * scale, (float)t->h * scale));
                ImGui::TextColored(muted_v4(), "%s", att.name.c_str());
            }
            ImGui::EndGroup();
            copy_block_menu("chat_msg", m.text, all); /* right-click a turn -> Copy / Copy all */
            ImGui::PopID();
            ImGui::Spacing();
        }

        /* the in-progress assistant text (raw — markdown only once the turn settles), or a thinking cue */
        if (!s.conductor_streaming.empty()) {
            ImGui::TextColored(accent_v4(), "HyperCat");
            WrappedText("%s", s.conductor_streaming.c_str());
        } else if (s.conductor_busy) {
            ImGui::TextColored(muted_v4(), "HyperCat is thinking\xE2\x80\xA6");
        }

        /* the inline tool cue — the 128x128 art (drawn compact) + the tool name, while a tool runs. */
        const ToolCue want_cue = chat_tool_cue_for(s.conductor_active_tool);
        if (want_cue != cue_) {
            cue_ = want_cue;
            if (const char *cs = chat_tool_sheet(cue_)) tool_player_.set_animation(sprites.animation(cs));
        }
        if (cue_ != ToolCue::None) {
            tool_player_.advance(dt);
            const SpriteSheet *tsheet = sprites.sheet(chat_tool_sheet(cue_));
            const float        kCue = 128.0f; /* the art's NATIVE size — pixel-perfect (no fractional scale) */
            if (tsheet) {
                tool_player_.draw(*tsheet, ImVec2(kCue, kCue));
                ImGui::SameLine();
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(muted_v4(), "running %s\xE2\x80\xA6",
                               s.conductor_active_tool.c_str()); /* untrusted tool name via "%s" */
        }

        copy_region_menu("chat_copy", all, true); /* right-click empty space -> Copy all (settled turns) */

        /* C: follow the tail while pinned to the bottom; once the operator scrolls up, stop following and offer a
         * jump-to-latest button instead (honored next frame, when the scroll metrics have settled). */
        const float maxy = ImGui::GetScrollMaxY();
        if (want_scroll_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            want_scroll_bottom_ = false;
        } else if (ImGui::GetScrollY() >= maxy - 1.0f) {
            ImGui::SetScrollHereY(1.0f); /* still at the bottom — follow the tail */
        } else if (maxy > 0.0f) {
            /* pin a small affordance to the visible bottom-right of the scroll child (screen-space, so it does not
             * scroll with the content); a click requests the jump on the next frame. */
            const char  *lbl  = "Jump to latest \xE2\x86\x93"; /* down arrow */
            const ImVec2 lsz  = ImGui::CalcTextSize(lbl);
            const float  bw   = lsz.x + ImGui::GetStyle().FramePadding.x * 2.0f;
            const float  bh   = lsz.y + ImGui::GetStyle().FramePadding.y * 2.0f;
            const float  gap  = 8.0f + ImGui::GetStyle().ScrollbarSize;
            const ImVec2 wpos = ImGui::GetWindowPos();
            const ImVec2 wsz  = ImGui::GetWindowSize();
            ImGui::SetCursorScreenPos(ImVec2(wpos.x + wsz.x - bw - gap, wpos.y + wsz.y - bh - 8.0f));
            if (ImGui::Button(lbl)) want_scroll_bottom_ = true;
        }
    }
    ImGui::EndChild();
}

/* P3b: the conversations picker — a header popup over the dedicated conductor store's conversation list (filled in
 * the snapshot). Click a row to SWITCH, "+ New chat" to start one, inline RENAME, confirm-gated DELETE (the ACTIVE
 * conversation can't be deleted — the host refuses it too). Untrusted titles render via a snprintf("%s")-built
 * label (ImGui treats it as a literal, not a format string). At most one inline rename / delete-confirm at a time. */
void ConductorChatView::draw_conversation_picker(const UiSnapshot &s, DrawCtx &ctx)
{
    char btn[80];
    snprintf(btn, sizeof btn, "Conversations (%zu)", s.conductor_conversations.size());
    if (ImGui::Button(btn)) ImGui::OpenPopup("##convpicker");
    if (!ImGui::BeginPopup("##convpicker")) return;

    if (ImGui::Selectable("+ New chat")) { /* default Selectable closes the popup */
        ctx.commands.push_back({UiCommand::Kind::ConductorNewChat, "", "", 0, {}});
        rename_id_.clear();
        confirm_del_id_.clear();
    }
    ImGui::Separator();
    if (s.conductor_conversations.empty()) ImGui::TextColored(muted_v4(), "no saved conversations yet");

    for (const auto &cv : s.conductor_conversations) {
        ImGui::PushID(cv.id.c_str());
        const bool active = (cv.id == s.conductor_current_session_id);
        if (rename_id_ == cv.id) { /* inline rename editor */
            ImGui::SetNextItemWidth(200.0f);
            const bool enter =
                ImGui::InputText("##rn", rename_buf_, sizeof rename_buf_, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::SmallButton("save") || enter) && rename_buf_[0]) {
                ctx.commands.push_back({UiCommand::Kind::ConductorRenameChat, cv.id, rename_buf_, 0, {}});
                rename_id_.clear();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("cancel")) rename_id_.clear();
        } else if (confirm_del_id_ == cv.id) { /* delete confirm */
            ImGui::TextColored(muted_v4(), "delete?");
            ImGui::SameLine();
            if (ImGui::SmallButton("yes")) {
                ctx.commands.push_back({UiCommand::Kind::ConductorDeleteChat, cv.id, "", 0, {}});
                confirm_del_id_.clear();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("no")) confirm_del_id_.clear();
        } else { /* the conversation row: title + turn count; click to switch (default Selectable closes the popup) */
            char row[220];
            snprintf(row, sizeof row, "%s%s  (%d turns)", active ? "* " : "", cv.title.c_str(), cv.turns);
            /* AllowOverlap so the inline rename/del SmallButtons (submitted after, on the same line) win the
             * hit-test instead of being swallowed by this full-width Selectable — without it the buttons are dead. */
            if (ImGui::Selectable(row, active, ImGuiSelectableFlags_AllowOverlap) && !active)
                ctx.commands.push_back({UiCommand::Kind::ConductorResumeChat, cv.id, "", 0, {}});
            ImGui::SameLine();
            if (ImGui::SmallButton("rename")) {
                rename_id_ = cv.id;
                snprintf(rename_buf_, sizeof rename_buf_, "%s", cv.title.c_str());
                confirm_del_id_.clear();
            }
            if (!active) {
                ImGui::SameLine();
                if (ImGui::SmallButton("del")) {
                    confirm_del_id_ = cv.id;
                    rename_id_.clear();
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::EndPopup();
}

void ConductorChatView::draw(const UiSnapshot &s, DrawCtx &ctx, const SpriteRegistry &sprites,
                             OperatorTextureCache &tex, bool *open)
{
    if (!ImGui::Begin("Conductor", open)) {
        ImGui::End();
        return;
    }

    const float       dt = ImGui::GetIO().DeltaTime * 1000.0f;
    const ChatAvatarIn cur = avatar_input(s);

    /* --- advance the avatar phase state machine (mirrors MascotView's "re-point on change", plus a one-shot
     *     hold so the messagesent flourish plays to completion before falling back to base). --- */
    const ChatAvatar tgt = chat_avatar_for(prev_, cur);
    if (sent_hold_ms_ > 0.0f) sent_hold_ms_ -= dt;
    if (tgt == ChatAvatar::Sent) {
        retarget_avatar(ChatAvatar::Sent, sprites); /* a reply just settled -> start the one-shot */
        sent_hold_ms_ = sprites.animation(chat_avatar_sheet(ChatAvatar::Sent)).total_ms();
    } else if (tgt == ChatAvatar::Dispatch || tgt == ChatAvatar::Received) {
        sent_hold_ms_ = 0.0f; /* a live phase overrides a lingering flourish */
        if (!primed_ || avatar_ != tgt) retarget_avatar(tgt, sprites);
    } else { /* Base — but let an in-flight messagesent finish first */
        if (sent_hold_ms_ <= 0.0f && (!primed_ || avatar_ != ChatAvatar::Base))
            retarget_avatar(ChatAvatar::Base, sprites);
    }
    primed_ = true;
    prev_ = cur;
    avatar_player_.advance(dt);

    /* --- the header: the avatar + name + status. The sheet may be null (decode failed / not loaded) -> the
     *     name line still renders (graceful degradation, never a crash). Drawn at the art's NATIVE 96px so the
     *     NEAREST sampler maps 1:1 (any fractional scale drops pixels unevenly — pixel art must render native). */
    const float        kAvatar = 96.0f;
    const SpriteSheet *asheet = sprites.sheet(chat_avatar_sheet(avatar_));
    if (asheet) {
        avatar_player_.draw(*asheet, ImVec2(kAvatar, kAvatar));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextColored(accent_v4(), "HyperCat");
    ImGui::TextColored(muted_v4(), "%s", avatar_status_word(avatar_, s.conductor_online));
    ImGui::EndGroup();
    if (!s.conductor_online)
        ImGui::TextColored(muted_v4(), "offline — set a model + key to bring the conductor up.");
    else if (!s.conductor_notice.empty())
        ImGui::TextColored(muted_v4(), "%s", s.conductor_notice.c_str()); /* budget / resume notice */
    if (s.conductor_online) draw_conversation_picker(s, ctx); /* P3b: the conversations picker */
    ImGui::Separator();

    /* size the input box to its content: 1..4 visible lines, then it scrolls. Computed BEFORE the scrollback so its
     * footer reservation matches this frame's box height (the source of the grow-upward behaviour). */
    {
        int nl = 1;
        for (char c : input_)
            if (c == '\n') nl++;
        const int shown = nl < 1 ? 1 : (nl > 4 ? 4 : nl);
        /* WithSpacing matches InputTextMultiline's internal per-line advance, so `shown` lines fit without an inner
         * scroll for <=4 lines and the footer reservation below stays consistent with the box's real height. */
        input_height_ = shown * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.0f;
    }

    draw_scrollback(s, sprites, tex, dt);

    /* A: the staged-attachment tray — chips for files queued for the NEXT message; the leading ✕ removes one. A
     * single horizontally-scrolling row whose height is reserved in draw_scrollback's footer. */
    if (!s.conductor_staged.empty()) {
        ImGui::BeginChild("conductor_staged", ImVec2(0, ImGui::GetFrameHeight()), 0,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (size_t i = 0; i < s.conductor_staged.size(); i++) {
            if (i) ImGui::SameLine(0.0f, 14.0f);
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("\xC3\x97")) /* multiplication sign -> unstage */
                ctx.commands.push_back({UiCommand::Kind::ConductorUnstage, "", "", (int)i, {}});
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::AlignTextToFramePadding();
            const auto &chip = s.conductor_staged[i];
            ImGui::TextColored(muted_v4(), "%s%s", chip.is_image ? "image: " : "", chip.name.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    /* the input — a multiline box (1..4 lines, grows upward). Enter sends a ConductorSay the host routes to
     * Conductor::say(); Shift+Enter inserts a newline (see chat_input_cb). */
    const bool disabled = !s.conductor_online;
    if (disabled) ImGui::BeginDisabled();
    if (refocus_) { /* a send last frame asked to put the cursor back in the box */
        ImGui::SetKeyboardFocusHere();
        refocus_ = false;
    }
    ChatInputState ist{&input_, &pending_send_, &clear_input_};
    ImGui::InputTextMultiline("##conductorsay", input_.data(), input_.capacity() + 1,
                              ImVec2(-FLT_MIN, input_height_),
                              ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter |
                                  ImGuiInputTextFlags_CallbackAlways,
                              chat_input_cb, &ist);
    /* the send hint + an explicit Send (the deliberate path alongside Enter) */
    ImGui::TextColored(muted_v4(), "Enter to send \xC2\xB7 Shift+Enter for a new line");
    ImGui::SameLine();
    {
        const float sendw   = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float stopw   = ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        /* right-align: [Stop] [Send] while a turn streams (E — Stop interrupts it without ending the session), else
         * just [Send]. Stop shows only when busy, and busy implies online, so the disabled wrapper never hides it. */
        const float need  = s.conductor_busy ? stopw + spacing + sendw : sendw;
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > need) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - need); /* right-align the button(s) */
        if (s.conductor_busy) {
            if (ImGui::SmallButton("Stop"))
                ctx.commands.push_back({UiCommand::Kind::ConductorStopTurn, "", "", 0, {}});
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Send")) pending_send_ = true;
    }
    if (disabled) ImGui::EndDisabled();

    /* dispatch a send (Enter or the Send button); guard empty; refocus the box next frame */
    if (pending_send_) {
        pending_send_ = false;
        if (!disabled && (!input_.empty() || !s.conductor_staged.empty())) {
            ctx.commands.push_back({UiCommand::Kind::ConductorSay, input_, "", 0, {}});
            input_.clear();      /* clears the box for the inactive (Send-button) case */
            clear_input_ = true; /* and wipe the ACTIVE edit state via the callback (the Enter case) */
            refocus_ = true;     /* keep/reclaim focus so the callback fires + the box is ready for the next line */
        }
    }

    ImGui::End();
}

} // namespace hc::ui
