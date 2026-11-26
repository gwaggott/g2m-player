# Security policy

## Reporting a problem

Please report security problems privately, not in a public issue: use
**Report a vulnerability** on the repository's
[Security tab](https://github.com/gwaggott/g2m-player/security/advisories/new).

Include what's affected (version, OS, plugin format and host), how to
reproduce it and what it allows. A file that triggers it, such as a
crafted `.g2m` model, is the most useful thing to attach.

This is a one-person project, so replies may take a week or two. Once a
problem is confirmed I'll aim to fix it in the next release and credit
you in the advisory, unless you'd rather not be named.

## What's in scope

The player runs inside your audio host, so a bug in how it reads its
inputs runs with the host's permissions. The parts that read data from
outside are:

- `.g2m` model files, loaded from disk or bundled;
- the plugin state and presets a host saves and restores;
- MIDI arriving on the plugin's input.

A crash, memory corruption or hang caused by any of these is in scope.
Wrong notes, missed notes and audio glitches are ordinary bugs: open an
issue for those.

## Supported versions

Only the latest release, and `main`, get security fixes.
