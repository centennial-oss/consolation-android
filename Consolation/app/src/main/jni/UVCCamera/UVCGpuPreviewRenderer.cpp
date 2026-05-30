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
	"out vec2 vTex;\n"
	"void main() {\n"
	"  vTex = aTex;\n"
	"  gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"}\n";

static const char *yuyv_fragment_shader_src =
	"#version 300 es\n"
	"precision mediump float;\n"
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
	"precision mediump float;\n"
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
	"precision mediump float;\n"
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
	"precision mediump float;\n"
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

static const char *bgr_fragment_shader_src =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform sampler2D uBgr;\n"
	"in vec2 vTex;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"  vec3 bgr = texture(uBgr, vTex).rgb;\n"
	"  fragColor = vec4(bgr.b, bgr.g, bgr.r, 1.0);\n"
	"}\n";

static const char *p010_fragment_shader_src =
	"#version 300 es\n"
	"precision mediump float;\n"
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
	"precision mediump float;\n"
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
	PROGRAM_COUNT
};

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
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface surface = EGL_NO_SURFACE;
	EGLConfig config = nullptr;
	ANativeWindow *window = nullptr;
	GLuint programs[PROGRAM_COUNT] = {};
	GLuint textures[3] = {};
	int mjpegTextureWidths[3] = {};
	int mjpegTextureHeights[3] = {};
	GLuint hardwareTexture = 0;
	EGLImageKHR hardwareImage = EGL_NO_IMAGE_KHR;
	void *hardwareBuffer = nullptr;
	GLuint vbo = 0;
	int surface_width = 0;
	int surface_height = 0;

	bool ensureEgl(ANativeWindow *target);
	bool ensureSurface(ANativeWindow *target);
	void destroySurface();
	void destroyGl();
	GLuint program(ProgramKind kind);
	void setupGeometry();
	bool drawHardwareBuffer(uvc_frame_t *frame);
	bool uploadAndDraw(uvc_frame_t *frame);
	void resetMjpegTextureStorage();
	bool uploadR8(GLuint tex, int width, int height, const void *data);
	bool uploadR8Stride(GLuint tex, int width, int height, int stride,
		const void *data);
	bool uploadMjpegPlane(int plane, int width, int height, int stride,
		const void *data);
	bool uploadRG8(GLuint tex, int width, int height, const void *data);
	bool uploadRGB8(GLuint tex, int width, int height, const void *data);
	bool uploadR16UI(GLuint tex, int width, int height, const void *data);
	bool uploadRG16UI(GLuint tex, int width, int height, const void *data);
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
	if (!impl || !frame || !window || !frame->data)
		return false;
	if (!impl->ensureEgl(window) || !impl->ensureSurface(window))
		return false;
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
	setupGeometry();
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
		resetMjpegTextureStorage();
	}
	if (hardwareImage != EGL_NO_IMAGE_KHR) {
		PFNEGLDESTROYIMAGEKHRPROC destroyImage =
			(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		if (destroyImage)
			destroyImage(display, hardwareImage);
		hardwareImage = EGL_NO_IMAGE_KHR;
		hardwareBuffer = nullptr;
	}
	if (hardwareTexture) {
		glDeleteTextures(1, &hardwareTexture);
		hardwareTexture = 0;
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
	}
	if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT) {
		eglDestroyContext(display, context);
		context = EGL_NO_CONTEXT;
	}
	config = nullptr;
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
	default:
		return 0;
	}
	programs[kind] = link_program(src);
	return programs[kind];
}

void UVCGpuPreviewRenderer::Impl::setupGeometry()
{
	if (!vbo)
		glGenBuffers(1, &vbo);
	static const GLfloat vertices[] = {
		-1.0f,  1.0f, 0.0f, 0.0f,
		-1.0f, -1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 1.0f,
	};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
}

