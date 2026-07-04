/* ui_panels_files — the interactive workspace FILE BROWSER (W4 P4.1, redesigned as a desktop-style split
 * explorer). One responsibility: let the operator browse + create / open / rename / delete files in the
 * per-context sandbox jail. A resizable two-pane layout — a folder TREE on the left, a sortable CONTENTS table
 * (Name / Size / Kind) on the right — with a toolbar (Up + breadcrumb + New), right-click context menus, and
 * modal rename/delete/new dialogs. Split out of ui_panels_content.cpp (which keeps the read-only Viewer + the
 * IDE Editor) when the browser grew past the cluster smell-test.
 *
 * PURE RENDERER: it reads the UiSnapshot (the host lists the jail recursively into `s.files`, full ws-relative
 * paths, ~2 Hz) and EMITS UiCommands (Create/Delete/Rename/OpenFile) the host dispatches through the jailed
 * sandbox — it never touches the filesystem itself. Opening a file routes by kind (Phase C.2): AUDIO -> the
 * Music Player (an AudioPlay command + `ctx.show.music_player`), everything else -> the Viewer (`ctx.show.viewer`)
 * or, via the context menu, the Editor — the same DrawCtx side-effect the task-detail selector uses.
 * Navigation (the selected folder, tree expansion, row
 * selection) is purely local UI state derived each frame from `s.files`; no host round-trip and no new command.
 * The operator IS the human-in-the-loop, so the create/rename/delete commands execute host-side directly (still
 * jailed); a destructive delete is confirm-gated by a modal. Single-threaded — UI/GL thread only; the
 * function-static state is the established single-instance panel idiom (cf. draw_agenda_builder_panel). */

#include "ui_panels.hpp"  /* DrawCtx, PanelVis, draw_files_panel decl */
#include "ui_docview.hpp" /* kind_label_from_name — the Kind column */
#include "ui_theme.hpp"   /* PanelHeader, muted_v4, accent_v4, err_v4 */

#include "hc_ui.hpp"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hc::ui {

namespace {

/* --- pure path helpers (full ws-relative paths; '/' separated; "" = the workspace root) --- */

std::string parent_of(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
std::string leaf_of(const std::string &p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
std::string join_path(const std::string &dir, const std::string &leaf)
{
    return dir.empty() ? leaf : dir + "/" + leaf;
}

/* Case-insensitive ASCII compare (for stable name ordering). */
int ci_cmp(const std::string &a, const std::string &b)
{
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}
bool ci_less(const std::string &a, const std::string &b) { return ci_cmp(a, b) < 0; }

/* Decode a %XX-escaped leaf for DISPLAY ONLY (e.g. the agent workspaces are named `agent%3AA` on disk →
 * shown as `agent:A`). Only printable, non-DEL bytes are decoded (never inject a control byte into the label);
 * an incomplete/invalid escape is passed through verbatim. The raw path is always used for commands. */
int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string display_name(const std::string &leaf)
{
    std::string out;
    out.reserve(leaf.size());
    for (size_t i = 0; i < leaf.size(); ++i) {
        if (leaf[i] == '%' && i + 2 < leaf.size()) {
            int hi = hexval(leaf[i + 1]), lo = hexval(leaf[i + 2]);
            if (hi >= 0 && lo >= 0) {
                unsigned char c = (unsigned char)(hi * 16 + lo);
                if (c >= 0x20 && c != 0x7f) {
                    out += (char)c;
                    i += 2;
                    continue;
                }
            }
        }
        out += leaf[i];
    }
    return out;
}

std::string human_size(long b)
{
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof buf, "%ld B", b);
    else if (b < 1024L * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", (double)b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", (double)b / (1024.0 * 1024.0));
    return std::string(buf);
}

/* --- the tree model, built each frame from the flat path list --- */

struct FileNode {
    std::string path; /* full ws-relative path (for commands) */
    std::string leaf; /* the last component (for display)     */
    long        size = 0;
    bool        is_dir = false;
};
struct FileTree {
    std::map<std::string, std::vector<FileNode>> children; /* parent dir -> its direct children */
    std::set<std::string>                        dirs;     /* every directory path              */
    bool is_dir(const std::string &p) const { return p.empty() || dirs.count(p) != 0; }
};
FileTree build_tree(const std::vector<FileEntry> &files)
{
    FileTree t;
    for (const auto &e : files) {
        t.children[parent_of(e.path)].push_back({e.path, leaf_of(e.path), e.size, e.is_dir});
        if (e.is_dir) t.dirs.insert(e.path);
    }
    return t;
}

/* A deferred CRUD intent the contents pane / toolbar raises; draw_files_panel turns it into a modal. */
struct PendingFileAction {
    enum Kind { None, Rename, Delete, NewFile, NewFolder } kind = None;
    std::string target;              /* rename/delete target, OR the parent dir for a new entry */
    bool        target_is_dir = false;
};

/* Right-align a short label within the current table cell. */
void right_text(const char *s, ImVec4 col)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float tw = ImGui::CalcTextSize(s).x;
    if (tw < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - tw));
    ImGui::TextColored(col, "%s", s);
}

