"""MCP server for StopMotionDigital animation.
Lets AI agents read projects, manipulate frames, and generate animation."""

from __future__ import annotations
import json, math, re, subprocess, struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from fastmcp import FastMCP

mcp = FastMCP("StopMotionDigital")

# ── math helpers (row-vector convention, matches C++ lcMul = a*b) ─────────

PI = math.pi
DTOR = PI / 180.0
RTOD = 180.0 / PI

@dataclass
class Vec3:
    x: float = 0.0; y: float = 0.0; z: float = 0.0
    def __add__(self, o): return Vec3(self.x+o.x, self.y+o.y, self.z+o.z)
    def __sub__(self, o): return Vec3(self.x-o.x, self.y-o.y, self.z-o.z)
    def __mul__(self, s): return Vec3(self.x*s, self.y*s, self.z*s)
    def __rmul__(self, s): return Vec3(self.x*s, self.y*s, self.z*s)
    def dot(self, o): return self.x*o.x + self.y*o.y + self.z*o.z
    def cross(self, o): return Vec3(self.y*o.z-self.z*o.y, self.z*o.x-self.x*o.z, self.x*o.y-self.y*o.x)
    def norm(self): return math.sqrt(self.dot(self))
    def normalized(self): n=self.norm(); return Vec3(self.x/n, self.y/n, self.z/n) if n else Vec3()
    def to_list(self): return [self.x, self.y, self.z]
    @staticmethod
    def from_list(v): return Vec3(v[0], v[1], v[2])

@dataclass
class Mat44:
    """4×4 row-major matrix, last row is [0,0,0,1] for affine."""
    r0: Vec3 = field(default_factory=Vec3)
    r1: Vec3 = field(default_factory=Vec3)
    r2: Vec3 = field(default_factory=Vec3)
    t: Vec3  = field(default_factory=Vec3)

    @staticmethod
    def identity():
        return Mat44(Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1), Vec3(0,0,0))

    @staticmethod
    def translation(v: Vec3):
        return Mat44(Vec3(1,0,0), Vec3(0,1,0), Vec3(0,0,1), v)

    @staticmethod
    def rotation_x(rad: float):
        c, s = math.cos(rad), math.sin(rad)
        return Mat44(Vec3(1,0,0), Vec3(0,c,s), Vec3(0,-s,c), Vec3(0,0,0))

    @staticmethod
    def rotation_y(rad: float):
        c, s = math.cos(rad), math.sin(rad)
        return Mat44(Vec3(c,0,-s), Vec3(0,1,0), Vec3(s,0,c), Vec3(0,0,0))

    @staticmethod
    def rotation_z(rad: float):
        c, s = math.cos(rad), math.sin(rad)
        return Mat44(Vec3(c,s,0), Vec3(-s,c,0), Vec3(0,0,1), Vec3(0,0,0))

    @staticmethod
    def from_pos_rot(pos: Vec3, rot33: list[float]):
        """9 floats row-major 3×3 + position vector."""
        return Mat44(Vec3(rot33[0],rot33[1],rot33[2]),
                     Vec3(rot33[3],rot33[4],rot33[5]),
                     Vec3(rot33[6],rot33[7],rot33[8]), pos)

    def mul(self, b: Mat44) -> Mat44:
        a = self
        r0 = Vec3(a.r0.dot(b.r0), a.r0.dot(b.r1), a.r0.dot(b.r2))
        r1 = Vec3(a.r1.dot(b.r0), a.r1.dot(b.r1), a.r1.dot(b.r2))
        r2 = Vec3(a.r2.dot(b.r0), a.r2.dot(b.r1), a.r2.dot(b.r2))
        t = a.t.dot(b.r0) + b.t.x, a.t.dot(b.r1) + b.t.y, a.t.dot(b.r2) + b.t.z
        return Mat44(r0, r1, r2, Vec3(*t))

    def get_translation(self) -> Vec3:
        return self.t

    def get_rotation_33(self) -> list[float]:
        return [self.r0.x, self.r0.y, self.r0.z,
                self.r1.x, self.r1.y, self.r1.z,
                self.r2.x, self.r2.y, self.r2.z]

    def affine_inverse(self) -> Mat44:
        a = self
        det = a.r0.x * (a.r1.y*a.r2.z - a.r1.z*a.r2.y) \
            - a.r0.y * (a.r1.x*a.r2.z - a.r1.z*a.r2.x) \
            + a.r0.z * (a.r1.x*a.r2.y - a.r1.y*a.r2.x)
        inv_det = 1.0 / det
        r0 = Vec3((a.r1.y*a.r2.z - a.r1.z*a.r2.y)*inv_det,
                  (a.r0.z*a.r2.y - a.r0.y*a.r2.z)*inv_det,
                  (a.r0.y*a.r1.z - a.r0.z*a.r1.y)*inv_det)
        r1 = Vec3((a.r1.z*a.r2.x - a.r1.x*a.r2.z)*inv_det,
                  (a.r0.x*a.r2.z - a.r0.z*a.r2.x)*inv_det,
                  (a.r0.z*a.r1.x - a.r0.x*a.r1.z)*inv_det)
        r2 = Vec3((a.r1.x*a.r2.y - a.r1.y*a.r2.x)*inv_det,
                  (a.r0.y*a.r2.x - a.r0.x*a.r2.y)*inv_det,
                  (a.r0.x*a.r1.y - a.r0.y*a.r1.x)*inv_det)
        t = Vec3(-a.t.dot(r0), -a.t.dot(r1), -a.t.dot(r2))
        return Mat44(r0, r1, r2, t)

# ── easing ────────────────────────────────────────────────────────────────

EASING_LINEAR, EASING_EASE_IN, EASING_EASE_OUT, EASING_EASE_IN_OUT = 0, 1, 2, 3

def apply_easing(t: float, easing: int) -> float:
    if easing == EASING_EASE_IN:    return t*t*t
    if easing == EASING_EASE_OUT:   return 1.0 - (1.0-t)**3
    if easing == EASING_EASE_IN_OUT:
        return 4*t*t*t if t < 0.5 else 1.0 - (-2*t+2)**3 * 0.5
    return t

# ── lerp helpers ──────────────────────────────────────────────────────────

def lerp_vec3(a: Vec3, b: Vec3, t: float) -> Vec3:
    return a + (b - a) * t

