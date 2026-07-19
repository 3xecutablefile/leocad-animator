"""Render animate.json frames to PNGs using LeoCAD CLI, then stitch to MP4."""
import json, math, subprocess, sys, time, os
from pathlib import Path

def ldraw_line(code: int, pos: list, rot33: list[float], filename: str) -> str:
    tx, ty, z_neg = pos[0], pos[1], -pos[2]
    # LDraw rot layout: r00, r20n, r10_, r02n, r22, r12n, r01_, r21n, r11
    # where internal rot33 = [r00, -r01_, -r02n, -r10_, r11, r12n, -r20n, r21n, r22]
    ldraw_r = [rot33[0], -rot33[6], -rot33[3], -rot33[2], rot33[8], rot33[5], -rot33[1], rot33[7], rot33[4]]
    vs = " ".join(f"{v:.6f}" for v in ldraw_r)
    return f"1 {code} {tx:.6f} {z_neg:.6f} {ty:.6f} {vs} {filename}"

LEOCAD = "/Applications/StopMotionDigital.app/Contents/MacOS/StopMotionDigital"
OUT_DIR = Path.home() / "Desktop" / "testmcp_frames"
OUT_DIR.mkdir(exist_ok=True)

# Load the reference LDR file to get piece data
ldr_path = Path.home() / "Desktop" / "testmcp.ldr"
aj_path = Path.home() / "Desktop" / "testmcp.animate.json"

# Read animate.json
with open(aj_path) as f:
    aj = json.load(f)

# Get the first model's frames
model_name = list(aj["models"].keys())[0]
frames_data = aj["models"][model_name]["frames"]
n_frames = len(frames_data)
print(f"rendering {n_frames} frames...")

# Parse reference LDR to get color codes and filenames for each piece
lines = ldr_path.read_text().splitlines()
piece_lookup = {}  # index -> {color, filename}
piece_idx = 0
for line in lines:
    s = line.strip()
    if not s.startswith("1 "):
        continue
    parts = s.split()
    if len(parts) < 15:
        continue
    try:
        code = int(parts[1])
        fname = parts[-1].strip().strip('"').lower()
        piece_lookup[piece_idx] = {"color": code, "filename": fname}
        piece_idx += 1
    except (ValueError, IndexError):
        continue

# Also parse groups (for group structure we just need them as meta)
groups = []
in_group = False
for line in lines:
    s = line.strip()
    if s.upper().startswith("0 GROUP"):
        groups.append(s.replace("0 GROUP", "0 BEGIN GROUP", 1))
        in_group = True
    elif s.strip().upper() == "0 END GROUP":
        groups.append(s)
        in_group = False

# Clear frames dir
for f in OUT_DIR.glob("*.png"):
    f.unlink()

# Render each frame
for idx, frame in enumerate(frames_data):
    pieces_data = frame.get("pieces", [])
    piece_dict = {p["index"]: p for p in pieces_data} if isinstance(pieces_data, list) else pieces_data
    
    # Build the LDR content
    ldr_lines = ["0 FILE testmcp.ldr"]
    
    # Add groups from reference
    for g in groups:
        ldr_lines.append(g)
    
    # Add pieces at frame positions
    for pi, info in piece_lookup.items():
        color = info["color"]
        fname = info["filename"]
        if pi in piece_dict:
            pd = piece_dict[pi]
            pos = pd.get("position", [0,0,0])
            rot = pd.get("rotation", [1,0,0,0,1,0,0,0,1])
        else:
            pos = [0,0,0]
            rot = [1,0,0,0,1,0,0,0,1]
        ldr_lines.append(ldraw_line(color, pos, rot, fname))

    # Close groups
    for g in reversed(groups):
        if g.startswith("0 BEGIN"):
            ldr_lines.append("0 END GROUP")
    
    tmp_ldr = OUT_DIR / f"frame_{idx:04d}.ldr"
    tmp_ldr.write_text("\n".join(ldr_lines))
    
    out_png = OUT_DIR / f"frame_{idx:04d}.png"
    
    # Read camera from frame data
    cam = frame.get("camera", {})
    cam_pos = cam.get("position", [0, -100, 60])
    cam_tgt = cam.get("target", [0, 0, 40])
    cam_up = cam.get("up", [0, 0, 1])
    cam_args = [str(v) for v in cam_pos + cam_tgt + cam_up]
    
    # Render with LeoCAD CLI
    cmd = [LEOCAD, "-i", str(out_png),
           "-l", "/Applications/Studio 2.0/ldraw",
           "--camera-position"] + cam_args + [
           "--width", "1280", "--height", "720",
           "-f", "1", "-t", "1",
           str(tmp_ldr)]
    
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        print(f"  frame {idx}: error: {result.stderr.strip()[:100]}")
        # Try again once
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    
    if (idx + 1) % 10 == 0:
        print(f"  rendered {idx+1}/{n_frames}")
    
    # Clean up temp file
    tmp_ldr.unlink()

print(f"all {n_frames} frames rendered to {OUT_DIR}")

# Stitch with ffmpeg
mp4_path = Path.home() / "Desktop" / "testmcp.mp4"

# Delete existing if any
mp4_path.unlink(missing_ok=True)

ffmpeg_cmd = [
    "ffmpeg", "-y",
    "-framerate", "24",
    "-i", str(OUT_DIR / "frame_%04d.png"),
    "-c:v", "libx264",
    "-pix_fmt", "yuv420p",
    "-crf", "18",
    str(mp4_path)
]

print("stitching with ffmpeg...")
result = subprocess.run(ffmpeg_cmd, capture_output=True, text=True, timeout=120)
if result.returncode == 0:
    print(f"MP4 saved: {mp4_path} ({mp4_path.stat().st_size / 1024:.0f} KB)")
else:
    print(f"ffmpeg error: {result.stderr[:500]}")
