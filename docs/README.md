# About StopMotionDigital

StopMotionDigital is a fork of [LeoCAD](https://github.com/leozide/leocad)
that adds stop-motion keyframe animation on top of LeoCAD's existing set
builder and minifig wizard. It's available for free under the GNU Public
License v2 and works on the Windows, Linux and macOS Operating Systems.

## What's new in this fork

An **Animate** dock (View > Toolbars > Animate) adds a stop-motion capture
workflow, styled after camera-capture tools like Eagle Animation, on top of
LeoCAD's existing per-step keyframe engine:

- **Capture Frame** — the main action. Pose your pieces, click it, and it
  inserts a new frame and snapshots every piece's position/rotation, like
  pressing the shutter on a stop-motion camera. No selecting objects or
  toggling a record mode first.
- **Onion Skin** — shows a small reference thumbnail of the previous frame
  next to the capture button, so you can judge how far to move something.
  (It's a side-by-side reference image, not a true overlay on the 3D view.)
- **Duplicate Frame** — holds the current pose for one more frame without
  capturing anything new (useful for holding a beat).
- **Delete Frame** — removes the current frame.
- A **filmstrip** along the bottom shows a thumbnail per frame; click one to
  jump to it.
- **Play** / **fps** — play back the animation in the viewport.
- **Export Animation...** renders the frame range to an animated GIF, MP4
  (via `ffmpeg` if it's on your `PATH`), or a PNG sequence.

Camera/light keyframing and the two-pass GIF palette optimization aren't
wired up yet — use the existing Properties dock keyframe checkboxes for
cameras/lights, and expect larger-than-optimal single-pass GIFs for now.

## Posing a minifig

A minifig isn't one object — it's several separate pieces (head, torso, arms,
hands, legs) that happen to sit at the same spot. Whether you can grab an arm
and rotate it on its own depends on how it was grouped when placed:

**New minifigs (Minifig Wizard):** the wizard's **Posable** checkbox is on by
default. With it checked, the wizard groups the minifig by limb assembly
instead of as one single blob — the head, torso, right arm (+ hand + held
item), left arm (+ hand + held item), right leg, and left leg are each their
own group. Click any part and its whole limb assembly is selected together;
rotate it and only that limb moves. A hand always stays grouped with its own
arm on purpose — they shouldn't be posed apart from each other.

**Minifigs placed with Posable off, or from an older save:** these come in as
one single group covering the entire minifig, so selecting any part selects
(and moves) the whole figure.

1. Click the minifig, then **Piece > Ungroup** (**Cmd+U** on Mac, **Ctrl+U**
   elsewhere) to dissolve it into individual, independently-selectable pieces.
2. Optionally, re-group the pieces that should move as one rigid unit (most
   commonly a hand with its own arm, and anything it's holding): select just
   those pieces and **Piece > Group** (**Cmd+G** / **Ctrl+G**).
3. Now select individual limbs/groups and use the **Rotate** tool (Tools
   toolbar) to pose them — rotation snap defaults to 1° for fine control.
4. Pose, then hit **Capture Frame** in the Animate dock.


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
