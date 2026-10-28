"""生成地形分层材质贴图：assets/terrain/{sand,rock,reef,moss}.png 与对应的 *_n.png 法线贴图。

每层两张 512x512 贴图：
  <层>.png    RGB = 颜色（sRGB），A = 高度（0 低 / 1 高，地形 shader 用它做「高度混合」）
  <层>_n.png  切线空间法线（RG 有效，资源层按 _n 后缀识别为法线并压缩成 BC5）

贴图必须四方连续（地形上会平铺几百次）：
  - 分形噪声用「频域合成」：在 FFT 频域里给随机相位乘上 1/f^β 的振幅谱，逆变换回来天然周期
  - 细胞噪声（Worley）用周期距离：dx = min(|dx|, N - |dx|)
  - 正弦纹理（沙纹、岩层）只用整数频率
法线由高度的周期中心差分得到：n = normalize(-dh/du * s, -dh/dv * s, 1)，v 沿图像行向下。

依赖：numpy。运行：python tools/make_terrain_textures.py
"""
import os
import struct
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "terrain")
N = 512
rng = np.random.default_rng(20261028)

# ---------------------------------------------------------------- PNG


def write_png(path, rgba):
    """rgba: (H, W, 4) uint8"""
    h, w, _ = rgba.shape
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))  # 每行前加过滤类型 0

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

# ---------------------------------------------------------------- 四方连续噪声


def spectral_noise(beta, fmin=1.0, fmax=None):
    """频域合成的分形噪声，归一化到 [0,1]。beta 越大越平滑（2 ≈ 自然地貌）。"""
    k = np.fft.fftfreq(N) * N
    kx, ky = np.meshgrid(k, k)
    f = np.sqrt(kx * kx + ky * ky)
    amp = np.zeros_like(f)
    band = f >= fmin
    if fmax is not None:
        band &= f <= fmax
    amp[band] = f[band] ** (-beta / 2.0)
    phase = np.exp(2j * np.pi * rng.random((N, N)))
    img = np.real(np.fft.ifft2(amp * phase))
    img -= img.min()
    return img / max(img.max(), 1e-9)


def worley(cells):
    """周期细胞噪声：返回 (F1, F2)，到最近/次近特征点的距离（以格子尺寸为单位）。"""
    pts = (np.stack(np.meshgrid(np.arange(cells), np.arange(cells)), -1).reshape(-1, 2) + rng.random((cells * cells, 2))) * (N / cells)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    f1 = np.full((N, N), 1e9, np.float32)
    f2 = np.full((N, N), 1e9, np.float32)
    for px, py in pts:
        dx = np.abs(xx - px)
        dy = np.abs(yy - py)
        dx = np.minimum(dx, N - dx)
        dy = np.minimum(dy, N - dy)
        d = np.sqrt(dx * dx + dy * dy)
        f2 = np.where(d < f1, f1, np.minimum(f2, d))
        f1 = np.minimum(f1, d)
    scale = N / cells
    return f1 / scale, f2 / scale


def coords():
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    return xx / N, yy / N


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def normalize01(x):
    x = x - x.min()
    return x / max(x.max(), 1e-9)

# ---------------------------------------------------------------- 输出


