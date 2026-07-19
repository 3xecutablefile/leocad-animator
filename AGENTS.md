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

### Full 8-angle code review + fix pass (all confirmed findings fixed)
Ran an 8-angle review (3 correctness, reuse, simplification, efficiency, altitude, conventions) over
the full `upstream/master...HEAD` diff. Every CONFIRMED/PLAUSIBLE finding was fixed:
- **`mMinifigFamily` was never persisted** (biggest find): `lcGroup::mMinifigFamily` is a runtime
  pointer with no serialization, so Walk Cycle/Ragdoll/Mirror Pose/Alt+click-select-minifig silently
  stopped working on ANY project after save+reopen. Fixed by writing/reading a new
  `0 !LEOCAD GROUP MINIFIG_FAMILY <family-group-name>` LDraw meta line right after each group's
  `GROUP BEGIN` line (`lc_model.cpp` `SaveLDraw`/`LoadLDraw`). Load side resolves names to pointers
  in a second pass after the whole file is parsed (`PendingMinifigFamily`), not assuming file order.
- **Ghost pass use-after-free**: `lc_view.cpp`'s `OnDraw` ghost pass dereferenced raw `lcPiece*` keys
  from `mGhostPositions`/`mGhostRotations` with no check they still exist in the model - fixed with a
  `LivePieces` set built each pass, skipping stale keys.
- **`RemoveEmptyGroups` could delete a still-referenced family-tag group**: it only counted piece
  membership and `mGroup` parent references, not `mMinifigFamily` references from sibling groups -
  fixed by treating "referenced as another group's family tag" as unconditionally non-removable.
- **Auto-keyframe digest was widget-global, not per-model**: `mAutoKeyframeInitialized`/
  `mLastAutoKeyframeDigest` lived on `lcAnimateWidget` itself; switching the active model compared
  the new model's live pieces against the OLD model's stale digest, spuriously inserting a bogus
  keyframe. Moved into `lcAnimateDocumentState` (per-model).
- **`DuplicateClicked` missing empty-`Frames` guard**: `DeleteClicked` had one, `DuplicateClicked`
  didn't - reachable via Clear/Delete Keyframe dropping `State.Frames` to empty while the Duplicate
  button stays visible in Constant Keyframe mode. Added the guard.
- **Walk Cycle / Ragdoll's `RunInHistorySequence` was a no-op**: piece mutations happened in a loop
  *before* `RunInHistorySequence` was called, so `BeginEditHistory`'s `SaveStartState` snapshotted
  the pieces already at their final generated pose; the callback itself never touched piece state
  (net piece state returns to `NeutralFrame` via `ApplyFrame(..., 0)` at the end anyway) - so nothing
  was ever pushed to the undo stack despite the misleading `tr("Walk Cycle")` description. Removed
  the wrapper, added the same honest "not undoable via Ctrl+Z" disclosure Capture/Duplicate/Delete
  Frame already have.
- **`BakeKeyframes` O(FrameCount × KeyframeCount) bracket search**: re-scanned all keyframes for
  every output frame despite `Keyframes` being sorted - replaced with a single advancing cursor
  (O(FrameCount + KeyframeCount)). Also removed two dead no-op loops and added a `CurrentFrameIndex`
  clamp so every `BakeKeyframes` caller (not just `ModeChanged`) keeps the index in bounds.
- **Export dialog crash on empty `Frames`**: `lcAnimateExportDialog`'s Start/End spin boxes collapse
  to a degenerate range when `Frames` is empty (reachable via Clear/Delete Keyframe), and the render
  loop indexed the empty vector out of bounds. `ExportClicked` now refuses to open the dialog when
  `State.Frames` is empty; `Accept()` also guards directly.