def axis_angle_lerp(rot_a: list[float], rot_b: list[float], t: float) -> list[float]:
    """Lerp between two 3×3 rotation matrices via axis-angle. Matches C++ BakeKeyframes."""
    def mat_to_quat(m):
        tr = m[0]+m[4]+m[8]
        if tr > 0:
            s = math.sqrt(tr+1)*2; qw = s/4; qx=(m[5]-m[7])/s; qy=(m[6]-m[2])/s; qz=(m[3]-m[1])/s
        elif m[0]>m[4] and m[0]>m[8]:
            s=math.sqrt(1+m[0]-m[4]-m[8])*2; qw=(m[5]-m[7])/s; qx=s/4; qy=(m[1]+m[3])/s; qz=(m[6]+m[2])/s
        elif m[4]>m[8]:
            s=math.sqrt(1+m[4]-m[0]-m[8])*2; qw=(m[6]-m[2])/s; qx=(m[1]+m[3])/s; qy=s/4; qz=(m[5]+m[7])/s
        else:
            s=math.sqrt(1+m[8]-m[0]-m[4])*2; qw=(m[3]-m[1])/s; qx=(m[6]+m[2])/s; qy=(m[5]+m[7])/s; qz=s/4
        return qw, qx, qy, qz

    def quat_to_mat(q):
        qw,qx,qy,qz = q
        xx,yy,zz = qx*qx,qy*qy,qz*qz
        xy,xz,yz = qx*qy,qx*qz,qy*qz
        wx,wy,wz = qw*qx,qw*qy,qw*qz
        return [1-2*(yy+zz), 2*(xy-wz), 2*(xz+wy),
                2*(xy+wz), 1-2*(xx+zz), 2*(yz-wx),
                2*(xz-wy), 2*(yz+wx), 1-2*(xx+yy)]

    def quat_to_axis_angle(q):
        qw,qx,qy,qz = q
        angle = 2*math.acos(max(-1,min(1,qw)))
        s = math.sqrt(1-qw*qw)
        if s < 1e-6: return 0,0,1,angle
        return qx/s, qy/s, qz/s, angle

    def axis_angle_to_quat(ax, ay, az, angle):
        s = math.sin(angle/2)
        return math.cos(angle/2), ax*s, ay*s, az*s

    def mat_mul(a, b):
        return [a[0]*b[0]+a[1]*b[3]+a[2]*b[6],
                a[0]*b[1]+a[1]*b[4]+a[2]*b[7],
                a[0]*b[2]+a[1]*b[5]+a[2]*b[8],
                a[3]*b[0]+a[4]*b[3]+a[5]*b[6],
                a[3]*b[1]+a[4]*b[4]+a[5]*b[7],
                a[3]*b[2]+a[4]*b[5]+a[5]*b[8],
                a[6]*b[0]+a[7]*b[3]+a[8]*b[6],
                a[6]*b[1]+a[7]*b[4]+a[8]*b[7],
                a[6]*b[2]+a[7]*b[5]+a[8]*b[8]]

    def transpose(m):
        return [m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]]

    rdelta = mat_mul(rot_b, transpose(rot_a))
    q = mat_to_quat(rdelta)
    ax, ay, az, ang = quat_to_axis_angle(q)
    iang = ang * t
    iq = axis_angle_to_quat(ax, ay, az, iang)
    im = quat_to_mat(iq)
    result = mat_mul(im, rot_a)
    return result

# ── LDraw file reading ────────────────────────────────────────────────────

@dataclass
class LDrawPiece:
    index: int
    color_code: int
    pos: Vec3
    rot: list[float]
    filename: str
    description: str = ""

@dataclass
class LDrawGroup:
    name: str
    minifig_family: str = ""  # name of the group tagged as this minifig's family (see below)
    piece_indices: list[int] = field(default_factory=list)

@dataclass
class LDrawProject:
    path: Path
    pieces: list[LDrawPiece] = field(default_factory=list)
    groups: list[LDrawGroup] = field(default_factory=list)

    def piece_by_index(self, idx: int) -> LDrawPiece | None:
        for p in self.pieces:
            if p.index == idx: return p
        return None


# ── LDraw parser (simple: handles type 1 lines and 0 STEP/BEGIN/end-group markers) ──

# ponytail: no full MPD support, no submodel inlining. Add when needed.

# StopMotionDigital (LeoCAD) actually writes "0 !LEOCAD GROUP BEGIN <name>" / "0 !LEOCAD GROUP END"
# (see lc_model.cpp SaveLDraw) - NOT the bare "0 GROUP <n> <name>" MLCAD-style syntax these regexes
# used to expect, which never matched a real project file (every read_project/
# generate_walk_cycle call against an app-saved file silently saw zero groups).
GROUP_BEGIN_RE = re.compile(r'0\s+!LEOCAD\s+GROUP\s+BEGIN\s+(.+)', re.I)
GROUP_END_RE = re.compile(r'0\s+!LEOCAD\s+GROUP\s+END', re.I)
# A Posable minifig's per-limb groups are tagged as belonging together via an explicit
# "0 !LEOCAD GROUP MINIFIG_FAMILY <family-group-name>" line right after that group's BEGIN line
# (see lcGroup::mMinifigFamily and lc_model.cpp's LoadLDraw/SaveLDraw) - the group name itself does
# NOT encode which minifig it belongs to (it's just "Minifig Right Arm #1" etc, no figure name).
GROUP_FAMILY_RE = re.compile(r'0\s+!LEOCAD\s+GROUP\s+MINIFIG_FAMILY\s+(.+)', re.I)
LINE1_RE = re.compile(r'^1\s+(\d+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+"?(.+?)"?\s*(?:$|0\s)', re.I)
LINE1_SIMPLE_RE = re.compile(r'^1\s+(\d+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+([-\d.e]+)\s+(.+)$')

def parse_ldraw(path: str | Path) -> LDrawProject:
    """Parse an LDraw file into a LDrawProject with pieces and groups."""
    path = Path(path)
    text = path.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines()
    proj = LDrawProject(path=path)
    current_groups: list[LDrawGroup] = []
    groups_by_name: dict[str, LDrawGroup] = {}
    pending_family: list[tuple[LDrawGroup, str]] = []
    piece_idx = 0

    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue

        m = GROUP_BEGIN_RE.match(stripped)
        if m:
            gname = m.group(1).strip()
            g = groups_by_name.get(gname)
            if g is None:
                g = LDrawGroup(name=gname)
                groups_by_name[gname] = g
                proj.groups.append(g)
            current_groups.append(g)
            continue

        mf = GROUP_FAMILY_RE.match(stripped)
        if mf and current_groups:
            pending_family.append((current_groups[-1], mf.group(1).strip()))
            continue

        if GROUP_END_RE.match(stripped):
            if current_groups:
                current_groups.pop()
            continue

        m2 = LINE1_SIMPLE_RE.match(stripped)
        if not m2:
            continue

        code = int(m2.group(1))
        ldraw_vals = [float(m2.group(i)) for i in range(2, 14)]
        fname = m2.group(14).strip().strip('"').lower()

        # LDraw → internal: pos is (tx, tz_neg, ty) in LDraw line
        tx, tz_neg, ty = ldraw_vals[0], ldraw_vals[1], ldraw_vals[2]
        # LDraw rotation layout: r00,r20_neg,r10,r02_neg,r22,r12_neg,r01,r21_neg,r11
        r00,r20n,r10,r02n,r22,r12n,r01,r21n,r11 = ldraw_vals[3:12]
        # Convert to row-major 3×3 with Z sign fixed
        rot33 = [r00, -r01, -r02n,
                -r10, r11, r12n,
                -r20n, r21n, r22]
        pos = Vec3(tx, ty, -tz_neg)

        piece = LDrawPiece(index=piece_idx, color_code=code,
                          pos=pos, rot=rot33, filename=fname)
        idx = piece.index
        for g in current_groups:
            g.piece_indices.append(idx)
        proj.pieces.append(piece)
        piece_idx += 1

    # Resolve MINIFIG_FAMILY lines now that every group has been seen. minifig_family ends up holding
    # the shared family group's NAME (e.g. "Minifig Right Arm #1") - group names don't carry a custom
    # tools like generate_walk_cycle use to mean "one minifig" (see get_project_status,
    # which surfaces the available identifiers via its "minifigs" list).
    for group, family_name in pending_family:
        group.minifig_family = family_name

    return proj


def _find_group_end_index(lines: list[str], group_name: str) -> int | None:
    """Line index of the "0 !LEOCAD GROUP END" matching the first BEGIN of group_name, tracking
    nesting depth so a group containing nested sub-groups still resolves to its own END line."""
    for i, line in enumerate(lines):
        m = GROUP_BEGIN_RE.match(line.strip())
        if m and m.group(1).strip() == group_name:
            depth = 1
            for j in range(i + 1, len(lines)):
                s = lines[j].strip()
                if GROUP_BEGIN_RE.match(s):
                    depth += 1
                elif GROUP_END_RE.match(s):
                    depth -= 1
                    if depth == 0:
                        return j
            return None  # unterminated group - malformed file, treat as not found
    return None


# ── animate.json read/write ───────────────────────────────────────────────

@dataclass
class AnimateFrame:
    pieces: dict[int, dict] = field(default_factory=dict)
    camera: dict | None = None

@dataclass
class AnimateData:
    model_name: str = ""
    frames: list[AnimateFrame] = field(default_factory=list)

