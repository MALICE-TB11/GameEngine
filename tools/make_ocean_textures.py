"""生成海洋泡沫细节贴图：assets/ocean/foam.png（四方连续）。

泡沫不是均匀的白色，而是一团团气泡和细丝：用两个尺度的细胞噪声（气泡边缘）叠加分形噪声（团块）。
shader 里用「泡沫覆盖率 × 2 - (1 - 贴图值)」做阈值：覆盖率低时只剩最亮的气泡边，覆盖率高时连成一片。

依赖：numpy。运行：python tools/make_ocean_textures.py
"""
import os

import numpy as np

import make_terrain_textures as tt  # 复用四方连续噪声与 PNG 写出

OUT = os.path.join(tt.ROOT, "assets", "ocean")


def make_foam():
    a1, a2 = tt.worley(18)
    b1, b2 = tt.worley(41)
    bubbles = tt.smoothstep(0.0, 0.25, a2 - a1)       # 大气泡的边缘（细胞边界）
    small = tt.smoothstep(0.0, 0.2, b2 - b1)
    clumps = tt.spectral_noise(1.8, 2, 20)
    streaks = tt.spectral_noise(1.2, 8)
    v = tt.normalize01(0.45 * (1.0 - bubbles) + 0.25 * (1.0 - small) + 0.2 * clumps + 0.1 * streaks)
    rgba = np.empty((tt.N, tt.N, 4), np.uint8)
    g = np.clip(v * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgba[..., 0] = g
    rgba[..., 1] = g
    rgba[..., 2] = g
    rgba[..., 3] = 255
    tt.write_png(os.path.join(OUT, "foam_data.png"), rgba)  # _data 后缀 → 资源层按「数据」贴图烘焙（线性，不做 sRGB 转换）
    print("  foam_data.png")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("生成海洋贴图 → " + OUT)
    make_foam()