- **Ragdoll's ghost preview was missing the QImage fallback** Walk Cycle's preview has (added in the
  same commit family specifically because the OpenGL ghost pass alone isn't reliable) - both dialogs'
  preview lambdas now share one `ShowMinifigProjectionGhost` helper, which also killed ~90 lines of
  duplicated save/mutate/render/restore code between them.
- `SetAlphaScale`'s `mAlphaScaleDirty` flag was set but never read in `lcContext::FlushState` - wired
  into the dirty-check gate.
- Duplicated "convert Frames into Keyframes" block (WalkCycleClicked/RagdollClicked, ~17 lines each)
  factored into one `RebuildKeyframesFromFrames` helper.

### smd_mcp: registration + core parsing bugs (see MCP section below for details)
The MCP server was never actually usable end to end: not registered for Claude Code at all (only
`.opencode.jsonc` existed), `create_project` crashed if the target directory didn't exist yet, and -
the critical one - the LDraw group parser was looking for a syntax (`0 GROUP <n> <name>`) that
StopMotionDigital's own C++ save code has never written (real format: `0 !LEOCAD GROUP BEGIN
<name>`), so `read_project`/`generate_walk_cycle`/`generate_ragdoll` saw **zero groups** on every
real app-saved project. Fixed; see "MCP Server" section for the full breakdown.

### Ragdoll Death Animation Generator (v3 — joint-articulated rewrite)
Procedural stop-motion death/ragdoll animation for Posable minifigs. Selected minifig + dialog →
frames: knockback, obstacle hit, bounce, settle in sprawled pose. **Not** real-time physics —
deliberate stop-motion look.

**v1/v2 problem** (user: "why does a minifig combust on death anim", "should unsocket parts", "death
animations are the same"): every limb GROUP (e.g. forearm + hand together) rotated as one rigid body
around the body's center of mass via a raw single-axis quaternion spin, with full-intensity noise
regardless of how hard the hit was. That reads as a rigid slab orbiting a point, not an articulated
fall, and looked identical in kind for "Got shot" and "Explosion" alike.

**v3 fix**:
- Arms/legs now swing from their real shoulder/hip pivot via `MinifigWizard::SetAngle` (same math
  Walk Cycle uses) — `RArmDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_RARM], RArmNeutralInv)`,
  applied to the piece's start pose *before* the whole-body tumble transform, so articulation and
  tumble compose correctly instead of both fighting for the same pivot.
- Head pivots on the wizard's neck position (`Matrices[LC_MFW_HEAD].GetTranslation()`), not body
  center; still free-axis rotation (no single-DOF wizard angle suits a backward head flop).
- **`Intensity` derived from Knockback** (`qBound(0.2, Knockback/10, 1.0)`), scales peak flail/splay/
  headLag angles, noise amplitude, and per-piece scatter jitter. A "Got shot" death (small Knockback)
  gets a controlled buckle-and-collapse; "Explosion" (large Knockback) keeps the full flail.
- Per-piece jitter (`PerPieceScatterJitter`, one random vector per piece) added on top of the shared
  per-group scatter direction, so multi-piece limbs visibly separate piece-from-piece at impact
  instead of moving as one rigid block — the closest approximation to "unsocketing" without a real
  joint graph.
- Presets rebalanced: Punched=3 studs, **Got shot=0.5 studs** (mostly a vertical collapse, was 12),
  Fell off cliff=1.5 studs (height-dominant), Explosion=20 studs (unchanged, full chaos is correct
  here). Default Knockback lowered from 8 to 2 studs.

**Implementation**: `RagdollClicked` in `lc_animatewidget.cpp`. `RunInHistorySequence` wrapper was
removed - it was a no-op, see the review-fix-pass entry above.

## IN PROGRESS / BROKEN

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

## MCP Server — AI animation agent (`smd_mcp/`)

An MCP server at `smd_mcp/` lets AI agents (Claude Code, opencode, etc) create
and manipulate StopMotionDigital animations by editing `.ldr` + `.animate.json`
files directly. Registered for Claude Code via `.mcp.json` at the repo root
(auto-discovered, no setup needed) and for opencode via `.opencode.jsonc`, both
under the name `smd`. See `smd_mcp/README.md` for registering it elsewhere.

### Piece positioning — must use `MinifigWizard::Calculate()` math

**Never hand-place minifig pieces.** The C++ `MinifigWizard::Calculate()` in
`common/minifig.cpp:317-486` is the only correct source for piece transforms.
The wizard chain is:

```
Root = Translate(0, 0, 72)

