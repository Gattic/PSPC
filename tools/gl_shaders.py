#!/usr/bin/env python3
"""Generate the OpenGL dialect of PSPC's GE shaders from the D3D11 source.

Only the resource bindings, entry points and language syntax differ. Keep the GE
arithmetic in one source; unsupported changes fail shader compilation in GPU tests.

Usage: gl_shaders.py d3d11_shaders.cpp d3d11_geometry.cpp opengl_shaders.h
"""

import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig")
geometry_source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8-sig")


def block(name, text=None):
    return re.search(
        r"static const char " + name + r'\[\] = R"HLSL\(([\s\S]*?)\)HLSL";',
        source if text is None else text,
    ).group(1)


def casts(s):
    pattern = re.compile(r"\((float[234]?|uint[234]?|int[234]?|double)\)")

    # Cast operands are primary expressions, including calls, swizzles and indexing.
    def primary(start):
        i = start
        while s[i].isspace():
            i += 1
        if s[i] in "+-~!":
            return primary(i + 1)
        if s[i] == "(":
            level = 1
            i += 1
            while level:
                if s[i] == "(":
                    level += 1
                if s[i] == ")":
                    level -= 1
                i += 1
        else:
            m = re.match(r"[\w.]+", s[i:])
            if not m:
                raise ValueError(s[start : start + 100])
            i += len(m.group())
        while i < len(s):
            if s[i] in "([":
                close = ")" if s[i] == "(" else "]"
                opening = s[i]
                level = 1
                i += 1
                while level:
                    if s[i] == opening:
                        level += 1
                    if s[i] == close:
                        level -= 1
                    i += 1
            elif s[i] == ".":
                m = re.match(r"\.\w+", s[i:])
                i += len(m.group())
            else:
                break
        return i

    while True:
        matches = list(pattern.finditer(s))
        if not matches:
            return s
        m = matches[-1]
        end = primary(m.end())
        s = s[: m.start()] + m[1] + "(" + s[m.end() : end] + ")" + s[end:]


def dialect(s):
    s = casts(s)
    for old, new in [
        ("float2", "vec2"),
        ("float3", "vec3"),
        ("float4", "vec4"),
        ("uint2", "uvec2"),
        ("uint3", "uvec3"),
        ("uint4", "uvec4"),
        ("int2", "ivec2"),
        ("int3", "ivec3"),
        ("int4", "ivec4"),
        ("bool4", "bvec4"),
    ]:
        s = re.sub(r"\b" + old + r"\b", new, s)
    s = s.replace("asuint(", "floatBitsToUint(").replace("lerp(", "mix(")
    s = re.sub(r"\[(?:loop|unroll|branch)\]", "", s)
    s = re.sub(r"\((st\.(?:flags|rtt) & [\w]+)\)(?!\s*(?:!=|==))", r"((\1) != 0)", s)
    s = re.sub(r"\((wrapMode & [12])\) \?", r"((\1) != 0u) ?", s)
    s = s.replace("if (src >> 15)", "if ((src >> 15) != 0u)").replace(
        "if (targetWriteMask)", "if (targetWriteMask != 0u)"
    )
    s = s.replace("(raw & 0x8000) ?", "((raw & 0x8000) != 0u) ?")
    s = s.replace("sf < 0 ? -ts : ts", "mix(ts, -ts, lessThan(sf, ivec3(0)))").replace(
        "df < 0 ? -td : td", "mix(td, -td, lessThan(df, ivec3(0)))"
    )
    return s


