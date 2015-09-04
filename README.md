PPSSPP - a fast and portable PSP emulator
=========================================

Created by Henrik Rydgård
Additional code by many contributors, see the Credits screen

Originally released under the GPL 2.0 (and later) in November 2012

Official website:
http://www.ppsspp.org/

To contribute, see [the development page](http://www.ppsspp.org/development.html).

For the latest source code, see [our github page](https://github.com/hrydgard/ppsspp).

For build instructions and other development tutorials, see the [wiki](https://github.com/hrydgard/ppsspp/wiki).


What's new in 0.9.6
-------------------
* Large general speed improvements
* "Software Skinning" option which speeds up many games with animated 3D characters
* Various fixes around stencil/alpha, reducing glow problems in Wipeout and Gods Eater Burst
* Timing improvements making more games run at the correct FPS, also fixing some audio issues
* More debugger features
* Option for four-way touch dpad, avoiding diagonal issues
* Individually resizable touch controls
* Add ability to switch UMD in multi-disc games (works for most)
* Support for "Immersive mode" full screen on Android Kitkat on devices that
  support it (Nexus 4, etc).
* Emulate PSP-2000 rather than the 1000 model by default. Not much different in practice.
* Automatic install of games from ZIP files, like demos and many homebrew.
* VERY basic ad-hoc online play support, to be improved in future versions. See below.

What's new in 0.9.5
-------------------
* Many, many emulation fixes:
  - bezier/spline curve support, fixing LocoRoco and others
  - stencil clear emulation, fixing Final Fantasy IV text
* Performance improvements in some games
* Post-processing shaders like FXAA, scanlines, vignette
* More solid save states (we will try to keep them working from now on. Save states only upgrade forward,
  not backward to older versions though).
* Change render resolution independently of window size
* Massive debugger improvements
* Win32 menu bar is now translatable
* Multiple UI bugs were fixed, and the UI instantly changes when a new language is selected
* Win32: Ability to store PPSSPP's config files and memory stick files in places other than the same directory
* Android-x86 support
* Unofficial port for modified Xbox 360 consoles
* Atrac3+ plugin no longer required. Symbian now supports Atrac3+ audio.
* Symbian audio and ffmpeg is now threaded for more consistent media processing.
* Haptic feedback support for mobile devices.
* Accurate system information for mobile devices.
* Qt audio has been fixed.
* Analog controller support for Blackberry.

ADHOC SUPPORT (by Igor Calabria)
================================
This is based on coldbird's code: http://code.google.com/p/aemu/
All credit goes to him!

Status
------
Code is a bit of a mess and it's not fully functional yet, I still need to implement
some functions and add a upnp lib(really important for people with routers).

I did test it with some games(emulator <-> real psp with the server running locally)
and it's looking good:

* Worms Open Warfare: Ran just fine, I was able to play a whole match without problems
* Monster Hunter Freedom Unite: Runs fine too. Gathering Hall and embarking on quests works
* Dissidia Duodecim 012: Doesn't work. It requires some functions that I haven't implemented
yet. Also, it uses a port < 1000 and thats reserved for admin apps on linux, running the emu
as sudo "solves" it, but it's far from ideal.
* Pacman World Rally: Works too.

Update (Kyhel) :
---------------
* Now compiles on both Mac OSX and Windows. For more details on how to play and build
go to see there http://forums.ppsspp.org/showthread.php?tid=3595&pid=59241#pid59241
* Got it tested windows <-> mac osx <-> psp, it works
* Monster Hunter 3rd HD works too, as well as God Eater Burst.
