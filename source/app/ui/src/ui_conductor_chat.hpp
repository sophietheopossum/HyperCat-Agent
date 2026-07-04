#ifndef HC_UI_CONDUCTOR_CHAT_HPP
#define HC_UI_CONDUCTOR_CHAT_HPP

/* ui_conductor_chat — the Conductor chat panel (Conductor P5). INTERNAL to hc_ui.
 *
 * The operator's primary surface: a conversation with HyperCat (the front-door agent). It renders the
 * conductor's conversation (markdown for the settled assistant turns, the live streamed text below), the
 * 96x96 state avatar in the header, the 128x128 tool cue while a tool runs, and an input box that emits a
 * ConductorSay UiCommand the host routes to Conductor::say(). A pure renderer of the UiSnapshot's conductor_*
 * fields — it never touches the conductor directly.
 *
 * S2: the avatar reflects the conductor's conversational PHASE (base at rest; messagereceived loops while a
 * turn is in flight; messagesent plays once as a reply settles; taskagendadispatch loops while run_agenda
 * runs) and the tool cue shows the running tool's KIND (read / write / generic). The phase + cue classifiers
 * are PURE (offline-testable); the ConductorChatView owns the SpritePlayers (playheads — no GPU state) + the
 * previous-frame inputs for edge detection, mirroring MascotView. The sheets are BORROWED from the UiApp-owned
 * SpriteRegistry (the view owns no textures, imposes no teardown order).
 * State: also holds ONE function-static InputText buffer (the chat input — the ImGui idiom, like the other
 * panels); it is UI-thread-only and never enters the snapshot. Threading: single-threaded (UI/GL thread). */

#include "hc_ui.hpp"
#include "ui_panels.hpp" /* DrawCtx */
#include "ui_sprite.hpp" /* SpritePlayer */

#include <cstddef>
#include <string>

namespace hc::ui {

class SpriteRegistry;       /* the UiApp-owned sheet catalog the avatar + tool cue borrow from */
class OperatorTextureCache; /* A: the UiApp-owned chat-image texture cache (a dedicated instance) */

/* The conductor avatar's conversational phase (the 96x96 header sprite). */
enum class ChatAvatar { Base, Received, Sent, Dispatch };
/* The running tool's kind for the 128x128 inline cue. None = no tool running (the cue is hidden). */
enum class ToolCue { None, Read, Write, Generic };

/* The avatar-relevant slice of a frame's conductor snapshot — the pure classifier's input, so the phase logic
 * is offline-testable without a UiSnapshot or ImGui. */
struct ChatAvatarIn {
    bool        online = false;
    bool        busy = false;       /* a turn is in flight */
    bool        streaming = false;  /* streamed assistant text present this frame */
    std::string active_tool;        /* the running tool name ("" = none) */
    std::size_t assistant_msgs = 0; /* settled assistant-message count (rises when a reply lands) */
};

/* PURE: the avatar phase for this frame given the previous + current inputs. Precedence
 * Dispatch > Received (a turn in flight) > Sent (a reply just settled this frame) > Base. Offline-testable. */
ChatAvatar chat_avatar_for(const ChatAvatarIn &prev, const ChatAvatarIn &cur);
/* PURE: the tool cue for a running tool name ("" -> None; the read/write tool sets; any other tool -> Generic,
 * the owner's placeholder for a tool without dedicated art). */
ToolCue chat_tool_cue_for(const std::string &active_tool);
/* The catalog sheet / default-animation name for an avatar phase / a tool cue (the keys in sprites.manifest).
 * chat_tool_sheet(None) returns nullptr. */
const char *chat_avatar_sheet(ChatAvatar);
const char *chat_tool_sheet(ToolCue);

/* The conductor chat panel + its sprite-animation state. See the file banner for the ownership contract. */
class ConductorChatView {
public:
    ConductorChatView() = default;
    ~ConductorChatView() = default;
    ConductorChatView(const ConductorChatView &) = delete;
    ConductorChatView &operator=(const ConductorChatView &) = delete;

    /* Draw the panel for this frame from `s`; emits ConductorSay into ctx.commands; borrows sheets from
     * `sprites` (the host has ensured them loaded). `*open` follows the window close box. */
    void draw(const UiSnapshot &s, DrawCtx &ctx, const SpriteRegistry &sprites, OperatorTextureCache &tex,
              bool *open);

private:
    void retarget_avatar(ChatAvatar want, const SpriteRegistry &sprites); /* (re)point the avatar player */
    /* the conversation scrollback + the inline tool cue (split out of draw() — the scrollable content region). */
    void draw_scrollback(const UiSnapshot &s, const SpriteRegistry &sprites, OperatorTextureCache &tex, float dt);
    /* P3b: the conversations picker popup — list / switch / New / inline rename / confirm-gated delete. Emits the
     * Conductor{NewChat,ResumeChat,RenameChat,DeleteChat} commands. UI-thread-only state. */
    void draw_conversation_picker(const UiSnapshot &s, DrawCtx &ctx);

    SpritePlayer avatar_player_;
    SpritePlayer tool_player_;
    ChatAvatarIn prev_{};
    bool         primed_ = false;          /* false until the first phase is applied */
    ChatAvatar   avatar_ = ChatAvatar::Base;
    float        sent_hold_ms_ = 0.0f;     /* remaining play time of the one-shot messagesent flourish */
    ToolCue      cue_ = ToolCue::None;
    /* P3b conversations picker: at most one inline rename + one delete-confirm open at a time (UI-thread-only). */
    std::string  rename_id_;          /* the conversation being renamed inline ("" = none) */
    char         rename_buf_[64] = "";
    std::string  confirm_del_id_;     /* the conversation pending a delete confirm ("" = none) */
    /* The chat input box (multiline, auto-grown 1..4 lines, expands upward). All UI-thread-only; never enters the
     * snapshot. The CharFilter callback sets pending_send_ on Enter-without-Shift (Shift+Enter inserts a newline). */
    std::string  input_;              /* the operator's in-progress message (grown via a resize callback) */
    float        input_height_ = 0.0f;/* this frame's input box height — drives the scrollback footer reservation */
    bool         pending_send_ = false;/* an Enter (no Shift) or the Send button asked to send this frame */
    bool         clear_input_ = false;/* a send asked to wipe the box — done in the InputText callback (active state) */
    bool         refocus_ = false;    /* refocus the input on the frame after a send */
    bool         want_scroll_bottom_ = false; /* C: a jump-to-latest click asks to pin to the bottom next frame */
};

} // namespace hc::ui

#endif /* HC_UI_CONDUCTOR_CHAT_HPP */
