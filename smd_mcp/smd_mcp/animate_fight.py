"""Generate two fighting minifigs — writes .ldr + .animate.json to ~/Desktop/"""
from pathlib import Path
import sys, math, json
sys.path.insert(0, str(Path(__file__).parent.parent))
from smd_mcp.server import Vec3, Mat44, AnimateData, AnimateFrame, save_animate_json
from smd_mcp.server import _wizard_matrix

DTOR = math.pi/180.0; RTOD = 180.0/math.pi

def rot_x(a): return Mat44.rotation_x(a*DTOR)
def rot_y(a): return Mat44.rotation_y(a*DTOR)
def rot_z(a): return Mat44.rotation_z(a*DTOR)
def trans(v): return Mat44.translation(v)

WIZARD_ROOT_OFFSET = Vec3(0, 0, 72)

def limb_tf(limb, angle, pos_offset=Vec3(0,0,0)):
    """World position+rotation for a limb. pos_offset is the minifig's foot position."""
    wm = _wizard_matrix(limb, angle)
    p = wm.t - WIZARD_ROOT_OFFSET + pos_offset
    return p, wm.get_rotation_33()

def pack(pos, rot):
    return {"position": pos.to_list(), "rotation": rot}

# ── fighters on the same Z=0 ground plane, separated in X ────────────────
# Each fighter is defined by its foot position (x, y, 0). The wizard places
# the body at z=72 above that.
A_POS = Vec3(-35, 0, 72)   # left fighter (wizard root = waist height)
B_POS = Vec3(35, 0, 72)    # right fighter

# ── piece-to-group mapping ───────────
# For each fighter we have 8 pieces:  torso, head, rarm, rhand, larm, lhand, rleg, lleg
# in that order.
def make_ldr_pieces(pos_offset):
    pieces = []
    for limb, key in [("BODY","torso"),("BODY","hips"),("HEAD","head"),
                      ("RARM","rarm"),("RHAND","rhand"),
                      ("LARM","larm"),("LHAND","lhand"),
                      ("RLEG","rleg"),("LLEG","lleg")]:
        p, rot = limb_tf(limb, 0, pos_offset)
        pieces.append({"key": key, "pos": p, "rot": rot})
    return pieces

A_BASE = make_ldr_pieces(A_POS)
B_BASE = make_ldr_pieces(B_POS)

# ── build LDR ─────────────────────────────────────────────────────────────
def ldraw_line(code, pos, rot, fname):
    tx, ty, zneg = pos.x, pos.y, -pos.z
    lr = [rot[0], -rot[6], -rot[3], -rot[2], rot[8], rot[5], -rot[1], rot[7], rot[4]]
    vs = " ".join(f"{v:.6f}" for v in lr)
    return f"1 {code} {tx:.6f} {zneg:.6f} {ty:.6f} {vs} {fname}"

FNAME = {"torso":"973.dat","hips":"3815.dat","head":"3626bp01.dat","rarm":"3819.dat","rhand":"3820.dat",
         "larm":"3818.dat","lhand":"3820.dat","rleg":"3817.dat","lleg":"3816.dat"}
COLOR = {"torso":4,"hips":4,"head":14,"rarm":4,"rhand":14,"larm":4,"lhand":14,"rleg":4,"lleg":4}
COLOR_B = {"torso":1,"hips":1,"head":14,"rarm":1,"rhand":14,"larm":1,"lhand":14,"rleg":1,"lleg":1}

piece_index = 0
lines = ["0 FILE fight.ldr\n"]
for prefix, base, cmap in [("Alpha", A_BASE, COLOR), ("Beta", B_BASE, COLOR_B)]:
    for name in ["Torso","Head","RightArm","LeftArm","RightLeg","LeftLeg"]:
        key_map = {"Torso":["torso","hips"],"Head":["head"],"RightArm":["rarm","rhand"],
                   "LeftArm":["larm","lhand"],"RightLeg":["rleg"],"LeftLeg":["lleg"]}
        lines.append(f"0 GROUP 1 Minifig {prefix} {name} #1")
        for k in key_map[name]:
            p = base[[pi["key"] for pi in base].index(k)]["pos"]
            r = base[[pi["key"] for pi in base].index(k)]["rot"]
            lines.append(ldraw_line(cmap[k], p, r, FNAME[k]))
            piece_index += 1
        lines.append("0 END GROUP")