BODY  = Offset_BODY  × Root
NECK  = Offset_NECK  × Root                         (optional)
HEAD  = Offset_HEAD  × RotZ(-angle) × Trans(0,0,24) × Root
HATS  = Offset_HATS  × RotZ(-angle) × HEAD          (optional)
HATS2 = Offset_HATS2 × RotX(-angle) × HATS          (optional)

RARM  = Offset_RARM × RotX(-angle) × RotY(-9.791°) × Trans(15.552,0,-8.88) × Root
RHAND = Offset_RHAND × RotY(-angle) × RotX(45°) × Trans(5,-10,-19) × RARM
LARM  = Offset_LARM × RotX(-angle) × RotY(9.791°) × Trans(-15.552,0,-8.88) × Root
LHAND = Offset_LHAND × RotY(-angle) × RotX(45°) × Trans(-5,-10,-19) × LARM

BODY2 (= hips) = Offset_BODY2 × Trans(0,0,-32) × Root
RLEG  = Offset_RLEG × RotX(-angle) × Trans(0,0,-44) × Root
LLEG  = Offset_LLEG × RotX(-angle) × Trans(0,0,-44) × Root
```

Where `Offset_<TYPE>` is the piece's per-type offset matrix from
`resources/minifig.ini`. The offset for the default part is always identity
for standard pieces (the 0-th entry in each `mSettings[Type]` array).

**Implications for AI agents:**
- **Never compute positions manually.** Call the MCP tool `animate_transform`,
  `generate_walk_cycle`, `set_frame_piece`, or the Python wizard chain in
  `smd_mcp/server.py` (`_wizard_matrix()` + `_piece_settings_offset()`).
- The Python `_wizard_matrix()` in `server.py` mirrors `Calculate()` exactly
  (including `minifig.ini` offsets).
- **Always include both `973.dat` (torso) and `3815.dat` (hips)** in the Torso
  group — they both go at the same root position but are separate pieces.
- Left/right legs share the same position offset from root — their geometry
  already encodes left/right stance. Leg angle = `RotX(-angle)` applied at
  `Trans(0,0,-44)`.

### Minifig identification

Group names do **not** encode a custom figure name - the app names them just
`"Minifig <Assembly> #N"` (e.g. `"Minifig Right Arm #1"`, auto-numbered), so
there is no `<Name>` to match against. What actually links a minifig's 6
per-limb groups together is `lcGroup::mMinifigFamily`, a runtime C++ pointer
persisted in the `.ldr` as a `0 !LEOCAD GROUP MINIFIG_FAMILY <family-group-
name>` line right after that group's `GROUP BEGIN` line (see `lc_model.cpp`
`SaveLDraw`/`LoadLDraw`) - the family group is whichever of the 6 was created
first, and every sibling (including that first one, self-referencing) points
at it by name.

The MCP server's `LDrawGroup.minifig_family` field holds that **family
group's name string** (e.g. `"Minifig Torso #1"`) - this is also the
`minifig_name` identifier `generate_walk_cycle`/`generate_ragdoll` take.
`get_project_status`'s `"minifigs"` list surfaces the available identifiers
for a project, since there's no human-readable name to guess.

Typical limb piece contents (for reference, not how minifigs are identified):
- Torso contains `973.dat` + `3815.dat`
- Head contains `3626bp01.dat` (and optional hats)
- RightArm contains `3819.dat` + `3820.dat`
- LeftArm contains `3818.dat` + `3820.dat`
- RightLeg contains `3817.dat`
- LeftLeg contains `3816.dat`

**Building a minifig via `add_piece`**: pass the *same* `minifig_family`
string on every one of the 6 groups' first `add_piece` call (conventionally
the first group's own `group_name`) - see `add_piece`'s docstring in
`server.py`.

### Walk cycle

Use `generate_walk_cycle` tool. It implements the full `Calculate()` chain
with gait warping. Gait options: Walk, Jog, Run. Stride angle default 25°.

### Camera

Camera data per frame: `{position, target, up, projection}`. Position/target
use LeoCAD internal coordinates (X=right, Y=forward, Z=up). Default camera:
`position=[0,-120,80] target=[0,0,72] up=[0,0,1]` — behind the action,
elevated, looking at waist height.

For dynamic scenes, set camera on keyframes and use `interpolate_frames`
or `bake_keyframes` to fill intermediate camera positions.

### Rendering

The `export_video` tool provides instructions. To actually render:
```
uv run python -m smd_mcp.render_frames
```
This reads `~/Desktop/fight.ldr` + `fight.animate.json`, renders each frame
via LeoCAD CLI (`-i` + `--camera-position`), and stitches with ffmpeg.

### All MCP tools (30)

| Tool | What it does |
|---|---|
| `read_project` | Parse .ldr, return pieces + groups + frames |
| `get_project_status` | Summary: piece/frame/minifig counts |
| `create_project` | New empty .ldr + .animate.json |
| `add_piece` | Add a piece to the scene |
| `capture_frame` | Snapshot current .ldr positions as a new frame |
| `get_frame` | Read a frame's piece/camera data |
| `list_frames` | List all frame indices |
| `set_frame_piece` | Set position/rotation for one piece in one frame |
| `set_frame_camera` | Set camera for one frame |
| `duplicate_frame` | Copy frame to end |
| `delete_frames` | Remove frames by index |
| `reverse_frames` | Reverse frame order |
| `loop_animation` | Create a seamless loop |
| `time_remap_frames` | Stretch/squeeze frame timing |
| `interpolate_frames` | Smooth between two keyframes |
| `stagger_pieces` | Offset piece motion across frames |
| `animate_transform` | Animate piece with easing across frame range |
| `ease_frames` | Apply easing to a range |
| `follow_curve` | Bezier path animation |
| `bake_keyframes` | Interpolate sparse keyframes into full animation |
| `bounce_effect` | Physics bounce on pieces |
| `swing_effect` | Pendulum swing |
| `wave_effect` | Sequential wave across pieces |
| `shake_effect` | Camera/piece shake |
| `explosion_effect` | Scatter + spin pieces outward |
| `randomize_frame` | Add noise to positions/rotations |
| `generate_walk_cycle` | Walk/jog/run for a named minifig |
| `generate_ragdoll` | Death/fall animation for a named minifig |
| `batch_transform_pieces` | Move/rotate groups of pieces at once |
| `export_video` | Instructions for rendering to MP4 |

### Agent workflow for a scene

1. `create_project` — set up .ldr + .animate.json
2. `add_piece` for each minifig piece, OR write .ldr directly with groups
3. `capture_frame` — snapshot initial pose as frame 0
4. `generate_walk_cycle` / `set_frame_piece` / `animate_transform` — build
   key poses
5. `interpolate_frames` or `bake_keyframes` — fill between keyframes
6. `set_frame_camera` on key frames for dynamic camera
7. Render via `uv run python -m smd_mcp.render_frames`

### Known issues

- The `_wizard_matrix` in `server.py` must be kept in sync with
  `MinifigWizard::Calculate()` and `resources/minifig.ini`. If C++ code
  changes, update the Python copy.
- `export_video` doesn't actually render — it returns instructions. Use
  `render_frames.py` or open the project in StopMotionDigital and use
  File → Export Animation.
- Camera in `render_frames.py` uses `--camera-position` with LeoCAD internal
  coords (X=right, Y=forward, Z=up). Must match frame data format.

## Relevant Files

- `smd_mcp/smd_mcp/server.py`: MCP server with all 30 tools, wizard chain,
  walk cycle, ragdoll, effects
- `smd_mcp/smd_mcp/animate_fight.py`: Example — generates 120-frame fight
  scene for two minifigs
- `smd_mcp/smd_mcp/render_frames.py`: Renders .animate.json → PNG frames →
  ffmpeg MP4
- `smd_mcp/pyproject.toml`: uv project config, entry point `smd-mcp`
- `.opencode.jsonc`: MCP server registration (`type: "local"`)
- `common/minifig.cpp`: `Calculate()` — source of truth for piece transforms
- `resources/minifig.ini`: Per-piece-type offset matrices
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