def load_animate_json(path: str | Path) -> AnimateData | None:
    aj_path = Path(str(path).rsplit('.', 1)[0] + '.animate.json')
    if not aj_path.exists():
        return None
    data = json.loads(aj_path.read_text(encoding='utf-8'))
    result = AnimateData()
    models = data.get("models", {})
    # Find the first model
    for mname, mdata in models.items():
        result.model_name = mname
        for fdata in mdata.get("frames", []):
            frame = AnimateFrame()
            for pd in fdata.get("pieces", []):
                frame.pieces[int(pd["index"])] = {
                    "position": pd["position"],
                    "rotation": pd["rotation"]
                }
            if "camera" in fdata:
                frame.camera = fdata["camera"]
            result.frames.append(frame)
        break  # only first model
    return result

def save_animate_json(path: str | Path, data: AnimateData):
    aj_path = Path(str(path).rsplit('.', 1)[0] + '.animate.json')
    model_key = data.model_name or Path(path).name
    frames_json = []
    for f in data.frames:
        fj = {"pieces": []}
        for idx, pd in f.pieces.items():
            fj["pieces"].append({"index": idx, "position": pd["position"], "rotation": pd["rotation"]})
        if f.camera:
            fj["camera"] = f.camera
        frames_json.append(fj)
    payload = {"models": {model_key: {"frames": frames_json}}}
    aj_path.write_text(json.dumps(payload, indent=2), encoding='utf-8')


# ── wizard chain math (standard minifig, no .ini parsing) ────────────────
# ponytail: hardcodes the chain for standard humanoid minifig.
# Non-humanoid (droid, skeleton) will need their own chains. Add when needed.

def _wizard_matrix(slot: str, angle: float) -> Mat44:
    """Compute wizard world matrix for one slot at given angle (degrees)."""
    a = angle * DTOR
    root = Mat44.translation(Vec3(0, 0, 72))

    if slot == "BODY":
        return root

    if slot == "HEAD":
        mat = Mat44.rotation_z(-a)
        mat = Mat44(Mat44.identity().r0, Mat44.identity().r1, Mat44.identity().r2, Vec3(0, 0, 24))
        return mat.mul(root)

    if slot == "RLEG":
        mat = Mat44.rotation_x(-a)
        mat = Mat44(mat.r0, mat.r1, mat.r2, Vec3(0, 0, -44))
        return mat.mul(root)

    if slot == "LLEG":
        mat = Mat44.rotation_x(-a)
        mat = Mat44(mat.r0, mat.r1, mat.r2, Vec3(0, 0, -44))
        return mat.mul(root)

    if slot == "RARM":
        mat = Mat44.rotation_x(-a)
        mat2 = Mat44.rotation_y(-9.791 * DTOR)
        mat2 = Mat44(mat2.r0, mat2.r1, mat2.r2, Vec3(15.552, 0, -8.88))
        mat = mat.mul(mat2)
        return mat.mul(root)

    if slot == "LARM":
        mat = Mat44.rotation_x(-a)
        mat2 = Mat44.rotation_y(9.791 * DTOR)
        mat2 = Mat44(mat2.r0, mat2.r1, mat2.r2, Vec3(-15.552, 0, -8.88))
        mat = mat.mul(mat2)
        return mat.mul(root)

    if slot == "RHAND":
        mat = Mat44.rotation_y(-a)
        mat2 = Mat44.rotation_x(45 * DTOR)
        mat = mat.mul(mat2)
        mat = Mat44(mat.r0, mat.r1, mat.r2, Vec3(5, -10, -19))
        rarm_neutral = _wizard_matrix("RARM", 0)
        return mat.mul(rarm_neutral)

    if slot == "LHAND":
        mat = Mat44.rotation_y(-a)
        mat2 = Mat44.rotation_x(45 * DTOR)
        mat = mat.mul(mat2)
        mat = Mat44(mat.r0, mat.r1, mat.r2, Vec3(-5, -10, -19))
        larm_neutral = _wizard_matrix("LARM", 0)
        return mat.mul(larm_neutral)

    return Mat44.identity()


SLOTS = ["RLEG", "LLEG", "RARM", "LARM", "RHAND", "LHAND", "BODY", "HEAD"]
SLOT_LIMBS = ["RLEG", "LLEG", "RARM", "LARM", "RHAND", "LHAND"]


def compute_walk_cycle_deltas(stride_angle: float, arm_swing: float,
                              gait_idx: int = 0, steps: int = 24,
                              direction: float = 0.0) -> list[dict]:
    """Generate walk cycle frame deltas. Returns a list of per-frame delta dicts."""
    dir_rad = direction * DTOR
    forward = Vec3(-math.sin(dir_rad), -math.cos(dir_rad), 0)

    neutral: dict[str, Mat44] = {}
    neutral_inv: dict[str, Mat44] = {}
    for s in SLOT_LIMBS:
        neutral[s] = _wizard_matrix(s, 0)
        neutral_inv[s] = neutral[s].affine_inverse()

    rleg_hip = _wizard_matrix("RLEG", 0)
    total_leg_len = -rleg_hip.get_translation().z + 44
    fwd_disp = total_leg_len * math.sin(stride_angle * DTOR)
    step_dist = (2.0 * fwd_disp) / steps

    frames = []
    for step in range(steps):
        phase = 2*PI * step / (steps - 1) if steps > 1 else 0
        wave = math.sin(phase)
        gait_wave = (
            math.sin(phase + 0.25*wave) if gait_idx == 2 else
            0.85*wave + 0.15*math.sin(phase*2) if gait_idx == 1 else
            wave
        )
        right_angle = stride_angle * gait_wave
        left_angle = -right_angle

        fwd_vec = forward * (step_dist * (step + 1))
        fwd_mat = Mat44.translation(fwd_vec)

        deltas = {}
        deltas["RLEG"] = _wizard_matrix("RLEG", right_angle).mul(neutral_inv["RLEG"])
        deltas["LLEG"] = _wizard_matrix("LLEG", left_angle).mul(neutral_inv["LLEG"])

        asw = -arm_swing * gait_wave
        deltas["RARM"] = _wizard_matrix("RARM", asw).mul(neutral_inv["RARM"])
        deltas["LARM"] = _wizard_matrix("LARM", -asw).mul(neutral_inv["LARM"])
        deltas["RHAND"] = _wizard_matrix("RHAND", asw).mul(neutral_inv["RHAND"])
        deltas["LHAND"] = _wizard_matrix("LHAND", -asw).mul(neutral_inv["LHAND"])

        frames.append({"deltas": {s: {"rot": d.get_rotation_33(), "trans": d.get_translation().to_list()}
                                   for s, d in deltas.items()},
                       "forward": fwd_vec.to_list(),
                       "step": step})
    return frames


# ── MCP tools ─────────────────────────────────────────────────────────────

@mcp.tool()
def read_project(project_path: str) -> dict[str, Any]:
    """Read an LDraw .ldr/.lcd project and its companion .animate.json.
    Returns piece catalog, groups, and frame data."""
    path = Path(project_path).expanduser()
    if not path.exists():
        return {"error": f"File not found: {path}"}
    proj = parse_ldraw(path)
    anim = load_animate_json(path)

    pieces_out = []
    for p in proj.pieces:
        pieces_out.append({
            "index": p.index,
            "filename": p.filename,
            "color_code": p.color_code,
            "position": p.pos.to_list(),
            "rotation": p.rot,
        })
    groups_out = []
    for g in proj.groups:
        groups_out.append({
            "name": g.name,
            "minifig_family": g.minifig_family,
            "piece_indices": g.piece_indices,
        })
    frames_out = []
    if anim:
        for i, f in enumerate(anim.frames):
            fp = []
            for idx, pd in f.pieces.items():
                fp.append({"index": idx, "position": pd["position"], "rotation": pd["rotation"]})
            frames_out.append({"index": i, "piece_count": len(fp), "pieces": fp, "camera": f.camera})

    info = {"file": str(path), "piece_count": len(proj.pieces),
            "group_count": len(proj.groups), "frame_count": len(frames_out)}
    return {"info": info, "pieces": pieces_out, "groups": groups_out, "frames": frames_out}


