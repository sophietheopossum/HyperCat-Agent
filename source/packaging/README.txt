HyperCat 0.1.2 - Linux test build (Ubuntu 22.04 / 24.04, x86-64)
================================================================

A standalone, multi-agent workspace application.

This bundle is RELOCATABLE. The three programs find each other by sitting in the
same directory, so it runs wherever it is installed.

For the full guide, see MANUAL.md (installation, a tour of every panel, the
settings reference, and troubleshooting).


CONTENTS
  hypercat          the host application (the window you interact with)
  agentd            a worker-agent process (the host spawns these)
  hc_audio_helper   a sandboxed audio decoder (the host spawns this per track)
  install.sh        the installer (see below)
  MANUAL.md         the full user manual
  README.txt        this file
  LICENSE.txt       the HyperCat license (Apache License 2.0)
  DISCLAIMER.txt    important: pre-release test build, AS IS, no warranty
  THIRD_PARTY.txt   third-party components and their licenses


INSTALL  (system-wide, to /opt/hypercat)
  ./install.sh
This installs the runtime libraries and fonts through your system package
manager (it will ask for sudo), copies the programs to /opt/hypercat, and adds a
`hypercat` launcher to your PATH. To remove it later:  ./install.sh --uninstall

You can also run it WITHOUT installing, straight from this folder:  ./hypercat
(you will need the runtime libraries already present; see DEPENDENCIES).


SET YOUR API KEY AND MODEL  (required for the agents to do anything)
HyperCat talks to an LLM provider (OpenRouter by default). The agents need TWO
things to come online: your API key, and a model id. Provide both through
environment variables. The key is read from the environment only and is never
stored on disk:

  export OPENROUTER_API_KEY="sk-or-..."
  export HC_MODEL="anthropic/claude-opus-4.1"   # any model id your provider offers

With a key but no model, the conductor stays offline. To make these persist
across logins, add both lines to  ~/.profile  (then log out and back in, or run
`source ~/.profile`). Without a key, HyperCat still launches, but the conductor
and workers stay offline. You can also set the key inside the app, which can save
it to your OS keyring; see MANUAL.md.

Optional, same mechanism:
  HC_BASE_URL   point at a different OpenAI-compatible endpoint
                (default: https://openrouter.ai/api/v1)


RUN
  hypercat            (if installed)
  ./hypercat          (from this folder)


WHERE YOUR DATA LIVES
  ~/.local/share/hypercat       projects, sessions, memory, conversations
  (override with HC_DATA_DIR=/some/path)
A throwaway per-run socket directory is created under /tmp and removed on exit.
Uninstalling does NOT delete ~/.local/share/hypercat. Remove it by hand if you
want a clean slate.


DEPENDENCIES (installed for you by install.sh)
  Runtime libraries: libglfw3, libopenal1, libcjson1, libcurl4(t64),
    libasound2(t64), libgl1/libglx0, libx11-6, libsecret-1-0, libglib2.0-0
    (and their transitive dependencies).
  Fonts (for the intended look): fonts-jetbrains-mono, fonts-noto-cjk,
    fonts-dejavu-core. If absent, HyperCat falls back to a built-in font.

See DISCLAIMER.txt and LICENSE.txt before use.
