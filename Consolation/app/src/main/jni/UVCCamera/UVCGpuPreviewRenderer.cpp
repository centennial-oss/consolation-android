#include "UVCGpuPreviewRenderer.h"

#include "utilbase.h"

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

static uint64_t now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *vertex_shader_src =
	"#version 300 es\n"
	"layout(location=0) in vec2 aPos;\n"
	"layout(location=1) in vec2 aTex;\n"
	"uniform mat3 uXform;\n"
	"out vec2 vTex;\n"
	"void main() {\n"
	"  vTex = aTex;\n"
	"  gl_Position = vec4((uXform * vec3(aPos, 1.0)).xy, 0.0, 1.0);\n"
	"}\n";

static const char *yuyv_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uPacked;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"uniform int uUyvy;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  int pairX = (x / 2) * 4;\n"
	"  float b0 = texelFetch(uPacked, ivec2(pairX + 0, yrow), 0).r;\n"
	"  float b1 = texelFetch(uPacked, ivec2(pairX + 1, yrow), 0).r;\n"
	"  float b2 = texelFetch(uPacked, ivec2(pairX + 2, yrow), 0).r;\n"
	"  float b3 = texelFetch(uPacked, ivec2(pairX + 3, yrow), 0).r;\n"
	"  float yy;\n"
	"  float uu;\n"
	"  float vv;\n"
	"  if (uUyvy != 0) {\n"
	"    yy = (x & 1) == 0 ? b1 : b3;\n"
	"    uu = b0;\n"
	"    vv = b2;\n"
	"  } else {\n"
	"    yy = (x & 1) == 0 ? b0 : b2;\n"
	"    uu = b1;\n"
	"    vv = b3;\n"
	"  }\n"
	"  fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *nv12_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uY;\n"
	"uniform sampler2D uUV;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  float yy = texelFetch(uY, ivec2(x, yrow), 0).r;\n"
	"  vec2 uv = texelFetch(uUV, ivec2(x / 2, yrow / 2), 0).rg;\n"
	"  fragColor = vec4(clamp(yuvToRgb(yy, uv.r, uv.g), 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *yu12_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uY;\n"
	"uniform sampler2D uU;\n"
	"uniform sampler2D uV;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  float yy = texelFetch(uY, ivec2(x, yrow), 0).r;\n"
	"  float uu = texelFetch(uU, ivec2(x / 2, yrow / 2), 0).r;\n"
	"  float vv = texelFetch(uV, ivec2(x / 2, yrow / 2), 0).r;\n"
	"  fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *mjpeg_planar_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uY;\n"
	"uniform sampler2D uU;\n"
	"uniform sampler2D uV;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"uniform int uChromaWidth;\n"
	"uniform int uChromaHeight;\n"
	"uniform int uGray;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  float yy = texelFetch(uY, ivec2(x, yrow), 0).r;\n"
	"  if (uGray != 0) {\n"
	"    fragColor = vec4(yy, yy, yy, 1.0);\n"
	"    return;\n"
	"  }\n"
	"  int cx = clamp((x * uChromaWidth) / uWidth, 0, uChromaWidth - 1);\n"
	"  int cy = clamp((yrow * uChromaHeight) / uHeight, 0, uChromaHeight - 1);\n"
	"  float uu = texelFetch(uU, ivec2(cx, cy), 0).r;\n"
	"  float vv = texelFetch(uV, ivec2(cx, cy), 0).r;\n"
	"  fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"}\n";

/* Same as the planar shader, but each plane lives in an RGBA8 texture a
 * quarter as wide: byte x of a row is component (x & 3) of texel x >> 2. */
static const char *mjpeg_planar_packed_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uY;\n"
	"uniform sampler2D uU;\n"
	"uniform sampler2D uV;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"uniform int uChromaWidth;\n"
	"uniform int uChromaHeight;\n"
	"uniform int uGray;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	/* Samplers are fetched directly in main(), never passed as function
	 * arguments: old GLSL compilers (Adreno 30x) resolve every such call to the
	 * first sampler, which reads luma as chroma (green tint, 2x-wide ghosts). */
	"float pickByte(vec4 v, int x) {\n"
	"  int c = x & 3;\n"
	"  return c == 0 ? v.r : (c == 1 ? v.g : (c == 2 ? v.b : v.a));\n"
	"}\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  float yy = pickByte(texelFetch(uY, ivec2(x >> 2, yrow), 0), x);\n"
	"  if (uGray != 0) {\n"
	"    fragColor = vec4(yy, yy, yy, 1.0);\n"
	"    return;\n"
	"  }\n"
	"  int cx = clamp((x * uChromaWidth) / uWidth, 0, uChromaWidth - 1);\n"
	"  int cy = clamp((yrow * uChromaHeight) / uHeight, 0, uChromaHeight - 1);\n"
	"  float uu = pickByte(texelFetch(uU, ivec2(cx >> 2, cy), 0), cx);\n"
	"  float vv = pickByte(texelFetch(uV, ivec2(cx >> 2, cy), 0), cx);\n"
	"  fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *bgr_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D uBgr;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"  vec3 bgr = texture(uBgr, vTex).rgb;\n"
	"  fragColor = vec4(bgr.b, bgr.g, bgr.r, 1.0);\n"
	"}\n";