@mcp.tool()
def get_frame(project_path: str, frame_index: int) -> dict[str, Any]:
    """Get piece data for one frame from the animate.json."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_index < 0 or frame_index >= len(anim.frames):
        return {"error": "Frame not found"}
    f = anim.frames[frame_index]
    pieces = [{"index": idx, "position": pd["position"], "rotation": pd["rotation"]}
              for idx, pd in f.pieces.items()]
    return {"frame_index": frame_index, "pieces": pieces, "camera": f.camera}


@mcp.tool()
def list_frames(project_path: str) -> list[dict[str, Any]]:
    """List all frames in the project with index and piece count."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return []
    return [{"index": i, "piece_count": len(f.pieces), "has_camera": f.camera is not None}
            for i, f in enumerate(anim.frames)]


@mcp.tool()
def capture_frame(project_path: str) -> dict[str, Any]:
    """Capture current piece positions from the LDraw file as a new frame.
    Reads the file fresh so the caller should save the project first."""
    path = Path(project_path).expanduser()
    proj = parse_ldraw(path)
    anim = load_animate_json(path) or AnimateData(model_name=Path(path).name)
    frame = AnimateFrame()
    for p in proj.pieces:
        frame.pieces[p.index] = {"position": p.pos.to_list(), "rotation": p.rot}
    anim.frames.append(frame)
    save_animate_json(path, anim)
    return {"frame_index": len(anim.frames)-1, "piece_count": len(frame.pieces)}


@mcp.tool()
def delete_frames(project_path: str, frame_indices: list[int]) -> dict[str, Any]:
    """Delete frames by index. Indices are stable until save."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    removed = []
    for idx in sorted(frame_indices, reverse=True):
        if 0 <= idx < len(anim.frames):
            anim.frames.pop(idx)
            removed.append(idx)
    save_animate_json(path, anim)
    return {"deleted_indices": removed, "remaining": len(anim.frames)}


@mcp.tool()
def duplicate_frame(project_path: str, frame_index: int) -> dict[str, Any]:
    """Duplicate a frame. Creates a copy at the end."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_index < 0 or frame_index >= len(anim.frames):
        return {"error": "Frame not found"}
    import copy
    dup = copy.deepcopy(anim.frames[frame_index])
    anim.frames.append(dup)
    save_animate_json(path, anim)
    return {"new_index": len(anim.frames)-1, "piece_count": len(dup.pieces)}


@mcp.tool()
def set_frame_piece(project_path: str, frame_index: int, piece_index: int,
                    position: list[float] | None = None,
                    rotation: list[float] | None = None) -> dict[str, Any]:
    """Set a piece's position and/or rotation in a specific frame."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_index < 0 or frame_index >= len(anim.frames):
        return {"error": "Frame not found"}
    f = anim.frames[frame_index]
    if piece_index not in f.pieces:
        # read current from ldraw if available
        proj = parse_ldraw(path)
        ref = proj.piece_by_index(piece_index)
        if ref:
            f.pieces[piece_index] = {"position": ref.pos.to_list(), "rotation": ref.rot}
        else:
            f.pieces[piece_index] = {"position": [0,0,0], "rotation": [1,0,0,0,1,0,0,0,1]}
    if position:
        f.pieces[piece_index]["position"] = position
    if rotation:
        f.pieces[piece_index]["rotation"] = rotation
    save_animate_json(path, anim)
    return {"frame_index": frame_index, "piece_index": piece_index,
            "position": f.pieces[piece_index]["position"],
            "rotation": f.pieces[piece_index]["rotation"]}


@mcp.tool()
def generate_walk_cycle(project_path: str, minifig_name: str,
                        stride_angle: float = 25.0,
                        arm_swing: float = 15.0,
                        direction: float = 0.0,
                        gait: str = "Walk",
                        insert_at: int | None = None) -> dict[str, Any]:
    """Generate walk cycle frames for a minifig.
    gait: 'Walk', 'Jog', or 'Run'.
    Insert frames at `insert_at` index (None = append)."""
    gait_map = {"Walk": 0, "Jog": 1, "Run": 2}
    gait_idx = gait_map.get(gait, 0)
    steps = 24

    path = Path(project_path).expanduser()
    proj = parse_ldraw(path)
    anim = load_animate_json(path) or AnimateData(model_name=Path(path).name)

    # Identify minifig groups
    fig_groups = [g for g in proj.groups if g.minifig_family.lower() == minifig_name.lower()]
    if not fig_groups:
        return {"error": f"No minifig '{minifig_name}' found. Available: " +
                str(list(set(g.minifig_family for g in proj.groups if g.minifig_family)))}

    # Gather piece indices per limb
    def pieces_in_group(name_part: str) -> list[int]:
        for g in fig_groups:
            if name_part.lower() in g.name.lower():
                return g.piece_indices
        return []

    right_leg = pieces_in_group("RightLeg")
    left_leg = pieces_in_group("LeftLeg")
    right_arm = pieces_in_group("RightArm")
    left_arm = pieces_in_group("LeftArm")
    head = pieces_in_group("Head")
    torso = pieces_in_group("Torso")

    all_limb_idx = set(right_leg + left_leg + right_arm + left_arm + head + torso)
    other_idx = [p.index for p in proj.pieces if p.index not in all_limb_idx]

    # Map pieces to slots by filename
    # Standard minifig part filenames
    def find_slot(pieces_idx: list[int], filenames: list[str]) -> int | None:
        for idx in pieces_idx:
            p = proj.piece_by_index(idx)
            if p and any(f in p.filename for f in filenames):
                return idx
        return None

    rleg_piece = find_slot(right_leg, ["3817", "3825", "3830", "6265", "6267"])
    lleg_piece = find_slot(left_leg, ["3816", "3824", "3831", "6264", "6266"])
    rarm_piece = find_slot(right_arm, ["3819", "3821", "3823", "3833", "3841", "3845"])
    larm_piece = find_slot(left_arm, ["3818", "3820", "3822", "3832", "3840", "3844"])
    rhand_piece = find_slot(right_arm, ["3820", "98373"])
    lhand_piece = find_slot(left_arm, ["3820", "98373"])

    if rleg_piece is None and right_leg:
        rleg_piece = right_leg[0]
    if lleg_piece is None and left_leg:
        lleg_piece = left_leg[0]

    # Snapshot current frame from LDraw file as reference positions
    start_positions: dict[int, Vec3] = {}
    start_rotations: dict[int, list[float]] = {}
    for p in proj.pieces:
        start_positions[p.index] = p.pos
        start_rotations[p.index] = p.rot

    deltas_list = compute_walk_cycle_deltas(stride_angle, arm_swing, gait_idx, steps, direction)

    new_frames = []
    neutral_frame = AnimateFrame()
    for p in proj.pieces:
        neutral_frame.pieces[p.index] = {"position": p.pos.to_list(), "rotation": p.rot}
    new_frames.append(neutral_frame)

    for df in deltas_list:
        frame = AnimateFrame()
        for p in proj.pieces:
            idx = p.index
            pos = start_positions[idx]
            rot = start_rotations[idx]
            mat = Mat44.from_pos_rot(pos, rot)

            slot = None
            if idx in right_leg:
                slot = "RLEG"
                if idx == rhand_piece: slot = "RHAND"
            elif idx in left_leg:
                slot = "LLEG"
                if idx == lhand_piece: slot = "LHAND"
            elif idx in right_arm:
                slot = "RARM"
                if idx == rhand_piece: slot = "RHAND"
            elif idx in left_arm:
                slot = "LARM"
                if idx == lhand_piece: slot = "LHAND"

            if slot and slot in df["deltas"]:
                d = df["deltas"][slot]
                dmat = Mat44.from_pos_rot(Vec3(*d["trans"]), d["rot"])
                fwd = Mat44.translation(Vec3(*df["forward"]))
                new_mat = dmat.mul(mat).mul(fwd)
                frame.pieces[idx] = {"position": new_mat.get_translation().to_list(),
                                     "rotation": new_mat.get_rotation_33()}
            else:
                # Other pieces = translate with forward motion
                new_pos = pos + Vec3(*df["forward"])
                frame.pieces[idx] = {"position": new_pos.to_list(), "rotation": rot}

        new_frames.append(frame)

    # Insert frames
    if insert_at is not None:
        # Replace the first (neutral) or insert
        if insert_at < len(anim.frames):
            anim.frames[insert_at:insert_at+1] = new_frames  # replace one with our block
        else:
            anim.frames.extend(new_frames)
    else:
        anim.frames.extend(new_frames)

    save_animate_json(path, anim)
    return {"frames_generated": len(new_frames), "total_frames": len(anim.frames),
            "start_index": insert_at if insert_at is not None else len(anim.frames) - len(new_frames),
            "minifig": minifig_name, "gait": gait, "direction": direction}


@mcp.tool()
def bake_keyframes(project_path: str, keyframes: list[dict],
                   frames: int = 200) -> dict[str, Any]:
    """Bake sparse keyframes into frames with easing interpolation.
    keyframes: list of {time: int, pieces: {index: {position, rotation}}, easing: int}
    easing: 0=Linear, 1=EaseIn, 2=EaseOut, 3=EaseInOut."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path) or AnimateData(model_name=Path(path).name)
    proj = parse_ldraw(path)

    # Sort by time
    kfs = sorted(keyframes, key=lambda k: k["time"])
    if not kfs:
        return {"error": "Need at least one keyframe"}
    if len(kfs) == 1:
        # Single keyframe → hold
        kf = kfs[0]
        frame = AnimateFrame()
        for idx, pd in kf["pieces"].items():
            frame.pieces[int(idx)] = pd
        anim.frames = [frame]
        save_animate_json(path, anim)
        return {"frames_baked": 1}

    t_min = kfs[0]["time"]
    t_max = kfs[-1]["time"]

    new_frames = []
    for t in range(t_min, t_max + 1):
        # Find bracketing keyframes
        kf_a = kfs[0]
        kf_b = kfs[-1]
        for i in range(len(kfs) - 1):
            if kfs[i]["time"] <= t <= kfs[i+1]["time"]:
                kf_a = kfs[i]
                kf_b = kfs[i+1]
                break

        span = kf_b["time"] - kf_a["time"]
        if span == 0:
            raw_t = 0.0
        else:
            raw_t = (t - kf_a["time"]) / span
        eased = apply_easing(raw_t, kf_a.get("easing", 0))

        frame = AnimateFrame()
        # Interpolate all pieces present in either keyframe
        all_idx = set()
        for kf in (kf_a, kf_b):
            all_idx.update(int(i) for i in kf["pieces"].keys())

        for idx in sorted(all_idx):
            a = kf_a["pieces"].get(str(idx), None)
            b = kf_b["pieces"].get(str(idx), None)
            if a is None and b:
                frame.pieces[idx] = b
            elif b is None and a:
                frame.pieces[idx] = a
            elif a and b:
                pos = lerp_vec3(Vec3.from_list(a["position"]),
                                Vec3.from_list(b["position"]), eased)
                rot = axis_angle_lerp(a["rotation"], b["rotation"], eased)
                frame.pieces[idx] = {"position": pos.to_list(), "rotation": rot}

        new_frames.append(frame)

    anim.frames = new_frames
    save_animate_json(path, anim)
    return {"frames_baked": len(new_frames)}


