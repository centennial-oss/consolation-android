#ifndef UVCGPUPREVIEWRENDERER_H_
#define UVCGPUPREVIEWRENDERER_H_

#include <stdint.h>
#include <android/native_window.h>
#include "libuvc/libuvc.h"

class UVCGpuPreviewRenderer {
public:
	UVCGpuPreviewRenderer();
	~UVCGpuPreviewRenderer();

	bool render(uvc_frame_t *frame, ANativeWindow *window, uint64_t *frame_ready_ns);
	void resetSurface();
	void shutdown();

private:
	struct Impl;
	Impl *impl;
};

#endif /* UVCGPUPREVIEWRENDERER_H_ */
