#include "UVCGpuPreviewRenderer.h"

#include "utilbase.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
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

enum ProgramKind {
	PROGRAM_YUYV = 0,
	PROGRAM_NV12,
	PROGRAM_YU12,
	PROGRAM_BGR,
	PROGRAM_P010,
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
	GLuint vbo = 0;
	int surface_width = 0;
	int surface_height = 0;

	bool ensureEgl(ANativeWindow *target);
	bool ensureSurface(ANativeWindow *target);
	void destroySurface();
	void destroyGl();
	GLuint program(ProgramKind kind);
	void setupGeometry();
	bool uploadAndDraw(uvc_frame_t *frame);
	bool uploadR8(GLuint tex, int width, int height, const void *data);
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
	if (!impl->uploadAndDraw(frame)) {
		/* Drop the EGLSurface before CPU fallback tries ANativeWindow_lock. */
		impl->destroySurface();
		return false;
	}
	if (eglSwapBuffers(impl->display, impl->surface) != EGL_TRUE) {
		LOGW("gpu-preview: eglSwapBuffers failed err=0x%x", eglGetError());
		impl->destroySurface();
		return false;
	}
	if (frame_ready_ns)
		*frame_ready_ns = now_ns();
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
	case PROGRAM_BGR:
		src = bgr_fragment_shader_src;
		break;
	case PROGRAM_P010:
		src = p010_fragment_shader_src;
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