@mcp.tool()
def export_video(project_path: str, output_path: str = "",
                 frame_rate: int = 12, format: str = "mp4") -> dict[str, Any]:
    """Export animation to video.
    Tries to use StopMotionDigital's CLI export, falls back to generating
    an ffmpeg command for the user."""
    path = Path(project_path).expanduser()
    if not output_path:
        output_path = str(path.with_suffix('.mp4'))

    # Try using the app's export
    app_bin = "/Applications/StopMotionDigital.app/Contents/MacOS/StopMotionDigital"
    if Path(app_bin).exists():
        try:
            # The app may support CLI export via --export or similar
            # Fall through to generate instructions
            pass
        except Exception:
            pass

    return {"message": f"To export, open '{path}' in StopMotionDigital and use File → Export Animation, "
                       f"or run: ffmpeg -framerate {frame_rate} -i frames_%04d.png -c:v libx264 '{output_path}'",
            "project": str(path), "output": output_path, "frame_rate": frame_rate}


# ── bezier / path helpers ────────────────────────────────────────────────

def cubic_bezier(t: float, p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3) -> Vec3:
    u = 1 - t
    return p0*u*u*u + p1*3*u*u*t + p2*3*u*t*t + p3*t*t*t

# ── new MCP tools ─────────────────────────────────────────────────────────

@mcp.tool()
def create_project(project_path: str, model_name: str = "") -> dict[str, Any]:
    """Create a new empty animation project. Writes an .ldr file and .animate.json."""
    path = Path(project_path).expanduser()
    if path.exists():
        return {"error": f"File already exists: {path}"}
    path.parent.mkdir(parents=True, exist_ok=True)
    name = model_name or path.stem
    ldr = f"0 {name}\n0 Name: {path.name}\n0 Author: StopMotionDigital MCP\n"
    path.write_text(ldr, encoding='utf-8')
    save_animate_json(path, AnimateData(model_name=name))
    return {"status": "created", "path": str(path)}


@mcp.tool()
def add_piece(project_path: str, filename: str, color_code: int = 4,
              position: list[float] | None = None,
              rotation: list[float] | None = None,
              group_name: str = "",
              minifig_family: str = "") -> dict[str, Any]:
    """Add an LDraw piece to the project file. Appends a type-1 line to the .ldr.

    group_name puts the piece in a named group (created if it doesn't exist yet, otherwise the
    piece joins the existing group). To build a Posable minifig's 6 limb groups (Head/Torso/
    RightArm/LeftArm/RightLeg/LeftLeg), pass minifig_family on each group's FIRST add_piece call -
    all 6 must use the exact same minifig_family string (conventionally the first group's own
    group_name) so generate_walk_cycle/get_project_status recognize them as one
    minifig. This mirrors StopMotionDigital's own lcGroup::mMinifigFamily tag."""
    path = Path(project_path).expanduser()
    if not path.exists():
        r = create_project(project_path)
        if "error" in r:
            return r
    pos = position or [0, 0, 0]
    rot = rotation or [1, 0, 0, 0, 1, 0, 0, 0, 1]
    # Internal → LDraw: negate r01, r02n, r10, r12n, r20n, r21n
    r00,r01,r02 = rot[0], -rot[1], -rot[2]
    r10,r11,r12 = -rot[3], rot[4], -rot[5]
    r20,r21,r22 = -rot[6], -rot[7], rot[8]
    # LDraw order: color tx tz_neg ty r00 r20_neg r10 r02_neg r22 r12_neg r01 r21_neg r11 filename
    ldr_line = f"1 {color_code} {pos[0]:g} {-pos[2]:g} {pos[1]:g} {r00:g} {r20:g} {r10:g} {r02:g} {r22:g} {r12:g} {r01:g} {r21:g} {r11:g} {filename}\n"
    if group_name:
        text = path.read_text(encoding='utf-8')
        lines = text.splitlines()
        end_idx = _find_group_end_index(lines, group_name)
        if end_idx is not None:
            lines.insert(end_idx, ldr_line.rstrip())
        else:
            # Group doesn't exist yet in this file - create it (StopMotionDigital's real
            # "0 !LEOCAD GROUP BEGIN/END" syntax, not the bare MLCAD-style syntax this used to write,
            # which the app's own loader never recognized).
            lines.append(f"0 !LEOCAD GROUP BEGIN {group_name}")
            if minifig_family:
                lines.append(f"0 !LEOCAD GROUP MINIFIG_FAMILY {minifig_family}")
            lines.append(ldr_line.rstrip())
            lines.append("0 !LEOCAD GROUP END")
        path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    else:
        with open(path, 'a') as f:
            f.write(ldr_line)
    proj = parse_ldraw(path)
    new_idx = proj.pieces[-1].index if proj.pieces else 0
    # Add to all existing frames
    anim = load_animate_json(path)
    if anim:
        for f in anim.frames:
            f.pieces[new_idx] = {"position": pos, "rotation": rot}
        save_animate_json(path, anim)
    return {"piece_index": new_idx, "filename": filename, "position": pos}


