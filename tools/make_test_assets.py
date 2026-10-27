"""生成测试用 glTF 资源：assets/models/ruins.glb

一座小型遗迹（底座、两根石柱、横梁、倒塌的石柱、青铜环、发光符文板），用来验证：
  - glTF 加载：多网格、多材质、节点层级（青铜环是右石柱的子节点）
  - PBR：程序化生成的 albedo / 法线 / 粗糙度 / AO 贴图（AO 打包在粗糙度贴图的 R 通道，glTF 常见做法）
  - 金属材质与自发光

只依赖 Python 标准库。运行：python tools/make_test_assets.py
"""
import json
import math
import os
import random
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "models", "ruins.glb")
TEX = 256

# ---------------------------------------------------------------- PNG

def png_rgba(width, height, pixels):
    """pixels: bytes/bytearray，RGBA8，逐行。返回 PNG 文件字节。"""
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # 过滤类型 0（None）
        raw += pixels[y * width * 4:(y + 1) * width * 4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")

# ---------------------------------------------------------------- 程序化纹理

random.seed(7)
LATTICE = 16
GRID = [[random.random() for _ in range(LATTICE)] for _ in range(LATTICE)]


def value_noise(x, y, period):
    """可平铺的值噪声：格点按 period 取模。x, y 以格点为单位。"""
    xi, yi = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - xi, y - yi
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)

    def g(i, j):
        return GRID[(i % period) % LATTICE][(j % period) % LATTICE]

    a = g(xi, yi) + (g(xi + 1, yi) - g(xi, yi)) * sx
    b = g(xi, yi + 1) + (g(xi + 1, yi + 1) - g(xi, yi + 1)) * sx
    return a + (b - a) * sy


def fbm(u, v):
    total, amp, freq = 0.0, 0.5, 4
    for _ in range(4):
        total += amp * value_noise(u * freq, v * freq, freq)
        amp *= 0.5
        freq *= 2
    return total


def stone_height(px, py):
    """石块 + 灰缝：每行 4 块，隔行错开半块；灰缝处高度下陷。"""
    u, v = px / TEX, py / TEX
    rows = 4
    row = int(v * rows)
    bu = u * 2 + (0.5 if row % 2 else 0.0)
    fu, fv = bu - math.floor(bu), v * rows - row
    edge = min(fu, 1 - fu, fv * 0.5, (1 - fv) * 0.5)  # 到灰缝的距离
    block = min(1.0, edge / 0.04)
    return 0.65 * block + 0.35 * fbm(u, v), block


def make_stone_textures():
    heights = [[0.0] * TEX for _ in range(TEX)]
    blocks = [[0.0] * TEX for _ in range(TEX)]
    for y in range(TEX):
        for x in range(TEX):
            heights[y][x], blocks[y][x] = stone_height(x, y)

    albedo, normal, orm = bytearray(), bytearray(), bytearray()
    strength = 6.0
    for y in range(TEX):
        for x in range(TEX):
            h, b = heights[y][x], blocks[y][x]
            n2 = fbm(x / TEX + 0.37, y / TEX + 0.11)
            # albedo：米灰色石头，灰缝更暗，带一点苔绿色斑
            tone = 0.72 + 0.4 * (n2 - 0.5)
            r, g, bl = 0.62 * tone, 0.58 * tone, 0.50 * tone
            moss = max(0.0, n2 - 0.6) * 1.5
            r, g, bl = r * (1 - moss) + 0.25 * moss, g * (1 - moss) + 0.35 * moss, bl * (1 - moss) + 0.15 * moss
            dark = 0.45 + 0.55 * b
            albedo += bytes([int(255 * min(1, c * dark)) for c in (r, g, bl)]) + b"\xff"
            # 法线：高度场的中心差分。glTF 约定 +Y 指向贴图「上方」（行号减小的方向）
            dhx = heights[y][(x + 1) % TEX] - heights[y][(x - 1) % TEX]
            dhy = heights[(y + 1) % TEX][x] - heights[(y - 1) % TEX][x]
            nx, ny, nz = -dhx * strength, dhy * strength, 1.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            normal += bytes([int(255 * (c * inv * 0.5 + 0.5)) for c in (nx, ny, nz)]) + b"\xff"
            # R = AO（灰缝处遮蔽），G = 粗糙度，B = 金属度
            ao = 0.55 + 0.45 * b
            rough = 0.72 + 0.25 * (1 - b) + 0.1 * (n2 - 0.5)
            orm += bytes([int(255 * ao), int(255 * min(1, rough)), 0, 255])
    return png_rgba(TEX, TEX, albedo), png_rgba(TEX, TEX, normal), png_rgba(TEX, TEX, orm)

