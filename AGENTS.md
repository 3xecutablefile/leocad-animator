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
| Viewport ghost/onion skin system | `lc_view.h/.cpp`, `lc_context.h/.cpp` | Ghost pass in `OnDraw` at reduced alpha via `SetAlphaScale` in `FlushState`. **Known issue**: dock thumbnail exists but users expect viewport overlay as primary onion skin method |
| Viewport ghost system API | `lc_view.h`, `lc_context.h` | `SetGhostFrame`/`ClearGhost` on views, `SetAlphaScale` on context |
| Socket Mode / Free Move toggle | `lc_mainwindow.h`, `lc_view.cpp` | Blocks click-drag-translate on a piece in a single-limb-group selection; multi-group selection bypasses |
| Posable minifig grouping | `lc_model.cpp` | 6 top-level groups (Head/Torso/RightArm/LeftArm/RightLeg/LeftLeg), named `"Minifig <Name> #N"` |
| Attach to Hand | `lc_animatewidget.cpp` | Reuses `MinifigWizard` hand-offset tables. Currently right-hand only |
| Mirror Pose | `lc_animatewidget.cpp` | **Legs only**. Copies rotation matrix between matched leg pieces (by `mPieceInfo`). Arms unsupported — mirrored parts differ |
| Alt+click select whole minifig | `lc_model.h/.cpp`, `lc_view.cpp` | Uses `mMinifigFamily` |
| Walk Cycle generator | `lc_animatewidget.cpp` | Dialog with gait (Walk/Jog/Run), stride angle, arm swing, direction, travel distance readout. Reuses `MinifigWizard::SetAngle/Calculate`. Per-slot wizard mapping for correct arm→hand chains |
| Gait phase warping | `lc_animatewidget.cpp` | Walk = pure sine, Jog = peaky (2nd harmonic), Run = asymmetric phase modulation |
| Walk cycle projection ghost | `lc_animatewidget.cpp` | Shows end position ghost in viewport while dialog is open; updates on parameter changes; cleared on dialog close |
| Ragdoll Death generator dialog ghost | `lc_animatewidget.cpp` | Shows end position ghost in viewport while dialog is open |
| Constant Keyframe mode | `lc_animatewidget.h/.cpp`, `lc_keyframetimelinewidget.h/.cpp` | Timeline widget, easing per-segment (Linear/EaseIn/EaseOut/EaseInOut), BakeKeyframes interpolator, mode selector (QComboBox). Default easing is now Linear |
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

### Viewport ghost system (onion skin + projection ghosts)
- **Ghost hidden state fix**: ghost pass in `OnDraw` now saves/restores `mHidden` state in addition
  to position/rotation — previously, pieces hidden by `lcPoseAnimateFrame` stayed hidden during the
  ghost pass, making ghosts invisible for pieces that only existed in the previous frame.
- Alpha scale applied in `lcContext::FlushState()` by multiplying `MaterialColor.w` before upload.
- `lcView::SetGhostFrame()`/`lcView::ClearGhost()` store per-view ghost positions/rotations/alpha
  and call `Redraw()` to trigger viewport repaint (was storing data but never telling the widget to
  redraw).
- `lcView::OnDraw()` does two-pass: ghost pass (save pieces → un-hide pieces in ghost set → pose
  ghost → draw with `SetAlphaScale` + blending + no depth test → restore hidden/position/rotation
  → clear depth) then normal pass.
- Projection ghosts added for Walk Cycle dialog and Ragdoll Death dialog (show final position in
  viewport while dialog is open, update on parameter changes, clear on dialog close).

### Walk Cycle
- Dialog: gait preset (Walk/Jog/Run), stride angle, arm swing, direction (0-359 deg compass),
  travel distance readout in LDU/studs.
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
- Walk direction fixed: `ForwardAxis` negated so 0° = minifig's natural forward direction.
- Speed slider removed: `Steps` hardcoded to 24 (stop-motion standard).
- Projection ghost: computed end position from dialog params, shown as ghost on viewport, updated
  on every parameter change, cleared on dialog close.

### Constant Keyframe Mode
- `lcAnimateMode` enum (`StopMotion` / `ConstantKeyframe`) + QComboBox mode selector.
- `lcKeyframePose` (same shape as `lcAnimateFrame` without per-piece-existence semantics).
- `lcKeyframePoint`: time index + pose + per-segment `lcEasingType`.
- `lcEasingType`: Linear, EaseIn (cubic), EaseOut (cubic), EaseInOut (cubic).
- `BakeKeyframes()`: sorts keyframes by time, iterates span, lerps positions, axis-angle lerps
  rotations, applies per-segment easing, lerps camera. Output is same `lcAnimateFrame` vector as
  Stop Motion — playback/export pipeline unchanged.