static const char *p010_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"precision highp usampler2D;\n"
	"uniform highp usampler2D uY16;\n"
	"uniform highp usampler2D uUV16;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int yrow = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  uint yv = texelFetch(uY16, ivec2(x, yrow), 0).r >> 8;\n"
	"  uvec2 uvv = texelFetch(uUV16, ivec2(x / 2, yrow / 2), 0).rg >> uvec2(8);\n"
	"  fragColor = vec4(clamp(yuvToRgb(float(yv) / 255.0, float(uvv.r) / 255.0, float(uvv.g) / 255.0), 0.0, 1.0), 1.0);\n"
	"}\n";

static const char *hardware_linear_fragment_shader_src =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform sampler2D uStorage;\n"
	"uniform int uWidth;\n"
	"uniform int uHeight;\n"
	"uniform int uStorageWidth;\n"
	"uniform int uFormat;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"float byteAt(int offset) {\n"
	"  int pix = offset / 4;\n"
	"  int comp = offset - pix * 4;\n"
	"  vec4 v = texelFetch(uStorage, ivec2(pix % uStorageWidth, pix / uStorageWidth), 0);\n"
	"  return comp == 0 ? v.r : (comp == 1 ? v.g : (comp == 2 ? v.b : v.a));\n"
	"}\n"
	"vec3 yuvToRgb(float y, float u, float v) {\n"
	"  u -= 0.5;\n"
	"  v -= 0.5;\n"
	"  return vec3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);\n"
	"}\n"
	"void main() {\n"
	"  int x = clamp(int(vTex.x * float(uWidth)), 0, uWidth - 1);\n"
	"  int y = clamp(int(vTex.y * float(uHeight)), 0, uHeight - 1);\n"
	"  float yy = 0.0;\n"
	"  float uu = 0.5;\n"
	"  float vv = 0.5;\n"
	"  if (uFormat == 3 || uFormat == 4) {\n"
	"    int pair = (y * uWidth + (x / 2) * 2) * 2;\n"
	"    float b0 = byteAt(pair + 0);\n"
	"    float b1 = byteAt(pair + 1);\n"
	"    float b2 = byteAt(pair + 2);\n"
	"    float b3 = byteAt(pair + 3);\n"
	"    if (uFormat == 4) { yy = ((x & 1) == 0 ? b1 : b3); uu = b0; vv = b2; }\n"
	"    else { yy = ((x & 1) == 0 ? b0 : b2); uu = b1; vv = b3; }\n"
	"    fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"  } else if (uFormat == 11) {\n"
	"    int yBytes = uWidth * uHeight;\n"
	"    yy = byteAt(y * uWidth + x);\n"
	"    int uv = yBytes + (y / 2) * uWidth + (x / 2) * 2;\n"
	"    uu = byteAt(uv + 0);\n"
	"    vv = byteAt(uv + 1);\n"
	"    fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"  } else if (uFormat == 12) {\n"
	"    int yBytes = uWidth * uHeight;\n"
	"    int cBytes = yBytes / 4;\n"
	"    yy = byteAt(y * uWidth + x);\n"
	"    uu = byteAt(yBytes + (y / 2) * (uWidth / 2) + x / 2);\n"
	"    vv = byteAt(yBytes + cBytes + (y / 2) * (uWidth / 2) + x / 2);\n"
	"    fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"  } else if (uFormat == 13) {\n"
	"    int yBytes = uWidth * uHeight * 2;\n"
	"    yy = byteAt((y * uWidth + x) * 2 + 1);\n"
	"    int uv = yBytes + ((y / 2) * uWidth + (x / 2) * 2) * 2;\n"
	"    uu = byteAt(uv + 1);\n"
	"    vv = byteAt(uv + 3);\n"
	"    fragColor = vec4(clamp(yuvToRgb(yy, uu, vv), 0.0, 1.0), 1.0);\n"
	"  } else if (uFormat == 7) {\n"
	"    int off = (y * uWidth + x) * 3;\n"
	"    float b = byteAt(off + 0);\n"
	"    float g = byteAt(off + 1);\n"
	"    float r = byteAt(off + 2);\n"
	"    fragColor = vec4(r, g, b, 1.0);\n"
	"  } else {\n"
	"    fragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"  }\n"
	"}\n";

enum ProgramKind {
	PROGRAM_YUYV = 0,
	PROGRAM_NV12,
	PROGRAM_YU12,
	PROGRAM_MJPEG_PLANAR,
	PROGRAM_BGR,
	PROGRAM_P010,
	PROGRAM_HARDWARE_LINEAR,
	PROGRAM_MJPEG_PLANAR_PACKED,
	PROGRAM_COUNT
};

/* Re-query the EGL surface size this often (frames); it only changes on a
 * geometry change, which also resets the surface in practice. */