bool UVCGpuPreviewRenderer::Impl::drawHardwareBuffer(uvc_frame_t *frame)
{
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer =
		(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
			"eglGetNativeClientBufferANDROID");
	PFNEGLCREATEIMAGEKHRPROC createImage =
		(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
			"glEGLImageTargetTexture2DOES");
	if (!getNativeClientBuffer || !createImage || !imageTargetTexture)
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
				PFNEGLDESTROYIMAGEKHRPROC destroyImage =
					(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
				if (destroyImage)
					destroyImage(display, hardwareImage);
				hardwareImage = EGL_NO_IMAGE_KHR;
			}
			EGLClientBuffer clientBuffer = getNativeClientBuffer(
				(AHardwareBuffer *)frame->library_hardware_buffer);
			if (!clientBuffer)
				break;
			hardwareImage = createImage(display, EGL_NO_CONTEXT,
				EGL_NATIVE_BUFFER_ANDROID, clientBuffer, NULL);
			if (hardwareImage == EGL_NO_IMAGE_KHR) {
				LOGW("gpu-preview: eglCreateImageKHR(AHB) failed err=0x%x",
					eglGetError());
				break;
			}
			hardwareBuffer = frame->library_hardware_buffer;
		}

		if (!hardwareTexture)
			glGenTextures(1, &hardwareTexture);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hardwareTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		imageTargetTexture(GL_TEXTURE_2D, hardwareImage);

		GLuint prog = program(PROGRAM_HARDWARE_LINEAR);
		if (!prog)
			break;
		glUseProgram(prog);
		glUniform1i(glGetUniformLocation(prog, "uStorage"), 0);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), (int)frame->width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), (int)frame->height);
		glUniform1i(glGetUniformLocation(prog, "uStorageWidth"), (int)desc.width);
		glUniform1i(glGetUniformLocation(prog, "uFormat"), (int)frame->frame_format);

		EGLint sw = 0;
		EGLint sh = 0;
		eglQuerySurface(display, surface, EGL_WIDTH, &sw);
		eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
		glViewport(0, 0, sw, sh);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			(const void *)0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
			(const void *)(2 * sizeof(GLfloat)));
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);

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

void UVCGpuPreviewRenderer::Impl::resetMjpegTextureStorage()
{
	memset(mjpegTextureWidths, 0, sizeof(mjpegTextureWidths));
	memset(mjpegTextureHeights, 0, sizeof(mjpegTextureHeights));
}

bool UVCGpuPreviewRenderer::Impl::uploadR8(GLuint tex, int width, int height,
	const void *data)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
		GL_UNSIGNED_BYTE, data);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadR8Stride(GLuint tex, int width, int height,
	int stride, const void *data)
{
	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride > width ? stride : 0);
	const bool ok = uploadR8(tex, width, height, data);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	return ok;
}

bool UVCGpuPreviewRenderer::Impl::uploadMjpegPlane(int plane, int width,
	int height, int stride, const void *data)
{
	if (plane < 0 || plane >= 3 || width <= 0 || height <= 0 || !data)
		return false;

	GLuint tex = textures[plane];
	glBindTexture(GL_TEXTURE_2D, tex);
	if (mjpegTextureWidths[plane] != width || mjpegTextureHeights[plane] != height) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
			GL_UNSIGNED_BYTE, NULL);
		if (glGetError() != GL_NO_ERROR) {
			mjpegTextureWidths[plane] = 0;
			mjpegTextureHeights[plane] = 0;
			return false;
		}
		mjpegTextureWidths[plane] = width;
		mjpegTextureHeights[plane] = height;
	}

	glPixelStorei(GL_UNPACK_ROW_LENGTH, stride > width ? stride : 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED,
		GL_UNSIGNED_BYTE, data);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadRG8(GLuint tex, int width, int height,
	const void *data)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width, height, 0, GL_RG,
		GL_UNSIGNED_BYTE, data);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadRGB8(GLuint tex, int width, int height,
	const void *data)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB,
		GL_UNSIGNED_BYTE, data);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadR16UI(GLuint tex, int width, int height,
	const void *data)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, width, height, 0, GL_RED_INTEGER,
		GL_UNSIGNED_SHORT, data);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadRG16UI(GLuint tex, int width, int height,
	const void *data)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16UI, width, height, 0, GL_RG_INTEGER,
		GL_UNSIGNED_SHORT, data);
	return glGetError() == GL_NO_ERROR;
}