/* The LEFT pane: a recursive folder tree. Clicking a node selects it as the current dir (drives the contents
 * pane); expansion is tracked by ImGui's per-id storage (no manual set). Folders only — files live on the right. */
void draw_tree_node(const FileTree &tree, const std::string &dir, const std::string &label,
                    std::string &selected_dir)
{
    std::vector<const FileNode *> subdirs;
    auto                          it = tree.children.find(dir);
    if (it != tree.children.end())
        for (const auto &n : it->second)
            if (n.is_dir) subdirs.push_back(&n);
    std::sort(subdirs.begin(), subdirs.end(),
              [](const FileNode *a, const FileNode *b) { return ci_less(a->leaf, b->leaf); });

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected_dir == dir) flags |= ImGuiTreeNodeFlags_Selected;
    if (dir.empty()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    bool leaf = subdirs.empty();
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    bool open = ImGui::TreeNodeEx(dir.empty() ? "##root" : dir.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_dir = dir;
    if (!leaf && open) {
        for (const FileNode *n : subdirs)
            draw_tree_node(tree, n->path, display_name(n->leaf), selected_dir);
        ImGui::TreePop();
    }
}

/* dirs first (desktop convention, independent of direction), then the active sort column, name as the tiebreak. */
bool node_less(const FileNode &a, const FileNode &b, int col, bool asc)
{
    if (a.is_dir != b.is_dir) return a.is_dir;
    int c = 0;
    if (col == 1) c = (a.size < b.size) ? -1 : (a.size > b.size ? 1 : 0);
    else if (col == 2)
        c = std::strcmp(a.is_dir ? "folder" : kind_label_from_name(a.leaf),
                        b.is_dir ? "folder" : kind_label_from_name(b.leaf));
    else c = ci_cmp(a.leaf, b.leaf);
    if (c == 0) c = ci_cmp(a.leaf, b.leaf);
    return asc ? c < 0 : c > 0;
}

/* Route an opened file by kind (Phase C.2): audio -> the Music Player (play from the WORKSPACE jail); everything
 * else -> the Viewer. EXTENSION-based (no bytes available at routing time) — so a MISNAMED audio file (e.g. a
 * `.dat` with MP3 magic) routes to the Viewer, whose DocKind::Audio case shows an honest "open it in the Music
 * Player" hint. The host re-reads + jails the path regardless of this routing. */
void open_file_entry(DrawCtx &ctx, const std::string &path)
{
    if (std::strcmp(kind_label_from_name(path), "audio") == 0) {
        ctx.commands.push_back({UiCommand::Kind::AudioPlay, path, "", 1, {}}); /* n=1 -> the workspace jail */
        ctx.show.music_player = true;
    } else {
        ctx.commands.push_back({UiCommand::Kind::OpenFile, path, "", 0, {}});
        ctx.show.viewer = true;
    }
}

/* The RIGHT pane: the selected dir's direct children in a sortable Name/Size/Kind table, with per-row + empty-
 * space context menus. Double-click a folder descends; double-click a file opens it in the Viewer. Runs a bit
 * past the function smell-test because each row's BeginPopupContextItem MUST execute inside that row's live
 * PushID scope — the table loop is one indivisible interaction unit, not separable without losing the per-row id. */
void draw_contents(const FileTree &tree, std::string &selected_dir, std::string &selected_entry, DrawCtx &ctx,
                   PendingFileAction &act)
{
    std::vector<FileNode> rows;
    auto                  it = tree.children.find(selected_dir);
    if (it != tree.children.end()) rows = it->second;

    if (rows.empty())
        ImGui::TextColored(muted_v4(), "this folder is empty \xE2\x80\x94 right-click to create a file or folder");

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                             ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH;
    if (!rows.empty() && ImGui::BeginTable("contents", 3, tflags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort,
                                0.60f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs *sp = ImGui::TableGetSortSpecs()) {
            int  col = 0;
            bool asc = true;
            if (sp->SpecsCount > 0) {
                col = sp->Specs[0].ColumnIndex;
                asc = sp->Specs[0].SortDirection != ImGuiSortDirection_Descending;
            }
            std::sort(rows.begin(), rows.end(),
                      [col, asc](const FileNode &a, const FileNode &b) { return node_less(a, b, col, asc); });
        }

        for (const auto &n : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(n.path.c_str());
            std::string label = display_name(n.leaf);
            if (n.is_dir) label += "/";
            bool sel = (selected_entry == n.path);
            if (ImGui::Selectable(label.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_entry = n.path;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (n.is_dir) selected_dir = n.path;
                    else open_file_entry(ctx, n.path); /* audio -> player, else -> viewer */
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                selected_entry = n.path;
                if (n.is_dir) {
                    if (ImGui::MenuItem("Open")) selected_dir = n.path;
                    ImGui::Separator();
                    if (ImGui::MenuItem("New File here")) {
                        act.kind = PendingFileAction::NewFile;
                        act.target = n.path;
                    }
                    if (ImGui::MenuItem("New Folder here")) {
                        act.kind = PendingFileAction::NewFolder;
                        act.target = n.path;
                    }
                } else {
                    if (ImGui::MenuItem("Open")) open_file_entry(ctx, n.path); /* audio -> player, else -> viewer */
                    if (ImGui::MenuItem("Open in Editor")) {
                        ctx.commands.push_back({UiCommand::Kind::OpenFile, n.path, "", 0, {}});
                        ctx.show.editor = true;
                    }
                    if (ImGui::MenuItem("Attach to chat")) { /* A: queue this file for the next conductor message */
                        ctx.commands.push_back({UiCommand::Kind::ConductorAttach, n.path, "", 0, {}});
                        ctx.show.chat = true;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Rename\xE2\x80\xA6")) {
                    act.kind = PendingFileAction::Rename;
                    act.target = n.path;
                    act.target_is_dir = n.is_dir;
                }
                if (ImGui::MenuItem("Delete\xE2\x80\xA6")) {
                    act.kind = PendingFileAction::Delete;
                    act.target = n.path;
                    act.target_is_dir = n.is_dir;
                }
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(n.path.c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            if (n.is_dir) ImGui::TextColored(muted_v4(), "\xE2\x80\x94"); /* em dash */
            else right_text(human_size(n.size).c_str(), muted_v4());

            ImGui::TableNextColumn();
            ImGui::TextColored(muted_v4(), "%s", n.is_dir ? "folder" : kind_label_from_name(n.leaf));
        }
        ImGui::EndTable();
    }

    /* right-click an empty part of the pane -> New (over a row, the per-row menu wins via NoOpenOverItems) */
    if (ImGui::BeginPopupContextWindow("contents_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("New File")) {
            act.kind = PendingFileAction::NewFile;
            act.target = selected_dir;
        }
        if (ImGui::MenuItem("New Folder")) {
            act.kind = PendingFileAction::NewFolder;
            act.target = selected_dir;
        }
        ImGui::EndPopup();
    }
}

/* --- the modals (reuse the settings/about BeginPopupModal idiom) --- */

void rename_modal(DrawCtx &ctx, const std::string &target, char *name_buf, size_t bufsz)
{
    if (!ImGui::BeginPopupModal("Rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string parent = parent_of(target);
    ImGui::TextColored(muted_v4(), "rename in %s", parent.empty() ? "(root)" : parent.c_str());
    ImGui::SetNextItemWidth(280.0f);
    bool enter = ImGui::InputText("##rename", name_buf, bufsz, ImGuiInputTextFlags_EnterReturnsTrue);
    bool ok = ImGui::Button("Rename") || enter;
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel");
    if (ok && name_buf[0]) {
        ctx.commands.push_back({UiCommand::Kind::RenameFile, target, join_path(parent, name_buf), 0, {}});
        ImGui::CloseCurrentPopup();
    } else if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void delete_modal(DrawCtx &ctx, const std::string &target, bool is_dir)
{
    if (!ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Delete this item? This can't be undone.");
    ImGui::TextColored(accent_v4(), "%s", target.c_str());
    if (is_dir) ImGui::TextColored(muted_v4(), "(only an empty folder can be removed)");
    ImGui::Spacing();
    if (ImGui::Button("Delete")) {
        ctx.commands.push_back({UiCommand::Kind::DeleteFile, target, "", is_dir ? 1 : 0, {}});
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void new_modal(DrawCtx &ctx, const std::string &parent, int new_is_dir, char *name_buf, size_t bufsz)
{
    if (!ImGui::BeginPopupModal("New", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextColored(muted_v4(), "new %s in %s", new_is_dir ? "folder" : "file",
                       parent.empty() ? "(root)" : parent.c_str());
    ImGui::SetNextItemWidth(280.0f);
    bool enter = ImGui::InputText("##newname", name_buf, bufsz, ImGuiInputTextFlags_EnterReturnsTrue);
    bool ok = ImGui::Button("Create") || enter;
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel");
    if (ok && name_buf[0]) {
        ctx.commands.push_back({UiCommand::Kind::CreateFile, join_path(parent, name_buf), "", new_is_dir, {}});
        ImGui::CloseCurrentPopup();
    } else if (cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace

/* The panel entry point: toolbar (Up/breadcrumb/New) -> the resizable tree|contents split -> modal dispatch.
 * Runs past the function smell-test because the three phases share the single-frame `act` (the CRUD intent the
 * pane/toolbar raise and the modal block resolves) — splitting them would only thread `act` through an out-param
 * for no isolation gain. */
void draw_files_panel(const UiSnapshot &s, DrawCtx &ctx, bool *open)
{
    if (!ImGui::Begin("Files", open)) {
        ImGui::End();
        return;
    }

    static std::string selected_dir;     /* the current folder ("" = root) — drives the contents pane     */
    static std::string selected_entry;   /* the highlighted contents row (full path)                      */
    static std::string modal_target;     /* rename/delete subject, captured at trigger time               */
    static std::string modal_parent;     /* new-entry parent dir, captured at trigger time                */
    static bool        modal_is_dir = false;
    static int         modal_new_is_dir = 0;
    static char        name_buf[256] = ""; /* the InputText backing SHARED by the Rename + New modals — safe
                                            * because they are keyed on distinct modal ids (mutually exclusive,
                                            * at most one open) and each is re-seeded at its trigger below */

    FileTree tree = build_tree(s.files);
    if (!selected_dir.empty() && !tree.is_dir(selected_dir)) selected_dir.clear(); /* stale (deleted) -> root */

    PendingFileAction act;

    /* --- toolbar: Up + breadcrumb + New --- */
    ImGui::BeginDisabled(selected_dir.empty());
    if (ImGui::SmallButton("Up")) selected_dir = parent_of(selected_dir);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushID("crumbs");
    if (ImGui::SmallButton("root")) selected_dir.clear();
    {
        std::string acc;
        size_t      i = 0;
        int         idx = 0;
        while (i < selected_dir.size()) {
            size_t      j = selected_dir.find('/', i);
            std::string seg = selected_dir.substr(i, j == std::string::npos ? std::string::npos : j - i);
            acc = acc.empty() ? seg : acc + "/" + seg;
            i = (j == std::string::npos) ? selected_dir.size() : j + 1;
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(muted_v4(), "/");
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::PushID(idx++);
            if (ImGui::SmallButton(display_name(seg).c_str())) selected_dir = acc;
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    {
        const char *nl = "+ New";
        float       bw = ImGui::CalcTextSize(nl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float       right = ImGui::GetWindowContentRegionMax().x - bw;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < right) ImGui::SetCursorPosX(right);
        if (ImGui::SmallButton(nl)) ImGui::OpenPopup("new_menu");
        if (ImGui::BeginPopup("new_menu")) {
            if (ImGui::MenuItem("New File")) {
                act.kind = PendingFileAction::NewFile;
                act.target = selected_dir;
            }
            if (ImGui::MenuItem("New Folder")) {
                act.kind = PendingFileAction::NewFolder;
                act.target = selected_dir;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::Separator();

    /* --- the split: a resizable 2-column table; each cell holds an independently-scrolling child --- */
    float footer = s.files_truncated ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
    float h = ImGui::GetContentRegionAvail().y - footer;
    if (h < 1.0f) h = 1.0f;
    if (ImGui::BeginTable("files_split", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthStretch, 0.34f);
        ImGui::TableSetupColumn("contents", ImGuiTableColumnFlags_WidthStretch, 0.66f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("tree_pane", ImVec2(0, h))) draw_tree_node(tree, "", "root", selected_dir);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if (ImGui::BeginChild("contents_pane", ImVec2(0, h)))
            draw_contents(tree, selected_dir, selected_entry, ctx, act);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    if (s.files_truncated)
        ImGui::TextColored(err_v4(), "listing truncated at the cap \xE2\x80\x94 some nested entries are hidden");

    /* --- a raised intent becomes a modal (seed the buffers at trigger time) --- */
    switch (act.kind) {
    case PendingFileAction::Rename:
        modal_target = act.target;
        modal_is_dir = act.target_is_dir;
        std::snprintf(name_buf, sizeof name_buf, "%s", leaf_of(act.target).c_str());
        ImGui::OpenPopup("Rename");
        break;
    case PendingFileAction::Delete:
        modal_target = act.target;
        modal_is_dir = act.target_is_dir;
        ImGui::OpenPopup("Delete?");
        break;
    case PendingFileAction::NewFile:
        modal_parent = act.target;
        modal_new_is_dir = 0;
        name_buf[0] = '\0';
        ImGui::OpenPopup("New");
        break;
    case PendingFileAction::NewFolder:
        modal_parent = act.target;
        modal_new_is_dir = 1;
        name_buf[0] = '\0';
        ImGui::OpenPopup("New");
        break;
    case PendingFileAction::None:
        break;
    }

    rename_modal(ctx, modal_target, name_buf, sizeof name_buf);
    delete_modal(ctx, modal_target, modal_is_dir);
    new_modal(ctx, modal_parent, modal_new_is_dir, name_buf, sizeof name_buf);

    ImGui::End();
}

} // namespace hc::ui