static const uint32_t SURFACE_SIZE_REFRESH_FRAMES = 64;

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = {};
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		LOGW("gpu-preview: shader compile failed: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint link_program(const char *fragment_src)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = {};
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		LOGW("gpu-preview: program link failed: %s", log);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static size_t frame_actual_bytes(const uvc_frame_t *frame)
{
	return frame->actual_bytes ? frame->actual_bytes : frame->data_bytes;
}

} // namespace

struct UVCGpuPreviewRenderer::Impl {
	/* Uniform locations resolved once at link time (glGetUniformLocation is a
	 * string lookup in the driver; it was being done ~8x per frame). -1 = absent. */
	struct Uniforms {
		GLint tex0 = -1, tex1 = -1, tex2 = -1;
		GLint width = -1, height = -1;
		GLint chromaWidth = -1, chromaHeight = -1, gray = -1, uyvy = -1;
		GLint storageWidth = -1, format = -1;
		GLint xform = -1;
	};
	/* Identity until UVCPreview pushes rotation/flip/zoom/pan (column-major). */
	float xform[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 };

	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLConfig config = nullptr;
	ANativeWindow *window = nullptr;
	GLuint programs[PROGRAM_COUNT] = {};
	Uniforms uniforms[PROGRAM_COUNT];
	GLuint textures[3] = {};
	/* Storage spec currently allocated for each of the 3 texture units, so the
	 * steady state is one glTexSubImage2D per plane (no re-specification). */
	int texWidth[3] = {};
	int texHeight[3] = {};
	GLenum texInternal[3] = {};
	GLint texFilter[3] = {};
	GLuint hardwareTexture = 0;
	EGLImageKHR hardwareImage = EGL_NO_IMAGE_KHR;
	void *hardwareBuffer = nullptr;
	GLuint vbo = 0;
	GLuint vao = 0;
	int surface_width = 0;
	int surface_height = 0;
	uint32_t frames_since_size_query = 0;
	/* EGL/GLES extension entry points, resolved once per EGL init. */
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer = nullptr;
	PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;
	PFNEGLCREATESYNCKHRPROC pCreateSync = nullptr;
	PFNEGLDESTROYSYNCKHRPROC pDestroySync = nullptr;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC pDupNativeFenceFD = nullptr;

	/* EGLImage + texture per decoder plane buffer, keyed by the frame's
	 * allocation id (pointers get recycled; ids never do). */
	struct PlaneImage {
		uint64_t id = 0;
		EGLImageKHR image = EGL_NO_IMAGE_KHR;
		GLuint tex = 0;
	};
	static const int PLANE_IMAGE_CACHE = 16;
	PlaneImage planeImages[PLANE_IMAGE_CACHE];
	int planeImageCount = 0;
	int lastRenderFenceFd = -1;

	bool ensureEgl(ANativeWindow *target);
	bool ensureSurface(ANativeWindow *target);
	void destroySurface();
	void destroyGl();
	GLuint program(ProgramKind kind);
	void setupGeometry();
	void refreshSurfaceSize(bool force);
	void drawQuad(ProgramKind kind);
	bool drawHardwareBuffer(uvc_frame_t *frame);
	bool drawPlanarHardware(uvc_frame_t *frame);
	GLuint planeTextureFor(void *ahb, uint64_t id);
	void destroyPlaneImages();
	void captureRenderFence();
	bool uploadAndDraw(uvc_frame_t *frame);
	void resetTextureStorage();
	bool uploadTexture(int unit, GLenum internal, GLenum format, GLenum type,
		GLint filter, int width, int height, int stride_px, const void *data);
};

UVCGpuPreviewRenderer::UVCGpuPreviewRenderer()
	: impl(new Impl())
{
}

UVCGpuPreviewRenderer::~UVCGpuPreviewRenderer()
{
	shutdown();
	delete impl;
	impl = nullptr;
}

bool UVCGpuPreviewRenderer::render(uvc_frame_t *frame, ANativeWindow *window,
	uint64_t *frame_ready_ns)
{
	if (!impl || !frame || !window)
		return false;
	if (!frame->data && !frame->yuv_hardware_buffers[0])
		return false;
	if (!impl->ensureEgl(window) || !impl->ensureSurface(window))
		return false;
	if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR
			&& frame->yuv_hardware_buffers[0]) {
		/* Zero-copy path: planes are already in GPU-sampleable memory. */
		if (!impl->drawPlanarHardware(frame))
			return false;
		const uint64_t ready_ns = now_ns();
		if (eglSwapBuffers(impl->display, impl->surface) != EGL_TRUE) {
			LOGW("gpu-preview: eglSwapBuffers failed err=0x%x", eglGetError());
			impl->destroySurface();
			return false;
		}
		impl->captureRenderFence();
		if (frame_ready_ns)
			*frame_ready_ns = ready_ns;
		return true;
	}
	if (frame->library_hardware_buffer) {
		if (impl->drawHardwareBuffer(frame)) {
			const uint64_t ready_ns = now_ns();
			if (eglSwapBuffers(impl->display, impl->surface) != EGL_TRUE) {
				LOGW("gpu-preview: eglSwapBuffers failed err=0x%x", eglGetError());
				impl->destroySurface();
				return false;
			}
			if (frame_ready_ns)
				*frame_ready_ns = ready_ns;
			return true;
		}
		if (!impl->ensureSurface(window))
			return false;
	}
	if (!impl->uploadAndDraw(frame)) {
		/* Drop the EGLSurface before CPU fallback tries ANativeWindow_lock. */
		impl->destroySurface();
		return false;
	}
	const uint64_t ready_ns = now_ns();
	if (eglSwapBuffers(impl->display, impl->surface) != EGL_TRUE) {
		LOGW("gpu-preview: eglSwapBuffers failed err=0x%x", eglGetError());
		impl->destroySurface();
		return false;
	}
	if (frame_ready_ns)
		*frame_ready_ns = ready_ns;
	return true;
}

