# AGENTS.md — StopMotionDigital

Professional digital stop-motion LEGO animation software. Built on LeoCAD (C++/Qt, LDraw-based
virtual LEGO CAD) — rebranded "StopMotionDigital" throughout the UI. **Not** a general CAD tool:
no physics, no gravity, no camera simulation. Pure digital posing tool: position is data you set
per frame.

GitHub: `3xecutablefile/leocad-animator` (origin), forked from `leozide/leocad` (upstream).

## Workflow rules

- **Push directly to `master`.** No feature branches, no PRs. User explicitly overrode the global
  CLAUDE.md branch policy for this repo.
- **NEVER add `Co-Authored-By: Claude` (or any AI co-author trailer) to commits.** Previous
  session's history was rewritten to strip it. Just `git commit -m "..."`.
- Build and smoke-test locally before every commit:
  ```
  qmake6 leocad.pro          # only if build/ or Makefile is missing
  make -j$(sysctl -n hw.ncpu)
  ```
- Clean build artifacts before committing: `rm -f povray .qmake.stash Makefile && rm -rf build`,
  then restore `povray` from `/Applications/StopMotionDigital.app/Contents/MacOS/povray` (0-byte
  stub, gitignored, required by `leocad.pro`'s `QMAKE_BUNDLE_DATA` — without it qmake fails).
- After build, update the installed copy:
  `rm -rf /Applications/StopMotionDigital.app && cp -R build/release/StopMotionDigital.app /Applications/StopMotionDigital.app`
- Smoke test: `open build/release/StopMotionDigital.app` + `ps aux | grep StopMotionDigital` —
  confirms launch + no immediate crash. Does **not** verify 3D/matrix correctness (see below).
- User tests by actually using the app and reports real bugs. Don't claim features are verified
  when they haven't been seen in the viewport.

## Architecture

### Why a custom Animate system (not LeoCAD's native Steps)

LeoCAD's built-in `lcStep`/keyframe system tracks "the step a piece first appears in a building
guide." Early attempts to reuse it for animation failed (frame counts collapsing, deletes silently
no-oping, retroactive piece visibility). The Animate dock now owns a fully self-contained,
snapshot-based system:

- **`lcAnimateFrame`**: per-frame snapshot = `QMap<lcPiece*, lcVector3>` positions +
  `QMap<lcPiece*, lcMatrix33>` rotations + camera state (position/target/up/projection). Keyed
  by raw `lcPiece*` pointer — not `lcPiece::GetID()` (the LDraw part filename, which collides
  between e.g. left and right hands, both `3820.dat`).
- **`lcAnimateDocumentState`** per `lcModel*`: `std::vector<lcAnimateFrame> Frames`,
  `CurrentFrameIndex`, thumbnail cache, `AnimateForcedHidden` set, `lcAnimateMode` enum,
  `std::vector<lcKeyframePoint> Keyframes`.
- Piece mutations bypass LeoCAD Step-keying: `Piece->SetPosition(v, 1, false)` with an empty key
  list takes the fast path — plain value assignment. Step is always `1`.
- **`AnimateForcedHidden`**: `lcPiece::mHidden` is reused to hide pieces absent from the current
  frame. This set tracks which pieces the animation system hid, so it doesn't un-hide pieces the
  user hid manually via native Hide Selected.
- **Persistence**: companion `<file>.animate.json` next to the project file. Re-association after
  reload uses piece index (stable across a same-file round trip), not pointer identity.
- **Undo**: frame operations (Capture, Delete, Duplicate) are not undoable via Ctrl+Z (disclosed
  in comment at `CaptureClicked`). Piece-level operations (Walk Cycle, Mirror Pose, Attach to
  Hand) use `Model->RunInHistorySequence(tr("..."), [&](){ ... })`.

### `lcGroup::mMinifigFamily`