def save_layer(name, color, height, normal_strength):
    """color: (N,N,3) sRGB [0,1]；height: (N,N) [0,1]"""
    rgba = np.empty((N, N, 4), np.uint8)
    rgba[..., :3] = np.clip(color * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgba[..., 3] = np.clip(height * 255.0 + 0.5, 0, 255).astype(np.uint8)
    write_png(os.path.join(OUT, name + ".png"), rgba)

    # 周期中心差分：np.roll 让边界自动取到对边，法线贴图同样四方连续
    dhdu = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * N
    dhdv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5 * N
    s = normal_strength / N
    n = np.stack([-dhdu * s, -dhdv * s, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    nrm = np.empty((N, N, 4), np.uint8)
    nrm[..., :3] = np.clip((n * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    nrm[..., 3] = 255
    write_png(os.path.join(OUT, name + "_n.png"), nrm)
    print("  " + name + ".png / " + name + "_n.png")


def lerp_color(a, b, t):
    return np.asarray(a)[None, None, :] * (1.0 - t[..., None]) + np.asarray(b)[None, None, :] * t[..., None]


def make_sand():
    u, v = coords()
    warp = spectral_noise(2.2, 1, 8)
    # 沙纹：整数频率的正弦 + 低频扭曲相位（仍然四方连续）
    ripple = 0.5 + 0.5 * np.sin(2 * np.pi * (14 * u + 3 * v) + 6.0 * warp)
    grain = spectral_noise(0.6, 64)  # 高频沙粒
    low = spectral_noise(2.0, 1, 6)
    height = normalize01(0.55 * ripple ** 1.5 + 0.25 * grain + 0.2 * low)
    color = lerp_color((0.70, 0.60, 0.43), (0.84, 0.75, 0.57), normalize01(0.6 * low + 0.4 * grain))
    color *= (0.92 + 0.08 * ripple)[..., None]
    save_layer("sand", color, height, 2.0)


def make_rock():
    u, v = coords()
    # 两个尺度的细胞边界当裂缝；只占一小部分权重，避免看起来像人工铺的石板
    a1, a2 = worley(5)
    b1, b2 = worley(13)
    crack_mask = smoothstep(0.35, 0.65, spectral_noise(2.0, 1, 8))  # 裂缝时断时续，不是完整的网
    cracks = 1.0 - (1.0 - smoothstep(0.0, 0.08, a2 - a1)) * crack_mask
    cracks *= 0.75 + 0.25 * smoothstep(0.0, 0.06, b2 - b1)
    fbm = spectral_noise(1.6, 1)
    strata = 0.5 + 0.5 * np.sin(2 * np.pi * (7 * v) + 7.0 * spectral_noise(2.2, 1, 6))
    height = normalize01(0.25 * cracks + 0.55 * fbm + 0.2 * strata)
    tone = normalize01(0.45 * fbm + 0.25 * strata + 0.3 * spectral_noise(1.0, 16))
    color = lerp_color((0.30, 0.29, 0.27), (0.56, 0.53, 0.48), tone)
    color *= (0.7 + 0.3 * cracks)[..., None]  # 裂缝稍暗
    save_layer("rock", color, height, 6.0)


def make_reef():
    f1, f2 = worley(14)
    blobs = 1.0 - smoothstep(0.0, 0.75, f1)  # 珊瑚团块
    pores = spectral_noise(0.8, 48)
    fbm = spectral_noise(2.0, 1, 12)
    height = normalize01(0.6 * blobs + 0.25 * pores + 0.15 * fbm)
    hue = spectral_noise(2.4, 1, 6)
    base = lerp_color((0.40, 0.42, 0.35), (0.58, 0.49, 0.40), smoothstep(0.35, 0.65, hue))  # 橄榄灰 ↔ 褪色的珊瑚黄褐
    color = base * (0.6 + 0.4 * height)[..., None]
    save_layer("reef", color, height, 5.0)


def make_moss():
    clumps = spectral_noise(2.0, 2, 24)
    fiber = spectral_noise(0.5, 80)
    height = normalize01(0.65 * clumps + 0.35 * fiber)
    color = lerp_color((0.16, 0.22, 0.08), (0.36, 0.43, 0.17), normalize01(0.7 * clumps + 0.3 * fiber))
    # 团块之间露出一点泥土
    dirt = smoothstep(0.35, 0.15, clumps)
    color = color * (1 - dirt[..., None]) + np.asarray((0.30, 0.24, 0.16))[None, None, :] * dirt[..., None]
    save_layer("moss", color, height, 3.0)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("生成地形贴图 → " + OUT)
    make_sand()
    make_rock()
    make_reef()
    make_moss()