LDR = Path.home() / "Desktop" / "fight.ldr"
LDR.write_text("\n".join(lines))

# Build piece map: (fighter_prefix, key) -> index
pi_map = {}
idx = 0
for prefix, _, _ in [("Alpha", A_BASE, COLOR), ("Beta", B_BASE, COLOR_B)]:
    for name in ["Torso","Head","RightArm","LeftArm","RightLeg","LeftLeg"]:
        keys = {"Torso":["torso","hips"],"Head":["head"],"RightArm":["rarm","rhand"],
                "LeftArm":["larm","lhand"],"RightLeg":["rleg"],"LeftLeg":["lleg"]}[name]
        for k in keys:
            pi_map[(prefix, k)] = idx
            idx += 1

print(f"wrote {idx} pieces, groups")

# ── FRAME GENERATION ───────────────────────────────────────────────────────
# We define movement per body part as (position offset, angle) per fighter per frame.

def make_frame(alpha_state, beta_state, cam):
    """alpha/beta_state: dict of {key: (pos_offset, angle_deg)} or {key: angle_deg}
    pos_offset: Vec3 offset from A_POS / B_POS
    """
    pieces = {}
    for prefix, pos0, state in [("Alpha", A_POS, alpha_state), ("Beta", B_POS, beta_state)]:
        for limb_key in ["torso","hips","head","rarm","rhand","larm","lhand","rleg","lleg"]:
            idx = pi_map[(prefix, limb_key)]
            val = state.get(limb_key, 0)
            if isinstance(val, tuple):
                offset, angle = val  # (Vec3 offset, angle)
            else:
                offset, angle = Vec3(0,0,0), val
            
            pos = pos0 + offset
            # For non-BODY pieces, use wizard to compute relative position
            limb_type = {"torso":"BODY","hips":"BODY","head":"HEAD","rarm":"RARM","rhand":"RHAND",
                        "larm":"LARM","lhand":"LHAND","rleg":"RLEG","lleg":"LLEG"}[limb_key]
            wm = _wizard_matrix(limb_type, angle)
            piece_pos = wm.t - WIZARD_ROOT_OFFSET + pos
            pieces[idx] = pack(piece_pos, wm.get_rotation_33())
    return {"pieces": pieces, "camera": cam}

def cam(pos, tgt, up=[0,0,1]):
    return {"position": pos, "target": tgt, "up": up, "projection": 0}

lerp = lambda a,b,t: a+(b-a)*t
lerp_v = lambda a,b,t: [lerp(a[i],b[i],t) for i in range(3)]

frames = []

# ── 120 FRAMES (5s @ 24fps) ───────────────────────────────────────────────

