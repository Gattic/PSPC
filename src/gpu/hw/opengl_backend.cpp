// SPDX-License-Identifier: GPL-2.0-or-later
// Ordered integer-image GE rasterization. Guest memory is synchronized eagerly;
// no signal handler or inaccessible VRAM mappings are needed on Linux. Transform-mode
// triangles can be transformed and assembled by compute shaders (gpu/geometry.h).
#define GL_GLEXT_PROTOTYPES
#include "gpu/hw/opengl_backend.h"
#include "common/log.h"
#include "gpu/color.h"
#include "gpu/gemath.h"
#include "gpu/hw/draw_data.h"
#include "opengl_shaders.h"
#include <GL/glcorearb.h>
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

namespace gpu::hw {
namespace {
const char vertex[] = R"GLSL(#version 450 core
layout(location=0) in vec2 position;
layout(location=1) in vec2 inZF;
layout(location=2) in uvec4 inC0;
layout(location=3) in uvec4 inC1;
layout(location=4) in vec3 inSTQ;
layout(location=5) in uint inState;
layout(location=6) in float inLOD;
uniform vec2 extent;
out noperspective vec4 color0;
out noperspective vec3 color1;
out noperspective vec3 stq;
out noperspective vec2 zf;
flat out uint state;
flat out float lod;
void main() {
 gl_Position=vec4(position*2.0/extent-1.0,0,1);
 color0=vec4(inC0); color1=vec3(inC1.rgb); stq=inSTQ; zf=inZF; state=inState; lod=inLOD;
})GLSL";
const char presentVertex[] = R"GLSL(#version 450 core
void main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); gl_Position=vec4(p*2.0-1.0,0,1); }
)GLSL";
const char presentPixel[] = R"GLSL(
layout(binding=0) uniform usampler2D picture;
layout(binding=1) uniform usampler2D overlayImage;
uniform ivec2 sourceSize, overlaySize;
uniform vec2 destination, ratio;
uniform float windowHeight;
uniform uint sourceFormat;
layout(location=0) out vec4 outputColor;
vec4 Overlay(vec2 p) {
 p-=0.5; ivec2 a=ivec2(floor(p)); vec2 f=fract(p);
 vec4 c[4]; for(int k=0;k<4;++k) {
  uint v=texelFetch(overlayImage,clamp(a+ivec2(k&1,k>>1),ivec2(0),overlaySize-1),0).r;
  c[k]=vec4(v&255u,(v>>8)&255u,(v>>16)&255u,v>>24)/255.0;
 }
 return mix(mix(c[0],c[1],f.x),mix(c[2],c[3],f.x),f.y);
}
void main() {
 vec2 p=vec2(gl_FragCoord.x,windowHeight-gl_FragCoord.y)-destination;
 vec2 a=floor(p)*ratio, b=a+ratio;
 vec3 c=ratio.x>1.0 || ratio.y>1.0 ? AreaAverage(picture,ivec2(0),sourceSize,a,b,sourceFormat)
    : Bilinear(picture,sourceSize,(a+b)*0.5,sourceFormat);
 if(overlaySize.x>0) { vec4 o=Overlay(p*ratio*vec2(overlaySize)/vec2(sourceSize)); c=o.rgb+c*(1.0-o.a); }
 outputColor=vec4(c,1);
}
)GLSL";
GLuint Shader(GLenum kind, const std::string &text) {
	GLuint shader = glCreateShader(kind);
	const char *p = text.c_str();
	glShaderSource(shader, 1, &p, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint n = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &n);
		std::string error((size_t)std::max(n, 1), '\0');
		glGetShaderInfoLog(shader, n, nullptr, error.data());
		glDeleteShader(shader);
		throw std::runtime_error(error);
	}
	return shader;
}
GLuint Program(const std::string &first, const std::string &second = {}) {
	GLuint a = Shader(second.empty() ? GL_COMPUTE_SHADER : GL_VERTEX_SHADER, first), b = 0;
	try {
		if (!second.empty())
			b = Shader(GL_FRAGMENT_SHADER, second);
	} catch (...) {
		glDeleteShader(a);
		throw;
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, a);
	if (b)
		glAttachShader(p, b);
	glLinkProgram(p);
	glDeleteShader(a);
	if (b)
		glDeleteShader(b);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		GLint n = 0;
		glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
		std::string error((size_t)std::max(n, 1), '\0');
		glGetProgramInfoLog(p, n, nullptr, error.data());
		glDeleteProgram(p);
		throw std::runtime_error(error);
	}
	return p;
}
GLuint Texture(int w, int h, GLenum format = GL_R32UI) {
	GLuint t = 0;
	glCreateTextures(GL_TEXTURE_2D, 1, &t);
	glTextureStorage2D(t, 1, format, w, h);
	glTextureParameteri(t, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTextureParameteri(t, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (glGetError() != GL_NO_ERROR) {
		glDeleteTextures(1, &t);
		throw std::runtime_error("OpenGL texture allocation failed");
	}
	return t;
}
void UInt(GLuint p, const char *n, unsigned v) { glProgramUniform1ui(p, glGetUniformLocation(p, n), v); }
void IVec(GLuint p, const char *n, int x, int y) { glProgramUniform2i(p, glGetUniformLocation(p, n), x, y); }
void UVec(GLuint p, const char *n, unsigned x, unsigned y) { glProgramUniform2ui(p, glGetUniformLocation(p, n), x, y); }
void FVec(GLuint p, const char *n, float x, float y) { glProgramUniform2f(p, glGetUniformLocation(p, n), x, y); }
void Barrier() { glMemoryBarrier(GL_ALL_BARRIER_BITS); }
// The vertex transform's GLSL (tools/gl_shaders.py) for one entry point, with gemath.cpp's tables. Doubles
// are core since GLSL 4.00: the texture-level gradient and the reciprocal w of vertices behind the camera
// are computed in doubles, as on the CPU.
std::string GeometrySource(const char *entry) {
	std::string tables = "const ivec2 kRecip[128] = ivec2[128](";
	char buf[48];
	for (int i = 0; i < 128; ++i) {
		snprintf(buf, sizeof(buf), "%sivec2(%d, %d)", i ? ", " : "", geRecipSegments[i].b, geRecipSegments[i].m);
		tables += buf;
	}
	tables += ");\nconst ivec2 kRsqrt[256] = ivec2[256](";
	for (int i = 0; i < 256; ++i) {
		const GERecipSegment &s = geRsqrtSegments[i >> 7][i & 127];
		snprintf(buf, sizeof(buf), "%sivec2(%d, %d)", i ? ", " : "", s.b, s.m);
		tables += buf;
	}
	tables += ");\n";
	std::string src = kGLGeometry;
	src.replace(src.find("TABLES"), 6, tables);
	return std::string("#version 450 core\n#define DOUBLES 1\n#define ENTRY ") + entry + "\n" + src;
}
struct Surface {
	u32 addr = 0;
	int stride = 0, fmt = 0, h = 0;
	GLuint texture = 0;
	std::vector<u32> shadow;
};
class OpenGLBackend final : public RenderBackend, private DrawData {
	SDL_Window *window_ = nullptr;
	SDL_GLContext context_ = nullptr;
	bool ownedWindow_ = false, ownedVideo_ = false, visible_ = false;
	bool serialFragments_ = false;
	GLint maxViewport_[2] = {};
	std::recursive_mutex mutex_;
	GLuint dummyDepth_ = 0, vao_ = 0, vb_ = 0, sb_ = 0, fbo_ = 0, programs_[16] = {}, expand_ = 0, reduce_ = 0,
		   display_ = 0, upscale_ = 0, present_ = 0;
	std::vector<std::unique_ptr<Surface>> surfaces_;
	// Vertex transform on the GPU (gpu/geometry.h): CSVertex and CSTriangle, and their storage buffers
	// (the shaders' bindings); GEO_OUT holds CSTriangle's GPU vertices, drawn as a vertex buffer.
	enum { GEO_INPUTS, GEO_DRAWS, GEO_TRANSFORMS, GEO_JOBS, GEO_CLIP, GEO_OUT, GEO_BUFFERS };
	static constexpr size_t CLIP_STRIDE = 64;  // ClipV in std430
	GLuint geoPrograms_[2] = {}, geoShaders_[2] = {}, geoBuffers_[GEO_BUFFERS] = {};
	size_t geoBytes_[GEO_BUFFERS] = {};
	bool geoCompiling_ = false, geoParallel_ = false, geoVerify_ = false, printStats_ = false;
	std::atomic<bool> geoEnabled_{false};
	std::chrono::steady_clock::time_point geoStart_;
	TransformStats geoStats_;
	u64 syncs_ = 0;
	struct Current {
		std::unique_lock<std::recursive_mutex> lock;
		SDL_Window *previousWindow;
		SDL_GLContext previous;
		Current(OpenGLBackend &b)
			: lock(b.mutex_), previousWindow(SDL_GL_GetCurrentWindow()), previous(SDL_GL_GetCurrentContext()) {
			if (SDL_GL_MakeCurrent(b.window_, b.context_) != 0)
				throw std::runtime_error(SDL_GetError());
		}
		~Current() { SDL_GL_MakeCurrent(previousWindow, previous); }
	};
	void Compute(GLuint p, GLuint src, GLuint dst, int w, int h, int fmt, int outputScale = 1) {
		glUseProgram(p);
		glBindTextureUnit(0, src);
		glBindImageTexture(0, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
		UVec(p, "origin", 0, 0);
		UVec(p, "size", w, h);
		UInt(p, "copyScale", scale_);
		UInt(p, "format", fmt);
		UInt(p, "outScale", outputScale);
		int multiplier = p == expand_ ? scale_ : p == display_ ? outputScale : 1;
		glDispatchCompute((w * multiplier + 7) / 8, (h * multiplier + 7) / 8, 1);
		Barrier();
	}
	u32 Read(const Surface &s, int x, int y) {
		int bpp = s.fmt == 3 ? 4 : 2;
		u32 off = (s.addr - VRAM_BASE + (u32)(y * s.stride + x) * bpp) & (VRAM_SIZE - 1), v = 0;
		memcpy(&v, VramPtr() + off, bpp);
		return v;
	}
	void Validate(Surface &s) {
		std::vector<u32> pixel((size_t)scale_ * scale_);
		for (int y = 0; y < s.h; ++y)
			for (int x = 0; x < s.stride; ++x) {
				u32 value = Read(s, x, y);
				auto &previous = s.shadow[(size_t)y * s.stride + x];
				if (value == previous)
					continue;
				// Upload only the changed PSP pixel: its neighbours may contain finer geometry.
				std::fill(pixel.begin(), pixel.end(), value);
				glTextureSubImage2D(s.texture, 0, x * scale_, y * scale_, scale_, scale_, GL_RED_INTEGER,
									GL_UNSIGNED_INT, pixel.data());
				previous = value;
			}
		Barrier();
	}
	Surface *Find(u32 addr, int stride, int fmt) {
		addr = FoldAddress(addr);
		for (auto &s : surfaces_)
			if (s->addr == addr && s->stride == stride && s->fmt == fmt)
				return s.get();
		return nullptr;
	}
	Surface *Get(u32 addr, int stride, int fmt, int height) {
		Surface *s = Find(addr, stride, fmt);
		if (s) {
			auto it =
				std::find_if(surfaces_.begin(), surfaces_.end(), [s](const auto &item) { return item.get() == s; });
			std::rotate(it, it + 1, surfaces_.end());
		}
		if (!s) {
			if (surfaces_.size() >= 4) {
				glDeleteTextures(1, &surfaces_.front()->texture);
				surfaces_.erase(surfaces_.begin());
			}
			auto item = std::make_unique<Surface>();
			item->addr = FoldAddress(addr);
			item->stride = stride;
			item->fmt = fmt;
			s = item.get();
			surfaces_.push_back(std::move(item));
		}
		if (s->h < height) {
			GLuint replacement = Texture(stride * scale_, height * scale_);
			std::vector<u32> native((size_t)stride * height);
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < stride; ++x)
					native[(size_t)y * stride + x] = Read(*s, x, y);
			GLuint upload = Texture(stride, height);
			glTextureSubImage2D(upload, 0, 0, 0, stride, height, GL_RED_INTEGER, GL_UNSIGNED_INT, native.data());
			Compute(expand_, upload, replacement, stride, height, fmt);
			glDeleteTextures(1, &upload);
			if (s->texture) {
				glCopyImageSubData(s->texture, GL_TEXTURE_2D, 0, 0, 0, 0, replacement, GL_TEXTURE_2D, 0, 0, 0, 0,
								   stride * scale_, s->h * scale_, 1);
				glDeleteTextures(1, &s->texture);
			}
			s->shadow.resize(native.size());
			std::copy(native.begin() + (size_t)s->h * stride, native.end(), s->shadow.begin() + (size_t)s->h * stride);
			s->texture = replacement;
			s->h = height;
		}
		Validate(*s);
		return s;
	}
	void Download(Surface &s) {
		GLuint target = Texture(s.stride, s.h);
		Compute(reduce_, s.texture, target, s.stride, s.h, s.fmt);
		std::vector<u32> data(s.shadow.size());
		glGetTextureImage(target, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, (GLsizei)(data.size() * 4), data.data());
		glDeleteTextures(1, &target);
		int bpp = s.fmt == 3 ? 4 : 2;
		for (int y = 0; y < s.h; ++y)
			for (int x = 0; x < s.stride; ++x) {
				size_t i = (size_t)y * s.stride + x;
				memcpy(VramPtr() + ((s.addr - VRAM_BASE + (u32)i * bpp) & (VRAM_SIZE - 1)), &data[i], bpp);
				s.shadow[i] = data[i];
			}
	}
	GLuint Decode(const SamplerState &ss) {
		u32 levels[8];
		int w, h;
		TextureAtlasLayout(ss, levels, &w, &h);
		std::vector<u32> data((size_t)w * h);
		for (int i = 0; i <= ss.maxLevel; ++i)
			DecodeTextureLevel(ss, i, std::min<int>(ss.levels[i].w, 512), std::min<int>(ss.levels[i].h, 512),
							   data.data() + (levels[i] & 65535), w);
		GLuint t = Texture(w, h, GL_RGBA8UI);
		glTextureSubImage2D(t, 0, 0, 0, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, data.data());
		int factor = TextureUpscale(texUpscale_, w, h);
		if (factor == 1)
			return t;
		GLuint dest = Texture(w * factor, h * factor, GL_RGBA8UI);
		glUseProgram(upscale_);
		glBindTextureUnit(0, t);
		glBindImageTexture(0, dest, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
		UInt(upscale_, "factor", factor);
		UInt(upscale_, "wrapMode", (ss.clampS ? 0 : 1) | (ss.clampT ? 0 : 2));
		for (int i = 0; i <= ss.maxLevel; ++i) {
			int lw = std::min<int>(ss.levels[i].w, 512), lh = std::min<int>(ss.levels[i].h, 512);
			IVec(upscale_, "lvOrigin", levels[i] & 65535, 0);
			IVec(upscale_, "lvSize", lw, lh);
			glDispatchCompute((lw * factor + 7) / 8, (lh * factor + 7) / 8, 1);
		}
		Barrier();
		glDeleteTextures(1, &t);
		return dest;
	}
	void DrawPicture(GLuint texture, int width, int height, int format, GLuint overlay = 0, int ow = 0, int oh = 0) {
		int ww, wh;
		SDL_GL_GetDrawableSize(window_, &ww, &wh);
		if (ww <= 0 || wh <= 0)
			return;
		int w = std::min(ww, (int)((int64_t)wh * width / height)), h = (int)((int64_t)w * height / width),
			x = (ww - w) / 2, y = (wh - h) / 2;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, ww, wh);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glViewport(x, y, w, h);
		glUseProgram(present_);
		glBindVertexArray(vao_);
		glBindTextureUnit(0, texture);
		glBindTextureUnit(1, overlay);
		IVec(present_, "sourceSize", width, height);
		IVec(present_, "overlaySize", ow, oh);
		UInt(present_, "sourceFormat", format);
		FVec(present_, "destination", x, wh - y - h);
		FVec(present_, "ratio", (float)width / w, (float)height / h);
		glProgramUniform1f(present_, glGetUniformLocation(present_, "windowHeight"), (float)wh);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		SDL_GL_SwapWindow(window_);
	}
	// Whether the primitives can be drawn here with the states, and the target rows they need. Color and
	// depth overlapping in memory, or coordinates outside a target's stride, need the exact memory order
	// of the software reference.
	bool Drawable(const BatchPrim *prims, size_t count, const RasterState *states, size_t n, int *height, bool *usesDepth) const {
		bool supported = true;
		*height = 1;
		*usesDepth = false;
		const PixelState &ps = states[0].pix;
		for (size_t i = 0; i < n; ++i) {
			const auto &p = states[i].pix;
			*usesDepth |= p.depthWrite || p.depthTestFunc != GE_COMP_ALWAYS || (p.clearMode && p.depthClear);
		}
		for (size_t i = 0; i < count; ++i) {
			*height = std::max(*height, prims[i].range.y2 / SUBPIX + 1);
			supported &= prims[i].range.x1 >= 0 && prims[i].range.y1 >= 0 && prims[i].range.x2 / SUBPIX < ps.fbStride &&
						 (!*usesDepth || prims[i].range.x2 / SUBPIX < ps.zbStride);
		}
		if (*usesDepth) {
			u32 cb = (u32)*height * ps.fbStride * (ps.fbFormat == 3 ? 4 : 2), zb = (u32)*height * ps.zbStride * 2;
			bool overlap = cb >= VRAM_SIZE || zb >= VRAM_SIZE || ((ps.fbOffset - ps.zbOffset) & (VRAM_SIZE - 1)) < zb ||
						   ((ps.zbOffset - ps.fbOffset) & (VRAM_SIZE - 1)) < cb;
			supported &= !overlap;
		}
		return supported && ps.fbStride > 0 && (!*usesDepth || ps.zbStride > 0) && *height * scale_ <= MAX_TEXTURE_SIDE &&
			   ps.fbStride * scale_ <= MAX_TEXTURE_SIDE && (!*usesDepth || ps.zbStride * scale_ <= MAX_TEXTURE_SIDE);
	}
	// Starts compiling the vertex transform's shaders: on the driver's own threads when it can
	// (KHR_parallel_shader_compile), and until they're ready the CPU transforms.
	void InitGeometry(const OpenGLOptions &o) {
		const char *env = getenv("PSPC_GPU_TRANSFORM");
		if (!o.gpuTransform || (env && *env == '0'))
			return;
		geoVerify_ = o.verifyTransform || getenv("PSPC_GPU_TRANSFORM_VERIFY") != nullptr;
		const bool wait = o.waitForShaders || getenv("PSPC_GPU_TRANSFORM_WAIT") != nullptr;
		if (!wait && SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile")) {
			auto threads = (PFNGLMAXSHADERCOMPILERTHREADSKHRPROC)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
			if (threads) {
				threads(0xFFFFFFFFu);
				geoParallel_ = true;
			}
		}
		geoStart_ = std::chrono::steady_clock::now();
		const char *entries[2] = { "CSVertex", "CSTriangle" };
		for (int i = 0; i < 2; ++i) {
			const std::string text = GeometrySource(entries[i]);
			const char *p = text.c_str();
			geoShaders_[i] = glCreateShader(GL_COMPUTE_SHADER);
			glShaderSource(geoShaders_[i], 1, &p, nullptr);
			glCompileShader(geoShaders_[i]);
			geoPrograms_[i] = glCreateProgram();
			glAttachShader(geoPrograms_[i], geoShaders_[i]);
			glLinkProgram(geoPrograms_[i]);
		}
		glCreateBuffers(GEO_BUFFERS, geoBuffers_);
		geoCompiling_ = true;
		if (!geoParallel_)
			PollGeometry();
	}
	// Turns the GPU transform on once its shaders are ready (without parallel compiling, they are).
	void PollGeometry() {
		if (geoParallel_) {
			for (GLuint p : geoPrograms_) {
				GLint done = 0;
				glGetProgramiv(p, GL_COMPLETION_STATUS_KHR, &done);
				if (!done)
					return;
			}
		}
		geoCompiling_ = false;
		bool ok = true;
		for (int i = 0; i < 2; ++i) {
			GLint linked = 0;
			glGetProgramiv(geoPrograms_[i], GL_LINK_STATUS, &linked);
			if (!linked && ok) {
				GLint n = 0;
				glGetShaderiv(geoShaders_[i], GL_INFO_LOG_LENGTH, &n);
				std::string error((size_t)std::max(n, 1), '\0');
				glGetShaderInfoLog(geoShaders_[i], n, nullptr, error.data());
				if (error.size() <= 1) {
					glGetProgramiv(geoPrograms_[i], GL_INFO_LOG_LENGTH, &n);
					error.assign((size_t)std::max(n, 1), '\0');
					glGetProgramInfoLog(geoPrograms_[i], n, nullptr, error.data());
				}
				LOGW("opengl", "vertex transform shaders unavailable; transforming on the CPU: %.2000s", error.c_str());
			}
			ok = ok && linked;
			glDeleteShader(geoShaders_[i]);
			geoShaders_[i] = 0;
		}
		if (!ok)
			return;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - geoStart_).count();
		LOGI("opengl", "vertex transform on the GPU (shaders ready in %d ms)%s", (int)ms, geoVerify_ ? ", verified against the CPU" : "");
		geoEnabled_.store(true, std::memory_order_release);
	}
	// CSVertex over the batch's vertices and CSTriangle over its jobs, into GEO_OUT; then, verifying, the
	// GPU's vertices against the CPU's.
	void RunGeometry(const GeometryBatch &g, const RasterState *states) {
		const size_t nIn = g.inputs.size(), nJobs = g.jobs.size(), outBytes = nJobs * GEO_JOB_VERTS * sizeof(GpuVertex);
		// A batch split while its draws were assembled shares their inputs with the batch before: only those
		// from the first its jobs use are transformed again.
		u32 firstIn = (u32)nIn;
		for (const GeJob &j : g.jobs)
			firstIn = std::min({ firstIn, j.v[0], j.v[1], j.v[2] });
		auto upload = [&](int i, const void *data, size_t bytes) {
			glNamedBufferData(geoBuffers_[i], bytes ? bytes : 16, bytes ? data : nullptr, GL_STREAM_DRAW);
		};
		auto reserve = [&](int i, size_t bytes) {
			if (geoBytes_[i] >= bytes)
				return;
			size_t cap = std::max<size_t>(geoBytes_[i] * 2, 1 << 20);
			while (cap < bytes)
				cap *= 2;
			glNamedBufferData(geoBuffers_[i], cap, nullptr, GL_DYNAMIC_COPY);
			geoBytes_[i] = cap;
		};
		upload(GEO_INPUTS, g.inputs.data(), nIn * sizeof(GeVertexIn));
		upload(GEO_DRAWS, g.draws.data(), g.draws.size() * sizeof(GeDraw));
		upload(GEO_TRANSFORMS, g.transforms.data(), g.transforms.size() * sizeof(GeTransform));
		upload(GEO_JOBS, g.jobs.data(), nJobs * sizeof(GeJob));
		reserve(GEO_CLIP, nIn * CLIP_STRIDE);
		reserve(GEO_OUT, outBytes);
		for (int i = 0; i < GEO_BUFFERS; ++i)
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, geoBuffers_[i]);
		if (nIn > firstIn) {
			glUseProgram(geoPrograms_[0]);
			UInt(geoPrograms_[0], "gCount", (u32)(nIn - firstIn));
			UInt(geoPrograms_[0], "gBase", firstIn);
			glDispatchCompute((GLuint)((nIn - firstIn + 63) / 64), 1, 1);
			glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		}
		glUseProgram(geoPrograms_[1]);
		UInt(geoPrograms_[1], "gCount", (u32)nJobs);
		UInt(geoPrograms_[1], "gScale", (u32)scale_);
		glDispatchCompute((GLuint)((nJobs + 63) / 64), 1, 1);
		Barrier();
		geoStats_.jobs += nJobs;
		geoStats_.vertices += nIn - firstIn;
		if (geoVerify_) {
			std::vector<GpuVertex> gpu(nJobs * GEO_JOB_VERTS);
			glGetNamedBufferSubData(geoBuffers_[GEO_OUT], 0, (GLsizeiptr)outBytes, gpu.data());
			VerifyGeometry(g, states, gpu.data(), geoStats_, "opengl");
		}
	}
	void PrintTransformStats() {
		if (geoEnabled_)
			LOGI("opengl", "GPU transform: %llu vertices, %llu triangle jobs%s", (unsigned long long)geoStats_.vertices,
				 (unsigned long long)geoStats_.jobs,
				 geoVerify_ ? (", " + std::to_string(geoStats_.mismatches) + " of " + std::to_string(geoStats_.checked) +
							   " jobs differ from the CPU").c_str() : "");
	}
	// geo: the batch's geometry, when it has PrimType::Geometry primitives. alone: one of them drawn by
	// itself, after its batch's geometry ran, with the batch's states (which its GPU vertices name).
	void DrawBatchImpl(const BatchPrim *prims, size_t count, const RasterState *states, size_t n, const GeometryBatch *geo, bool alone) {
		if (!count || !n)
			return;
		Current current(*this);
		if (geoCompiling_)
			PollGeometry();
		// Alone, only its own state counts.
		const RasterState *own = alone ? &states[prims[0].stateIndex] : states;
		int height;
		bool usesDepth;
		if (!Drawable(prims, count, own, alone ? 1 : n, &height, &usesDepth)) {
			if (geo && !alone)
				RunGeometry(*geo, states);
			// One aliased primitive must not send unrelated later states through software.
			if (count > 1 || (geo && !alone)) {
				for (size_t i = 0; i < count; ++i) {
					BatchPrim p = prims[i];
					if (p.type == PrimType::Geometry) {
						DrawBatchImpl(&p, 1, states, n, geo, true);
						continue;
					}
					p.stateIndex = 0;
					DrawBatchImpl(&p, 1, &states[prims[i].stateIndex], 1, nullptr, false);
				}
				return;
			}
			for (size_t i = 0; i < count; ++i) {
				const auto &p = prims[i];
				const auto &s = states[p.stateIndex];
				switch (p.type) {
				case PrimType::Triangle:
					DrawTriangle(p.v[0], p.v[1], p.v[2], p.range, s);
					break;
				case PrimType::Rect:
					DrawRectangle(p.v[0], p.v[1], p.range, s);
					break;
				case PrimType::ClearRect:
					ClearRectangle(p.v[0], p.v[1], p.range, s);
					break;
				case PrimType::Line:
					DrawLine(p.v[0], p.v[1], p.range, s);
					break;
				case PrimType::Point:
					DrawPoint(p.v[0], p.range, s);
					break;
				case PrimType::Geometry:  // TransformsGeometryFor leaves such draws to the CPU
					break;
				}
			}
			return;
		}
		const PixelState &ps = own[0].pix;
		Surface *color = Get(VRAM_BASE + ps.fbOffset, ps.fbStride, ps.fbFormat, height);
		Surface *depth = usesDepth ? Get(VRAM_BASE + ps.zbOffset, ps.zbStride, 4, height) : nullptr;
		BuildVertices(prims, count, states, geo);
		FillStates(states, n);
		if (geo && !alone)
			RunGeometry(*geo, states);
		glNamedBufferData(vb_, verts_.size() * sizeof(GpuVertex), verts_.data(), GL_STREAM_DRAW);
		glBindVertexArray(vao_);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, sb_);
		// The same viewport for every batch (llvmpipe clips primitives to it), with sides that are powers of
		// two (pixel positions map to clip space and back exactly): a primitive draws the same whatever the
		// rest of its batch covers, whether the CPU or the GPU transformed it.
		int extentW = 512, extentH = 512;
		while (extentW < color->stride)
			extentW *= 2;
		while (extentH < height)
			extentH *= 2;
		if (extentW * scale_ > maxViewport_[0] || extentH * scale_ > maxViewport_[1]) {
			extentW = color->stride;
			extentH = height;
		}
		bool gpuVb = false;  // the vertex array reads vb_
		for (const Segment &seg : segs_) {
			if (!seg.count)
				continue;
			const auto &st = states[seg.state];
			GLuint texture = 0, feedback = 0;
			if (st.enableTextures && !st.pix.clearMode) {
				// Indexed/swizzled feedback also sees all preceding draws in this batch.
				Download(*color);
				if (depth)
					Download(*depth);
				texture = Decode(st.samp);
				if (!st.samp.swizzle && st.samp.maxLevel == 0 && st.samp.texfmt <= GE_TFMT_8888) {
					for (auto &surface : surfaces_) {
						int bpp = surface->fmt == 3 ? 4 : 2;
						u32 addr = FoldAddress(st.samp.levels[0].addr);
						if (surface->fmt != st.samp.texfmt || surface->stride != st.samp.levels[0].bufw ||
							addr < surface->addr)
							continue;
						u32 pixel = (addr - surface->addr) / bpp, x = pixel % surface->stride,
							y = pixel / surface->stride;
						if (x + st.samp.levels[0].w > (unsigned)surface->stride ||
							y + st.samp.levels[0].h > (unsigned)surface->h)
							continue;
						Validate(*surface);
						feedback = Texture(surface->stride * scale_, surface->h * scale_);
						glCopyImageSubData(surface->texture, GL_TEXTURE_2D, 0, 0, 0, 0, feedback, GL_TEXTURE_2D, 0, 0,
										   0, 0, surface->stride * scale_, surface->h * scale_, 1);
						Barrier();
						for (size_t i = 0; i < n; ++i)
							if (SameTexture(states[i].samp, st.samp)) {
								auto &d = gpuStates_[i];
								d.rtt = 0x100 | surface->fmt | (scale_ << 12);
								d.texLevels[0] = (d.texLevels[0] & 0xFFFF0000u) | (x * scale_);
								d.rttY = y * scale_;
							}
						break;
					}
				}
			}
			if (seg.gpu != gpuVb) {
				glVertexArrayVertexBuffer(vao_, 0, seg.gpu ? geoBuffers_[GEO_OUT] : vb_, 0, sizeof(GpuVertex));
				gpuVb = seg.gpu;
			}
			glNamedBufferData(sb_, gpuStates_.size() * sizeof(DrawStateGpu), gpuStates_.data(), GL_STREAM_DRAW);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
			glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, color->stride * scale_);
			glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, height * scale_);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			glViewport(0, 0, extentW * scale_, extentH * scale_);
			glEnable(GL_SCISSOR_TEST);
			int x = std::max(0, st.scissor.x1 / SUBPIX), y = std::max(0, st.scissor.y1 / SUBPIX);
			glScissor(x * scale_, y * scale_, std::max(0, st.scissor.x2 / SUBPIX + 1 - x) * scale_,
					  std::max(0, st.scissor.y2 / SUBPIX + 1 - y) * scale_);
			GLuint p = programs_[st.pix.fbFormat * 4 + (st.pix.clearMode ? 2 : st.enableTextures ? 1 : 0)];
			glUseProgram(p);
			FVec(p, "extent", (float)extentW, (float)extentH);
			UInt(p, "scale", scale_);
			glBindTextureUnit(0, texture);
			glBindTextureUnit(1, feedback);
			glBindImageTexture(0, color->texture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
			glBindImageTexture(1, depth ? depth->texture : dummyDepth_, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
			const GLenum primitive = seg.lines ? GL_LINES : GL_TRIANGLES;
			if (serialFragments_) {
				// Each primitive covers a pixel at most once (multisampling is off).
				// Complete its image writes before drawing the next overlapping primitive.
				const u32 vertices = seg.lines ? 2 : 3;
				for (u32 first = seg.first; first < seg.first + seg.count; first += vertices) {
					glDrawArrays(primitive, first, vertices);
					Barrier();
				}
			} else {
				glDrawArrays(primitive, seg.first, seg.count);
				Barrier();
			}
			glDeleteTextures(1, &texture);
			glDeleteTextures(1, &feedback);
		}
		if (gpuVb)
			glVertexArrayVertexBuffer(vao_, 0, vb_, 0, sizeof(GpuVertex));
		Download(*color);
		if (depth)
			Download(*depth);
		if (glGetError() != GL_NO_ERROR)
			throw std::runtime_error("OpenGL GE draw failed");
	}

  public:
	~OpenGLBackend() override { ReleaseWindow(); }
	const char *Name() const override { return "opengl"; }
	void Init(const OpenGLOptions &o) {
		scale_ = std::clamp(o.scale, 1, 16);
		texFilter_ = std::clamp(o.textureFilter, 0, 2);
		texUpscale_ = std::clamp(o.textureUpscale, 1, 4);
		if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
			if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
				throw std::runtime_error(SDL_GetError());
			ownedVideo_ = true;
		}
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		window_ = static_cast<SDL_Window *>(o.window);
		visible_ = window_ != nullptr;
		if (!window_) {
			window_ = SDL_CreateWindow("PSPC GPU", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
			ownedWindow_ = true;
		}
		if (!window_)
			throw std::runtime_error(SDL_GetError());
		context_ = SDL_GL_CreateContext(window_);
		if (!context_)
			throw std::runtime_error(SDL_GetError());
		const char *serial = getenv("PSPC_GL_SERIAL_FRAGMENTS");
		serialFragments_ = serial && std::string(serial) == "1";
		if (!serialFragments_ && !SDL_GL_ExtensionSupported("GL_ARB_fragment_shader_interlock"))
			throw std::runtime_error("OpenGL ordered fragment shader interlock is unavailable");
		if (serialFragments_)
			LOGW("opengl", "Serial fragment verification mode: one primitive per draw with memory barriers");
		GLint side = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &side);
		if (side < MAX_TEXTURE_SIDE)
			throw std::runtime_error("OpenGL requires 16384-pixel textures");
		glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewport_);
		LOGI("opengl", "%s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
		SDL_GL_SetSwapInterval(o.vsync ? 1 : 0);
		for (int fmt = 0; fmt < 4; ++fmt)
			for (int variant = 0; variant < 3; ++variant) {
				std::string prefix = std::string("#version 450 core\n") +
					(serialFragments_ ? "#define PSPC_GL_SERIAL_FRAGMENTS 1\n" :
					 "#extension GL_ARB_fragment_shader_interlock : require\n") + "#define FMT " +
					std::to_string(fmt) + "\n#define TEX " + std::to_string(variant == 1) + "\n#define CLEAR " +
					std::to_string(variant == 2) + "\n";
				programs_[fmt * 4 + variant] = Program(vertex, prefix + kGLPixel);
			}
		expand_ = Program(std::string("#version 450 core\n#define ENTRY CSExpand\n") + kGLCompute);
		reduce_ = Program(std::string("#version 450 core\n#define ENTRY CSReduce\n") + kGLCompute);
		display_ = Program(std::string("#version 450 core\n#define ENTRY CSDisplay\n") + kGLCompute);
		upscale_ = Program(std::string("#version 450 core\n") + kGLUpscale);
		present_ = Program(
			presentVertex,
			std::string(
				"#version 450 core\nuint LoadRaw(usampler2D src, ivec3 p) { return texelFetch(src,p.xy,p.z).r; }\n") +
				kGLDisplay + presentPixel);
		dummyDepth_ = Texture(1, 1);
		glCreateVertexArrays(1, &vao_);
		glCreateBuffers(1, &vb_);
		glCreateBuffers(1, &sb_);
		glCreateFramebuffers(1, &fbo_);
		glVertexArrayVertexBuffer(vao_, 0, vb_, 0, sizeof(GpuVertex));
		const int count[] = {2, 2, 4, 4, 3, 1, 1};
		const int offset[] = {0, 8, 16, 20, 24, 36, 40};
		for (int i = 0; i < 7; ++i) {
			glEnableVertexArrayAttrib(vao_, i);
			glVertexArrayAttribBinding(vao_, i, 0);
			if (i == 2 || i == 3 || i == 5)
				glVertexArrayAttribIFormat(vao_, i, count[i], i == 5 ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE, offset[i]);
			else
				glVertexArrayAttribFormat(vao_, i, count[i], GL_FLOAT, GL_FALSE, offset[i]);
		}
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_BLEND);
		glDisable(GL_DITHER);
		printStats_ = getenv("PSPC_GPU_STATS") != nullptr;
		InitGeometry(o);
		SDL_GL_MakeCurrent(window_, nullptr);
	}
	void DrawBatch(const BatchPrim *prims, size_t count, const RasterState *states, size_t n) override {
		DrawBatchImpl(prims, count, states, n, nullptr, false);
	}
	bool TransformsGeometry() const override { return geoEnabled_.load(std::memory_order_acquire); }
	// A geometry primitive drawn by itself: its range is the scissor.
	bool TransformsGeometryFor(const RasterState &st) const override {
		BatchPrim p{};
		p.type = PrimType::Geometry;
		p.range = st.scissor;
		int height;
		bool usesDepth;
		return Drawable(&p, 1, &st, 1, &height, &usesDepth);
	}
	bool VerifiesGeometry() const override { return geoVerify_; }
	TransformStats GetTransformStats() const override { return geoStats_; }
	void DrawGeometryBatch(const BatchPrim *prims, size_t count, const RasterState *states, size_t n,
						   const GeometryBatch &geo) override {
		DrawBatchImpl(prims, count, states, n, geo.Empty() ? nullptr : &geo, false);
	}
	// DrawBatch writes back before returning, including texture feedback and block transfers.
	void Sync() override {
		if (printStats_ && ++syncs_ % 300 == 0)
			PrintTransformStats();
	}
	bool GetDisplayImage(u32 addr, int stride, int fmt, int w, int h, std::vector<u32> &out, int *ow, int *oh,
						 int requested) override {
		Current current(*this);
		Surface *s = Find(addr, stride, fmt);
		if (!s || w > stride || h <= 0 || w <= 0)
			return false;
		s = Get(addr, stride, fmt, std::max(h, s->h));
		int factor = requested > 0 ? std::min(requested, scale_) : scale_;
		GLuint dest = Texture(w * factor, h * factor);
		Compute(display_, s->texture, dest, w, h, fmt, factor);
		out.resize((size_t)w * h * factor * factor);
		glGetTextureImage(dest, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, (GLsizei)(out.size() * 4), out.data());
		glDeleteTextures(1, &dest);
		*ow = w * factor;
		*oh = h * factor;
		return true;
	}
	bool PresentImage(const u32 *rgba, int w, int h, int stride) override {
		if (!visible_ || !context_)
			return false;
		Current current(*this);
		GLuint t = Texture(w, h);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
		glTextureSubImage2D(t, 0, 0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, rgba);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		DrawPicture(t, w, h, 3);
		glDeleteTextures(1, &t);
		return true;
	}
	bool PresentDisplay(u32 addr, int stride, int fmt, int w, int h,
						const std::function<void(u32 *, int, int)> &overlay, bool capture) override {
		if (!visible_ || !context_)
			return false;
		Current current(*this);
		Surface *s = Find(addr, stride, fmt);
		if (!s || w > stride)
			return false;
		s = Get(addr, stride, fmt, std::max(h, s->h));
		if (capture) {
			std::vector<u32> pixels;
			int ow, oh;
			if (!GetDisplayImage(addr, stride, fmt, w, h, pixels, &ow, &oh, 0))
				return false;
			if (overlay)
				overlay(pixels.data(), ow, oh);
			return PresentImage(pixels.data(), ow, oh, ow);
		}
		GLuint picture = Texture(w * scale_, h * scale_);
		glCopyImageSubData(s->texture, GL_TEXTURE_2D, 0, 0, 0, 0, picture, GL_TEXTURE_2D, 0, 0, 0, 0, w * scale_,
						   h * scale_, 1);
		Barrier();
		GLuint over = 0;
		int ow = 0, oh = 0;
		if (overlay) {
			int ww, wh;
			SDL_GL_GetDrawableSize(window_, &ww, &wh);
			int factor = std::clamp(std::min((ww + 479) / 480, (wh + 271) / 272), 1, 16);
			ow = 480 * factor;
			oh = 272 * factor;
			std::vector<u32> pixels((size_t)ow * oh);
			overlay(pixels.data(), ow, oh);
			over = Texture(ow, oh);
			glTextureSubImage2D(over, 0, 0, 0, ow, oh, GL_RED_INTEGER, GL_UNSIGNED_INT, pixels.data());
		}
		DrawPicture(picture, w * scale_, h * scale_, fmt, over, ow, oh);
		glDeleteTextures(1, &picture);
		glDeleteTextures(1, &over);
		return true;
	}
	void ReleaseWindow() override {
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (context_) {
			SDL_Window *previousWindow = SDL_GL_GetCurrentWindow();
			SDL_GLContext previous = SDL_GL_GetCurrentContext();
			if (previous == context_) {
				previous = nullptr;
				previousWindow = nullptr;
			}
			SDL_GL_MakeCurrent(window_, context_);
			if (printStats_)
				PrintTransformStats();
			geoEnabled_.store(false, std::memory_order_release);
			geoCompiling_ = false;
			for (auto &s : surfaces_)
				glDeleteTextures(1, &s->texture);
			surfaces_.clear();
			for (GLuint p : programs_)
				if (p)
					glDeleteProgram(p);
			for (GLuint p : {expand_, reduce_, display_, upscale_, present_, geoPrograms_[0], geoPrograms_[1]})
				if (p)
					glDeleteProgram(p);
			for (GLuint s : geoShaders_)
				if (s)
					glDeleteShader(s);
			glDeleteBuffers(GEO_BUFFERS, geoBuffers_);
			glDeleteTextures(1, &dummyDepth_);
			glDeleteBuffers(1, &vb_);
			glDeleteBuffers(1, &sb_);
			glDeleteVertexArrays(1, &vao_);
			glDeleteFramebuffers(1, &fbo_);
			SDL_GL_MakeCurrent(window_, nullptr);
			SDL_GL_DeleteContext(context_);
			context_ = nullptr;
			SDL_GL_MakeCurrent(previousWindow, previous);
		}
		if (ownedWindow_ && window_)
			SDL_DestroyWindow(window_);
		window_ = nullptr;
		visible_ = false;
		if (ownedVideo_) {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
			ownedVideo_ = false;
		}
	}
};
} // namespace
std::unique_ptr<RenderBackend> CreateOpenGLBackend(const OpenGLOptions &options, std::string *error) {
	struct RestoreContext {
		SDL_Window *window = SDL_GL_GetCurrentWindow();
		SDL_GLContext context = SDL_GL_GetCurrentContext();
		~RestoreContext() {
			if (SDL_WasInit(SDL_INIT_VIDEO))
				SDL_GL_MakeCurrent(window, context);
		}
	} restore;
	try {
		auto backend = std::make_unique<OpenGLBackend>();
		backend->Init(options);
		return backend;
	} catch (const std::exception &e) {
		if (error)
			*error = e.what();
		return {};
	}
}
} // namespace gpu::hw
