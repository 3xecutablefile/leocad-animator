# AGENTS.md — StopMotionDigital (leocad-animator)

This file exists so a fresh agent (or a future session) can pick up this project with zero prior
context. Read this fully before touching code.

## What this project is

A fork of LeoCAD (C++/Qt, LDraw-based virtual LEGO CAD) repurposed into a digital stop-motion LEGO
animation app, rebranded "StopMotionDigital" throughout the UI. GitHub: `3xecutablefile/leocad-animator`
(origin), forked from `leozide/leocad` (upstream). Original plan was 3 tabs (Minifig Maker / Set
Builder / Animate); LeoCAD already provides the first two, so almost all of this project's original
work is the **Animate dock** (`common/lc_animatewidget.h/.cpp`) plus supporting changes elsewhere.

There is no physics, no gravity, no camera — this is purely a digital posing tool. "The minifig
won't fall over" because nothing simulates that; position is just data you set per frame.

## Non-negotiable workflow rules for this repo (override the user's global CLAUDE.md)

- **Push directly to `master`. No feature branches, no PRs.** The user explicitly overrode their
  own global branch/PR policy for this repo ("nigga from now on only push to master"). Do not
  create branches here.
- **NEVER add `Co-Authored-By: Claude` (or any AI co-author trailer) to commits.** The user was
  emphatic about this and had prior history rewritten (`git filter-branch --msg-filter`) to strip
  it once already. Just `git commit -m "..."` with no trailer, ever.
