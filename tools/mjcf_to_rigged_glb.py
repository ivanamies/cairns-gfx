#!/usr/bin/env python3
"""Convert a MuJoCo MJCF body model into a single rigged GLB.

Written for the NeuroMechFly Drosophila model shipped with `flygym`
(`flygym/data/mjcf/neuromechfly_seqik_kinorder_ypr.xml`), but the parsing is
generic over MJCF `<worldbody>` trees whose geometry is `<geom type="mesh">`.

Output shape (dictated by the consuming engine's glTF loader, which is
fastgltf-based and quite strict):

  * GLB binary, ONE embedded buffer, no external .bin / no images.
  * Scene 0 lists the root nodes.
  * <= 256 nodes total, <= 256 skin joints (indices packed into 8 bits).
  * Exactly one mesh with exactly one TRIANGLES primitive carrying
    POSITION / NORMAL / TEXCOORD_0 / JOINTS_0 / WEIGHTS_0 + uint indices.
  * Exactly one skin (index 0) with inverseBindMatrices and a skeleton root.
  * At least one animation clip (the engine refuses to build a skin without
    one), so we emit a constant 1-second "idle" clip that reproduces the bind
    pose exactly.

Rig layout: one node per MJCF `<body>` plus one node per MJCF `<joint>`.  A
body's joints are emitted as a chain, in document order, between the body's
own frame node and its children:

    parent_last -> B(pos,quat) -> j1 -> j2 -> ... -> jn -> (child bodies)

Joint nodes are identity-rotation at bind time and translate by the joint's
`pos`; the next node in the chain subtracts that translation again so the bind
pose is geometrically identical to MuJoCo's zero pose (standard pivot chain).
Geometry from body B is rigidly bound (weight 1.0) to B's *last* joint node --
the frame the geometry actually rides -- or to B's body node if B has no
joints.  Vertices are baked into bind-pose world space, so the inverse bind
matrix of a joint is simply the inverse of its bind world matrix.

Everything is deterministic: document order is preserved, no dict-iteration
order is relied upon, and the decimator is seed-free.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import trimesh

# --------------------------------------------------------------------------
# glTF constants (avoid depending on pygltflib's constant names)
# --------------------------------------------------------------------------
FLOAT = 5126
UNSIGNED_BYTE = 5121
UNSIGNED_SHORT = 5123
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
TRIANGLES = 4

MAX_NODES = 256   # Engine::kAnimMaxNodes
MAX_JOINTS = 256  # Engine::kAnimMaxJoints

# MJCF is Z-up, glTF is Y-up.  -90 degrees about X maps (x, y, z) -> (x, z, -y),
# as a glTF (x, y, z, w) quaternion.
MJCF_TO_GLTF_YUP = (-0.7071067811865476, 0.0, 0.0, 0.7071067811865476)
IDENTITY_ROTATION = (0.0, 0.0, 0.0, 1.0)


# --------------------------------------------------------------------------
# Small transform helpers.  Matrices are 4x4 row-major numpy (column vectors:
# world = M @ local), quaternions are handled explicitly at the boundaries
# because MuJoCo uses (w, x, y, z) while glTF uses (x, y, z, w).
# --------------------------------------------------------------------------
def _floats(text: Optional[str], default: Sequence[float]) -> np.ndarray:
    """Parse a whitespace-separated float list, falling back to `default`."""
    if text is None:
        return np.array(default, dtype=np.float64)
    vals = [float(v) for v in text.split()]
    return np.array(vals, dtype=np.float64)


def quat_wxyz_to_matrix(q: Sequence[float]) -> np.ndarray:
    """MuJoCo (w,x,y,z) quaternion -> 4x4 rotation matrix."""
    w, x, y, z = q
    n = float(np.sqrt(w * w + x * x + y * y + z * z))
    if n == 0.0:
        return np.eye(4)
    w, x, y, z = w / n, x / n, y / n, z / n
    m = np.eye(4)
    m[:3, :3] = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    return m


def matrix_to_quat_xyzw(m: np.ndarray) -> List[float]:
    """4x4 (or 3x3) rotation matrix -> glTF (x,y,z,w) quaternion."""
    r = m[:3, :3]
    tr = r[0, 0] + r[1, 1] + r[2, 2]
    if tr > 0.0:
        s = np.sqrt(tr + 1.0) * 2.0
        w = 0.25 * s
        x = (r[2, 1] - r[1, 2]) / s
        y = (r[0, 2] - r[2, 0]) / s
        z = (r[1, 0] - r[0, 1]) / s
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = np.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2]) * 2.0
        w = (r[2, 1] - r[1, 2]) / s
        x = 0.25 * s
        y = (r[0, 1] + r[1, 0]) / s
        z = (r[0, 2] + r[2, 0]) / s
    elif r[1, 1] > r[2, 2]:
        s = np.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2]) * 2.0
        w = (r[0, 2] - r[2, 0]) / s
        x = (r[0, 1] + r[1, 0]) / s
        y = 0.25 * s
        z = (r[1, 2] + r[2, 1]) / s
    else:
        s = np.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1]) * 2.0
        w = (r[1, 0] - r[0, 1]) / s
        x = (r[0, 2] + r[2, 0]) / s
        y = (r[1, 2] + r[2, 1]) / s
        z = 0.25 * s
    q = np.array([x, y, z, w], dtype=np.float64)
    q /= np.linalg.norm(q)
    return [float(v) for v in q]


def euler_to_matrix(angles: Sequence[float], seq: str, radians: bool) -> np.ndarray:
    """MJCF `euler` -> 4x4.  `seq` is the compiler's `eulerseq` (e.g. "XYZ").

    MuJoCo composes intrinsic rotations for uppercase axis letters and
    extrinsic for lowercase; the default "xyz" is extrinsic.  We build the
    product accordingly.
    """
    a = np.asarray(angles, dtype=np.float64)
    if not radians:
        a = np.deg2rad(a)
    m = np.eye(4)
    for angle, axis_char in zip(a, seq):
        c, s = np.cos(angle), np.sin(angle)
        step = np.eye(4)
        lower = axis_char.lower()
        if lower == "x":
            step[:3, :3] = [[1, 0, 0], [0, c, -s], [0, s, c]]
        elif lower == "y":
            step[:3, :3] = [[c, 0, s], [0, 1, 0], [-s, 0, c]]
        elif lower == "z":
            step[:3, :3] = [[c, -s, 0], [s, c, 0], [0, 0, 1]]
        else:
            raise ValueError("bad eulerseq axis %r" % axis_char)
        # Intrinsic (uppercase): post-multiply.  Extrinsic: pre-multiply.
        m = m @ step if axis_char.isupper() else step @ m
    return m


def axis_angle_to_matrix(vals: Sequence[float], radians: bool) -> np.ndarray:
    """MJCF `axisangle` (ax ay az angle) -> 4x4."""
    axis = np.asarray(vals[:3], dtype=np.float64)
    angle = float(vals[3])
    if not radians:
        angle = np.deg2rad(angle)
    n = np.linalg.norm(axis)
    if n == 0.0:
        return np.eye(4)
    axis = axis / n
    x, y, z = axis
    c, s, t = np.cos(angle), np.sin(angle), 1.0 - np.cos(angle)
    m = np.eye(4)
    m[:3, :3] = [
        [t * x * x + c, t * x * y - s * z, t * x * z + s * y],
        [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
        [t * x * z - s * y, t * y * z + s * x, t * z * z + c],
    ]
    return m


def frame_matrix(elem: ET.Element, radians: bool, eulerseq: str) -> np.ndarray:
    """Local frame of an MJCF element from its pos + orientation attribute."""
    pos = _floats(elem.get("pos"), (0.0, 0.0, 0.0))
    if elem.get("quat") is not None:
        rot = quat_wxyz_to_matrix(_floats(elem.get("quat"), (1, 0, 0, 0)))
    elif elem.get("euler") is not None:
        rot = euler_to_matrix(_floats(elem.get("euler"), (0, 0, 0)), eulerseq, radians)
    elif elem.get("axisangle") is not None:
        rot = axis_angle_to_matrix(_floats(elem.get("axisangle"), (0, 0, 1, 0)), radians)
    else:
        rot = np.eye(4)
    m = rot.copy()
    m[:3, 3] = pos
    return m


# --------------------------------------------------------------------------
# Rig model
# --------------------------------------------------------------------------
class Node:
    """One emitted glTF node (either an MJCF body frame or an MJCF joint)."""

    __slots__ = ("name", "kind", "parent", "translation", "rotation",
                 "mjcf_joint", "axis", "range", "world")

    def __init__(self, name: str, kind: str, parent: int,
                 translation: np.ndarray, rotation: List[float],
                 mjcf_joint: Optional[str] = None,
                 axis: Optional[List[float]] = None,
                 rng: Optional[List[float]] = None):
        self.name = name
        self.kind = kind                # "body" | "joint"
        self.parent = parent            # index into the node list, -1 for root
        self.translation = translation  # local translation (float64 xyz)
        self.rotation = rotation        # local rotation, glTF (x,y,z,w)
        self.mjcf_joint = mjcf_joint
        self.axis = axis                # hinge axis in the node's own frame
        self.range = rng
        self.world = np.eye(4)          # bind-pose world matrix (filled later)


class GeomBinding:
    """One `<geom type="mesh">` and the node whose frame it rides."""

    __slots__ = ("mesh_name", "file", "scale", "local", "node_index", "body")

    def __init__(self, mesh_name, file, scale, local, node_index, body):
        self.mesh_name = mesh_name
        self.file = file
        self.scale = scale        # MJCF asset mesh scale (3,)
        self.local = local        # geom frame inside the owning body frame
        self.node_index = node_index
        self.body = body


def parse_mjcf(mjcf_path: str, meshdir_override: Optional[str]):
    """Parse the MJCF into (nodes, geoms, meshdir).

    Nodes come out in DFS document order, so every parent precedes its
    children -- which is what the engine's node walk wants anyway.
    """
    tree = ET.parse(mjcf_path)
    root = tree.getroot()

    compiler = root.find("compiler")
    angle = (compiler.get("angle") if compiler is not None else None) or "degree"
    radians = angle.strip().lower() == "radian"
    eulerseq = (compiler.get("eulerseq") if compiler is not None else None) or "xyz"
    mjcf_dir = os.path.dirname(os.path.abspath(mjcf_path))
    if meshdir_override:
        meshdir = os.path.abspath(meshdir_override)
    else:
        md = compiler.get("meshdir") if compiler is not None else None
        meshdir = os.path.abspath(os.path.join(mjcf_dir, md)) if md else mjcf_dir

    # <asset><mesh name= file= scale=>: sorted for a stable report, but lookups
    # are by name so order does not affect output.
    mesh_assets: Dict[str, Tuple[str, np.ndarray]] = {}
    asset = root.find("asset")
    if asset is not None:
        for m in asset.findall("mesh"):
            name = m.get("name")
            f = m.get("file")
            if name is None or f is None:
                continue
            mesh_assets[name] = (f, _floats(m.get("scale"), (1.0, 1.0, 1.0)))

    nodes: List[Node] = []
    geoms: List[GeomBinding] = []

    def walk(body_elem: ET.Element, parent_node: int,
             parent_world: np.ndarray, parent_pivot: np.ndarray) -> None:
        """Emit body node + joint chain, then recurse into child bodies.

        `parent_world` is the parent *body*'s bind world matrix (pure MJCF
        forward kinematics); `parent_pivot` is the translation the parent's
        joint chain has already accumulated relative to that body frame, which
        this body's own translation must cancel out.
        """
        name = body_elem.get("name") or "body_%d" % len(nodes)
        local = frame_matrix(body_elem, radians, eulerseq)
        body_world = parent_world @ local

        body_index = len(nodes)
        nodes.append(Node(
            name=name,
            kind="body",
            parent=parent_node,
            # Cancel the parent's accumulated joint-pivot offset.  Parent joint
            # nodes are identity-rotation at bind time, so their translations
            # simply add and this subtraction restores the exact MJCF frame.
            translation=local[:3, 3] - parent_pivot,
            rotation=matrix_to_quat_xyzw(local),
        ))

        # Joint chain, in document order.  Each joint node translates by the
        # joint's pos; the next node subtracts the previous joint's pos.
        last = body_index
        pivot = np.zeros(3)
        for j in body_elem.findall("joint"):
            jtype = (j.get("type") or "hinge").strip()
            jpos = _floats(j.get("pos"), (0.0, 0.0, 0.0))
            jaxis = _floats(j.get("axis"), (0.0, 0.0, 1.0))
            jrange = j.get("range")
            rng = None
            if jrange is not None:
                r = _floats(jrange, (0.0, 0.0))
                rng = [float(r[0]), float(r[1])]
                if not radians:
                    rng = [float(np.deg2rad(v)) for v in rng]
            jname = j.get("name") or "%s_joint_%d" % (name, len(nodes))
            if jtype not in ("hinge", "ball", "slide"):
                # "free" joints have no fixed pivot; not present in this model.
                raise ValueError("unsupported joint type %r on body %r"
                                 % (jtype, name))
            nodes.append(Node(
                name=jname,
                kind="joint",
                parent=last,
                translation=jpos - pivot,
                rotation=[0.0, 0.0, 0.0, 1.0],  # identity at bind time
                mjcf_joint=jname,
                axis=[float(v) for v in jaxis],
                rng=rng,
            ))
            last = len(nodes) - 1
            pivot = jpos

        # Mesh geoms of this body ride the last node of the chain.
        for g in body_elem.findall("geom"):
            if (g.get("type") or "sphere") != "mesh":
                continue
            mesh_name = g.get("mesh")
            if mesh_name is None or mesh_name not in mesh_assets:
                continue
            f, scale = mesh_assets[mesh_name]
            geoms.append(GeomBinding(
                mesh_name=mesh_name,
                file=f,
                scale=scale,
                local=frame_matrix(g, radians, eulerseq),
                node_index=last,
                body=name,
            ))

        for child in body_elem.findall("body"):
            walk(child, last, body_world, pivot)

    worldbody = root.find("worldbody")
    if worldbody is None:
        raise ValueError("MJCF has no <worldbody>")
    for top in worldbody.findall("body"):
        walk(top, -1, np.eye(4), np.zeros(3))

    return nodes, geoms, meshdir, mesh_assets


def compute_bind_world(nodes: List[Node]) -> None:
    """Fill Node.world by composing local TRS down the (parent-first) list."""
    for i, n in enumerate(nodes):
        local = quat_wxyz_to_matrix(
            [n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]])
        local[:3, 3] = n.translation
        n.world = local if n.parent < 0 else nodes[n.parent].world @ local


def verify_forward_kinematics(mjcf_path: str, nodes: List[Node],
                              radians: bool, eulerseq: str) -> float:
    """Independently re-run MJCF zero-pose FK and compare to the rig's bind pose.

    Returns the maximum absolute deviation over all body frames.
    """
    tree = ET.parse(mjcf_path)
    by_name = {n.name: n for n in nodes if n.kind == "body"}
    worst = 0.0

    def walk(elem: ET.Element, parent_world: np.ndarray) -> None:
        nonlocal worst
        for b in elem.findall("body"):
            w = parent_world @ frame_matrix(b, radians, eulerseq)
            node = by_name.get(b.get("name"))
            if node is not None:
                worst = max(worst, float(np.max(np.abs(node.world - w))))
            walk(b, w)

    walk(tree.getroot().find("worldbody"), np.eye(4))
    return worst


# --------------------------------------------------------------------------
# Geometry: load, transform into bind-pose world space, weld, decimate
# --------------------------------------------------------------------------
def decimate(mesh: trimesh.Trimesh, target_faces: int) -> trimesh.Trimesh:
    """Reduce `mesh` to about `target_faces` triangles, deterministically.

    Prefers trimesh's quadric decimation (needs the optional
    `fast_simplification` backend); falls back to vertex clustering, which
    only needs numpy.
    """
    if len(mesh.faces) <= target_faces or target_faces < 4:
        return mesh
    try:
        out = mesh.simplify_quadric_decimation(face_count=int(target_faces))
        if out is not None and len(out.faces) > 0:
            return out
    except Exception:  # backend missing / degenerate input -> fall through
        pass
    return vertex_cluster_decimate(mesh, target_faces)


def vertex_cluster_decimate(mesh: trimesh.Trimesh,
                            target_faces: int) -> trimesh.Trimesh:
    """Grid vertex clustering: bisect the cell size until we hit the budget."""
    extent = float(np.max(mesh.extents))
    if extent <= 0.0:
        return mesh

    def cluster(pitch: float) -> trimesh.Trimesh:
        keys = np.floor(mesh.vertices / pitch).astype(np.int64)
        # numpy >= 2.0 returns `inverse` shaped (n, 1) for axis-wise unique.
        _, inverse = np.unique(keys, axis=0, return_inverse=True)
        inverse = np.asarray(inverse).reshape(-1)
        n_clusters = int(inverse.max()) + 1
        # Cluster representative = centroid of its member vertices.
        verts = np.zeros((n_clusters, 3), dtype=np.float64)
        counts = np.zeros(n_clusters, dtype=np.int64)
        np.add.at(verts, inverse, mesh.vertices)
        np.add.at(counts, inverse, 1)
        verts /= counts[:, None]
        faces = inverse[mesh.faces]
        keep = ((faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2])
                & (faces[:, 0] != faces[:, 2]))
        return trimesh.Trimesh(vertices=verts, faces=faces[keep], process=False)

    lo, hi = extent * 1e-4, extent  # pitch bounds: fine -> whole-mesh
    best = mesh
    for _ in range(24):
        mid = np.sqrt(lo * hi)
        out = cluster(mid)
        if len(out.faces) > target_faces:
            lo = mid          # too dense: coarsen
        else:
            best = out
            hi = mid          # sparse enough: try finer
    return best


def build_merged_mesh(geoms: List[GeomBinding], nodes: List[Node],
                      meshdir: str, max_faces_per_mesh: int,
                      verbose: bool = True):
    """Load every geom's STL, bake it into bind-pose world space and merge.

    Returns (positions, normals, joints, indices, stats).
    """
    # Face counts drive the per-mesh decimation budget.
    loaded: List[Tuple[GeomBinding, trimesh.Trimesh]] = []
    for g in geoms:
        path = os.path.normpath(os.path.join(meshdir, g.file))
        if not os.path.isfile(path):
            raise FileNotFoundError("mesh file not found: %s" % path)
        m = trimesh.load(path, file_type=os.path.splitext(path)[1][1:],
                         process=False, force="mesh")
        # STL stores every triangle independently: weld first so decimation
        # and normals see a real manifold.
        m.merge_vertices()
        # Drop zero-area triangles (API moved between trimesh versions).
        if hasattr(m, "nondegenerate_faces"):
            m.update_faces(m.nondegenerate_faces())
        else:  # trimesh < 4
            m.remove_degenerate_faces()
        m.remove_unreferenced_vertices()
        loaded.append((g, m))

    total_src = sum(len(m.faces) for _, m in loaded)
    all_pos, all_nrm, all_joint, all_idx = [], [], [], []
    vert_base = 0
    per_mesh_stats = []

    for g, m in loaded:
        # Per-STL budget: keep the mesh's own face count when it is already
        # cheap, otherwise clamp to --max-faces-per-mesh.  Dense parts (thorax,
        # head) therefore lose the most detail and tiny parts (claws, antenna
        # segments) survive untouched.
        budget = min(max_faces_per_mesh, len(m.faces))
        m = decimate(m, budget)

        # MJCF asset scale, then the geom frame, then the owning node's bind
        # world matrix.  trimesh flips winding automatically for mirrored
        # (negative-determinant) transforms, which this model uses for the
        # right-hand-side parts.
        xform = nodes[g.node_index].world @ g.local @ np.diag(
            [g.scale[0], g.scale[1], g.scale[2], 1.0])
        m.apply_transform(xform)

        v = np.asarray(m.vertices, dtype=np.float32)
        n = np.asarray(m.vertex_normals, dtype=np.float32)
        f = np.asarray(m.faces, dtype=np.uint32) + vert_base
        all_pos.append(v)
        all_nrm.append(n)
        all_joint.append(np.full(len(v), g.node_index, dtype=np.uint32))
        all_idx.append(f)
        vert_base += len(v)
        per_mesh_stats.append((g.mesh_name, len(m.faces)))

    positions = np.concatenate(all_pos, axis=0)
    normals = np.concatenate(all_nrm, axis=0)
    joints = np.concatenate(all_joint, axis=0)
    indices = np.concatenate(all_idx, axis=0).reshape(-1)

    # Guard against NaN / zero-length normals from degenerate triangles: a
    # zero normal would shade black, so substitute a fixed unit vector and
    # renormalise the rest.
    lengths = np.linalg.norm(normals, axis=1)
    bad = ~np.isfinite(normals).all(axis=1) | (lengths < 1e-8)
    n_bad = int(bad.sum())
    normals[bad] = np.array([0.0, 0.0, 1.0], dtype=np.float32)
    lengths[bad] = 1.0
    normals /= lengths[:, None].astype(np.float32)

    if verbose:
        print("  source triangles: %d across %d geoms" % (total_src, len(loaded)))
        if n_bad:
            print("  fixed %d degenerate vertex normals" % n_bad)
    return positions, normals, joints, indices, total_src


# --------------------------------------------------------------------------
# GLB assembly
# --------------------------------------------------------------------------
class BufferBuilder:
    """Accumulates the single GLB buffer and its bufferViews."""

    def __init__(self):
        self.blob = bytearray()
        self.views: List[dict] = []

    def add(self, data: bytes, target: Optional[int] = None) -> int:
        while len(self.blob) % 4:
            self.blob.append(0)
        offset = len(self.blob)
        self.blob.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.views.append(view)
        return len(self.views) - 1


def write_glb(out_path: str, nodes: List[Node], positions, normals, joints,
              indices, base_color: Sequence[float],
              up_axis_rotation: Sequence[float]) -> dict:
    """Emit the GLB.  Returns a stats dict (for reporting).

    `up_axis_rotation` is the (x,y,z,w) rotation put on the extra scene root
    that converts the model's up axis (see MJCF_TO_GLTF_YUP).
    """
    import pygltflib

    n_nodes = len(nodes)
    n_verts = len(positions)
    # +2: the mesh node and the up-axis root.
    if n_nodes + 2 > MAX_NODES:
        raise ValueError("node count %d exceeds engine limit %d"
                         % (n_nodes + 2, MAX_NODES))
    if n_nodes > MAX_JOINTS:
        raise ValueError("joint count %d exceeds engine limit %d"
                         % (n_nodes, MAX_JOINTS))

    buf = BufferBuilder()

    uv = np.zeros((n_verts, 2), dtype=np.float32)
    weights = np.zeros((n_verts, 4), dtype=np.float32)
    weights[:, 0] = 1.0                                   # rigid binding
    joints4 = np.zeros((n_verts, 4), dtype=np.uint8)      # < 256 joints
    joints4[:, 0] = joints.astype(np.uint8)

    # Inverse bind matrices: inverse of each joint's bind world matrix,
    # column-major as glTF requires.
    ibm = np.stack([np.linalg.inv(n.world).T for n in nodes]).astype(np.float32)

    # Constant "idle" clip: the skeleton root's bind rotation at t=0 and t=1.
    times = np.array([0.0, 1.0], dtype=np.float32)
    root_rot = np.array(nodes[0].rotation, dtype=np.float32)
    rot_values = np.stack([root_rot, root_rot]).astype(np.float32)

    v_pos = buf.add(positions.astype(np.float32).tobytes(), ARRAY_BUFFER)
    v_nrm = buf.add(normals.astype(np.float32).tobytes(), ARRAY_BUFFER)
    v_uv = buf.add(uv.tobytes(), ARRAY_BUFFER)
    v_jnt = buf.add(joints4.tobytes(), ARRAY_BUFFER)
    v_wgt = buf.add(weights.tobytes(), ARRAY_BUFFER)
    v_idx = buf.add(indices.astype(np.uint32).tobytes(), ELEMENT_ARRAY_BUFFER)
    v_ibm = buf.add(ibm.tobytes())
    v_time = buf.add(times.tobytes())
    v_rot = buf.add(rot_values.tobytes())

    pmin = [float(v) for v in positions.min(axis=0)]
    pmax = [float(v) for v in positions.max(axis=0)]

    A = pygltflib.Accessor
    accessors = [
        A(bufferView=v_pos, componentType=FLOAT, count=n_verts, type="VEC3",
          min=pmin, max=pmax),                                        # 0
        A(bufferView=v_nrm, componentType=FLOAT, count=n_verts, type="VEC3"),
        A(bufferView=v_uv, componentType=FLOAT, count=n_verts, type="VEC2"),
        A(bufferView=v_jnt, componentType=UNSIGNED_BYTE, count=n_verts,
          type="VEC4"),
        A(bufferView=v_wgt, componentType=FLOAT, count=n_verts, type="VEC4"),
        A(bufferView=v_idx, componentType=UNSIGNED_INT, count=len(indices),
          type="SCALAR"),                                             # 5
        A(bufferView=v_ibm, componentType=FLOAT, count=n_nodes, type="MAT4"),
        A(bufferView=v_time, componentType=FLOAT, count=2, type="SCALAR",
          min=[0.0], max=[1.0]),                                      # 7
        A(bufferView=v_rot, componentType=FLOAT, count=2, type="VEC4"),
    ]

    gltf_nodes = []
    children: List[List[int]] = [[] for _ in range(n_nodes)]
    for i, n in enumerate(nodes):
        if n.parent >= 0:
            children[n.parent].append(i)
    for i, n in enumerate(nodes):
        node = pygltflib.Node(
            name=n.name,
            translation=[float(v) for v in n.translation],
            rotation=[float(v) for v in n.rotation],
        )
        if children[i]:
            node.children = children[i]
        gltf_nodes.append(node)
    mesh_node_index = n_nodes
    gltf_nodes.append(pygltflib.Node(name="fly_mesh", mesh=0, skin=0))

    # Up-axis conversion.  MJCF is Z-up, glTF is Y-up, so a bare export lands
    # the fly on its back.  One extra root above BOTH the skeleton root and
    # the mesh node fixes it without rebaking anything: the skinning palette
    # is inv(mesh_world) * joint_world * invBind, and inserting R above both
    # gives inv(R*M) * (R*J) * IB == inv(M) * J * IB -- unchanged.  So the
    # baked vertices, the inverse bind matrices and the animation channel all
    # stay exactly as they are.
    #
    # This node is appended last on purpose: existing node indices (and hence
    # the skin's joint list, JOINTS_0 and the sidecar) do not shift.  The
    # engine builds its topological order by DFS from the scene roots, not by
    # node-array order, so a root at the end of the array is fine.
    root_node_index = mesh_node_index + 1
    gltf_nodes.append(pygltflib.Node(
        name="FlyRoot",
        rotation=list(up_axis_rotation),
        children=[0, mesh_node_index],
    ))

    primitive = pygltflib.Primitive(
        attributes=pygltflib.Attributes(
            POSITION=0, NORMAL=1, TEXCOORD_0=2, JOINTS_0=3, WEIGHTS_0=4),
        indices=5, material=0, mode=TRIANGLES)

    gltf = pygltflib.GLTF2(
        asset=pygltflib.Asset(version="2.0", generator="mjcf_to_rigged_glb.py"),
        scene=0,
        scenes=[pygltflib.Scene(name="scene", nodes=[root_node_index])],
        nodes=gltf_nodes,
        meshes=[pygltflib.Mesh(name="fly", primitives=[primitive])],
        skins=[pygltflib.Skin(name="fly_skin", inverseBindMatrices=6,
                              skeleton=0, joints=list(range(n_nodes)))],
        materials=[pygltflib.Material(
            name="fly_body",
            pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
                baseColorFactor=list(base_color),
                metallicFactor=0.0, roughnessFactor=0.85),
            doubleSided=True)],
        animations=[pygltflib.Animation(
            name="idle",
            samplers=[pygltflib.AnimationSampler(
                input=7, output=8, interpolation="LINEAR")],
            channels=[pygltflib.AnimationChannel(
                sampler=0,
                target=pygltflib.AnimationChannelTarget(node=0, path="rotation"))],
        )],
        accessors=accessors,
        bufferViews=[pygltflib.BufferView(**v) for v in buf.views],
        buffers=[pygltflib.Buffer(byteLength=len(buf.blob))],
    )
    gltf.set_binary_blob(bytes(buf.blob))
    gltf.save_binary(out_path)
    return {"vertices": n_verts, "triangles": len(indices) // 3,
            "nodes": len(gltf_nodes), "joints": n_nodes,
            "aabb_min": pmin, "aabb_max": pmax}


def write_sidecar(path: str, nodes: List[Node]) -> None:
    """Deterministic joint table, in skin-joint-index order."""
    entries = []
    for i, n in enumerate(nodes):
        entries.append({
            "index": i,
            "name": n.name,
            "kind": n.kind,
            "mjcf_joint": n.mjcf_joint,
            "axis": n.axis,
            "range": n.range,
            "parent": n.parent,
        })
    with open(path, "w") as fh:
        json.dump(entries, fh, indent=1, sort_keys=False)
        fh.write("\n")


# --------------------------------------------------------------------------
# Verification (re-open the written GLB and check the engine's invariants)
# --------------------------------------------------------------------------
def verify_glb(path: str) -> None:
    import pygltflib
    g = pygltflib.GLTF2().load_binary(path)
    blob = g.binary_blob()

    assert len(g.meshes) == 1, "expected exactly 1 mesh, got %d" % len(g.meshes)
    assert len(g.meshes[0].primitives) == 1, "expected exactly 1 primitive"
    prim = g.meshes[0].primitives[0]
    assert prim.mode == TRIANGLES, "primitive mode must be TRIANGLES"
    assert len(g.skins) == 1, "expected exactly 1 skin"
    assert len(g.buffers) == 1 and g.buffers[0].uri is None, "single embedded buffer"
    assert len(g.nodes) <= MAX_NODES, "node count %d > %d" % (len(g.nodes), MAX_NODES)
    skin = g.skins[0]
    assert skin.inverseBindMatrices is not None, "inverseBindMatrices missing"
    assert skin.skeleton is not None, "skin skeleton root missing"
    assert len(skin.joints) <= MAX_JOINTS
    assert g.scene == 0 and len(g.scenes) >= 1 and g.scenes[0].nodes, "scene 0 empty"
    assert len(g.scenes[0].nodes) == 1, "expected exactly one scene-0 root"
    scene_root = g.scenes[0].nodes[0]
    # Every node must be reachable from the scene root: the engine's topo walk
    # starts there, and an unreachable joint would keep an identity world.
    seen, stack = set(), [scene_root]
    while stack:
        ni = stack.pop()
        if ni in seen:
            continue
        seen.add(ni)
        stack.extend(g.nodes[ni].children or [])
    assert len(seen) == len(g.nodes), ("unreachable nodes: %d of %d"
                                       % (len(g.nodes) - len(seen), len(g.nodes)))
    assert skin.skeleton in (g.nodes[scene_root].children or []), \
        "skeleton root must hang off the scene root"
    assert scene_root not in skin.joints, \
        "the up-axis root must not be part of the skin joint list"
    assert g.animations and g.animations[0].channels, "no animation channel"

    mesh_nodes = [i for i, n in enumerate(g.nodes)
                  if n.mesh is not None and n.skin == 0]
    assert mesh_nodes, "no node references mesh=0 with skin=0"
    # The palette identity inv(mesh_world)*joint_world*invBind only cancels the
    # up-axis rotation if the mesh node sits under the same root as the joints.
    assert mesh_nodes[0] in (g.nodes[scene_root].children or []), \
        "mesh node must share the scene root with the skeleton"

    def read(idx):
        acc = g.accessors[idx]
        bv = g.bufferViews[acc.bufferView]
        dtype = {FLOAT: np.float32, UNSIGNED_BYTE: np.uint8,
                 UNSIGNED_SHORT: np.uint16, UNSIGNED_INT: np.uint32}[acc.componentType]
        ncomp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[acc.type]
        off = (bv.byteOffset or 0) + (acc.byteOffset or 0)
        arr = np.frombuffer(blob, dtype=dtype, count=acc.count * ncomp, offset=off)
        return arr.reshape(acc.count, ncomp) if ncomp > 1 else arr

    pos_acc = g.accessors[prim.attributes.POSITION]
    assert pos_acc.min is not None and pos_acc.max is not None, "POSITION min/max"
    pos = read(prim.attributes.POSITION)
    assert np.allclose(pos.min(axis=0), pos_acc.min, atol=1e-5)
    assert np.allclose(pos.max(axis=0), pos_acc.max, atol=1e-5)

    jnt = read(prim.attributes.JOINTS_0)
    wgt = read(prim.attributes.WEIGHTS_0)
    assert jnt.max() < len(skin.joints), "JOINTS_0 index >= joint count"
    assert jnt.max() < 256, "JOINTS_0 index >= 256"
    assert np.allclose(wgt.sum(axis=1), 1.0, atol=1e-6), "weights must sum to 1"

    idx = read(prim.indices)
    assert idx.max() < len(pos), "index out of range"
    assert len(idx) % 3 == 0, "index count not a multiple of 3"

    print("  verification: %d nodes, %d joints, %d verts, %d tris -- all checks passed"
          % (len(g.nodes), len(skin.joints), len(pos), len(idx) // 3))
    return pos


def default_mjcf_path() -> Optional[str]:
    """Locate the flygym NeuroMechFly MJCF, if flygym happens to be importable."""
    try:
        import flygym  # noqa: F401
    except Exception:
        return None
    data = os.path.join(os.path.dirname(flygym.__file__), "data")
    p = os.path.join(data, "mjcf", "neuromechfly_seqik_kinorder_ypr.xml")
    return p if os.path.isfile(p) else None


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert an MJCF body model into a single rigged GLB.")
    ap.add_argument("--mjcf", default=None,
                    help="path to the MJCF xml (defaults to the flygym "
                         "NeuroMechFly model if flygym is importable)")
    ap.add_argument("--meshdir", default=None,
                    help="override the mesh directory (default: the MJCF "
                         "compiler's meshdir, resolved next to the xml)")
    ap.add_argument("--out", default="fly.glb", help="output .glb path")
    ap.add_argument("--max-faces-per-mesh", type=int, default=1500,
                    help="triangle cap per source mesh after decimation")
    ap.add_argument("--up-axis", choices=("y", "z"), default="y",
                    help="up axis of the exported GLB: 'y' (default, glTF "
                         "conformant -- rotates the Z-up MJCF model onto +Y) "
                         "or 'z' (keep MJCF axes as-is)")
    ap.add_argument("--base-color", default="0.78,0.72,0.60,1.0",
                    help="material base color factor, comma separated RGBA")
    args = ap.parse_args(argv)

    mjcf_path = args.mjcf or default_mjcf_path()
    if not mjcf_path:
        ap.error("--mjcf is required (flygym is not importable for a default)")
    if not os.path.isfile(mjcf_path):
        ap.error("MJCF not found: %s" % mjcf_path)

    print("MJCF: %s" % mjcf_path)
    nodes, geoms, meshdir, mesh_assets = parse_mjcf(mjcf_path, args.meshdir)
    print("  meshdir: %s" % meshdir)
    n_bodies = sum(1 for n in nodes if n.kind == "body")
    n_joints = sum(1 for n in nodes if n.kind == "joint")
    print("  %d bodies, %d joints -> %d skeleton nodes, %d mesh geoms"
          % (n_bodies, n_joints, len(nodes), len(geoms)))

    compute_bind_world(nodes)

    # Bind pose must reproduce MuJoCo's zero pose exactly.
    tree_root = ET.parse(mjcf_path).getroot()
    comp = tree_root.find("compiler")
    radians = ((comp.get("angle") if comp is not None else None) or "degree").lower() == "radian"
    eulerseq = (comp.get("eulerseq") if comp is not None else None) or "xyz"
    err = verify_forward_kinematics(mjcf_path, nodes, radians, eulerseq)
    assert err < 1e-9, "bind pose deviates from MJCF forward kinematics: %g" % err
    print("  bind pose matches MJCF zero-pose FK (max abs error %.3g)" % err)

    # worldMatrix(joint) * inverseBind(joint) must be identity at bind time.
    worst_ident = 0.0
    for n in nodes:
        worst_ident = max(worst_ident,
                          float(np.max(np.abs(n.world @ np.linalg.inv(n.world)
                                              - np.eye(4)))))
    assert worst_ident < 1e-6, "inverse bind check failed: %g" % worst_ident
    print("  world * inverseBind == identity (max abs error %.3g)" % worst_ident)

    positions, normals, joints, indices, total_src = build_merged_mesh(
        geoms, nodes, meshdir, args.max_faces_per_mesh)

    base_color = [float(v) for v in args.base_color.split(",")]
    out_path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    up_rot = MJCF_TO_GLTF_YUP if args.up_axis == "y" else IDENTITY_ROTATION
    stats = write_glb(out_path, nodes, positions, normals, joints, indices,
                      base_color, up_rot)

    sidecar = os.path.splitext(out_path)[0] + ".joints.json"
    write_sidecar(sidecar, nodes)

    print("wrote %s" % out_path)
    print("wrote %s" % sidecar)
    print("  nodes: %d (%d skin joints)" % (stats["nodes"], stats["joints"]))
    print("  vertices: %d  triangles: %d (from %d source triangles)"
          % (stats["vertices"], stats["triangles"], total_src))
    size = os.path.getsize(out_path)
    print("  file size: %d bytes (%.2f MiB)" % (size, size / (1024 * 1024)))
    print("  baked (MJCF Z-up) AABB min: [%.4f %.4f %.4f]" % tuple(stats["aabb_min"]))
    print("  baked (MJCF Z-up) AABB max: [%.4f %.4f %.4f]" % tuple(stats["aabb_max"]))
    # Scene-space AABB: the up-axis root rotates the whole model, so report the
    # box the engine will actually see.
    r = quat_wxyz_to_matrix([up_rot[3], up_rot[0], up_rot[1], up_rot[2]])[:3, :3]
    corners = np.array([[stats["aabb_min"][0] if (k & 1) else stats["aabb_max"][0],
                         stats["aabb_min"][1] if (k & 2) else stats["aabb_max"][1],
                         stats["aabb_min"][2] if (k & 4) else stats["aabb_max"][2]]
                        for k in range(8)])
    world = corners @ r.T
    wmin, wmax = world.min(axis=0), world.max(axis=0)
    print("  scene-space (%s-up) AABB min: [%.4f %.4f %.4f]"
          % (args.up_axis, wmin[0], wmin[1], wmin[2]))
    print("  scene-space (%s-up) AABB max: [%.4f %.4f %.4f]"
          % (args.up_axis, wmax[0], wmax[1], wmax[2]))
    ext = wmax - wmin
    print("  scene-space extent: [%.4f %.4f %.4f] (MJCF units, ~mm)" % tuple(ext))

    verify_glb(out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