@mcp.tool()
def animate_transform(project_path: str, piece_index: int,
                      target_position: list[float],
                      target_rotation: list[float],
                      frame_range: list[int] | None = None,
                      easing: str = "Linear") -> dict[str, Any]:
    """Animate a piece from its current state to target over a frame range with easing.
    easing: 'Linear','EaseIn','EaseOut','EaseInOut'."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or not anim.frames:
        return {"error": "No frames to animate"}
    start_f, end_f = frame_range if frame_range else [0, len(anim.frames)-1]
    end_f = min(end_f, len(anim.frames)-1)
    if end_f <= start_f:
        return {"error": "frame_range must have at least 2 frames"}
    easing_map = {"linear": 0, "easein": 1, "easeout": 2, "easeinout": 3}
    ei = easing_map.get(easing.lower(), 0)
    span = end_f - start_f
    # Read start state from first frame
    p0 = anim.frames[start_f].pieces.get(piece_index)
    if not p0:
        return {"error": f"Piece {piece_index} not in frame {start_f}"}
    start_pos = Vec3.from_list(p0["position"])
    start_rot = p0["rotation"]
    tgt_pos = Vec3.from_list(target_position)
    for i in range(start_f, end_f + 1):
        t = (i - start_f) / span if span else 0
        eased = apply_easing(t, ei)
        pos = lerp_vec3(start_pos, tgt_pos, eased)
        rot = axis_angle_lerp(start_rot, target_rotation, eased)
        anim.frames[i].pieces[piece_index] = {"position": pos.to_list(), "rotation": rot}
    save_animate_json(path, anim)
    return {"piece": piece_index, "frames_changed": end_f - start_f + 1,
            "from": start_pos.to_list(), "to": tgt_pos.to_list()}


@mcp.tool()
def interpolate_frames(project_path: str, frame_a: int, frame_b: int,
                       count: int = 5, replace: bool = False) -> dict[str, Any]:
    """Generate intermediate frames between two frames by lerping. Optionally replace the range."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_a >= len(anim.frames) or frame_b >= len(anim.frames):
        return {"error": "Frame indices out of range"}
    fa = anim.frames[frame_a]
    fb = anim.frames[frame_b]
    new_frames = []
    all_idx = set(fa.pieces.keys()) | set(fb.pieces.keys())
    for step in range(1, count + 1):
        t = step / (count + 1)
        f = AnimateFrame()
        for idx in all_idx:
            a = fa.pieces.get(idx)
            b = fb.pieces.get(idx)
            if a and b:
                pos = lerp_vec3(Vec3.from_list(a["position"]),
                                Vec3.from_list(b["position"]), t)
                rot = axis_angle_lerp(a["rotation"], b["rotation"], t)
                f.pieces[idx] = {"position": pos.to_list(), "rotation": rot}
            elif a:
                f.pieces[idx] = a
            elif b:
                f.pieces[idx] = b
        new_frames.append(f)
    if replace:
        anim.frames[frame_a+1:frame_b] = new_frames
    else:
        anim.frames[frame_a+1:frame_a+1] = new_frames
    save_animate_json(path, anim)
    return {"frames_generated": len(new_frames), "between": [frame_a, frame_b], "total": len(anim.frames)}


@mcp.tool()
def reverse_frames(project_path: str, start_frame: int, end_frame: int) -> dict[str, Any]:
    """Reverse the order of frames in a range."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or start_frame >= len(anim.frames) or end_frame >= len(anim.frames):
        return {"error": "Frame indices out of range"}
    anim.frames[start_frame:end_frame+1] = list(reversed(anim.frames[start_frame:end_frame+1]))
    save_animate_json(path, anim)
    return {"reversed": [start_frame, end_frame], "total": len(anim.frames)}


@mcp.tool()
def loop_animation(project_path: str, start_frame: int, end_frame: int,
                   repeats: int = 1, smooth: bool = False) -> dict[str, Any]:
    """Create a loop by appending copies of a frame range.
    smooth=True adds an interpolation frame to blend end→start."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    import copy
    loop = copy.deepcopy(anim.frames[start_frame:end_frame+1])
    if smooth and end_frame >= start_frame:
        fa = anim.frames[end_frame]
        fb = anim.frames[start_frame]
        bridge = AnimateFrame()
        for idx in set(fa.pieces.keys()) | set(fb.pieces.keys()):
            a = fa.pieces.get(idx); b = fb.pieces.get(idx)
            if a and b:
                pos = lerp_vec3(Vec3.from_list(a["position"]), Vec3.from_list(b["position"]), 0.5)
                rot = axis_angle_lerp(a["rotation"], b["rotation"], 0.5)
                bridge.pieces[idx] = {"position": pos.to_list(), "rotation": rot}
        for _ in range(repeats):
            anim.frames.extend(loop)
            if smooth:
                anim.frames.append(copy.deepcopy(bridge))
    else:
        for _ in range(repeats):
            anim.frames.extend(loop)
    save_animate_json(path, anim)
    return {"frames_added": len(loop) * repeats, "total": len(anim.frames)}


@mcp.tool()
def time_remap_frames(project_path: str, start_frame: int, end_frame: int,
                      speed: float = 2.0) -> dict[str, Any]:
    """Speed up (speed>1) or slow down (speed<1) a frame range by duplicating/skipping frames."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    span = anim.frames[start_frame:end_frame+1]
    if speed >= 1:
        # Skip every Nth frame
        step = int(round(speed))
        remapped = [span[i] for i in range(0, len(span), max(1, step))]
    else:
        # Duplicate each frame
        dupes = max(1, int(round(1.0 / speed)))
        remapped = []
        for f in span:
            for _ in range(dupes):
                import copy
                remapped.append(copy.deepcopy(f))
    anim.frames[start_frame:end_frame+1] = remapped
    save_animate_json(path, anim)
    return {"frames_before": len(span), "frames_after": len(remapped), "speed": speed, "total": len(anim.frames)}


@mcp.tool()
def ease_frames(project_path: str, start_frame: int, end_frame: int,
                easing: str = "EaseInOut") -> dict[str, Any]:
    """Apply easing to the piece positions across a frame range.
    Each piece's trajectory gets the easing curve applied."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or end_frame <= start_frame:
        return {"error": "Need at least 2 frames"}
    easing_map = {"linear": 0, "easein": 1, "easeout": 2, "easeinout": 3}
    ei = easing_map.get(easing.lower(), 3)
    span = end_frame - start_frame
    # Collect start/end states per piece
    first = anim.frames[start_frame]
    last = anim.frames[end_frame]
    for i in range(start_frame, end_frame + 1):
        t = (i - start_frame) / span
        eased = apply_easing(t, ei)
        for idx in list(anim.frames[i].pieces.keys()):
            a = first.pieces.get(idx)
            b = last.pieces.get(idx)
            if a and b:
                pos = lerp_vec3(Vec3.from_list(a["position"]),
                                Vec3.from_list(b["position"]), eased)
                rot = axis_angle_lerp(a["rotation"], b["rotation"], eased)
                anim.frames[i].pieces[idx] = {"position": pos.to_list(), "rotation": rot}
    save_animate_json(path, anim)
    return {"frames_updated": span + 1, "easing": easing}