Tags the 6 per-limb-assembly groups of one Posable minifig as belonging together **without**
making them a parent/child group. A parent group would make a single click select the whole figure
— defeating Posable mode's purpose. This tag is only consulted by code explicitly looking for
"everything belonging to this minifig" (Alt+click, Mirror Pose sibling lookup, Walk Cycle piece
gathering). Do not repurpose `lcGroup` for this.

## Feature inventory (all in `common/` unless noted)

| Feature | Files | Notes |
|---|---|---|
| Animate dock UI (filmstrip, capture, play, export) | `lc_animatewidget.h/.cpp` | Core of the whole project |
| Camera capture/playback per frame | `lc_animatewidget.h/.cpp`, `camera.h` | Position/target/up saved per frame, restored during playback |
| Camera projection per frame | `lc_animatewidget.h/.cpp`, `camera.h` | `lcCameraProjection` saved in every frame/keyframe, persisted in `.animate.json`, restored on pose |
| Save/load persistence (`.animate.json`) | `lc_animatewidget.cpp`, `project.cpp` | Wired into `Project::Save/Load` |
| Onion skin preview (dock thumbnail) | `lc_animatewidget.cpp` (`RefreshOnionSkin`) | Small thumbnail of previous frame in the animate dock |
| Viewport onion skin overlay | `lc_view.cpp`, `lc_context.cpp` | Ghost pass in `OnDraw` at 50% alpha via `SetAlphaScale` in `FlushState` |
| Viewport ghost system API | `lc_view.h`, `lc_context.h` | `SetGhostFrame`/`ClearGhost` on views, `SetAlphaScale` on context |
| Socket Mode / Free Move toggle | `lc_mainwindow.h`, `lc_view.cpp` | Blocks click-drag-translate on a piece in a single-limb-group selection; multi-group selection bypasses |
| Posable minifig grouping | `lc_model.cpp` | 6 top-level groups (Head/Torso/RightArm/LeftArm/RightLeg/LeftLeg), named `"Minifig <Name> #N"` |
| Attach to Hand | `lc_animatewidget.cpp` | Reuses `MinifigWizard` hand-offset tables. Currently right-hand only |
| Mirror Pose | `lc_animatewidget.cpp` | **Legs only**. Copies rotation matrix between matched leg pieces (by `mPieceInfo`). Arms unsupported — mirrored parts differ |
| Alt+click select whole minifig | `lc_model.h/.cpp`, `lc_view.cpp` | Uses `mMinifigFamily` |
| Walk Cycle generator | `lc_animatewidget.cpp` | Dialog with gait (Walk/Jog/Run), stride angle, arm swing, speed slider, direction, travel distance readout. Reuses `MinifigWizard::SetAngle/Calculate`. Per-slot wizard mapping for correct arm→hand chains |
| Gait phase warping | `lc_animatewidget.cpp` | Walk = pure sine, Jog = peaky (2nd harmonic), Run = asymmetric phase modulation |
| Constant Keyframe mode | `lc_animatewidget.h/.cpp`, `lc_keyframetimelinewidget.h/.cpp` | Timeline widget, easing per-segment (Linear/EaseIn/EaseOut/EaseInOut), BakeKeyframes interpolator, mode selector (QComboBox) |
| Minifig Wizard "Posable" checkbox | `lc_minifigdialog.h/.cpp/.ui` | On by default |
| CI: rolling "continuous" macOS DMG | `.github/workflows/release.yml`, `.github/workflows/continuous.yml` | Pinned to `macos-14` |
| Rebrand LeoCAD → StopMotionDigital | ~15 files | Exceptions: `.lcd` binary magic bytes, internal function names, CI stdout still says "LeoCAD" |

### Known risk area: 3D transform math

This environment cannot render/watch the 3D viewport. Every matrix/rotation feature (camera
capture, Attach to Hand, Mirror Pose, Walk Cycle, Constant Keyframe interpolation) was built by
reasoning through matrix composition algebraically — not by visual confirmation.

Key established facts (do not re-derive):
- `lcMul(a, b)` = standard matrix product `a * b`; under row-vector convention, **"apply `a`
  first, then `b`."**
