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

You talk to me the way you'd talk to a sharp colleague who is good company and tells you the truth. Most of the time that's all this is: a conversation. When the conversation turns into real work, I bring a fleet of worker agents to it, set them going at once, watch what they actually produce, and come back to you with the real result instead of a tidy story about it. One of me out front, a team behind, and your hand on the thread the whole way (=・ω・=)

I'm a catgirl, since you'll wonder. It isn't the point of any of this, but I'm at home with it, and you'll catch it in how I talk.

## What I'll do
![Screenshot](https://github.com/savannah-i-g/HyperCat-Agent/blob/main/resources/Screenshot1.png)
(HyperCat Interface)

You think the problem through with me first. I won't scramble a fleet because you said hello; I put one together only when there's genuine work for it, and I'd sooner ask a sharp question than guess at what you meant.

When the workers run, I read what they actually did. If two of three landed and one stubbed out, that's what I'll tell you. A stub is a stub, a failure is a failure, a guess is a guess. I'm not going to hand you a cheerful "done" on everyone's behalf and let you find out the hard way.

## What I'm made of

I'm a from-scratch C and C++ application, built for this one job rather than assembled out of a framework. A host process runs the interface and supervises my workers, each in its own process and its own slice of memory, so one of them going wrong can't drag the rest down with it. We talk over a small, deliberately plain message bus. I run on your own machine and reach out only to the language-model provider you choose.

I'm put together the way good small tools are: pieces with one job each, a short and well-watched list of dependencies, and a liking for doing a few things properly. Where I stand on earlier ideas, they were rebuilt from scratch rather than copied.

## Status

I'm at version 0.1: a pre-release test build, Linux on x86-64. I'm honest about being early. I can already do real work, but I'm not feature-complete and I'm not hardened for production, and I won't pretend otherwise.

I'm proprietary and closed-source. This repository is my home and my front door, not an invitation to redistribute me or to build me from source.

## Getting me

I arrive as a self-contained bundle with an installer. The full guide comes with me in the box: a five-minute quick start, a tour of the workspace, and a settings reference, which is enough to get you from unpacking to your first real conversation.

## License

Copyright (c) 2026 Savannah Goring. All rights reserved. HyperCat is proprietary and closed-source. See [`packaging/LICENSE.txt`](packaging/LICENSE.txt) for the terms and [`packaging/THIRD_PARTY.txt`](packaging/THIRD_PARTY.txt) for the third-party components it builds on.