common = (
    block("kCommon")
    .split("cbuffer TargetCB")[0]
    .replace("uint3 pad2;", "uint pad2[3];")
)
pixel = block("kColorConv") + block("kPixel1") + block("kPixel2")
pixel = re.sub(
    r"StructuredBuffer<DrawState>[\s\S]*?RasterizerOrderedTexture2D<uint> zbuf : register\(u1\);",
    "",
    pixel,
)
pixel = pixel.replace("tex.Load(", "LoadTex(").replace("rttTex.Load(", "LoadRtt(")
pixel = re.sub(
    r"(fbuf|zbuf)\[hp\] = ([^;]+);",
    r"imageStore(\1, ivec2(hp), uvec4(\2, 0, 0, 0));",
    pixel,
)
pixel = re.sub(r"(fbuf|zbuf)\[hp\]", r"imageLoad(\1, ivec2(hp)).r", pixel)
fragment = (
    dialect(common)
    + """
#ifdef PSPC_GL_SERIAL_FRAGMENTS
#define beginInvocationInterlockARB()
#define endInvocationInterlockARB()
#else
layout(pixel_interlock_ordered) in;
#endif
layout(std430, binding=0) readonly buffer DrawStates { DrawState states[]; };
layout(binding=0) uniform usampler2D tex;
layout(binding=1) uniform usampler2D rttTex;
layout(r32ui, binding=0) coherent uniform uimage2D fbuf;
layout(r32ui, binding=1) coherent uniform uimage2D zbuf;
uniform uint scale;
struct PSIn { vec4 pos; vec4 c0; vec3 c1; vec3 stq; vec2 zf; uint state; float lod; };
in noperspective vec4 color0;
in noperspective vec3 color1;
in noperspective vec3 stq;
in noperspective vec2 zf;
flat in uint state;
flat in float lod;
uvec4 LoadTex(ivec3 p) { return texelFetch(tex, p.xy, p.z); }
uint LoadRtt(ivec3 p) { return texelFetch(rttTex, p.xy, p.z).r; }
"""
    + dialect(pixel)
    + """
void main() {
    PSIn i = PSIn(gl_FragCoord, color0, color1, stq, zf, state, lod);
    beginInvocationInterlockARB();
    PSMain(i);
    endInvocationInterlockARB();
}
"""
)
up = block("kUpscale")
up = up[up.index("float4 UpTexel") :]
up = re.sub(
    r"\[numthreads\(8, 8, 1\)\]\s*void CSUpscale\(uint3 id : SV_DispatchThreadID\)",
    "void CSUpscale(uint3 id)",
    up,
)
up = up.replace("upSrc.Load(", "LoadUp(")
up = re.sub(r"upDst\[([^\n]+?)\] = ([^;]+);", r"imageStore(upDst, ivec2(\1), \2);", up)
# HLSL's component-wise boolean operators are scalar in GLSL. Evaluate the
# same four corner predicates separately, retaining the vector color math.
a = up.index("\tbool4 lv0 =")
b = up.index("\tfloat4 m =", a)
up = (
    up[:a]
    + """
    bool4 px;
    for (int k=0; k<4; ++k) {
        bool lv0 = e[k] != f[k] && e[k] != h[k];
        bool lv1 = lv0 && ((!Eq(f[k], b[k]) && !Eq(f[k], c[k])) || (!Eq(h[k], d[k]) && !Eq(h[k], g[k])) ||
                    (Eq(e[k], i[k]) && ((!Eq(f[k], f4[k]) && !Eq(f[k], i4[k])) || (!Eq(h[k], h5[k]) && !Eq(h[k], i5[k])))) || Eq(e[k], g[k]) || Eq(e[k], c[k]));
        bool lv2L = e[k] != g[k] && d[k] != g[k];
        bool lv2U = e[k] != c[k] && b[k] != c[k];
        float wd1 = WeightedDistance(e,c,g,i,h5,f4,h,f)[k];
        float wd2 = WeightedDistance(h,d,i5,f,i4,b,e,i)[k];
        bool edri = wd1 <= wd2 && lv0;
        bool edr = wd1 < wd2 && lv1;
        bool edrL = 2.0 * abs(f[k]-g[k]) <= abs(h[k]-c[k]) && lv2L && edr;
        bool edrU = abs(f[k]-g[k]) >= 2.0 * abs(h[k]-c[k]) && lv2U && edr;
        fx45[k] *= float(edr); fx30[k] *= float(edrL);
        fx60[k] *= float(edrU); fx45i[k] *= float(edri);
        px[k] = abs(e[k]-f[k]) <= abs(e[k]-h[k]);
    }
"""
    + up[b:]
)
up = up.replace(
    "bool4 Eq(float4 a, float4 b) {\n\treturn Df(a, b) < 15.0;\n}",
    "bool Eq(float a, float b) { return abs(a-b) < 15.0; }",
)
up = up.replace("Luma(E).xxxx", "float4(Luma(E))").replace(
    "(1.0 / (float)factor).xxxx", "float4(1.0 / (float)factor)"
)
upscale = (
    """
layout(local_size_x=8, local_size_y=8) in;
layout(binding=0) uniform usampler2D upSrc;
layout(rgba8ui, binding=0) writeonly uniform uimage2D upDst;
uniform ivec2 lvOrigin, lvSize;
uniform uint factor, wrapMode;
vec4 saturate(vec4 v) { return clamp(v, 0.0, 1.0); }
uvec4 LoadUp(ivec3 p) { return texelFetch(upSrc,p.xy,p.z); }
"""
    + dialect(up)
    + "\nvoid main() { CSUpscale(gl_GlobalInvocationID); }\n"
)
display = block("kDisplay").replace("Texture2D<uint>", "usampler2D")
display = re.sub(r"(\w+)\.Load\(", r"LoadRaw(\1, ", display)
display = dialect(display).replace("vec3 sum = 0.0;", "vec3 sum = vec3(0.0);")
display = display.replace(
    "clamp(i, 0, size - 1)", "clamp(i, ivec2(0), size - 1)"
).replace("clamp(i + 1, 0, size - 1)", "clamp(i + 1, ivec2(0), size - 1)")
compute = block("kCompute")[block("kCompute").index("// 1x rows") :]
compute = re.sub(r"\[numthreads\(8, 8, 1\)\]", "", compute).replace(
    "uint3 id : SV_DispatchThreadID", "uint3 id"
)
compute = re.sub(
    r"dstTex\[([^\]]+)\] = ([^;]+);",
    r"imageStore(dstTex, ivec2(\1), uvec4(\2, 0, 0, 0));",
    compute,
)
compute = re.sub(r"srcTex\[([^\]]+)\]", r"texelFetch(srcTex, ivec2(\1), 0).r", compute)
compute = (
    """
layout(local_size_x=8, local_size_y=8) in;
layout(binding=0) uniform usampler2D srcTex;
layout(r32ui,binding=0) writeonly uniform uimage2D dstTex;
uniform uvec2 origin, size;
uniform uint copyScale, format, outScale;
vec3 saturate(vec3 v) { return clamp(v,0.0,1.0); }
uint LoadRaw(usampler2D src, ivec3 p) { return texelFetch(src,p.xy,p.z).r; }
"""
    + display
    + dialect(compute)
    + "\nvoid main() { ENTRY(gl_GlobalInvocationID); }\n"
)
# Vertex transform and triangle assembly (gpu/geometry.h): one program per entry point (ENTRY,
# CSVertex or CSTriangle); the backend puts the GE's tables in place of TABLES. The structured buffers
# become std430 storage blocks with the C++ layouts, except that a vec3 there is 16-byte aligned:
# GeVertexIn's vectors and GeShadeLight's padding become arrays. ClipV is the shaders' own (a 64-byte
# stride here). D3D11 compiles these with IEEE strictness; GLSL needs `precise` on the doubles too, or
# their multiply-adds may be fused.
geo = "".join(block("kGeometry%d" % i, geometry_source) for i in range(1, 5))
geo = re.sub(
    r"StructuredBuffer<GeVertexIn>[\s\S]*?cbuffer GeoCB[^}]*\};",
    """layout(std430, binding=0) readonly buffer GeoInputs { GeVertexIn gInputs[]; };
layout(std430, binding=1) readonly buffer GeoDraws { GeDraw gDraws[]; };
layout(std430, binding=2) readonly buffer GeoTransforms { GeTransform gTransforms[]; };
layout(std430, binding=3) readonly buffer GeoJobs { GeJob gJobs[]; };
layout(std430, binding=4) buffer GeoClip { ClipV gClipOut[]; };
#define gClipIn gClipOut
layout(std430, binding=5) writeonly buffer GeoOut { uint gOut[]; };
uniform uint gCount, gScale, gBase;""",
    geo,
)
geo = geo.replace("float3 nrm; float3 pos;", "float nrm[3]; float pos[3];")
geo = re.sub(r"\bsv\.(nrm|pos)\b", r"float3(sv.\1[0], sv.\1[1], sv.\1[2])", geo)
geo = geo.replace("uint flags; uint3 pad; };", "uint flags; uint pad[3]; };")
geo = geo.replace(
    "(ClipV)0", "ClipV(float4(0.0, 0.0, 0.0, 0.0), 0, 0, 0, 0u, 0u, 0.0, 0.0, 0.0, 0.0, 0.0)"
).replace("(GpuV)0", "GpuV(0.0, 0.0, 0.0, 0.0, 0u, 0u, 0.0, 0.0, 0.0, 0u, 0.0)")
geo = re.sub(
    r"void StoreVertex\(uint index, GpuV g\) \{[\s\S]*?\n\}",
    """void StoreVertex(uint index, GpuV g) {
	uint a = index * 11u;
	gOut[a] = asuint(g.x);
	gOut[a + 1u] = asuint(g.y);
	gOut[a + 2u] = asuint(g.z);
	gOut[a + 3u] = asuint(g.fog);
	gOut[a + 4u] = g.c0;
	gOut[a + 5u] = g.c1;
	gOut[a + 6u] = asuint(g.s);
	gOut[a + 7u] = asuint(g.t);
	gOut[a + 8u] = asuint(g.q);
	gOut[a + 9u] = g.state;
	gOut[a + 10u] = asuint(g.lod);
}""",
    geo,
)
geo = re.sub(
    r"\[numthreads\(64, 1, 1\)\]\s*void (\w+)\(uint3 id : SV_DispatchThreadID\)",
    r"void \1(uint3 id)",
    geo,
)
geo = re.sub(r"\n(\t+)double ", r"\n\1precise double ", geo)
geo = geo.replace("static const ", "const ").replace("asfloat(", "uintBitsToFloat(")
geo = geo.replace("firstbithigh(", "findMSB(")
# GLSL reserves GL_ macros, and has a built-in sign() and a flat qualifier.
geo = re.sub(r"\bGL_(?=[A-Z_]+\b)", "LIGHT_", geo)
geo = re.sub(r"\bsign\b", "sgn", geo)
geo = re.sub(r"\bflat\b(?! shading)", "flatShade", geo)
geometry = (
    "\nlayout(local_size_x=64) in;\n"
    + dialect(geo)
    + "\nvoid main() { ENTRY(gl_GlobalInvocationID); }\n"
)
output = "// Generated by tools/gl_shaders.py; do not edit.\n#pragma once\nnamespace gpu::hw {\n"
for name, text in [
    ("kGLPixel", fragment),
    ("kGLUpscale", upscale),
    ("kGLCompute", compute),
    ("kGLDisplay", display),
    ("kGLGeometry", geometry),
]:
    output += "inline const char " + name + '[] = R"GLSL(' + text + ')GLSL";\n'
output += "}\n"
p = pathlib.Path(sys.argv[3])
p.parent.mkdir(parents=True, exist_ok=True)
if not p.exists() or p.read_text() != output:
    p.write_text(output)