# Phase 1: Face-off (0-19)
for i in range(20):
    fi = i/20.0
    t = (i+1)/20.0
    a_state = {"torso": 0, "head": 0, "rarm": lerp(0,15,t), "larm": lerp(0,-15,t),
               "rleg": lerp(0,5,t), "lleg": lerp(0,-5,t)}
    b_state = {"torso": 0, "head": 0, "rarm": lerp(0,-20,t), "larm": lerp(0,-20,t),
               "rleg": lerp(0,-5,t), "lleg": lerp(0,5,t)}
    cp = lerp_v([0,-140,75], [0,-120,72], t)
    ct = lerp_v([0,0,72], [0,0,72], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 2: Alpha winds up (20-35)
for i in range(16):
    t = (i+1)/16.0
    ra = lerp(15, 75, t)
    la = lerp(-15, -35, t)
    off = Vec3(0, lerp(0, 8, t), 0)
    a_state = {"torso":(off, lerp(0,8,t)), "head":-8, "rarm":ra, "larm":la,
               "rleg":lerp(5,12,t), "lleg":lerp(-5,-12,t)}
    b_state = {"torso":(Vec3(0,lerp(0,-5,t),0), 0), "head":5, "rarm":-35, "larm":-35,
               "rleg":5, "lleg":-5}
    cp = lerp_v([0,-120,72], [15,-80,70], t)
    ct = lerp_v([0,0,72], [0,0,72], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 3: Alpha PUNCH (36-47)
for i in range(12):
    t = (i+1)/12.0
    if i < 4:  # swing
        ra = lerp(75, -60, t*2.5)
    elif i < 8:  # impact + hold
        ra = lerp(75, -60, 1.0)  # at max punch
    else:  # retract
        ra = lerp(-60, -30, (t-0.666)*3)
    la = lerp(-35, -25, t)
    off_a = Vec3(0, lerp(8, 18, t), 0)
    off_b = Vec3(0, lerp(-5, -15, t), lerp(0, 3, t))
    # On impact frame 4-5, head snaps
    hb = 15 if i < 4 else (45 if i < 6 else lerp(45, 25, (t-0.5)*2))
    rb = lerp(-35, -15, t)
    lb = lerp(-35, -15, t)
    a_state = {"torso":(off_a, lerp(8,12,min(t*2,1))), "head":-10,
               "rarm":ra, "larm":la, "rleg":12, "lleg":-12}
    b_state = {"torso":(off_b, lerp(0,-10,min(t*2,1))), "head":hb,
               "rarm":rb, "larm":lb, "rleg":lerp(5,15,t), "lleg":lerp(-5,-15,t)}
    # Camera: tight, side-on to the impact
    cp = lerp_v([15,-80,70], [20,-40,55], t)
    ct = lerp_v([0,0,72], [-5,0,65], t)
    # Camera shake on impact
    if i in (4,5):
        shake = 4*((-1)**i)
        cp = [cp[0]+shake, cp[1], cp[2]]
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 4: Beta staggers back (48-67)
for i in range(20):
    t = (i+1)/20.0
    a_state = {"torso":(Vec3(0,lerp(18,10,t),0), lerp(12,5,t)), "head":-5,
               "rarm":lerp(-30,0,t), "larm":lerp(-25,-10,t), "rleg":10, "lleg":-10}
    rb = lerp(-15, 25, t)
    lb = lerp(-15, 15, t)
    off_b = Vec3(0, lerp(-15, -30, t), lerp(3, -5, t))
    b_state = {"torso":(off_b, lerp(-10, 0, t)), "head":lerp(25, 15, t),
               "rarm":rb, "larm":lb, "rleg":lerp(15,20,t), "lleg":lerp(-15,-20,t)}
    cp = lerp_v([20,-40,55], [0,-60,65], t)
    ct = lerp_v([-5,0,65], [-3,0,60], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 5: Beta counter-windup (68-79)
for i in range(12):
    t = (i+1)/12.0
    # Beta right arm winds up for a punch
    off_b = Vec3(0, lerp(-30, -20, t), lerp(-5, -3, t))
    off_a = Vec3(0, lerp(10, 5, t), 0)
    a_state = {"torso":(off_a, 5), "head":5,
               "rarm":0, "larm":-10, "rleg":5, "lleg":-5}
    b_state = {"torso":(off_b, lerp(0,8,t)), "head":5,
               "rarm":lerp(25, 80, t), "larm":lerp(15, -30, t),
               "rleg":lerp(20, -10, t), "lleg":lerp(-20, 10, t)}
    # Camera from behind Beta, low angle
    cp = lerp_v([0,-60,65], [25,-30,45], t)
    ct = lerp_v([-3,0,60], [-5,0,68], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 6: Beta swings, Alpha ducks (80-95)
for i in range(16):
    t = (i+1)/16.0
    # Beta punch forward
    rb = lerp(80, -50, t)
    lb = lerp(-30, -20, t)
    off_b = Vec3(0, lerp(-20, -10, t), lerp(-3, 0, t))
    # Alpha ducks down
    off_a = Vec3(0, lerp(5, 0, t), lerp(0, -12, t))  # drop z
    ra = lerp(0, -35, t)
    la = lerp(-10, 0, t)
    a_state = {"torso":(off_a, lerp(5, -15, t)), "head":lerp(5, 20, t),
               "rarm":ra, "larm":la, "rleg":5, "lleg":-5}
    b_state = {"torso":(off_b, lerp(8, -5, t)), "head":-5,
               "rarm":rb, "larm":lb, "rleg":lerp(-10, 5, t), "lleg":lerp(10, -5, t)}
    cp = lerp_v([25,-30,45], [-25,-15,30], t)
    ct = lerp_v([-5,0,68], [0,0,60], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 7: Alpha uppercut (96-107)
for i in range(12):
    t = (i+1)/12.0
    # Alpha rises and uppercuts
    off_a = Vec3(0, lerp(0, 5, t), lerp(-12, 2, t))  # rise up
    la = lerp(0, 50, t)  # left arm wind (will use for uppercut)
    ra = lerp(-35, -15, t)
    a_state = {"torso":(off_a, lerp(-15, 5, t)), "head":lerp(20, -10, t),
               "rarm":ra, "larm":la, "rleg":5, "lleg":-5}
    # Beta recovering
    off_b = Vec3(0, lerp(-10, -12, t), 0)
    b_state = {"torso":(off_b, lerp(-5, -8, t)), "head":10,
               "rarm":lerp(-50, -10, t), "larm":lerp(-20, 10, t),
               "rleg":lerp(5, 10, t), "lleg":lerp(-5, -10, t)}
    cp = lerp_v([-25,-15,30], [-10,-25,40], t)
    ct = lerp_v([0,0,60], [-5,0,65], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

# Phase 8: Alpha uppercut connects, Beta goes down (108-119)
for i in range(12):
    t = (i+1)/12.0
    if i < 6:
        # Uppercut strikes
        la = lerp(50, -45, t*2)  # arm swings up through
        ra = lerp(-15, -10, t)
        off_a = Vec3(0, lerp(5, 10, t), lerp(2, 4, t))
        # Beta head snaps up, body goes back
        off_b = Vec3(0, lerp(-12, -25, t), lerp(0, 8, t))
        b_state = {"torso":(off_b, lerp(-8, -20, t)), "head":lerp(10, 55, t),
                   "rarm":lerp(-10, 30, t), "larm":lerp(10, 25, t),
                   "rleg":lerp(10, 25, t), "lleg":lerp(-10, -25, t)}
    else:
        # Beta collapses
        t2 = (i-6)/6.0
        la = lerp(-45, -20, t2)
        ra = lerp(-10, 0, t2)
        off_a = Vec3(0, lerp(10, 8, t2), lerp(4, 2, t2))
        off_b = Vec3(0, lerp(-25, -35, t2), lerp(8, -10, t2))
        b_state = {"torso":(off_b, lerp(-20, -30, t2)), "head":lerp(55, 20, t2),
                   "rarm":lerp(30, 60, t2), "larm":lerp(25, 15, t2),
                   "rleg":lerp(25, 35, t2), "lleg":lerp(-25, -35, t2)}
    a_state = {"torso":(off_a, lerp(5, 10, t)), "head":-5,
               "rarm":ra, "larm":la, "rleg":5, "lleg":-5}
    cp = lerp_v([-10,-25,40], [5,-35,55], t)
    ct = lerp_v([-5,0,65], [0,-5,50], t)
    frames.append(make_frame(a_state, b_state, cam(cp, ct)))

print(f"built {len(frames)} frames")

# ── write animate.json ─────────────────────────────────────────────────────
anim = AnimateData(model_name="fight.ldr")
for fd in frames:
    aff = AnimateFrame()
    aff.pieces = fd["pieces"]
    aff.camera = fd["camera"]
    anim.frames.append(aff)

save_animate_json(str(LDR), anim)
print(f"saved {len(anim.frames)} frames to {LDR}.animate.json")