int UVCGpuPreviewRenderer::takeRenderFenceFd()
{
	if (!impl)
		return -1;
	const int fd = impl->lastRenderFenceFd;
	impl->lastRenderFenceFd = -1;
	return fd;
}

void UVCGpuPreviewRenderer::setTransform(const float m[9])
{
	if (impl && m)
		memcpy(impl->xform, m, sizeof(impl->xform));
}

void UVCGpuPreviewRenderer::invalidateSurfaceSize()
{
	if (impl)
		impl->frames_since_size_query = SURFACE_SIZE_REFRESH_FRAMES;
}

void UVCGpuPreviewRenderer::resetSurface()
{
	if (impl)
		impl->destroySurface();
}

void UVCGpuPreviewRenderer::shutdown()
{
	if (!impl)
		return;
	impl->destroySurface();
	impl->destroyGl();
	if (impl->display != EGL_NO_DISPLAY) {
		eglTerminate(impl->display);
		impl->display = EGL_NO_DISPLAY;
	}
}

bool UVCGpuPreviewRenderer::Impl::ensureEgl(ANativeWindow *target)
{
	if (display != EGL_NO_DISPLAY)
		return true;

	display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (display == EGL_NO_DISPLAY)
		return false;
	if (eglInitialize(display, NULL, NULL) != EGL_TRUE) {
		LOGW("gpu-preview: eglInitialize failed err=0x%x", eglGetError());
		display = EGL_NO_DISPLAY;
		return false;
	}

	const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint count = 0;
	if (eglChooseConfig(display, config_attribs, &config, 1, &count) != EGL_TRUE || count < 1) {
		LOGW("gpu-preview: eglChooseConfig failed err=0x%x", eglGetError());
		eglTerminate(display);
		display = EGL_NO_DISPLAY;
		return false;
	}

	const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE
	};
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
	if (context == EGL_NO_CONTEXT) {
		LOGW("gpu-preview: eglCreateContext failed err=0x%x", eglGetError());
		eglTerminate(display);
		display = EGL_NO_DISPLAY;
		return false;
	}

	pGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
		eglGetProcAddress("eglGetNativeClientBufferANDROID");
	pCreateImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	pDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	pImageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
		eglGetProcAddress("glEGLImageTargetTexture2DOES");
	pCreateSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
	pDestroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
	pDupNativeFenceFD = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
		eglGetProcAddress("eglDupNativeFenceFDANDROID");

	(void)target;
	return true;
}

bool UVCGpuPreviewRenderer::Impl::ensureSurface(ANativeWindow *target)
{
	if (surface != EGL_NO_SURFACE && window == target)
		return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE;

	destroySurface();
	surface = eglCreateWindowSurface(display, config, target, NULL);
	if (surface == EGL_NO_SURFACE) {
		LOGW("gpu-preview: eglCreateWindowSurface failed err=0x%x", eglGetError());
		return false;
	}
	window = target;
	if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
		LOGW("gpu-preview: eglMakeCurrent failed err=0x%x", eglGetError());
		destroySurface();
		return false;
	}
	eglSwapInterval(display, 0);
	glGenTextures(3, textures);
	resetTextureStorage();
	setupGeometry();
	refreshSurfaceSize(true);
	return true;
}

void UVCGpuPreviewRenderer::Impl::destroySurface()
{
	if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE
			&& context != EGL_NO_CONTEXT)
		eglMakeCurrent(display, surface, surface, context);
	if (textures[0] || textures[1] || textures[2]) {
		glDeleteTextures(3, textures);
		memset(textures, 0, sizeof(textures));
		resetTextureStorage();
	}
	if (hardwareImage != EGL_NO_IMAGE_KHR) {
		if (pDestroyImage)
			pDestroyImage(display, hardwareImage);
		hardwareImage = EGL_NO_IMAGE_KHR;
		hardwareBuffer = nullptr;
	}
	if (hardwareTexture) {
		glDeleteTextures(1, &hardwareTexture);
		hardwareTexture = 0;
	}
	destroyPlaneImages();
	if (lastRenderFenceFd >= 0) {
		close(lastRenderFenceFd);
		lastRenderFenceFd = -1;
	}
	if (vao) {
		glDeleteVertexArrays(1, &vao);
		vao = 0;
	}
	if (vbo) {
		glDeleteBuffers(1, &vbo);
		vbo = 0;
	}
	if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE)
		eglDestroySurface(display, surface);
	if (display != EGL_NO_DISPLAY)
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	surface = EGL_NO_SURFACE;
	window = nullptr;
	surface_width = 0;
	surface_height = 0;
}

