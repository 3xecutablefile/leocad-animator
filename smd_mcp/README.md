# StopMotionDigital MCP Server

Lets AI agents (Claude, opencode, etc.) read, edit, and generate animation
data for StopMotionDigital projects.

## Quick start

```bash
cd smd_mcp
uv venv && source .venv/bin/activate && uv pip install -e .
```

## Usage

The server reads/writes `.ldr` files (LDraw text format) and their companion
`.animate.json` files. Save your project from StopMotionDigital first, then
call tools — changes go straight into the `.animate.json`.

### Configure in Claude Code

Already wired up: `.mcp.json` at the repo root registers this server for any
Claude Code session opened in this repo - no setup needed, it's picked up
automatically (Claude Code will ask you to approve it the first time). To
register it in another repo or for `claude mcp add` globally:

```bash
claude mcp add smd -- /path/to/uv --directory /path/to/smd_mcp run smd-mcp
```

### Configure in opencode

Already wired up in `.opencode.jsonc` at the repo root. To register elsewhere,
add to `~/.config/opencode/opencode.jsonc` (top-level key is `mcp`, not
`mcpServers` - opencode's schema differs from Claude Code's):

```json
{
  "mcp": {
    "smd": {
      "type": "local",
      "command": ["uv", "--directory", "/path/to/smd_mcp", "run", "smd-mcp"],
      "enabled": true
    }
  }
}
```

Or run directly (useful for testing the server starts cleanly):

```bash
uv --directory /path/to/smd_mcp run smd-mcp
```

## Tools (30 total)

### Project & Scene
- **`read_project`** — Read `.ldr` + `.animate.json`, returns pieces, groups, frames
- **`get_project_status`** — Quick summary: pieces, groups, minifigs, frames
- **`create_project`** — Create a new empty project
- **`add_piece`** — Add an LDraw piece to the scene (with optional group)
- **`get_frame`** — Get piece positions/rotations for one frame
- **`list_frames`** — List all frames with piece count
- **`set_frame_piece`** — Move/rotate a piece in a specific frame
- **`set_frame_camera`** — Set camera (position, target, up, projection) for a frame
- **`capture_frame`** — Snapshot current piece positions as a new frame
- **`duplicate_frame`** — Copy a frame
- **`delete_frames`** — Remove frames by index

### Timeline
- **`reverse_frames`** — Reverse frame order in a range
- **`loop_animation`** — Create seamless loop by repeating frame range
- **`time_remap_frames`** — Speed up (skip) or slow down (duplicate) frames
- **`interpolate_frames`** — Generate intermediate frames between two frames
- **`stagger_pieces`** — Offset animation timing across pieces

### Animation
- **`animate_transform`** — Animate a piece to target position/rotation with easing
- **`ease_frames`** — Apply easing curve to existing frame positions
- **`follow_curve`** — Animate along a cubic bezier path with look-at rotation
- **`bake_keyframes`** — Interpolate sparse keyframes into frames with easing
- **`bounce_effect`** — Physics bounce (gravity + decay)
- **`swing_effect`** — Pendulum oscillation on any axis
- **`wave_effect`** — Propagate wave through pieces sequentially
- **`shake_effect`** — Camera or piece shake with noise
- **`explosion_effect`** — Pieces fly outward from center with spin
- **`randomize_frame`** — Add random noise to positions/rotations

### Generation
- **`generate_walk_cycle`** — 24-frame walk cycle for posable minifig
- **`generate_ragdoll`** — Ragdoll death animation with randomized limbs
- **`batch_transform_pieces`** — Translate/rotate multiple pieces at once

### Export
- **`export_video`** — Export project to video file

## Workflow

1. Build scene in StopMotionDigital, save as `.ldr`
2. `read_project` → see pieces, groups, frames
3. `capture_frame` → save current state as frame 0
4. `animate_transform(piece=0, target=[100,0,50], easing="EaseInOut")`
5. `interpolate_frames(0, 2, count=8)` → smooth it out
6. `swing_effect(piece=1, amplitude=30, period=12, axis="X")`
7. `loop_animation(0, end_frame, repeats=3)`
8. Re-open in StopMotionDigital — everything in `.animate.json`