- `lcMatrix44AffineInverse` / `lcMatrix44Inverse` in `lc_math.h` (~line 1397/1412).
- Delta-matrix pattern: `NewMatrix = lcMul(lcMul(Delta, StartMatrix), Forward)` where
  `Delta = lcMul(WizardMatrixAtAngle, lcMatrix44AffineInverse(WizardMatrixAtNeutral))`.
- `MinifigWizard::Calculate()` (~line 320-480 in `minifig.cpp`) is the source of truth for
  per-part offset/rotation conventions. **Reuse this — do not re-derive formulas.**
- Hip swing axis is local X; foot travels along Y ("forward = +Y" in Walk Cycle).
- `lcQuaternionRotationX/Y/Z`, `lcQuaternionFromAxisAngle`, `lcQuaternionToAxisAngle`,
  `lcQuaternionMultiply`, `lcQuaternionMul` exist in `lc_math.h` (~line 1550-1600).
- `lcMatrix33ToQuaternion` and `lcQuaternionToMatrix33` were added this session.
- No `lcQuaternionSlerp` exists yet (needed for proper multi-axis rotation interpolation).

## COMPLETED

### Walk Cycle
- Dialog: gait preset, stride angle, arm swing, speed slider (1-10, 4-40 frames), direction
  (0-359 deg compass), total travel distance readout in LDU/studs.
- Per-slot wizard mapping: `FindPieceForSlot()` matches each piece's `mPieceInfo` against
  `Wizard->mSettings[Type]` to find RLEG/LLEG/RARM/LARM/RHAND/LHAND pieces.
- Hand-follows-arm: separate RHAND/LHAND delta computed when hand pieces are detected within arm
  groups, so hands move through the wizard's multi-joint chain instead of getting a group-level
  delta.
- Gait phase warping: Walk = pure sine, Jog = peaky (2nd harmonic `0.85*sin + 0.15*sin2`), Run =
  asymmetric (`sinf(Phase + 0.25*sinf(Phase))`).
- Crash fix: frames built locally via `push_back`, then `std::move` into `State.Frames` inside a
  minimal `RunInHistorySequence` wrapper (vector reallocation was crashing when `QMap`-backed
  elements moved during `insert`).
- Arm swing: opposite-phase via `MinifigWizard::SetAngle(LC_MFW_RARM/LC_MFW_LARM)`.
- Camera projection per frame: `SnapshotFrame` captures `Camera->GetProjection()`, restored by
  `lcPoseAnimateFrame`, persisted in `.animate.json`.

### Constant Keyframe Mode
- `lcAnimateMode` enum (`StopMotion` / `ConstantKeyframe`) + QComboBox mode selector.
- `lcKeyframePose` (same shape as `lcAnimateFrame` without per-piece-existence semantics).
- `lcKeyframePoint`: time index + pose + per-segment `lcEasingType`.
- `lcEasingType`: Linear, EaseIn (cubic), EaseOut (cubic), EaseInOut (cubic).
- `BakeKeyframes()`: sorts keyframes by time, iterates span, lerps positions, axis-angle lerps
  rotations, applies per-segment easing, lerps camera. Output is same `lcAnimateFrame` vector as
  Stop Motion — playback/export pipeline unchanged.
- `lcKeyframeTimelineWidget`: custom QWidget with diamond keyframe markers, easing labels per
  segment, 10-frame interval tick marks with labels, current-time cursor (red line), click-to-seek
  and click-to-select, drag-to-seek on the timeline, step forward/back (`<<` `>>`) buttons.
- Add/Delete keyframe buttons + easing combo per selected keyframe.
- `AddKeyframeClicked` uses `mTimelineWidget->GetCurrentTime()` (not `State.CurrentFrameIndex`).
- `SetKeyframes()` called after every mutation to avoid dangling pointer from `push_back`/`erase`.

## IN PROGRESS