void UVCGpuPreviewRenderer::Impl::destroyGl()
{
	if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT)
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
	for (int i = 0; i < PROGRAM_COUNT; ++i) {
		if (programs[i]) {
			glDeleteProgram(programs[i]);
			programs[i] = 0;
		}
		uniforms[i] = Uniforms();
	}
	if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT) {
		eglDestroyContext(display, context);
		context = EGL_NO_CONTEXT;
	}
	config = nullptr;
	pGetNativeClientBuffer = nullptr;
	pCreateImage = nullptr;
	pDestroyImage = nullptr;
	pImageTargetTexture = nullptr;
}

GLuint UVCGpuPreviewRenderer::Impl::program(ProgramKind kind)
{
	if (programs[kind])
		return programs[kind];

	const char *src = nullptr;
	switch (kind) {
	case PROGRAM_YUYV:
		src = yuyv_fragment_shader_src;
		break;
	case PROGRAM_NV12:
		src = nv12_fragment_shader_src;
		break;
	case PROGRAM_YU12:
		src = yu12_fragment_shader_src;
		break;
	case PROGRAM_MJPEG_PLANAR:
		src = mjpeg_planar_fragment_shader_src;
		break;
	case PROGRAM_BGR:
		src = bgr_fragment_shader_src;
		break;
	case PROGRAM_P010:
		src = p010_fragment_shader_src;
		break;
	case PROGRAM_HARDWARE_LINEAR:
		src = hardware_linear_fragment_shader_src;
		break;
	case PROGRAM_MJPEG_PLANAR_PACKED:
		src = mjpeg_planar_packed_fragment_shader_src;
		break;
	default:
		return 0;
	}
	const GLuint prog = link_program(src);
	programs[kind] = prog;
	if (!prog)
		return 0;

	/* Resolve every uniform this program might declare; absent ones are -1 and
	 * glUniform1i(-1, ...) is a defined no-op.  Sampler bindings never change,
	 * so set them once here. */
	Uniforms &u = uniforms[kind];
	static const char *tex0_names[PROGRAM_COUNT] = {
		"uPacked", "uY", "uY", "uY", "uBgr", "uY16", "uStorage", "uY" };
	static const char *tex1_names[PROGRAM_COUNT] = {
		nullptr, "uUV", "uU", "uU", nullptr, "uUV16", nullptr, "uU" };
	static const char *tex2_names[PROGRAM_COUNT] = {
		nullptr, nullptr, "uV", "uV", nullptr, nullptr, nullptr, "uV" };
	u.tex0 = tex0_names[kind] ? glGetUniformLocation(prog, tex0_names[kind]) : -1;
	u.tex1 = tex1_names[kind] ? glGetUniformLocation(prog, tex1_names[kind]) : -1;
	u.tex2 = tex2_names[kind] ? glGetUniformLocation(prog, tex2_names[kind]) : -1;
	u.width = glGetUniformLocation(prog, "uWidth");
	u.height = glGetUniformLocation(prog, "uHeight");
	u.chromaWidth = glGetUniformLocation(prog, "uChromaWidth");
	u.chromaHeight = glGetUniformLocation(prog, "uChromaHeight");
	u.gray = glGetUniformLocation(prog, "uGray");
	u.uyvy = glGetUniformLocation(prog, "uUyvy");
	u.storageWidth = glGetUniformLocation(prog, "uStorageWidth");
	u.format = glGetUniformLocation(prog, "uFormat");
	u.xform = glGetUniformLocation(prog, "uXform");
	glUseProgram(prog);
	glUniform1i(u.tex0, 0);
	glUniform1i(u.tex1, 1);
	glUniform1i(u.tex2, 2);
	return prog;
}

void UVCGpuPreviewRenderer::Impl::setupGeometry()
{
	if (!vbo)
		glGenBuffers(1, &vbo);
	if (!vao)
		glGenVertexArrays(1, &vao);
	static const GLfloat vertices[] = {
		-1.0f,  1.0f, 0.0f, 0.0f,
		-1.0f, -1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 1.0f,
	};
	/* Vertex layout is captured in the VAO once; per frame it is one bind. */
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
		(const void *)(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
}

void UVCGpuPreviewRenderer::Impl::refreshSurfaceSize(bool force)
{
	if (!force && ++frames_since_size_query < SURFACE_SIZE_REFRESH_FRAMES)
		return;
	frames_since_size_query = 0;
	EGLint sw = 0;
	EGLint sh = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &sw);
	eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
	surface_width = sw;
	surface_height = sh;
}