# ---------------------------------------------------------------- 网格

def box(sx, sy, sz, uv_scale=1.0):
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    faces = [  # 法线, u 轴, v 轴（u × v = 法线），面的半尺寸
        ((1, 0, 0), (0, 0, -1), (0, 1, 0), (hz, hy), hx),
        ((-1, 0, 0), (0, 0, 1), (0, 1, 0), (hz, hy), hx),
        ((0, 1, 0), (1, 0, 0), (0, 0, -1), (hx, hz), hy),
        ((0, -1, 0), (1, 0, 0), (0, 0, 1), (hx, hz), hy),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0), (hx, hy), hz),
        ((0, 0, -1), (-1, 0, 0), (0, 1, 0), (hx, hy), hz),
    ]
    P, N, T, UV, I = [], [], [], [], []
    for n, u, v, (eu, ev), d in faces:
        base = len(P)
        for cu, cv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            P.append(tuple(n[k] * d + u[k] * cu * eu + v[k] * cv * ev for k in range(3)))
            N.append(n)
            T.append((u[0], u[1], u[2], 1.0))
            # 贴图按物理尺寸平铺：1 米 = uv_scale 个贴图
            UV.append(((cu * eu + eu) * uv_scale, (ev - cv * ev) * uv_scale))
        I += [base, base + 1, base + 2, base, base + 2, base + 3]
    return P, N, T, UV, I


def cylinder(radius, height, segments=32, uv_scale=1.0):
    """沿 +Y，从 y=0 到 y=height，含上下底面。"""
    P, N, T, UV, I = [], [], [], [], []
    circ = 2 * math.pi * radius
    for s in range(segments + 1):
        a = s / segments * 2 * math.pi
        c, sn = math.cos(a), math.sin(a)
        for y in (0.0, height):
            P.append((radius * c, y, -radius * sn))
            N.append((c, 0.0, -sn))
            T.append((-sn, 0.0, -c, 1.0))  # u 增大的方向
            UV.append((s / segments * circ * uv_scale, (height - y) * uv_scale))
    for s in range(segments):
        a = s * 2
        I += [a, a + 2, a + 1, a + 1, a + 2, a + 3]
    for y, ny in ((height, 1.0), (0.0, -1.0)):
        center = len(P)
        P.append((0.0, y, 0.0)); N.append((0.0, ny, 0.0)); T.append((1.0, 0.0, 0.0, 1.0)); UV.append((0.5, 0.5))
        for s in range(segments + 1):
            a = s / segments * 2 * math.pi
            c, sn = math.cos(a), math.sin(a)
            P.append((radius * c, y, -radius * sn)); N.append((0.0, ny, 0.0)); T.append((1.0, 0.0, 0.0, 1.0))
            UV.append((0.5 + 0.5 * c, 0.5 + 0.5 * sn * ny))
        for s in range(segments):
            if ny > 0:
                I += [center, center + 1 + s, center + 2 + s]
            else:
                I += [center, center + 2 + s, center + 1 + s]
    return P, N, T, UV, I

# ---------------------------------------------------------------- GLB 组装