@mcp.tool()
def follow_curve(project_path: str, piece_index: int,
                 control_points: list[list[float]],
                 frame_range: list[int] | None = None) -> dict[str, Any]:
    """Animate a piece along a cubic bezier path defined by 4 control points.
    control_points: [[x,y,z],[x,y,z],[x,y,z],[x,y,z]]"""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or not anim.frames:
        return {"error": "No frames"}
    if len(control_points) != 4:
        return {"error": "Need exactly 4 control points"}
    start_f, end_f = frame_range if frame_range else [0, len(anim.frames)-1]
    end_f = min(end_f, len(anim.frames)-1)
    span = end_f - start_f
    if span <= 0:
        return {"error": "Frame range too short"}
    pts = [Vec3.from_list(p) for p in control_points]
    # Also rotate to face along the path tangent
    for i in range(start_f, end_f + 1):
        t = (i - start_f) / span
        pos = cubic_bezier(t, *pts)
        # Tangent = derivative of bezier
        u = 1 - t
        tangent = (pts[1]-pts[0])*3*u*u + (pts[2]-pts[1])*6*u*t + (pts[3]-pts[2])*3*t*t
        tn = tangent.normalized()
        # Build a look-at rotation (Z up, tangent as forward)
        up = Vec3(0, 0, 1)
        fwd = tn
        right = up.cross(fwd).normalized()
        new_up = fwd.cross(right)
        rot = [right.x, right.y, right.z,
               new_up.x, new_up.y, new_up.z,
               fwd.x, fwd.y, fwd.z]
        anim.frames[i].pieces[piece_index] = {"position": pos.to_list(), "rotation": rot}
    save_animate_json(path, anim)
    return {"piece": piece_index, "frames_animated": span + 1,
            "start": control_points[0], "end": control_points[3]}


@mcp.tool()
def bounce_effect(project_path: str, piece_index: int,
                  start_frame: int = 0, end_frame: int | None = None,
                  height: float = 40, bounces: int = 3,
                  decay: float = 0.6) -> dict[str, Any]:
    """Animate a piece with a physics bounce. Piece drops from height, bounces with decay."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or not anim.frames:
        return {"error": "No frames"}
    end_frame = end_frame or len(anim.frames)-1
    end_frame = min(end_frame, len(anim.frames)-1)
    span = end_frame - start_frame
    if span <= 0:
        return {"error": "Frame range too short"}
    # Get starting X,Y position, bounce on Z
    pf = anim.frames[start_frame].pieces.get(piece_index)
    if not pf:
        return {"error": f"Piece {piece_index} not in frame {start_frame}"}
    base_x, base_y = pf["position"][0], pf["position"][1]
    for i in range(start_frame, end_frame + 1):
        t = (i - start_frame) / span
        # Simulate: drop, bounce, repeat
        phase = t * bounces
        b_idx = int(phase)
        local_t = phase - b_idx
        peak = height * (decay ** b_idx)
        z = peak * (1 - (2*local_t - 1)**2) if local_t < 0.5 else 0
        z = max(0, z * (1 - local_t * 0.5))
        if b_idx % 2 == 0:
            z = peak * (1 - (2*local_t - 1)**2) * (1 - local_t * 0.3)
        else:
            z = peak * 0.6 * (1 - (2*(1-local_t) - 1)**2) * local_t
        z = max(0, z)
        pos = anim.frames[i].pieces.get(piece_index, {}).get("position", [0,0,0])
        anim.frames[i].pieces[piece_index] = {
            "position": [base_x, base_y, z],
            "rotation": anim.frames[i].pieces.get(piece_index, {}).get("rotation", [1,0,0,0,1,0,0,0,1])
        }
    save_animate_json(path, anim)
    return {"piece": piece_index, "frames_animated": span + 1, "bounces": bounces}


@mcp.tool()
def swing_effect(project_path: str, piece_index: int,
                 start_frame: int = 0, end_frame: int | None = None,
                 amplitude: float = 30.0, period: float = 12.0,
                 axis: str = "X") -> dict[str, Any]:
    """Apply pendulum/swing oscillation to a piece's rotation.
    axis: 'X', 'Y', or 'Z'. Period is in frames."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    end_frame = end_frame or len(anim.frames)-1
    end_frame = min(end_frame, len(anim.frames)-1)
    axis_idx = {"X": 0, "Y": 1, "Z": 2}.get(axis.upper(), 0)
    for i in range(start_frame, end_frame + 1):
        angle = amplitude * math.sin(2 * PI * (i - start_frame) / period) * DTOR
        rmat = Mat44.identity()
        if axis_idx == 0:
            rmat = Mat44.rotation_x(angle)
        elif axis_idx == 1:
            rmat = Mat44.rotation_y(angle)
        else:
            rmat = Mat44.rotation_z(angle)
        pf = anim.frames[i].pieces.get(piece_index)
        if pf:
            cur = Mat44.from_pos_rot(Vec3.from_list(pf["position"]), pf["rotation"])
            result = cur.mul(rmat)
            anim.frames[i].pieces[piece_index] = {
                "position": result.get_translation().to_list(),
                "rotation": result.get_rotation_33()
            }
    save_animate_json(path, anim)
    return {"piece": piece_index, "frames_animated": end_frame - start_frame + 1, "axis": axis}


@mcp.tool()
def explosion_effect(project_path: str,
                     center: list[float] | None = None,
                     piece_indices: list[int] | None = None,
                     start_frame: int = 0, end_frame: int | None = None,
                     force: float = 50.0,
                     frame_range: list[int] | None = None) -> dict[str, Any]:
    """Pieces fly outward from a center point like an explosion."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    end_frame = end_frame or len(anim.frames)-1
    end_frame = min(end_frame, len(anim.frames)-1)
    if frame_range:
        start_frame, end_frame = frame_range
    span = end_frame - start_frame
    if span <= 0:
        return {"error": "Frame range too short"}
    # Determine which pieces
    if piece_indices:
        indices = piece_indices
    else:
        indices = sorted(anim.frames[start_frame].pieces.keys())
    ctr = Vec3.from_list(center) if center else Vec3(0, 0, 0)
    # Get start positions
    start_positions: dict[int, Vec3] = {}
    for idx in indices:
        pf = anim.frames[start_frame].pieces.get(idx)
        if pf:
            start_positions[idx] = Vec3.from_list(pf["position"])
    for i in range(start_frame, end_frame + 1):
        t = (i - start_frame) / span
        eased = apply_easing(t, EASING_EASE_OUT)
        for idx in indices:
            if idx not in start_positions:
                continue
            sp = start_positions[idx]
            dir = sp - ctr
            d = dir.norm()
            if d < 0.001:
                continue
            dir = dir * (1.0 / d)
            offset = dir * force * eased
            # Add some rotation spin
            spin = Mat44.rotation_z(eased * PI * 2 * (hash(str(idx)) % 3 + 1))
            rmat = Mat44.from_pos_rot(sp + offset, [1,0,0,0,1,0,0,0,1])
            rmat = rmat.mul(spin)
            anim.frames[i].pieces[idx] = {
                "position": (sp + offset).to_list(),
                "rotation": rmat.get_rotation_33()
            }
    save_animate_json(path, anim)
    return {"pieces_animated": len(indices), "frames_animated": span + 1}


@mcp.tool()
def shake_effect(project_path: str,
                 target: str = "camera",
                 amplitude: float = 5.0,
                 frequency: float = 0.5,
                 start_frame: int = 0, end_frame: int | None = None,
                 decay: float = 1.0,
                 piece_indices: list[int] | None = None) -> dict[str, Any]:
    """Add camera or piece shake. target='camera' shakes camera, 'pieces' shakes selected pieces.
    frequency in cycles per frame. decay=1 is constant, <1 fades out."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    end_frame = end_frame or len(anim.frames)-1
    end_frame = min(end_frame, len(anim.frames)-1)
    for i in range(start_frame, end_frame + 1):
        t = (i - start_frame) / max(end_frame - start_frame, 1)
        dcy = decay ** (i - start_frame)
        amp = amplitude * dcy
        noise_x = amp * (math.sin(i * frequency * 17.371) * 0.5 + math.sin(i * frequency * 31.847) * 0.5)
        noise_y = amp * (math.sin(i * frequency * 41.113) * 0.5 + math.sin(i * frequency * 23.441) * 0.5)
        noise_z = amp * (math.sin(i * frequency * 53.719) * 0.5 + math.sin(i * frequency * 61.987) * 0.5)
        if target == "camera":
            if anim.frames[i].camera:
                c = anim.frames[i].camera
                c["position"] = [c["position"][j] + [noise_x, noise_y, noise_z][j] for j in range(3)]
                c["target"] = [c["target"][j] + [noise_x*0.3, noise_y*0.3, noise_z*0.3][j] for j in range(3)]
        elif target == "pieces" and piece_indices:
            for idx in piece_indices:
                pf = anim.frames[i].pieces.get(idx)
                if pf:
                    anim.frames[i].pieces[idx]["position"] = [
                        pf["position"][j] + [noise_x, noise_y, noise_z][j] for j in range(3)
                    ]
    save_animate_json(path, anim)
    return {"target": target, "frames_affected": end_frame - start_frame + 1}


