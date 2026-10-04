#!/usr/bin/env python3
"""Apply the mtk_kcal saturation change on top of the RGB-gain commit.
Run from the kernel repo root:  python3 apply_saturation.py
Whitespace-proof (no patch file). Aborts without touching anything if the
tree is not in the expected v1 state or was already converted."""
import sys

G = "drivers/misc/mediatek/video/common/corr10/ddp_gamma.c"
H = "drivers/misc/mediatek/video/include/ddp_gamma.h"
PH = "drivers/misc/mediatek/video/mt6765/videox/primary_display.h"
PC = "drivers/misc/mediatek/video/mt6765/videox/primary_display.c"

NEW_BLOCK = '/*\n * Rebuild g_rgb_matrix = Gain x Saturation (caller holds g_gamma_global_lock).\n * g_rgb_matrix is the last factor of coef x HWC matrix x g_rgb_matrix, so it\n * acts on the pixel first: saturate, then apply the per-channel gain.\n * Saturation matrix S = (1 - s) * ones x luma^T + s * I (rows sum to 1, so\n * white stays white). s == 0 -> grayscale, s == 1 -> identity.\n */\nstatic void disp_ccorr_rebuild_rgb_matrix(void)\n{\n\tstatic const int luma[3] = { 218, 732, 74 };\t/* Rec.709 x 1024 */\n\tint i, j, s_ij;\n\n\tfor (i = 0; i < 3; i++) {\n\t\tfor (j = 0; j < 3; j++) {\n\t\t\ts_ij = (1024 - g_user_sat) * luma[j];\n\t\t\tif (i == j)\n\t\t\t\ts_ij += g_user_sat * 1024;\n\t\t\ts_ij = DIV_ROUND_CLOSEST(s_ij, 1024);\n\t\t\tg_rgb_matrix[i][j] = DIV_ROUND_CLOSEST(\n\t\t\t\tg_user_gain[i] * s_ij, 1024);\n\t\t}\n\t}\n\tg_rgb_without_gamma = (g_user_gain[0] != RGB_GAIN_UNITY ||\n\t\tg_user_gain[1] != RGB_GAIN_UNITY ||\n\t\tg_user_gain[2] != RGB_GAIN_UNITY ||\n\t\tg_user_sat != 1024) ? 1 : 0;\n}\n\n/*\n * Set the user color state: per-channel gain (1024 == 1.0) and saturation\n * (1024 == 1.0), multiplied on top of the panel coefficients and the HWC\n * color matrix so night light / color mode updates do not wipe it.\n * cmdq == NULL: only store; disp_ccorr_start() writes it on next power-on.\n */\nint disp_ccorr_set_user_color(void *cmdq, int r, int g, int b, int sat)\n{\n\tint ret = 0;\n\n\tr = CCORR_CLIP(r, RGB_GAIN_MIN, RGB_GAIN_MAX);\n\tg = CCORR_CLIP(g, RGB_GAIN_MIN, RGB_GAIN_MAX);\n\tb = CCORR_CLIP(b, RGB_GAIN_MIN, RGB_GAIN_MAX);\n\tsat = CCORR_CLIP(sat, SAT_MIN, SAT_MAX);\n\n\tmutex_lock(&g_gamma_global_lock);\n\tg_user_gain[0] = r;\n\tg_user_gain[1] = g;\n\tg_user_gain[2] = b;\n\tg_user_sat = sat;\n\tdisp_ccorr_rebuild_rgb_matrix();\n\n\tCCORR_DBG("r[%d], g[%d], b[%d], sat[%d]", r, g, b, sat);\n\tif (cmdq != NULL)\n\t\tret = disp_ccorr_write_coef_reg(cmdq, CCORR0_MODULE_NAMING,\n\t\t\tDISP_CCORR0, 0);\n\tmutex_unlock(&g_gamma_global_lock);\n\treturn ret;\n}\n\nstatic void disp_ccorr_get_user_color(int *r, int *g, int *b, int *sat)\n{\n\tmutex_lock(&g_gamma_global_lock);\n\t*r = g_user_gain[0];\n\t*g = g_user_gain[1];\n\t*b = g_user_gain[2];\n\t*sat = g_user_sat;\n\tmutex_unlock(&g_gamma_global_lock);\n}\n\nstatic ssize_t user_color_apply(int r, int g, int b, int sat, size_t count)\n{\n\tint ret = primary_display_set_user_color(r, g, b, sat);\n\n\tif (ret < 0)\n\t\treturn ret;\n\t/* ret == 1: display asleep, stored, applied on resume */\n\tif (ret == 0)\n\t\tdisp_ccorr_trigger_refresh(DISP_CCORR0);\n\treturn count;\n}\n\n/* /sys/kernel/mtk_kcal/rgb : "R G B", each 256..1024 (1024 = unity) */\nstatic ssize_t rgb_show(struct kobject *kobj, struct kobj_attribute *attr,\n\tchar *buf)\n{\n\tint r, g, b, sat;\n\n\tdisp_ccorr_get_user_color(&r, &g, &b, &sat);\n\treturn scnprintf(buf, PAGE_SIZE, "%d %d %d\\n", r, g, b);\n}\n\nstatic ssize_t rgb_store(struct kobject *kobj, struct kobj_attribute *attr,\n\tconst char *buf, size_t count)\n{\n\tint r, g, b, cr, cg, cb, sat;\n\n\tif (sscanf(buf, "%d %d %d", &r, &g, &b) != 3)\n\t\treturn -EINVAL;\n\tif (r < RGB_GAIN_MIN || r > RGB_GAIN_MAX ||\n\t\tg < RGB_GAIN_MIN || g > RGB_GAIN_MAX ||\n\t\tb < RGB_GAIN_MIN || b > RGB_GAIN_MAX)\n\t\treturn -EINVAL;\n\n\tdisp_ccorr_get_user_color(&cr, &cg, &cb, &sat);\n\treturn user_color_apply(r, g, b, sat, count);\n}\n\n/* /sys/kernel/mtk_kcal/sat : 0..1536 (1024 = unity, 0 = grayscale) */\nstatic ssize_t sat_show(struct kobject *kobj, struct kobj_attribute *attr,\n\tchar *buf)\n{\n\tint r, g, b, sat;\n\n\tdisp_ccorr_get_user_color(&r, &g, &b, &sat);\n\treturn scnprintf(buf, PAGE_SIZE, "%d\\n", sat);\n}\n\nstatic ssize_t sat_store(struct kobject *kobj, struct kobj_attribute *attr,\n\tconst char *buf, size_t count)\n{\n\tint r, g, b, sat, sat_old;\n\n\tif (kstrtoint(buf, 10, &sat))\n\t\treturn -EINVAL;\n\tif (sat < SAT_MIN || sat > SAT_MAX)\n\t\treturn -EINVAL;\n\n\tdisp_ccorr_get_user_color(&r, &g, &b, &sat_old);\n\treturn user_color_apply(r, g, b, sat, count);\n}\n\nstatic struct kobj_attribute rgb_attr = __ATTR(rgb, 0644, rgb_show, rgb_store);\nstatic struct kobj_attribute sat_attr = __ATTR(sat, 0644, sat_show, sat_store);\n\nstatic int __init disp_ccorr_sysfs_init(void)\n{\n\tstruct kobject *kobj = kobject_create_and_add("mtk_kcal", kernel_kobj);\n\n\tif (!kobj)\n\t\treturn -ENOMEM;\n\tif (sysfs_create_file(kobj, &rgb_attr.attr) ||\n\t\tsysfs_create_file(kobj, &sat_attr.attr))\n\t\tkobject_put(kobj);\n\treturn 0;\n}\nlate_initcall(disp_ccorr_sysfs_init);'