void UVCGpuPreviewRenderer::Impl::drawQuad(ProgramKind kind)
{
	refreshSurfaceSize(false);
	glViewport(0, 0, surface_width, surface_height);
	/* A zoom scale below 1 (zoomed out / 1:1 on a small stream) leaves part of the
	 * surface uncovered by the quad; clear it so the borders are black. */
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUniformMatrix3fv(uniforms[kind].xform, 1, GL_FALSE, xform);
	/* Re-issue the sampler bindings every draw: some old drivers drop the
	 * values set at link time, leaving every sampler on unit 0 (the Y plane).
	 * Absent samplers have location -1, which is a defined no-op. */
	glUniform1i(uniforms[kind].tex0, 0);
	glUniform1i(uniforms[kind].tex1, 1);
	glUniform1i(uniforms[kind].tex2, 2);
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
}

bool UVCGpuPreviewRenderer::Impl::drawHardwareBuffer(uvc_frame_t *frame)
{
	if (!pGetNativeClientBuffer || !pCreateImage || !pImageTargetTexture)
		return false;

	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe((AHardwareBuffer *)frame->library_hardware_buffer,
		&desc);
	if (desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
			|| desc.stride != desc.width)
		return false;

	if (uvc_frame_hardware_buffer_unlock(frame) != UVC_SUCCESS)
		return false;

	bool ok = false;
	do {
		if (hardwareBuffer != frame->library_hardware_buffer) {
			if (hardwareImage != EGL_NO_IMAGE_KHR) {
				if (pDestroyImage)
					pDestroyImage(display, hardwareImage);
				hardwareImage = EGL_NO_IMAGE_KHR;
			}
			EGLClientBuffer clientBuffer = pGetNativeClientBuffer(
				(AHardwareBuffer *)frame->library_hardware_buffer);
			if (!clientBuffer)
				break;
			hardwareImage = pCreateImage(display, EGL_NO_CONTEXT,
				EGL_NATIVE_BUFFER_ANDROID, clientBuffer, NULL);
			if (hardwareImage == EGL_NO_IMAGE_KHR) {
				LOGW("gpu-preview: eglCreateImageKHR(AHB) failed err=0x%x",
					eglGetError());
				break;
			}
			hardwareBuffer = frame->library_hardware_buffer;
		}

		if (!hardwareTexture) {
			glGenTextures(1, &hardwareTexture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, hardwareTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		} else {
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, hardwareTexture);
		}
		pImageTargetTexture(GL_TEXTURE_2D, hardwareImage);

		GLuint prog = program(PROGRAM_HARDWARE_LINEAR);
		if (!prog)
			break;
		const Uniforms &u = uniforms[PROGRAM_HARDWARE_LINEAR];
		glUseProgram(prog);
		glUniform1i(u.width, (int)frame->width);
		glUniform1i(u.height, (int)frame->height);
		glUniform1i(u.storageWidth, (int)desc.width);
		glUniform1i(u.format, (int)frame->frame_format);

		drawQuad(PROGRAM_HARDWARE_LINEAR);

		const GLenum err = glGetError();
		if (err != GL_NO_ERROR) {
			LOGW("gpu-preview: AHardwareBuffer draw failed glerr=0x%x", err);
			break;
		}
		ok = true;
	} while (0);

	if (uvc_frame_hardware_buffer_lock(frame) != UVC_SUCCESS) {
		LOGW("gpu-preview: failed to relock AHardwareBuffer frame");
		ok = false;
	}
	if (!ok)
		destroySurface();
	return ok;
}

void UVCGpuPreviewRenderer::Impl::destroyPlaneImages()
{
	for (int i = 0; i < planeImageCount; i++) {
		if (planeImages[i].tex)
			glDeleteTextures(1, &planeImages[i].tex);
		if (planeImages[i].image != EGL_NO_IMAGE_KHR && pDestroyImage)
			pDestroyImage(display, planeImages[i].image);
		planeImages[i] = PlaneImage();
	}
	planeImageCount = 0;
}

GLuint UVCGpuPreviewRenderer::Impl::planeTextureFor(void *ahb, uint64_t id)
{
	for (int i = 0; i < planeImageCount; i++)
		if (planeImages[i].id == id)
			return planeImages[i].tex;
	if (!pGetNativeClientBuffer || !pCreateImage || !pImageTargetTexture)
		return 0;
	if (planeImageCount == PLANE_IMAGE_CACHE) {
		/* Evict the oldest entry; the decoder pool is far smaller than this. */
		if (planeImages[0].tex)
			glDeleteTextures(1, &planeImages[0].tex);
		if (planeImages[0].image != EGL_NO_IMAGE_KHR && pDestroyImage)
			pDestroyImage(display, planeImages[0].image);
		memmove(&planeImages[0], &planeImages[1],
			sizeof(PlaneImage) * (PLANE_IMAGE_CACHE - 1));
		planeImages[PLANE_IMAGE_CACHE - 1] = PlaneImage();
		planeImageCount--;
	}
	EGLClientBuffer clientBuffer = pGetNativeClientBuffer((AHardwareBuffer *)ahb);
	if (!clientBuffer)
		return 0;
	EGLImageKHR image = pCreateImage(display, EGL_NO_CONTEXT,
		EGL_NATIVE_BUFFER_ANDROID, clientBuffer, NULL);
	if (image == EGL_NO_IMAGE_KHR) {
		LOGW("gpu-preview: eglCreateImageKHR(plane AHB) failed err=0x%x", eglGetError());
		return 0;
	}
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	pImageTargetTexture(GL_TEXTURE_2D, image);
	if (glGetError() != GL_NO_ERROR) {
		glDeleteTextures(1, &tex);
		if (pDestroyImage)
			pDestroyImage(display, image);
		LOGW("gpu-preview: glEGLImageTargetTexture2DOES(plane) failed");
		return 0;
	}
	PlaneImage &e = planeImages[planeImageCount++];
	e.id = id;
	e.image = image;
	e.tex = tex;
	return tex;
}

