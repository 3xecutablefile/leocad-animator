# About LeoCAD Animator

LeoCAD Animator is a fork of [LeoCAD](https://github.com/leozide/leocad) that
adds stop-motion keyframe animation on top of LeoCAD's existing set builder
and minifig wizard. It's available for free under the GNU Public License v2
and works on the Windows, Linux and macOS Operating Systems.

## What's new in this fork

An **Animate** dock (View > Toolbars > Animate) adds a frame timeline on top
of LeoCAD's existing per-step keyframe engine:

- Scrub or play back frames (steps) at a chosen fps.
- **Record** toggles LeoCAD's existing "Add Keys" mode, so moving/rotating a
  piece automatically keys it at the current frame.
- **+ Frame** inserts a new blank frame after the current one.
- **Add Keyframe** keys the position/rotation of the current selection at the
  current frame without needing to move it first.
- **Export Animation...** renders the frame range to an animated GIF, MP4
  (via `ffmpeg` if it's on your `PATH`), or a PNG sequence.

Camera/light keyframing and the two-pass GIF palette optimization aren't
wired up yet — use the existing Properties dock keyframe checkboxes for
cameras/lights, and expect larger-than-optimal single-pass GIFs for now.


# Installation

You can download the latest version of LeoCAD and its Parts Library from
the main website at https://www.leocad.org

It's recommended that you install the latest drivers for your video card
since LeoCAD uses OpenGL to take advantage of hardware acceleration for
rendering.

## LeoCAD for Windows

  Download the latest LeoCAD-Windows.exe to your computer, double click on
  the icon to launch the installer and follow the instructions.

## LeoCAD for Linux

There are multiple ways to install LeoCAD on Linux.

* Snap Store
  
  You can find LeoCAD in your distribution's Snap Store. Alternatively, you
  can enter `sudo snap install leocad` to install it using the command line.

* AppImage

  Download the latest LeoCAD-Linux.AppImage, make the file executable
  (`chmod +x`) and run it.

* Flatpak

  You can also install LeoCAD releases as a Flatpak from Flathub:
    https://flathub.org/apps/details/org.leocad.LeoCAD

  Note: there might be a delay for new releases to appear there. If
  you have it already installed, it will be updated.

* From source

  If you prefer to compile LeoCAD yourself, go to the GitHub releases page
  at https://github.com/leozide/leocad/releases/latest and download the
  source archive from there. If you do not already have a Parts Library
  installed, you will need to download one and follow the installation
  instructions. More information on how to compile your own executable is
  available in the Documentation section of https://www.leocad.org

## LeoCAD for macOS

  Download the latest LeoCAD-macOS.dmg to your computer, double click on
  the icon to open the archive, copy LeoCAD.app to your Applications folder
  and then launch it from there.

New users should read the online tutorial located at
https://www.leocad.org/docs/tutorial1.html to learn how to use LeoCAD.


# Online Resources

- Website:
  https://www.leocad.org

- GitHub page:
  https://github.com/leozide/leocad

- Unstable builds:
  https://github.com/leozide/leocad/releases/tag/continuous


# Legal Disclaimer

LEGO(R) is a trademark of the LEGO Group of companies which does not sponsor,
authorize or endorse this software.