def rd(p): return open(p).read()
def wr(p, t): open(p, "w").write(t)
def die(m): print("ABORT:", m); sys.exit(1)

s, h, ph, pc = rd(G), rd(H), rd(PH), rd(PC)
if "disp_ccorr_set_user_color" in s: die("already applied")
for name, ok in [
    ("ddp_gamma.c v1 defines", "#define RGB_GAIN_MAX\t1024\n" in s),
    ("ddp_gamma.c v1 function", "int disp_ccorr_set_RGB_Gain(void *cmdq, int r, int g, int b)" in s),
    ("ddp_gamma.c sysfs init", "late_initcall(disp_ccorr_sysfs_init);" in s),
    ("ddp_gamma.h", "int disp_ccorr_set_RGB_Gain(void *cmdq, int r, int g, int b);" in h),
    ("primary_display.h", "int primary_display_set_rgb_gain(int r, int g, int b);" in ph),
    ("primary_display.c fn", "int primary_display_set_rgb_gain(int r, int g, int b)" in pc),
    ("primary_display.c call", "ret = disp_ccorr_set_RGB_Gain(handle, r, g, b);" in pc),
]:
    if not ok: die("expected v1 code not found: " + name)

old = "#define RGB_GAIN_MAX\t1024\n"
s = s.replace(old, old +
    "#define SAT_MIN\t\t0\n#define SAT_MAX\t\t1536\t/* 1.5x; HW coef range is +-2.0 */\n\n"
    "/* user color state, 1024 == 1.0 */\n"
    "static int g_user_gain[3] = { 1024, 1024, 1024 };\n"
    "static int g_user_sat = 1024;\n", 1)
i = s.index("/*\n * Set a user RGB gain (1024 == 1.0)")
j = s.index("late_initcall(disp_ccorr_sysfs_init);") + len("late_initcall(disp_ccorr_sysfs_init);")
s = s[:i] + NEW_BLOCK + s[j:]
h = h.replace("int disp_ccorr_set_RGB_Gain(void *cmdq, int r, int g, int b);",
              "int disp_ccorr_set_user_color(void *cmdq, int r, int g, int b, int sat);")
ph = ph.replace("int primary_display_set_rgb_gain(int r, int g, int b);",
                "int primary_display_set_user_color(int r, int g, int b, int sat);")
pc = pc.replace("int primary_display_set_rgb_gain(int r, int g, int b)",
                "int primary_display_set_user_color(int r, int g, int b, int sat)")
pc = pc.replace("ret = disp_ccorr_set_RGB_Gain(handle, r, g, b);",
                "ret = disp_ccorr_set_user_color(handle, r, g, b, sat);")
pc = pc.replace(" * Apply the user CCORR RGB gain through",
                " * Apply the user CCORR color state (RGB gain + saturation) through")
wr(G, s); wr(H, h); wr(PH, ph); wr(PC, pc)
print("OK: saturation applied to 4 files")