- Build and smoke-test locally (launch the app, confirm it doesn't crash) before every commit.
  Qt 6.8.3 is installed via Homebrew at `/opt/homebrew`. Build commands:
  ```
  cd /Users/NotSmartMan/leocad
  qmake6 leocad.pro          # only needed if build/ or Makefile is missing
  make -j$(sysctl -n hw.ncpu)
  ```
- Clean build artifacts before committing (`rm -f povray .qmake.stash Makefile && rm -rf build`),
  but **first** copy the root `povray` file back from an existing built app bundle
  (`/Applications/StopMotionDigital.app/Contents/MacOS/povray` — it's a 0-byte stub, gitignored,
  required by `leocad.pro`'s `QMAKE_BUNDLE_DATA` or qmake fails with "no rule to make target
  povray"). Don't just `rm -f povray` without restoring it after the final build of a session.
- After a successful build, this session has been updating the installed copy too:
  `rm -rf /Applications/StopMotionDigital.app && cp -R build/release/StopMotionDigital.app /Applications/StopMotionDigital.app`
- `open build/release/StopMotionDigital.app` + `ps aux | grep StopMotionDigital` is the smoke test
  used throughout — confirms it launches and stays running. This does **not** verify visual/3D
  correctness — there is no way in this environment to actually watch the 3D viewport. Flag that
  limitation explicitly when shipping anything involving matrix/rotation math (see "Known risk
  area" below) rather than silently claiming it's verified.

## Architecture: why the Animate system is NOT LeoCAD's native Step/keyframe system

LeoCAD natively has a per-`lcStep` building-instructions keyframe system (`lcModel::GetLastStep()`,
`InsertStepAction`, `mStepShow`/`mStepHide`, etc.). Early versions of this project tried to reuse
it for animation and hit a wall of bugs (frame counts collapsing, deletes silently no-oping, newly
posed pieces appearing in earlier frames retroactively) because that system's semantics are "the
step a piece first appears in a building guide," not "a captured animation frame." **This was
deliberately abandoned.** The Animate dock now owns a fully self-contained, parallel
snapshot-based system instead:

- `struct lcAnimateFrame` (`lc_animatewidget.h`): a captured frame = `QMap<lcPiece*, lcVector3>`
  positions + `QMap<lcPiece*, lcMatrix33>` rotations for every piece, plus camera
  position/target/up (`HasCamera` flag). Keyed by raw `lcPiece*` pointer — **not**
  `lcPiece::GetID()`, which is the LDraw part filename and collides between e.g. left/right hands
  (both literally the part `3820.dat`).
- `struct lcAnimateDocumentState`: one per `lcModel*` (`QMap<lcModel*, lcAnimateDocumentState>` in
  `lcAnimateWidget`), because `lcGetActiveModel()` is not stable — it switches when entering/
  exiting a submodel in place. Holds `std::vector<lcAnimateFrame> Frames`, `CurrentFrameIndex`,
  a thumbnail `QMap<int, QIcon>` cache, and `QSet<lcPiece*> AnimateForcedHidden`.
- Piece mutations bypass LeoCAD's Step-keying entirely: `Piece->SetPosition(v, Step, /*AddKey=*/false)`
  with an empty key list takes `lcObjectProperty<T>::ChangeKey`'s fast path, which is just a plain
  value assignment. Step is conventionally always `1` throughout this system — there is no real
  use of multiple LeoCAD steps.
- `lcPiece::mHidden` is reused to hide pieces absent from the current frame (so "add a piece at
  frame 7" doesn't show it retroactively at frame 1), but this flag is shared with LeoCAD's native
  Hide/Show Selected feature. Resolved via `QSet<lcPiece*> AnimateForcedHidden` — the animate
  system only ever un-hides a piece it hid itself, never one the user hid manually.
- **Persistence**: animation data is NOT part of the LDraw/binary file format. It's written to a
  companion `<file>.animate.json` next to the project file (`SaveAnimationData`/
  `LoadAnimationData` in `lc_animatewidget.cpp`, hooked into `Project::Save`/`Project::Load` in
  `project.cpp`). Re-association after reload uses **piece index** (stable across a same-file
  round trip since LDraw piece order is stable), not pointer identity (pointers don't survive
  reload).
- Undo: `lcModel::BeginHistorySequence`/`EndHistorySequence`/`BeginEditHistory`/`EndEditHistory`
  are **protected**, and they generically diff whole-model piece/group/camera/light state — they
  do nothing useful for something that only mutates the Animate widget's own frame list (Capture/
  Duplicate/Delete Frame are honestly **not** undoable via Ctrl+Z; this is disclosed in a comment
  at the top of `CaptureClicked`, not silently broken). For operations that genuinely DO mutate
  real piece state (Attach to Hand, Mirror Pose, Walk Cycle), use the public wrapper added for
  this purpose:
  ```cpp
  Model->RunInHistorySequence(tr("Description"), [&]() { /* piece mutations */ });
  ```
  (declared in `lc_model.h`, implemented in `lc_model.cpp` right after `SetObjectsKeyFrame`).

## Feature inventory (what's built, and where)

All in `common/` unless noted.

| Feature | Files | Notes |
|---|---|---|
| Animate dock UI (filmstrip, capture, play, onion skin, export) | `lc_animatewidget.h/.cpp` | Core of the whole project |
| Camera capture/playback per frame | `lc_animatewidget.h/.cpp`, `camera.h` (added `GetPosition/GetTargetPosition/GetUpVector` getters) | `lcView::GetCamera()` always returns a valid camera |
| Save/load persistence (`.animate.json`) | `lc_animatewidget.cpp`, `project.cpp` | Fixed a real "animation lost on reopen" bug — was never wired up before |
| Socket Mode / Free Move toggle | `lc_mainwindow.h` (flag), `lc_view.cpp` (`UpdateTrackTool`, ~line 2149) | Blocks click-drag-translate on a piece in a `"Minifig "`-prefixed group **only when the whole selection is confined to that one limb group** — a multi-group (whole-figure) drag is allowed through even with Socket Mode on |
| Posable minifig grouping | `lc_model.cpp` (`ShowMinifigDialog`, ~line 5582) | 6 separate top-level groups (Head/Torso/RightArm/LeftArm/RightLeg/LeftLeg), each named `"Minifig <Name> #N"`. **Deliberately no parent group** — see "Group hierarchy gotcha" below |
| Attach to Hand | `lc_animatewidget.cpp` (`AttachToHandClicked`) | Reuses `MinifigWizard`'s parsed `minifig.ini` hand-offset tables via a lazily-constructed static `MinifigWizard*`. Currently hardcoded to always use the right-hand offset table (documented `ponytail:` comment) |
| Mirror Pose | `lc_animatewidget.cpp` (`MirrorPoseClicked`) | **Legs only** — copies the rotation matrix verbatim from one leg piece to its counterpart (matched by identical `mPieceInfo`). Deliberately does not support arms (usually different mirrored LDraw parts, direct matrix copy would be wrong) |
| Alt+click select whole minifig | `lc_model.h/.cpp` (`SelectMinifigFamilyAction`), `lc_view.cpp` (`OnButtonDown`, Select case) | Uses `lcGroup::mMinifigFamily` (see below) |
| Walk Cycle generator | `lc_animatewidget.cpp` (`WalkCycleClicked`) | Reuses `MinifigWizard::SetAngle`/`Calculate()` (the exact math the Wizard's angle sliders use) rather than re-deriving hip rotation formulas. See "Known risk area" |
| Minifig Wizard "Posable" checkbox | `lc_minifigdialog.h/.cpp/.ui` | On by default |
| CI: rolling "continuous" release, macOS DMG | `.github/workflows/release.yml`, `.github/workflows/continuous.yml` | Pinned to `macos-14` — newer runner SDKs (26.5) break the Qt 6.8.3/5.15.2 build (`AGL not found`, `qyieldcpu.h` implicit-decl) |
| Rebrand LeoCAD → StopMotionDigital | ~15 files (`Info.plist`, `lc_aboutdialog.*`, `lc_application.cpp`, etc.) | Left alone deliberately: `.lcd` binary magic bytes, internal function names like `lcVector3LDrawToLeoCAD`, upstream Help-menu links |

### `lcGroup::mMinifigFamily` — important, easy to misuse

Added to `group.h`. Tags the 6 per-limb-assembly groups of one Posable minifig instance as
belonging together, **without** making them an actual parent/child group. This distinction matters:
`lcPiece::GetTopGroup()` walks the *real* `mGroup` parent chain, and a plain click-select
(`lcModel::SetObjectsSelected` → `SelectGroup(Piece->GetTopGroup(), ...)`) selects everything under
the top group. If the 6 assemblies were nested under one real parent group, a single click on any
limb would select the whole figure again — exactly what Posable mode exists to prevent. So
`mMinifigFamily` is a separate, selection-hierarchy-independent tag, only consulted explicitly by
code that wants "everything belonging to this minifig instance" (Alt+click select, Mirror Pose's
sibling lookup, Walk Cycle's piece gathering). Do not repurpose `mGroup` for this.

## Known risk area: 3D transform math verified algebraically, not visually

This environment cannot render/watch the 3D viewport — every matrix/rotation feature in this
project (camera capture, Attach to Hand, Mirror Pose, Walk Cycle) was built by reasoning through
the matrix composition algebraically (checking `lcMul`'s actual row-vector convention in
`lc_math.h` rather than assuming), not by visual confirmation. Walk Cycle already had one real bug
shipped and fixed this way: the first version alternated frames between the full +StrideAngle/
-StrideAngle extreme with **no in-between pose** (a hard binary flip, not a sweep), which the user
reported as "empty frames" and "not eased in/out." Fixed by switching to a sine-sweep per frame
(`sinf(LC_2PI * Step / Steps) * StrideAngle`), which gives every frame an actual in-between pose
and naturally eases at the extremes (sine's slope flattens near its peaks). **If you touch any of
this math, be upfront that you can't visually verify it — say so, don't claim it's confirmed
working.**

Key facts established while debugging this (don't re-derive):
- `lcMul(a, b)` in `lc_math.h` (~line 837) is standard matrix product `a * b`, and under this
  codebase's row-vector convention that means **"apply `a` first, then `b`."**
- `lcMatrix44AffineInverse` / `lcMatrix44Inverse` exist in `lc_math.h` (~line 1397/1412).
- To apply a "how did this change" delta computed in one reference frame (e.g. `MinifigWizard`'s
  fixed-origin space) onto a piece's actual current world matrix regardless of where that piece
  really is in the scene: `NewMatrix = lcMul(lcMul(Delta, StartMatrix), Forward)` where
  `Delta = lcMul(WizardMatrixAtTargetAngle, lcMatrix44AffineInverse(WizardMatrixAtNeutralAngle))`.
  Derivation is in the Walk Cycle commit message / this file's git history if needed again.
- `minifig.cpp`'s `MinifigWizard::Calculate()` (~line 320-480) is the source of truth for every
  per-part offset/rotation-axis convention (e.g. hip swing is `RotationX`, meaning the swing axis
  is local X and the foot travels along Y — this is where "forward = +Y" in Walk Cycle comes from).
  **Reuse this function via a `MinifigWizard` instance instead of re-deriving formulas** — that's
  the whole reason Walk Cycle calls `Wizard->SetAngle(...)` / reads back `Wizard->mMinifig.Matrices[...]`
  rather than hand-rolling hip rotation math.
- **No quaternion/SLERP or matrix↔quaternion conversion exists in `lc_math.h` yet.** It has
  `lcVector4`-based quaternion helpers (`lcQuaternionRotationX/Y/Z`, `lcQuaternionFromAxisAngle`,
  `lcQuaternionToAxisAngle`, `lcQuaternionMultiply`, `lcQuaternionMul` — all around line 1550-1600)
  but **no** `lcMatrix33`↔quaternion conversion and **no** slerp function. This is directly relevant
  to the in-progress work below.

## COMPLETED: "Constant Keyframe" mode

The user wants a second Animate mode selectable alongside the existing "Stop Motion" capture
workflow: a **Constant Keyframe** mode, explicitly compared to DaVinci Resolve's keyframe editor.
Requirements gathered via `AskUserQuestion` (all three answered — this is the locked-in scope, do
not re-ask):

1. **Separate keyframe timeline UI** (not reusing the stop-motion filmstrip) — a distinct
   Resolve-style track showing keyframe markers on a time axis.
2. **Easing curve shape per segment** is user-adjustable (not just duration/spacing between
   keyframes) — e.g. Linear / Ease In / Ease Out / Ease In-Out presets per segment between two
   consecutive keyframes. (Full draggable-Bezier-handle editing was not asked for explicitly and
   is likely overscope for v1 — a preset dropdown per segment satisfies the literal request
   without a bezier-curve-editor widget; confirm with the user if unsure before building a full
   curve editor.)
3. **Chain any number of keyframes** (A→B→C→D→...), not just two points at a time.

### Design direction settled on before work was interrupted (not yet implemented)

- New enum `lcAnimateMode { StopMotion, ConstantKeyframe }`, presumably a per-model or per-widget
  mode toggle in the Animate dock (exact UI placement not yet decided — a dropdown/radio pair
  matching the "Anim menu" phrasing the user used).
- New data types (not yet added to `lc_animatewidget.h`):
  ```cpp
  enum class lcEasingType { Linear, EaseIn, EaseOut, EaseInOut };

  struct lcKeyframePose // same shape as lcAnimateFrame minus the per-piece-existence semantics
  {
      QMap<lcPiece*, lcVector3> Positions;
      QMap<lcPiece*, lcMatrix33> Rotations;
      lcVector3 CameraPosition, CameraTarget, CameraUpVector;
      bool HasCamera = false;
  };

  struct lcKeyframePoint
  {
      int Time; // frame number on the new timeline
      lcKeyframePose Pose;
      lcEasingType SegmentEasing = lcEasingType::EaseInOut; // eases the segment FROM this point TO the next
  };
  ```
- **Key integration decision (settled, keep this)**: rather than building a second parallel
  playback/export/thumbnail pipeline, *bake* the interpolated result into the **same**
  `std::vector<lcAnimateFrame> Frames` type that Stop Motion mode already uses, regenerated
  whenever a keyframe/easing/timing changes. This means `Play`/`Timeout`/`ApplyFrame`/
  `RenderFrameThumbnail`/`ExportClicked` all keep working completely unchanged for Constant
  Keyframe mode too — only the *editing* UI and the *generation* step are new. Don't build a
  second Play/Export path.
- **Open, unsolved problem — read the "Known risk area" section above**: correct rotation
  interpolation between two `lcMatrix33` keyframe rotations requires either (a) quaternion SLERP
  (mathematically correct for any rotation, but needs matrix↔quaternion conversion functions that
  **do not exist yet** in `lc_math.h` and would need to be written and gotten right with no visual
  verification available), or (b) a scoped-down assumption exploiting that most posed rotations in
  this app are single-axis (hip/shoulder swings via `RotationX`, matching `minifig.cpp`'s
  convention) — decompose the rotation delta as an axis+angle via
  `lcQuaternionToAxisAngle`/`lcQuaternionFromAxisAngle` (which DO already exist) and lerp the angle
  linearly, which is exact for single-axis rotation and a reasonable (if imperfect) approximation
  otherwise. Given this project's track record of shipping matrix-math bugs invisible without
  visual testing (see Walk Cycle above), **strongly consider proposing option (b) to the user
  explicitly as a scoped-down v1**, or at minimum flag the SLERP math as unverified when it ships.
  Naive component-wise lerp of `lcMatrix33` entries (adding two rotation matrices' floats and
  renormalizing) is **wrong** — it doesn't stay orthonormal / doesn't produce constant-speed
  rotation — do not do this.
- Position interpolation is the easy part: linear lerp between keyframe positions, then remap
  `t` through whichever easing function the segment uses (standard cubic ease-in/out formulas,
  no existing helper in this codebase — will need to write `float ApplyEasing(lcEasingType, float t)`
  from scratch, this part is low-risk/well-known math).
- Camera interpolation: same position/target/up lerp, should probably follow the same segment
  easing as everything else in that segment.
- Not yet designed: the actual timeline widget (new `QWidget` subclass, probably
  `lc_keyframetimelinewidget.h/.cpp` following this project's naming convention — note
  `lc_keyframewidget.h/.cpp` and `lc_timelinewidget.h/.cpp` **already exist as upstream LeoCAD
  files** for the native Step system; do not confuse/collide with those, pick a distinct name),
  how keyframes are added/moved/deleted, how per-segment easing is selected in that UI, and how
  regeneration is triggered efficiently (avoid re-baking the whole `Frames` vector on every minor
  drag if it's expensive — probably fine to just always fully regenerate given typical keyframe
  counts are small, but worth a moment's thought before assuming).

### What was built (next steps list → done)

1. Option (b) chosen: axis-angle decomposition + linear angle lerp for rotation interpolation (exact for single-axis minifig rotations, reasonable approximation otherwise).
2. `lcEasingType` enum, `ApplyEasing()` inline, `lcKeyframePose`, `lcKeyframePoint` added to `lc_animatewidget.h`.
3. Per-model `std::vector<lcKeyframePoint> Keyframes` + `lcAnimateMode AnimateMode` in `lcAnimateDocumentState`.
4. `BakeKeyframes()` in `lc_animatewidget.cpp`: sorts keyframes by time, iterates `[FirstTime, LastTime]` span, lerps positions, axis-angle lerps rotations, lerps camera, applies per-segment easing.
5. `lcKeyframeTimelineWidget` (`common/lc_keyframetimelinewidget.h/.cpp`): custom QWidget with keyframe diamond markers, easing labels, segment frame ticks, current-time cursor, click-to-select/seek.
6. `lcAnimateMode` enum + QComboBox mode selector in the Animate dock UI row.
7. Build, smoke-test launch, commit to master, push, copied to `/Applications/StopMotionDigital.app`.
8. **Disclosure**: interpolation math is algebraically sound (axis-angle decomposition uses the existing `lcQuaternionToAxisAngle`/`lcQuaternionFromAxisAngle` pipeline in `lc_math.h`) but not visually verified in the 3D viewport. Same risk category as Walk Cycle.

### Remaining gaps for whoever continues

- Keyframe timeline widget is minimal: no drag-to-move keyframes, no rubber-band select, no right-click context menu. Click-to-seek and click-to-select work. Add/Delete buttons add/delete at current timeline position.
- Easing is per-segment, settable when a keyframe is selected (easing combo reflects the segment *from* that keyframe *to* the next). No visual curve editing.
- No keyframe at time 0 / at the end of the frame range — the user needs to explicitly add keyframes.
- Rotation interpolation uses axis-angle, which is correct for single-axis (minifig hip/shoulder swings). For arbitrary multi-axis rotations, full quaternion SLERP would be more correct — the `lcQuaternionSlerp` function needs to be written; `lcMatrix33ToQuaternion` and `lcQuaternionToMatrix33` already exist in `lc_math.h` from this session.

## Session housekeeping notes

- User's memory system already has repo-specific notes at
  `~/.claude/projects/-Users-NotSmartMan/memory/leocad_animator_master_branch_exception.md` and
  `no_coauthor_trailer.md` — this AGENTS.md is the more detailed/technical companion to those.
- The user tests by actually using the app and reports real bugs found that way (e.g. the Walk
  Cycle easing bug) — expect that feedback loop to continue; don't assume "it compiles and
  launches" means a feature is correct.