@mcp.tool()
def wave_effect(project_path: str,
                piece_indices: list[int],
                axis: str = "Z",
                amplitude: float = 20.0,
                wavelength: float = 4.0,
                start_frame: int = 0, end_frame: int | None = None) -> dict[str, Any]:
    """A wave propagates through pieces along their order index. Each piece oscillates sequentially."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    end_frame = end_frame or len(anim.frames)-1
    end_frame = min(end_frame, len(anim.frames)-1)
    axis_idx = {"X": 0, "Y": 1, "Z": 2}.get(axis.upper(), 2)
    for i in range(start_frame, end_frame + 1):
        t = (i - start_frame) / max(end_frame - start_frame, 1)
        for order, idx in enumerate(piece_indices):
            pf = anim.frames[i].pieces.get(idx)
            if not pf:
                continue
            wave_offset = math.sin(2 * PI * t - order * 2 * PI / wavelength)
            pos = Vec3.from_list(pf["position"])
            delta = [0, 0, 0]
            delta[axis_idx] = amplitude * wave_offset
            new_pos = pos + Vec3(*delta)
            anim.frames[i].pieces[idx]["position"] = new_pos.to_list()
    save_animate_json(path, anim)
    return {"pieces_animated": len(piece_indices), "frames_animated": end_frame - start_frame + 1}


@mcp.tool()
def batch_transform_pieces(project_path: str,
                           piece_indices: list[int],
                           translation: list[float] | None = None,
                           rotation: list[float] | None = None,
                           frame_range: list[int] | None = None) -> dict[str, Any]:
    """Translate and/or rotate multiple pieces across a frame range.
    translation: [dx,dy,dz] added to each piece. rotation: 3x3 matrix to apply."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    start_f, end_f = frame_range if frame_range else [0, len(anim.frames)-1]
    end_f = min(end_f, len(anim.frames)-1)
    tvec = Vec3.from_list(translation) if translation else Vec3()
    rmat = Mat44.from_pos_rot(Vec3(), rotation) if rotation else None
    for i in range(start_f, end_f + 1):
        for idx in piece_indices:
            pf = anim.frames[i].pieces.get(idx)
            if not pf:
                continue
            pos = Vec3.from_list(pf["position"]) + tvec
            rot = pf["rotation"]
            if rmat:
                cur = Mat44.from_pos_rot(pos, rot)
                result = cur.mul(rmat)
                pos = result.get_translation()
                rot = result.get_rotation_33()
            anim.frames[i].pieces[idx] = {"position": pos.to_list(), "rotation": rot}
    save_animate_json(path, anim)
    return {"pieces_changed": len(piece_indices), "frames_changed": end_f - start_f + 1}


@mcp.tool()
def stagger_pieces(project_path: str,
                   piece_indices: list[int],
                   start_delta: float = 0.0,
                   offset_frames: int = 2,
                   frame_range: list[int] | None = None) -> dict[str, Any]:
    """Offset the start of animation across pieces. Each subsequent piece starts
    `offset_frames` later. Use with interpolate_frames for delayed motion."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim:
        return {"error": "No animation data"}
    start_f, end_f = frame_range if frame_range else [0, len(anim.frames)-1]
    end_f = min(end_f, len(anim.frames)-1)
    # For each piece, shift its frames by offset*order
    for order, idx in enumerate(piece_indices):
        shift = int(start_delta + order * offset_frames)
        if shift == 0:
            continue
        # Get original frames
        orig = []
        for i in range(start_f, end_f + 1):
            orig.append(anim.frames[i].pieces.get(idx))
        # Re-insert shifted
        for i, pf in enumerate(orig):
            target_i = start_f + i + shift
            if target_i <= end_f and pf:
                anim.frames[target_i].pieces[idx] = pf
            elif target_i <= end_f and not pf:
                anim.frames[target_i].pieces.pop(idx, None)
    save_animate_json(path, anim)
    return {"pieces_staggered": len(piece_indices), "offset_frames": offset_frames}


@mcp.tool()
def set_frame_camera(project_path: str, frame_index: int,
                     position: list[float],
                     target: list[float],
                     up: list[float] | None = None,
                     projection: int = 0) -> dict[str, Any]:
    """Set the camera for a specific frame. projection: 0=Perspective, 1=Orthographic."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_index >= len(anim.frames):
        return {"error": "Frame not found"}
    anim.frames[frame_index].camera = {
        "position": position, "target": target,
        "up": up or [0, 1, 0], "projection": projection
    }
    save_animate_json(path, anim)
    return {"frame": frame_index, "camera": anim.frames[frame_index].camera}


@mcp.tool()
def randomize_frame(project_path: str, frame_index: int,
                    position_noise: float = 5.0,
                    rotation_noise: float = 10.0,
                    piece_indices: list[int] | None = None) -> dict[str, Any]:
    """Add random noise to piece positions/rotations in a frame.
    rotation_noise is in degrees."""
    path = Path(project_path).expanduser()
    anim = load_animate_json(path)
    if not anim or frame_index >= len(anim.frames):
        return {"error": "Frame not found"}
    import random
    rng = random.Random()
    indices = piece_indices or list(anim.frames[frame_index].pieces.keys())
    for idx in indices:
        pf = anim.frames[frame_index].pieces.get(idx)
        if not pf:
            continue
        pos = Vec3.from_list(pf["position"])
        pos += Vec3(rng.uniform(-1,1)*position_noise,
                    rng.uniform(-1,1)*position_noise,
                    rng.uniform(-1,1)*position_noise)
        noise_angle = rng.uniform(-1,1) * rotation_noise * DTOR
        cur = Mat44.from_pos_rot(pos, pf["rotation"])
        rmat = Mat44.rotation_x(rng.uniform(-1,1)*noise_angle)
        rmat = rmat.mul(Mat44.rotation_y(rng.uniform(-1,1)*noise_angle))
        rmat = rmat.mul(Mat44.rotation_z(rng.uniform(-1,1)*noise_angle))
        cur = cur.mul(rmat)
        anim.frames[frame_index].pieces[idx] = {
            "position": cur.get_translation().to_list(),
            "rotation": cur.get_rotation_33()
        }
    save_animate_json(path, anim)
    return {"frame": frame_index, "pieces_randomized": len(indices)}


@mcp.tool()
def get_project_status(project_path: str) -> dict[str, Any]:
    """Get a concise summary of the project: piece count, frame count, groups, minifigs."""
    path = Path(project_path).expanduser()
    proj = parse_ldraw(path)
    anim = load_animate_json(path)
    minifigs = list(set(g.minifig_family for g in proj.groups if g.minifig_family))
    frame_count = len(anim.frames) if anim else 0
    unanimated = len(anim.frames) == 0 if anim else True
    return {
        "file": str(path),
        "pieces": len(proj.pieces),
        "groups": len(proj.groups),
        "minifigs": minifigs,
        "frames": frame_count,
        "status": "no animation data" if unanimated else f"{frame_count} frames"
    }


def main():
    mcp.run()


if __name__ == "__main__":
    main()