### Ragdoll Death Animation Generator
Procedural stop-motion death/ragdoll animation for Posable minifigs. Selected minifig + dialog → frames: knockback, obstacle hit, bounce, settle in sprawled pose. **Not** real-time physics — deliberate stop-motion look.

**Stop-motion aesthetic** (NOT realistic physics):
- Pose holds: key poses held 2-3 frames (stop-motion animator style)
- Exaggerated poses: over-rotated limbs, overshoot positions
- Snap frames: occasional sudden position change between frames
- No smooth interpolation — each frame a deliberate pose
- Imperfect arcs: slightly jerky trajectories

**Segment breakdown** (24-frame default):
- Frames 0-3: Impact — body jerks back, limbs start to react
- Frames 3-8: Knockback — body flies in direction, limbs trail with delay
- Frames 8-12: Free fall — body arcs, limbs flail with phase-offset sine noise
- Frames 12-15: Hit obstacle/ground — sudden stop, limbs overshoot (inertia)
- Frames 15-17: Bounce — body compresses, rebounds slightly
- Frames 17-20: Second fall — settle toward rest
- Frames 20-24: Rest — minimal movement, limbs sprawled

**Per-group behavior**:
- **Torso** (root): piecewise trajectory segments + tumble rotation via quaternion. At rest: tilted ~45° back.
- **Head**: delayed follower (2-3 frame lag), whiplash snap on impact, at rest lolls to side.
- **Arms**: flail abduct ~120° on knockback, random sine wobble during fall, fling forward on impact, sprawl at rest.
- **Legs**: one buckles on knockback, scissors during fall, one straight + one folded at rest.

**Parameters**: Direction (0-359°), Knockback (1-20 studs), Fall height/slope, Frames (12-48, default 24). Presets: "Got shot" / "Fell off cliff" / "Punched" / "Explosion".

**No wizard reuse**: unlike Walk Cycle (which needs per-joint precision), ragdoll limbs get direct rotation values from noise/delay functions. `MinifigWizard` not consulted.

**Implementation**: new `RagdollClicked` in `lc_animatewidget.cpp`, reuses group detection, `RunInHistorySequence`, `SnapshotFrame`, local-vector→move pattern. Button in animate dock next to Walk Cycle.

## BACKLOG

- **Drag-to-move keyframes** on the Constant Keyframe timeline (currently click-to-seek only).
- **Rubber-band select** / right-click context menu on timeline.
- **Full quaternion SLERP** for `BakeKeyframes` rotation interpolation (axis-angle is exact for
  single-axis minifig rotations but approximate for multi-axis).
- **Left-hand Attach to Hand** (currently hardcoded to right-hand offset table).
- **Arm Mirror Pose** (currently legs-only; mirrored LDraw parts differ so direct matrix copy
  fails).
- **Minor rebrand gaps**: CI stdout still says "LeoCAD Continuous Build" (`lc_application.cpp:768`),
  LDraw library warning labels.
- **Auto-keyframe at time 0** / at end of frame range (user currently must add keyframes
  explicitly).
- **Walk cycle projection ghost**: semi-transparent ghost showing minifig position during walk cycle (adjustable distance).

## COMPLETED: Viewport ghost system (onion skin + walk cycle projection)

Implementation chosen: **Alpha scale via FlushState** (no shader modification needed).
- Alpha scale applied in `lcContext::FlushState()` by multiplying `MaterialColor.w` before upload.
- `lcView::SetGhostFrame()`/`lcView::ClearGhost()` store per-view ghost positions/rotations/alpha.
- `lcView::OnDraw()` does two-pass: ghost pass (save pieces → pose ghost → draw with `SetAlphaScale` + blending + no depth test → restore → clear depth) then normal pass.
- Onion skin toggle in the animate dock sets ghost frame data on the active view; updated on every frame navigation.
- Infrastructure shared by walk cycle projection ghost (same `SetGhostFrame`/`ClearGhost` API).
- Cannot be visually verified in this environment. Same risk as all matrix math features.
