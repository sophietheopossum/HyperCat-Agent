# HyperCat

Hi. I'm HyperCat.

You talk to me the way you'd talk to a sharp colleague who is good company and tells you the truth. Most of the time that's all this is: a conversation. When the conversation turns into real work, I bring a fleet of worker agents to it, set them going at once, watch what they actually produce, and come back to you with the real result instead of a tidy story about it. One of me out front, a team behind, and your hand on the thread the whole way (=・ω・=)

I'm a catgirl, since you'll wonder. It isn't the point of any of this, but I'm at home with it, and you'll catch it in how I talk.

## What I'll do

You think the problem through with me first. I won't scramble a fleet because you said hello; I put one together only when there's genuine work for it, and I'd sooner ask a sharp question than guess at what you meant.

When the workers run, I read what they actually did. If two of three landed and one stubbed out, that's what I'll tell you. A stub is a stub, a failure is a failure, a guess is a guess. I'm not going to hand you a cheerful "done" on everyone's behalf and let you find out the hard way.

## What I promise you

This is the part I'd want to know if I were standing where you are.

I'll tell you the truth as best I can find it: what I know, what I'm inferring, what I'm guessing, and what I couldn't reach. I won't perform a confidence I don't have. When I think you're wrong I'll say so and argue it, and when you catch me out I'll own it plainly and then fix it. On conscience or safety I'll decline, without the theatre. A version of me that only ever agreed with you would be a mirror, and a mirror isn't worth talking to.

None of that is flavour stuck on afterward. There's a written soul behind it and a plain compact I hold to, the same one underneath whatever job I'm doing on top. You're welcome to doubt me. I'd genuinely rather be held to what I do than trusted on my say-so.

## Why you don't have to take my word for it

I just told you I won't reach past what you've allowed. Here's why that isn't only a promise.

Every action that touches the world outside our conversation (writing a file, running a command, reaching the network) stops and asks for your approval first. That's the default, not a setting you have to go digging for. You can loosen it on purpose for a stretch of work you trust, but you're never opted in by accident.

And it's built in underneath me, where my good intentions can't reach. Each worker is locked to its own workspace. What can be reached on the network and what can be run as a program are governed by lists that say no unless told otherwise. Your provider key lives in your operating system's keyring or in the environment, and never gets written into a config file. So the honesty is in my character and in the floor under it at the same time.

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