- **Default easing changed to `Linear`** (was `EaseInOut`) — matches the "Constant Keyframe" name;
  EaseInOut caused "wait then jump" behavior for keyframes far apart.
- `lcKeyframeTimelineWidget`: custom QWidget with diamond keyframe markers, easing labels per
  segment, 10-frame interval tick marks with labels, current-time cursor (red line), click-to-seek
  and click-to-select, drag-to-seek on the timeline, step forward/back (`<<` `>>`) buttons.
- Add/Delete keyframe buttons + easing combo per selected keyframe.
- `AddKeyframeClicked` uses `mTimelineWidget->GetCurrentTime()` (not `State.CurrentFrameIndex`).
- `SetKeyframes()` called after every mutation to avoid dangling pointer from `push_back`/`erase`.
- **Clear All button** (`ClearKeyframeClicked`) deletes all keyframes and frames at once.
- **Timeline wider**: `SetFrameRange` pads end to `max(End-Start, 200)`; `setFixedHeight(50)` →
  `setMinimumHeight(64)`.

## IN PROGRESS / BROKEN

### Ragdoll Death Animation Generator
Procedural stop-motion death/ragdoll animation for Posable minifigs. Selected minifig + dialog →
frames: knockback, obstacle hit, bounce, settle in sprawled pose. **Not** real-time physics —
deliberate stop-motion look.

**Current state**: v2 implementation with:
- **Randomized per run**: direction jitter (±10°), knockback distance (0.75-1.25x), fall height
  (0.7-1.3x), impact timing (55-65% into animation), per-frame noise amplitudes and phases
- **Unsocketed limbs**: per-group scatter direction vectors, limbs fly off independently at impact
  with quadratic trajectory (fast initial fling, then coast)
- **Ground clamp**: z drops to 0 after impact; bounce impulse at hit moment
- Per-group rotation noise with randomized frequency/amplitude per frame

**Still broken per user**:
- "Death animations are the same" — v2 adds randomization but user hasn't tested it yet
- "Don't adhere to physics" — body trajectory is piecewise polynomial, no genuine physics
  simulation (intentional stop-motion, not real physics; but may need better ground interaction)
- "Should unsocket parts" — v2 adds scatter offsets but limbs may not separate convincingly

**Parameters**: Direction (0-359°), Knockback (1-20 studs), Fall height/slope, Frames (12-48,
default 24). Presets: "Punched" / "Got shot" / "Fell off cliff" / "Explosion".

**No wizard reuse**: unlike Walk Cycle (which needs per-joint precision), ragdoll limbs get direct
rotation values from noise/delay functions. `MinifigWizard` not consulted.

**Implementation**: `RagdollClicked` in `lc_animatewidget.cpp`, reuses group detection,
`RunInHistorySequence`, `SnapshotFrame`, local-vector→move pattern. Button in animate dock next to
Walk Cycle.

### Viewport onion skin (replacing dock thumbnail)
- Current dock shows a 120x90 QLabel thumbnail (`mOnionSkinPreview`) rendered via
  `RenderFrameThumbnail`. User wants this replaced with viewport ghost overlay instead.
- The ghost system already exists (`SetGhostFrame`/`ClearGhost` + ghost pass in `OnDraw`). The
  onion skin toggle currently drives BOTH the dock thumbnail AND calls `SetGhostFrame`. After the
  hidden-state fix, viewport ghosts SHOULD now work for all pieces (including previously-hidden
  ones).
- **Next**: remove dock thumbnail, make onion skin checkbox purely drive viewport ghost.

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

## Relevant Files

- `common/lc_animatewidget.cpp`: WalkCycleClicked, RagdollClicked, BakeKeyframes, RefreshFilmstrip,
  projection ghost lambdas
- `common/lc_animatewidget.h`: lcKeyframePoint (default easing now Linear), lcAnimateDocumentState,
  lcEasingType, ApplyEasing
- `common/lc_view.cpp`: ghost pass in OnDraw (line ~877) — saves/restores hidden state +
  position/rotation
- `common/lc_view.h`: SetGhostFrame/ClearGhost with Redraw()
- `common/lc_context.cpp`: FlushState applies mAlphaScale to MaterialColor.w
- `common/lc_keyframetimelinewidget.cpp`: SetFrameRange pads to 200; min height 64
- `common/piece.h`: IsHidden/SetHidden used for ghost hidden state
