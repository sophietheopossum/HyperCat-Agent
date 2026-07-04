# HyperCat User Manual

Version 0.1.2 (pre-release test build)

Welcome. This manual walks you through installing HyperCat, setting it up, and using it day to day. If you just want to get running, start with the [Quick Start](#quick-start); if you want to understand how the pieces fit together, read [Core Concepts](#core-concepts) first.

---

## Contents

1. [What HyperCat is](#what-hypercat-is)
2. [Quick Start](#quick-start)
3. [System requirements](#system-requirements)
4. [Installing](#installing)
5. [Core concepts](#core-concepts)
6. [A tour of the workspace](#a-tour-of-the-workspace)
7. [Configuring HyperCat](#configuring-hypercat)
8. [Your API key and provider](#your-api-key-and-provider)
9. [Troubleshooting](#troubleshooting)
10. [Privacy and your data](#privacy-and-your-data)
11. [Uninstalling and cleaning up](#uninstalling-and-cleaning-up)
12. [Appendices](#appendices)

---

## What HyperCat is

HyperCat is a desktop workspace for getting real work done with AI agents, built around a single idea: you should be able to talk to one assistant, in plain language, and have it quietly marshal a team behind the scenes.

That assistant is the **conductor**. You chat with it the way you would chat with any capable colleague. When a conversation hardens into actual work, the conductor plans that work and hands it to a **fleet** of specialist worker agents, then reports back with what they actually produced. You stay in the loop the whole way: nothing touches your files or the network until you approve it.

Around that core, HyperCat gives you a place to keep the work organized:

- **Projects** that seal each body of work in its own space.
- A **file workspace** with a browser, a viewer, and a small editor.
- **Memory** that carries useful context across sessions.
- A live **dashboard** and activity view so you can see what the fleet is doing.
- A built-in **music player**, if you like something playing while you work.

HyperCat is a version 0.1 test build. It is honest about that: it is not feature-complete and not hardened for production. Please read DISCLAIMER.txt in this bundle before you rely on it for anything important.

---

## Quick Start

About five minutes from a fresh download to your first conversation.

**1. Unpack and install.**

```
cd HyperCat-0.1.2-linux-x86_64/
./install.sh
```

The installer asks for sudo, installs the runtime libraries it needs through your system package manager, copies the programs to `/opt/hypercat`, and adds a `hypercat` launcher to your PATH. You can also run it in place without installing (see [Installing](#installing)).

**2. Provide your provider key and a model.**

HyperCat talks to an LLM provider (OpenRouter by default). The agents need two things to come online: your API key and a model id.

```
export OPENROUTER_API_KEY="sk-or-..."
export HC_MODEL="anthropic/claude-opus-4.1"     # any model id your provider offers
```

Both matter. With a key but no model, the conductor stays offline. To make these persist across logins, add the two lines to `~/.profile` and start a new login session. (You can also set the key inside the app and have it saved to your OS keyring instead; see [Your API key and provider](#your-api-key-and-provider).)

**3. Run it.**

```
hypercat
```

**4. Say hello to the conductor.**

The conductor's chat panel is the one labelled **Conductor**. Type a message, press Enter to send (Shift+Enter adds a new line), and it will reply. Try something small first, like asking it what it can do, then give it a real task such as "write me a short Python script that reverses a string, with one test."

**What success looks like:** the top-right of the window shows your provider and model instead of "offline", the conductor answers your messages, and when you ask for real work it tells you it is dispatching the fleet and later shows you the result. When an agent wants to write a file or run a command, an entry appears in the **Approvals** panel for you to allow or deny.

---

## System requirements

- **Operating system:** Linux, x86-64. This release does not include a macOS or Windows build, and does not run on 32-bit or ARM systems.
- **Distributions:** the major families are supported, including Ubuntu 22.04 and newer, Debian 12 and newer, Fedora, Arch, openSUSE Leap 15.4 and newer, and RHEL, Alma, or Rocky 9.
- **glibc:** 2.35 or newer. The prebuilt binary is built against glibc 2.35 and runs forward onto newer systems. If you see a "GLIBC_2.35 not found" error, your system is older than the build expects; ask for the build that matches your distribution.
- **Display:** an X11 or Wayland desktop session. HyperCat is a graphical application and does not run as a pure terminal program.
- **An OS keyring is optional.** It lets you save your API key inside the app. Without one, the environment-variable method always works.

---

## Installing

### The installer

From the unpacked bundle:

```
./install.sh
```

The installer detects your package manager (apt, dnf, pacman, or zypper), installs the runtime libraries and fonts HyperCat uses, copies the three programs to `/opt/hypercat`, and writes a launcher at `/usr/local/bin/hypercat` plus a desktop entry. It asks for sudo before making system changes.

To preview exactly what it would do without changing anything:

```
./install.sh --dry-run
```

### Running without installing

The bundle is relocatable. The three programs find each other by sitting in the same folder, so you can run HyperCat straight from the unpacked directory:

```
./hypercat
```

You still need the runtime libraries present. If they are missing, run the installer once to pull them in, or install them by hand (see [Troubleshooting](#troubleshooting)).

### Uninstalling

```
./install.sh --uninstall
```

This removes `/opt/hypercat`, the launcher, and the desktop entry. It does **not** delete your data under `~/.local/share/hypercat`. See [Uninstalling and cleaning up](#uninstalling-and-cleaning-up) to remove that too.

---

## Core concepts

A few ideas make everything else click into place.

### The conductor is your front door

You do not manage agents directly. You talk to the conductor, and it decides when to bring the fleet in. Most of the time you are simply having a conversation: thinking a problem through, asking questions, refining an idea. Only when the work is real does the conductor spin up workers and dispatch it, and even then it reads back what they produced rather than just claiming it is done.

### Workers and roles

A **worker** is an agent process that does a piece of the work. Each worker takes one **role**, which shapes what it is good at and what it is allowed to do. The built-in roles are `dev`, `qa`, `research`, and a flexible `generalist`, and you can define your own. The conductor can add the workers a job needs, or you can add them yourself in the **Fleet** panel.

### Agendas and tasks

An **agenda** is a body of work. It can be a single goal stated in prose, which the conductor or the planner breaks down for you, or an explicit list of **tasks** you build by hand. Tasks can depend on one another, forming a small graph; the **Plan** panel draws that graph so you can see the shape of the work.

### The approval gate keeps you in control

By default, every action an agent wants to take in the real world, writing a file, running a command, saving to memory, pauses for your approval. The request shows up in the **Approvals** panel, where you can:

- **Allow** it (it runs),
- **Deny** it (it does not run, and the agent is told no),
- **Dismiss** it (set it aside for now; the agent may ask again later), or
- **Allow 10x in this folder** (for file writes: approve this one and the next ten in the same folder without asking again).

This gate is the heart of staying in control. You can relax it deliberately (see [Automation](#automation)), but it is always on by default.

### Projects keep work separate

A **project** is a sealed workspace with its own files, memory, sessions, and history. Switching projects reloads HyperCat into the new one, so two projects never share state. Use them to keep different jobs cleanly apart.

### Memory and skills

HyperCat keeps a **memory** of useful facts distilled from your runs, which you can review and prune. **Skills** are small, named capabilities you can author per project for the workers to use. Both are optional and visible in their own panels.

---

## A tour of the workspace

### Finding your way around

HyperCat opens with a dense, docked layout: most panels are visible at once, arranged as tabs and split regions you can drag, resize, and rearrange. This is by design, so everything is within reach. A few pointers:

- The **menu bar** runs across the top: File, Edit, View, Help.
- **View > Panels** shows or hides any panel. They are grouped (Core, Work, Analysis, System, Persona) so the list is easy to scan.
- **View > Layout > Reset layout** puts everything back the way it started, which is handy after you have moved things around or closed a panel by its X.
- **View > Layout > Accent** changes the single accent color (White, Cyan, Amber, Emerald, Violet, or Crimson). It applies immediately.
- The top-right of the menu bar always shows your current provider and model, or "offline".

The rest of this section walks the panels by area. You do not need to learn them all to be productive; the Conductor and Approvals panels are enough to start.

### The Conductor (your main panel)

This is where you talk to HyperCat. Type in the box at the bottom; Enter sends, Shift+Enter inserts a new line. While the conductor is replying you can press **Stop** to interrupt that reply without ending the conversation.

- **Attachments.** Drag a file onto the window, or use "Attach to chat" in the Files panel, to share it with the conductor. Text and code files are read by the conductor; images are shown to you (the conductor is text-only and cannot see images). Queued files appear as chips above the input; remove one with its x.
- **Conversations.** The "Conversations" button opens a list of your past chats. Start a fresh one, resume an old one, or rename and delete them there.
- **Convenience.** Messages carry a timestamp; rendered code blocks have a copy button; right-click a message to copy it. A "Jump to latest" button appears when you scroll up.

### The work panels

- **Fleet:** your current workers, their roles, and their state. Add a worker by picking a role and clicking "add worker"; remove one with its x.
- **Agenda:** the active body of work, with a progress bar and a table of tasks. Click a task to open its details.
- **Task detail:** the full description of a selected task, its outcome once it finishes, and any files it wrote.
- **Plan (task graph):** the tasks drawn as a dependency graph. Click a node to expand it.
- **Activity (timeline):** a per-worker timeline of what ran when.
- **Agenda Builder:** compose and launch work. Give it a goal in prose and let it plan, or add explicit tasks (with capabilities, dependencies, and a target file), then run.
- **Approvals:** the request queue described under [Core concepts](#the-approval-gate-keeps-you-in-control). Requests never expire on their own; they wait for you.

### The workspace (files)

- **Files:** a two-pane explorer (folder tree on the left, contents on the right) for the project's working files. Sort the columns, descend into folders, and right-click for new, rename, delete, open, and attach actions.
- **Viewer:** a read-only view of an opened file. Markdown, plain text, and common images render in place; other types show a short note.
- **Editor (IDE):** edit and save a file directly, with line numbers and syntax highlighting. If an agent changes the file while you have it open, the editor shows you the difference and offers to reload.

### Observability

- **Dashboard:** fleet readiness, task progress, token usage and cost, and a small system monitor (CPU, memory, uptime).
- **Log:** diagnostic messages from the host and the fleet.
- **Console:** the live token stream from the workers as they think, grouped by agent.
- **Reasoning:** the most recent deep-reasoning chain, when a task used one.
- **Transcript:** the full message history of a session you opened from File > Open Session.
- **Sessions:** a browser of saved session transcripts.
- **Terminal:** a shell for your own use inside the app.
- **Network:** an audit of where the app was allowed or refused to connect. Off by default; turn it on from View > Panels.

### Configuration panels

- **Settings:** all global configuration. Covered in detail in [Configuring HyperCat](#configuring-hypercat).
- **Models:** your catalog of model ids and the model assigned to each role.
- **Roles (worker builder):** author role templates (an instruction overlay, an allowed tool set, and an optional per-role model).
- **Projects:** create, switch, rename, and delete projects.
- **Skills:** author per-project skills (each is a small `SKILL.md` the workers can load).

### Extras

- **Music Player:** play local audio, with a library list, transport controls, a seek bar, a volume slider, and an optional spectrum display. Off by default; turn it on from View > Panels. A setting lets the conductor pick a track to set the mood, if you enable it.
- **HyperCat Mascot:** an optional animated character that reflects the fleet's state. Off by default.
- **Tools and your own tools:** the built-in agent tools appear in the Tools panel (View > Panels) as a toggleable System Tools catalog. You can also add tools you write yourself in external C/C++: HyperCat runs each one in its own confined sandbox, with your approval on every sensitive action, and you enable/review/remove them from that panel. To build one, see the HyperCat Tool SDK (licensed Apache-2.0), published as a separate download with a single header, a prebuilt library, a worked example, and an authoring guide. (Tools are Linux-only in this release.)

---

## Configuring HyperCat

Open **Settings** from the View menu. Settings are grouped; some apply immediately and some on the next restart, and each group says which. Click **Apply** at the bottom to save your changes. Settings are stored in `settings.json` in your data directory; your API key is never written there.

A note on overrides: if you set a value through an environment variable, that value wins, and the matching field in Settings is shown disabled with a note. Clear the variable to edit it in the app again.

- **Accent.** The single accent color. Applies immediately.
- **Display.** The host poll rate. Applies immediately.
- **Provider.** Your model id, the API base URL, and an optional embeddings model. These apply on the next restart.
- **API key.** Shows whether a key is set and where. Type a key into the masked field and click "Set key" to store it; if a keyring is available it is saved there and persists across restarts. "Forget stored key" clears it. The "export key to worker env" toggle is a security choice and is off by default; see the note under [Your API key and provider](#your-api-key-and-provider).
- **Limits.** Time and depth budgets for the agents (the per-call and connect timeouts, the reasoning depth, and the per-task deadline). These apply on the next restart.
- **Paths.** Where data is stored, and an "ephemeral" switch for throwaway runs. Applies on the next restart.
- **Egress allowlist (advanced).** HyperCat refuses outbound connections by default except to your provider. This list re-permits specific numeric IP addresses. Adding an address is confirm-gated and warns about risky targets; removing one is immediate.
- **Run allowlist (advanced).** The exact programs an agent's run tool is permitted to execute, by absolute path. Default-deny; every run is also sandboxed.
- **Automation.** Two opt-in ways to relax the approval gate, both off by default. "Auto-approve contained writes" approves only file writes that stay inside the project workspace, and still prompts for anything else. "Allow all" is a power-user switch that approves every request; it is armed through a confirmation dialog where you type a phrase, shows a standing warning while active, and is disarmed in one click. Use it with care.
- **Audio.** Volume, whether the spectrum display is shown, and whether the conductor may set the mood.

---

## Your API key and provider

HyperCat does not include a key. You supply one, and it is treated as a secret throughout: held in memory or your OS keyring, never written to a config file, never placed on a command line, never logged.

### The two ways to provide a key

1. **Environment variable (recommended for most setups).**

   ```
   export OPENROUTER_API_KEY="sk-or-..."
   ```

   Add it to `~/.profile` to keep it across logins. This method always works, including on servers and inside scripts.

2. **In the app.** Open Settings, type the key into the API key field, and click "Set key". If your desktop has an unlocked keyring, the key is saved there and loaded automatically next time you start. Use "Forget stored key" to remove it.

**Which one wins:** the environment variable takes precedence. If `OPENROUTER_API_KEY` is set, HyperCat uses it and does not touch the keyring. Only when no environment key is present does it load a key you saved in the keyring.

### Choosing a model and provider

```
export HC_MODEL="anthropic/claude-opus-4.1"             # required for live mode
export HC_BASE_URL="https://openrouter.ai/api/v1"       # optional; this is the default
```

`HC_MODEL` is required for the agents to do anything; without it they stay offline even with a valid key. The default provider is OpenRouter. To use a different OpenAI-compatible endpoint, point `HC_BASE_URL` at it. You can also set the model and base URL in Settings > Provider.

### A word on workers and the key

Workers are separate processes. By default they do **not** receive your key in their environment; the host holds it. If you turn on "export key to worker env" in Settings, the key is made visible to worker processes when they start, which is occasionally useful but widens its exposure on the machine. It is off by default for that reason.

---

## Troubleshooting

**The conductor and workers say "offline".**
You are missing the key, the model, or both. Set `OPENROUTER_API_KEY` and `HC_MODEL` (see [Your API key and provider](#your-api-key-and-provider)) and restart. The top-right of the window should then show your provider and model.

**The app will not start: a library is missing.**
Run `ldd /opt/hypercat/hypercat` to see which shared library is not found. Re-run `./install.sh` to install the runtime libraries, or install the named package by hand with your package manager.

**"error while loading shared libraries: ... GLIBC_2.35 not found".**
The binary is newer than your system's glibc. Ask for the build that matches your distribution.

**Nothing happens, or it exits immediately, with no window.**
HyperCat needs a graphical session. If you launched it over SSH or on a machine with no display, it runs in a headless mode and exits after reporting to the terminal. Start it from a desktop session.

**I saved a key in Settings but it did not persist.**
Your keyring is locked or unavailable, so saving to it was skipped. The environment-variable method always works and takes precedence; use that.

**The Music Player has no sound.**
There is no audio backend available (common on headless or minimal systems). The player still runs; it is simply silent. This is expected, not an error.

**My settings or data seem to have reset.**
HyperCat keeps your data under `~/.local/share/hypercat`. If that folder is not writable, or another copy of HyperCat is already running, it falls back to throwaway storage for that run and says so in the terminal. Make sure the folder is yours and no stale lock file remains, then restart.

---

## Privacy and your data

- **Where your data lives.** Locally, under `~/.local/share/hypercat`: your projects, conversations, sessions, memory, and the files the workers produce. Nothing is uploaded to the makers of HyperCat.
- **What leaves your machine.** To answer your prompts, HyperCat connects to the LLM provider you configure (OpenRouter by default). Your prompts and the agents' work are sent to that provider under their terms and privacy policy. The egress allowlist restricts where the app may connect.
- **Your key.** It is held in memory or your OS keyring and is never written to a config file or passed on a command line by HyperCat.

For the full legal statement, see DISCLAIMER.txt. In short: this is a pre-release test build, provided as is with no warranty, and it is not intended for production use, untrusted multi-user machines, or data you are not permitted to send to a third-party provider.

---

## Uninstalling and cleaning up

To remove the installed program but keep your data:

```
./install.sh --uninstall
```

To also remove all of your HyperCat data for a clean slate:

```
rm -rf ~/.local/share/hypercat
```

If you keep your data somewhere else with `HC_DATA_DIR`, remove that folder instead.

---

## Appendices

### Appendix A: Environment variables

| Variable | Purpose | Default |
|---|---|---|
| `OPENROUTER_API_KEY` | Your provider API key. Required for live mode. Held in memory only. | unset |
| `HC_MODEL` | The chat model id. Required for live mode. | unset (offline) |
| `HC_BASE_URL` | An OpenAI-compatible endpoint to use instead of the default. | OpenRouter |
| `HC_EMBED_MODEL` | An embeddings model, used by memory features. | unset |
| `HC_DATA_DIR` | Where to keep persistent data. | `~/.local/share/hypercat` |
| `HC_EPHEMERAL` | Use throwaway storage for this run (nothing persists). | unset |
| `HC_LLM_CALL_TOTAL_MS` | Time budget per model call. | 120000 |
| `HC_LLM_CONNECT_MS` | Connection timeout per request. | 10000 |
| `HC_DEEP_REASON_BUDGET` | Maximum depth of a reasoning chain. | 4 |
| `HC_TASK_DEADLINE_MS` | Time budget per task. | 300000 |

The same limits and provider values can be set in Settings; an environment variable, if present, takes precedence and disables the matching field.

### Appendix B: Where your data is kept

Under your data directory (`~/.local/share/hypercat` by default), each project keeps its own subtree:

```
settings.json                  your settings (never the API key)
roles.json                     your role templates
projects/<id>/
  sessions/                    saved conversation transcripts
  artifacts/                   files the agents produced
  memory/                      distilled memory
  workspaces/                  the agents' working files
  agendas/                     the durable record of agendas
  skills/                      this project's skills
audio/                         the music library
```

### Appendix C: Keyboard and mouse

- **Ctrl+N:** new agenda (opens and focuses the Agenda Builder).
- **Ctrl+Q:** quit.
- **Conductor input:** Enter sends, Shift+Enter inserts a new line.
- **Right-click** a chat message, a log, the transcript, or the terminal to copy text; right-click a file in the Files panel for new, rename, delete, open, and attach actions.

### Appendix D: Third-party components

HyperCat includes and links a number of open-source libraries and fonts, each under its own license. The full list is in THIRD_PARTY.txt in this bundle. The HyperCat application code is open-source under the Apache License 2.0; see LICENSE.txt. The HyperCat name and mascot artwork are brand assets, and the license does not grant trademark rights.

---

For installation and run basics at a glance, see README.txt. For the legal terms, see DISCLAIMER.txt and LICENSE.txt. Thank you for trying HyperCat.