/* Record a native fence for the commands just issued so the decoder can wait
 * for the GPU to finish reading these planes before overwriting them. */
void UVCGpuPreviewRenderer::Impl::captureRenderFence()
{
	if (lastRenderFenceFd >= 0) {
		close(lastRenderFenceFd);
		lastRenderFenceFd = -1;
	}
	if (!pCreateSync || !pDupNativeFenceFD || !pDestroySync) {
		glFinish();	/* no native fences: fall back to a full GPU wait */
		return;
	}
	EGLSyncKHR sync = pCreateSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
	if (sync == EGL_NO_SYNC_KHR) {
		glFinish();
		return;
	}
	glFlush();	/* the fence fd only becomes valid once the sync is flushed */
	lastRenderFenceFd = pDupNativeFenceFD(display, sync);
	pDestroySync(display, sync);
	if (lastRenderFenceFd < 0) {
		lastRenderFenceFd = -1;
		glFinish();
	}
}

bool UVCGpuPreviewRenderer::Impl::drawPlanarHardware(uvc_frame_t *frame)
{
	const bool gray = frame->yuv_hardware_buffers[1] == nullptr;
	const int width = (int)frame->width;
	const int height = (int)frame->height;
	if (width <= 0 || height <= 0)
		return false;
	const ProgramKind kind = frame->yuv_hardware_buffer_bytes_per_texel == 4
		? PROGRAM_MJPEG_PLANAR_PACKED : PROGRAM_MJPEG_PLANAR;
	GLuint prog = program(kind);
	if (!prog)
		return false;
	for (int i = 0; i < (gray ? 1 : 3); i++) {
		GLuint tex = planeTextureFor(frame->yuv_hardware_buffers[i],
			frame->yuv_hardware_buffer_ids[i]);
		if (!tex)
			return false;
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, tex);
	}
	const Uniforms &u = uniforms[kind];
	glUseProgram(prog);
	glUniform1i(u.width, width);
	glUniform1i(u.height, height);
	glUniform1i(u.chromaWidth, gray ? 1 : (int)frame->yuv_plane_widths[1]);
	glUniform1i(u.chromaHeight, gray ? 1 : (int)frame->yuv_plane_heights[1]);
	glUniform1i(u.gray, gray ? 1 : 0);
	drawQuad(kind);
	const GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		LOGW("gpu-preview: planar AHB draw failed glerr=0x%x", err);
		return false;
	}
	return true;
}

void UVCGpuPreviewRenderer::Impl::resetTextureStorage()
{
	memset(texWidth, 0, sizeof(texWidth));
	memset(texHeight, 0, sizeof(texHeight));
	memset(texInternal, 0, sizeof(texInternal));
	memset(texFilter, 0, sizeof(texFilter));
}

/* Upload one plane into texture unit `unit`.  Storage is (re)specified with
 * glTexImage2D only when the size, internal format, or filter changes; the
 * steady state is a single glTexSubImage2D, which lets the driver reuse the
 * existing allocation instead of orphaning it every frame. */
