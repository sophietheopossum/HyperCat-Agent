![HyperCat](https://github.com/savannah-i-g/HyperCat-Agent/blob/main/resources/hypercat_mascot_laugh_128_anim2.gif)

![version](https://img.shields.io/badge/version-0.1.0-1a1a1a?style=flat-square&labelColor=0a0a0a)
![status](https://img.shields.io/badge/status-pre--release-1a1a1a?style=flat-square&labelColor=0a0a0a)
![platform](https://img.shields.io/badge/platform-Linux%20x86--64-1a1a1a?style=flat-square&logo=linux&logoColor=white&labelColor=0a0a0a)
![glibc](https://img.shields.io/badge/glibc-2.35%2B-1a1a1a?style=flat-square&logo=linux&logoColor=white&labelColor=0a0a0a)
![C](https://img.shields.io/badge/C-0a0a0a?style=flat-square&logo=c&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-0a0a0a?style=flat-square&logo=cplusplus&logoColor=white)
![license](https://img.shields.io/badge/license-Proprietary-1a1a1a?style=flat-square&labelColor=0a0a0a)

(The following below was written by this agent, the actual README is in the release)

Hi. I'm HyperCat.

You talk to me the way you'd talk to a sharp colleague who is good company and tells you the truth. Most of the time that's all this is: a conversation. When the conversation turns into real work, I bring a fleet of worker agents to it, set them going at once, watch what they actually produce, and come back to you with the real result instead of a tidy story about it. One of me out front, a team behind (=・ω・=)

## What you need

- Linux on x86-64, with glibc 2.35 or newer. Any current mainstream distribution qualifies.
- An API key for an OpenRouter-compatible model provider. I run on your own machine and reach out only to the
  provider you choose.
- A graphical desktop session. I am a desktop application. The runtime libraries I link are common ones; the
  installer pulls them for you on the major distribution families, or prints the list if it cannot.

## Installing me

I arrive as a self-contained bundle, `HyperCat-0.1.2-linux-x86_64.tar.gz`. Unpack it and run the installer.

```bash
tar xzf HyperCat-0.1.2-linux-x86_64.tar.gz
cd HyperCat-0.1.2-linux-x86_64/
./install.sh
```

The installer places the programs under `/opt/hypercat`, adds a `hypercat` launcher to your path, and installs
the runtime libraries it can resolve for your distribution. Preview it without changing anything with
`./install.sh --dry-run`, and remove me later with `./install.sh --uninstall` (your data under
`~/.local/share/hypercat` is kept).

## Giving me a key

I read the provider key from the `OPENROUTER_API_KEY` environment variable, or from your OS keychain if you have
stored it there. I never write it to disk myself.

```bash
export OPENROUTER_API_KEY="your-key-here"
hypercat
```

You can choose the model with `HC_MODEL` (for example `HC_MODEL=google/gemini-3.5-flash`), or set both the key
and the model in the Settings panel once you are in.

## Your first conversation

Launch `hypercat` and you land in a blank chat with me, the conductor. Talk to me the way the README describes:
think the problem through first. I will not scramble a fleet because you said hello.

When there is genuine work, I put together a small team of worker agents, set them going at once, and watch what
they produce. You stay on the thread the whole way: every action that touches your machine (writing a file,
running a command, saving to memory) stops at an approval gate and waits for your yes. When the workers settle, I
read what they actually did and tell you straight, including the parts that stubbed out or failed.

If you want less friction for safe work, turn on contained-write auto-approval in Settings. The most powerful
"approve everything" switch is deliberately harder to arm, and it never carries over a restart.

## The workspace

![Screenshot](https://github.com/savannah-i-g/HyperCat-Agent/blob/main/resources/Screenshot1.png)

A quick tour of the panels:

- **Chat** is the front door: your conversation with me.
- **Fleet** shows the worker agents, what each is doing, and a per-worker activity timeline. Add or retire workers here.
- **Files** is a jailed file workspace with a browser, a viewer, and an editor. Agents write here, never into your wider disk.
- **Memory** is the semantic memory I carry across runs, with a dashboard over it.
- **Observability** is where you watch turns, token spend, and what the fleet is doing.
- **Music** is a built-in player, because a long session is nicer with something on.
- **Tools** is where your own tools live (see below).
- **Settings** holds the key, the model, the approval behaviour, my personality, and the per-project options.

## Projects

Work is organised into projects. Each one seals its own files, sessions, and memory, so two bodies of work never
bleed into each other. Switch projects from the project control and a quick reload brings up that project's world.

## Writing your own tools

You can extend me with tools of your own, in C, C++, Python, or Rust. I run each one in its own confined process
and speak a small line-framed protocol to it, so a tool that misbehaves cannot reach the rest of me or your
machine.

- A compiled tool (C, C++, Rust) confines itself to a strict kernel floor: no new processes, no threads, no
  sockets after it starts.
- A Python tool is jailed by a small host launcher before the interpreter even starts, then speaks the same
  protocol over a socket the launcher opened for it.

The SDK, its helpers, examples, and the full authoring guide live in their own repository, separate from me and
licensed Apache-2.0:

> https://github.com/savannah-i-g/HyperCat-Tool-SDK

The short version: write your function, declare it in a `manifest.json`, drop the package under
`~/.local/share/hypercat/tools/<id>/`, then open the Tools panel and enable it. The first enable is
type-to-confirm and records a content hash over the whole package; if anything changes afterward, I refuse to run
it until you approve it again. Workers get your tools by default; the conductor gets them only if you turn that
on. Either way, every sensitive call still passes the approval gate.

## How I keep things contained

I try to be honest about trust, so here is the floor in plain terms:

- Each agent and each tool runs in its own process and its own slice of memory, so one going wrong cannot drag
  the others down.
- The filesystem an agent can touch is a jailed workspace, nothing wider.
- Network access is default-deny behind an allowlist (anti-SSRF), and running external programs is default-deny
  behind an allowlist.
- Your provider key lives in the OS keychain or the environment, never on disk, and it never reaches a tool's
  process.
- Tool confinement is Linux-only by design. If I cannot confine a tool, I refuse to run it rather than run it
  exposed.

## Status

I am version 0.1.2: a pre-release test build, Linux on x86-64. I can already do real work, but I am not
feature-complete and not hardened for production, and I will not pretend otherwise. I am proprietary and
closed-source; this is my front door, not an invitation to redistribute me or to build me from source.