class Builder:
    def __init__(self):
        self.bin = bytearray()
        self.gltf = {"asset": {"version": "2.0", "generator": "OceanEngine make_test_assets.py"},
                     "buffers": [], "bufferViews": [], "accessors": [], "meshes": [], "nodes": [],
                     "materials": [], "textures": [], "images": [], "samplers": [{}], "scenes": [{"nodes": []}], "scene": 0}

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.gltf["bufferViews"].append(bv)
        return len(self.gltf["bufferViews"]) - 1

    def accessor(self, values, typ, comp=5126, target=34962, minmax=False):
        flat = [c for v in values for c in (v if isinstance(v, tuple) else (v,))]
        fmt = "<%d%s" % (len(flat), "f" if comp == 5126 else "I")
        acc = {"bufferView": self.view(struct.pack(fmt, *flat), target), "componentType": comp, "count": len(values), "type": typ}
        if minmax:
            acc["min"] = [min(v[k] for v in values) for k in range(3)]
            acc["max"] = [max(v[k] for v in values) for k in range(3)]
        self.gltf["accessors"].append(acc)
        return len(self.gltf["accessors"]) - 1

    def mesh(self, name, geo, material):
        P, N, T, UV, I = geo
        prim = {"attributes": {"POSITION": self.accessor(P, "VEC3", minmax=True), "NORMAL": self.accessor(N, "VEC3"),
                               "TANGENT": self.accessor(T, "VEC4"), "TEXCOORD_0": self.accessor(UV, "VEC2")},
                "indices": self.accessor(I, "SCALAR", comp=5125, target=34963), "material": material}
        self.gltf["meshes"].append({"name": name, "primitives": [prim]})
        return len(self.gltf["meshes"]) - 1

    def texture(self, png):
        self.gltf["images"].append({"bufferView": self.view(png), "mimeType": "image/png"})
        self.gltf["textures"].append({"source": len(self.gltf["images"]) - 1, "sampler": 0})
        return len(self.gltf["textures"]) - 1

    def material(self, m):
        self.gltf["materials"].append(m)
        return len(self.gltf["materials"]) - 1

    def node(self, n, root=False):
        self.gltf["nodes"].append(n)
        idx = len(self.gltf["nodes"]) - 1
        if root:
            self.gltf["scenes"][0]["nodes"].append(idx)
        return idx

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.gltf["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.gltf, separators=(",", ":")).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        out = struct.pack("<III", 0x46546C67, 2, total)
        out += struct.pack("<II", len(js), 0x4E4F534A) + js
        out += struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(out)


def quat_axis(axis, deg):
    s = math.sin(math.radians(deg) / 2)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(math.radians(deg) / 2)]


def main():
    b = Builder()
    albedo, normal, orm = make_stone_textures()
    t_albedo, t_normal, t_orm = b.texture(albedo), b.texture(normal), b.texture(orm)

    stone = b.material({"name": "Stone",
                        "pbrMetallicRoughness": {"baseColorTexture": {"index": t_albedo}, "metallicRoughnessTexture": {"index": t_orm},
                                                 "metallicFactor": 1.0, "roughnessFactor": 1.0},
                        "normalTexture": {"index": t_normal, "scale": 1.0},
                        "occlusionTexture": {"index": t_orm, "strength": 1.0}})
    bronze = b.material({"name": "Bronze", "pbrMetallicRoughness": {"baseColorFactor": [0.80, 0.50, 0.22, 1.0],
                                                                   "metallicFactor": 1.0, "roughnessFactor": 0.32}})
    rune = b.material({"name": "GlowRune", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.06, 0.07, 1.0],
                                                                   "metallicFactor": 0.0, "roughnessFactor": 0.4},
                       "emissiveFactor": [0.2, 0.85, 1.0], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}})
    b.gltf["extensionsUsed"] = ["KHR_materials_emissive_strength"]

    m_base = b.mesh("Base", box(4.0, 0.3, 3.0, 0.5), stone)
    m_pillar = b.mesh("Pillar", cylinder(0.3, 2.6, 32, 0.5), stone)
    m_lintel = b.mesh("Lintel", box(3.6, 0.4, 0.7, 0.5), stone)
    m_ring = b.mesh("Ring", cylinder(0.36, 0.14, 40, 1.0), bronze)
    m_plaque = b.mesh("Plaque", box(0.8, 0.5, 0.05, 1.0), rune)

    ring = b.node({"name": "BronzeRing", "mesh": m_ring, "translation": [0.0, 1.6, 0.0]})
    children = [
        b.node({"name": "Base", "mesh": m_base, "translation": [0.0, 0.15, 0.0]}),
        b.node({"name": "Pillar_L", "mesh": m_pillar, "translation": [-1.4, 0.3, 0.0]}),
        # 青铜环是右石柱的子节点：验证加载器是否正确地级联父子变换
        b.node({"name": "Pillar_R", "mesh": m_pillar, "translation": [1.4, 0.3, 0.0], "children": [ring]}),
        b.node({"name": "Lintel", "mesh": m_lintel, "translation": [0.0, 3.1, 0.0]}),
        b.node({"name": "Fallen", "mesh": m_pillar, "translation": [2.6, 0.3, 1.8],
                "rotation": quat_axis((0.0, 0.0, 1.0), 90.0)}),
        b.node({"name": "Plaque", "mesh": m_plaque, "translation": [0.0, 0.55, 1.2],
                "rotation": quat_axis((1.0, 0.0, 0.0), -15.0)}),
    ]
    b.node({"name": "Ruins", "children": children}, root=True)
    b.write(OUT)
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