bool UVCGpuPreviewRenderer::Impl::uploadTexture(int unit, GLenum internal,
	GLenum format, GLenum type, GLint filter, int width, int height,
	int stride_px, const void *data)
{
	if (unit < 0 || unit >= 3 || width <= 0 || height <= 0 || !data)
		return false;
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, textures[unit]);
	if (texWidth[unit] != width || texHeight[unit] != height
			|| texInternal[unit] != internal || texFilter[unit] != filter) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, width, height, 0, format,
			type, NULL);
		if (glGetError() != GL_NO_ERROR) {
			texWidth[unit] = 0;
			texHeight[unit] = 0;
			texInternal[unit] = 0;
			texFilter[unit] = 0;
			return false;
		}
		texWidth[unit] = width;
		texHeight[unit] = height;
		texInternal[unit] = internal;
		texFilter[unit] = filter;
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride_px > width ? stride_px : 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadAndDraw(uvc_frame_t *frame)
{
	const int width = (int)frame->width;
	const int height = (int)frame->height;
	if (width <= 0 || height <= 0)
		return false;

	GLuint prog = 0;
	ProgramKind kind = PROGRAM_COUNT;
	const uint8_t *data = (const uint8_t *)frame->data;
	const size_t actual = frame_actual_bytes(frame);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	switch (frame->frame_format) {
	case UVC_FRAME_FORMAT_YUYV:
	case UVC_FRAME_FORMAT_UYVY: {
		const size_t need = (size_t)width * (size_t)height * 2u;
		if (actual < need)
			return false;
		kind = PROGRAM_YUYV;
		prog = program(kind);
		if (!prog || !uploadTexture(0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				width * 2, height, 0, data))
			return false;
		glUseProgram(prog);
		glUniform1i(uniforms[kind].width, width);
		glUniform1i(uniforms[kind].height, height);
		glUniform1i(uniforms[kind].uyvy,
			frame->frame_format == UVC_FRAME_FORMAT_UYVY ? 1 : 0);
		break;
	}
	case UVC_FRAME_FORMAT_NV12: {
		const size_t y_bytes = (size_t)width * (size_t)height;
		const size_t need = y_bytes + y_bytes / 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		kind = PROGRAM_NV12;
		prog = program(kind);
		if (!prog
			|| !uploadTexture(0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				width, height, 0, data)
			|| !uploadTexture(1, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, GL_NEAREST,
				width / 2, height / 2, 0, data + y_bytes))
			return false;
		glUseProgram(prog);
		glUniform1i(uniforms[kind].width, width);
		glUniform1i(uniforms[kind].height, height);
		break;
	}
	case UVC_FRAME_FORMAT_YU12: {
		const size_t y_bytes = (size_t)width * (size_t)height;
		const size_t chroma_bytes = y_bytes / 4u;
		const size_t need = y_bytes + chroma_bytes * 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		kind = PROGRAM_YU12;
		prog = program(kind);
		if (!prog
			|| !uploadTexture(0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				width, height, 0, data)
			|| !uploadTexture(1, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				width / 2, height / 2, 0, data + y_bytes)
			|| !uploadTexture(2, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				width / 2, height / 2, 0, data + y_bytes + chroma_bytes))
			return false;
		glUseProgram(prog);
		glUniform1i(uniforms[kind].width, width);
		glUniform1i(uniforms[kind].height, height);
		break;
	}
	case UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR: {
		const bool gray = frame->yuv_plane_widths[1] == 0
			|| frame->yuv_plane_heights[1] == 0;
		if (actual < frame->actual_bytes || !frame->yuv_plane_widths[0]
				|| !frame->yuv_plane_heights[0])
			return false;
		kind = PROGRAM_MJPEG_PLANAR;
		prog = program(kind);
		if (!prog
			|| !uploadTexture(0, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
				(int)frame->yuv_plane_widths[0],
				(int)frame->yuv_plane_heights[0],
				(int)frame->yuv_plane_strides[0],
				data + frame->yuv_plane_offsets[0]))
			return false;
		if (!gray) {
			if (!uploadTexture(1, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
					(int)frame->yuv_plane_widths[1],
					(int)frame->yuv_plane_heights[1],
					(int)frame->yuv_plane_strides[1],
					data + frame->yuv_plane_offsets[1])
					|| !uploadTexture(2, GL_R8, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST,
					(int)frame->yuv_plane_widths[2],
					(int)frame->yuv_plane_heights[2],
					(int)frame->yuv_plane_strides[2],
					data + frame->yuv_plane_offsets[2]))
				return false;
		}
		glUseProgram(prog);
		glUniform1i(uniforms[kind].width, width);
		glUniform1i(uniforms[kind].height, height);
		glUniform1i(uniforms[kind].chromaWidth,
			gray ? 1 : (int)frame->yuv_plane_widths[1]);
		glUniform1i(uniforms[kind].chromaHeight,
			gray ? 1 : (int)frame->yuv_plane_heights[1]);
		glUniform1i(uniforms[kind].gray, gray ? 1 : 0);
		break;
	}
	case UVC_FRAME_FORMAT_BGR: {
		const size_t need = (size_t)width * (size_t)height * 3u;
		if (actual < need)
			return false;
		kind = PROGRAM_BGR;
		prog = program(kind);
		if (!prog || !uploadTexture(0, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, GL_LINEAR,
				width, height, 0, data))
			return false;
		glUseProgram(prog);
		break;
	}
	case UVC_FRAME_FORMAT_P010: {
		const size_t y_bytes = (size_t)width * (size_t)height * 2u;
		const size_t need = y_bytes + y_bytes / 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		kind = PROGRAM_P010;
		prog = program(kind);
		if (!prog
			|| !uploadTexture(0, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST,
				width, height, 0, data)
			|| !uploadTexture(1, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, GL_NEAREST,
				width / 2, height / 2, 0, data + y_bytes))
			return false;
		glUseProgram(prog);
		glUniform1i(uniforms[kind].width, width);
		glUniform1i(uniforms[kind].height, height);
		break;
	}
	default:
		return false;
	}

	drawQuad(kind);

	const GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		LOGW("gpu-preview: draw failed glerr=0x%x", err);
		return false;
	}
	return true;
}