bool UVCGpuPreviewRenderer::Impl::uploadAndDraw(uvc_frame_t *frame)
{
	const int width = (int)frame->width;
	const int height = (int)frame->height;
	if (width <= 0 || height <= 0)
		return false;
	if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR)
		resetMjpegTextureStorage();

	GLuint prog = 0;
	const uint8_t *data = (const uint8_t *)frame->data;
	const size_t actual = frame_actual_bytes(frame);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	switch (frame->frame_format) {
	case UVC_FRAME_FORMAT_YUYV:
	case UVC_FRAME_FORMAT_UYVY: {
		const size_t need = (size_t)width * (size_t)height * 2u;
		if (actual < need)
			return false;
		prog = program(PROGRAM_YUYV);
		if (!prog || !uploadR8(textures[0], width * 2, height, data))
			return false;
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uPacked"), 0);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), height);
		glUniform1i(glGetUniformLocation(prog, "uUyvy"),
			frame->frame_format == UVC_FRAME_FORMAT_UYVY ? 1 : 0);
		break;
	}
	case UVC_FRAME_FORMAT_NV12: {
		const size_t y_bytes = (size_t)width * (size_t)height;
		const size_t need = y_bytes + y_bytes / 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		prog = program(PROGRAM_NV12);
		if (!prog
			|| !uploadR8(textures[0], width, height, data)
			|| !uploadRG8(textures[1], width / 2, height / 2, data + y_bytes))
			return false;
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uY"), 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[1]);
		glUniform1i(glGetUniformLocation(prog, "uUV"), 1);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), height);
		break;
	}
	case UVC_FRAME_FORMAT_YU12: {
		const size_t y_bytes = (size_t)width * (size_t)height;
		const size_t chroma_bytes = y_bytes / 4u;
		const size_t need = y_bytes + chroma_bytes * 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		prog = program(PROGRAM_YU12);
		if (!prog
			|| !uploadR8(textures[0], width, height, data)
			|| !uploadR8(textures[1], width / 2, height / 2, data + y_bytes)
			|| !uploadR8(textures[2], width / 2, height / 2,
				data + y_bytes + chroma_bytes))
			return false;
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uY"), 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[1]);
		glUniform1i(glGetUniformLocation(prog, "uU"), 1);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, textures[2]);
		glUniform1i(glGetUniformLocation(prog, "uV"), 2);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), height);
		break;
	}
	case UVC_FRAME_FORMAT_MJPEG_YUV_PLANAR: {
		const bool gray = frame->yuv_plane_widths[1] == 0
			|| frame->yuv_plane_heights[1] == 0;
		if (actual < frame->actual_bytes || !frame->yuv_plane_widths[0]
				|| !frame->yuv_plane_heights[0])
			return false;
		prog = program(PROGRAM_MJPEG_PLANAR);
		if (!prog
			|| !uploadMjpegPlane(0,
				(int)frame->yuv_plane_widths[0],
				(int)frame->yuv_plane_heights[0],
				(int)frame->yuv_plane_strides[0],
				data + frame->yuv_plane_offsets[0]))
			return false;
		if (!gray) {
			if (!uploadMjpegPlane(1,
					(int)frame->yuv_plane_widths[1],
					(int)frame->yuv_plane_heights[1],
					(int)frame->yuv_plane_strides[1],
					data + frame->yuv_plane_offsets[1])
					|| !uploadMjpegPlane(2,
					(int)frame->yuv_plane_widths[2],
					(int)frame->yuv_plane_heights[2],
					(int)frame->yuv_plane_strides[2],
					data + frame->yuv_plane_offsets[2]))
				return false;
		}
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uY"), 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[1]);
		glUniform1i(glGetUniformLocation(prog, "uU"), 1);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, textures[2]);
		glUniform1i(glGetUniformLocation(prog, "uV"), 2);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), height);
		glUniform1i(glGetUniformLocation(prog, "uChromaWidth"),
			gray ? 1 : (int)frame->yuv_plane_widths[1]);
		glUniform1i(glGetUniformLocation(prog, "uChromaHeight"),
			gray ? 1 : (int)frame->yuv_plane_heights[1]);
		glUniform1i(glGetUniformLocation(prog, "uGray"), gray ? 1 : 0);
		break;
	}
	case UVC_FRAME_FORMAT_BGR: {
		const size_t need = (size_t)width * (size_t)height * 3u;
		if (actual < need)
			return false;
		prog = program(PROGRAM_BGR);
		if (!prog || !uploadRGB8(textures[0], width, height, data))
			return false;
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uBgr"), 0);
		break;
	}
	case UVC_FRAME_FORMAT_P010: {
		const size_t y_bytes = (size_t)width * (size_t)height * 2u;
		const size_t need = y_bytes + y_bytes / 2u;
		if (actual < need || (width & 1) || (height & 1))
			return false;
		prog = program(PROGRAM_P010);
		if (!prog
			|| !uploadR16UI(textures[0], width, height, data)
			|| !uploadRG16UI(textures[1], width / 2, height / 2, data + y_bytes))
			return false;
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, textures[0]);
		glUniform1i(glGetUniformLocation(prog, "uY16"), 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, textures[1]);
		glUniform1i(glGetUniformLocation(prog, "uUV16"), 1);
		glUniform1i(glGetUniformLocation(prog, "uWidth"), width);
		glUniform1i(glGetUniformLocation(prog, "uHeight"), height);
		break;
	}
	default:
		return false;
	}

	EGLint sw = 0;
	EGLint sh = 0;
	eglQuerySurface(display, surface, EGL_WIDTH, &sw);
	eglQuerySurface(display, surface, EGL_HEIGHT, &sh);
	glViewport(0, 0, sw, sh);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
		(const void *)(2 * sizeof(GLfloat)));
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	const GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		LOGW("gpu-preview: draw failed glerr=0x%x", err);
		return false;
	}
	return true;
}
