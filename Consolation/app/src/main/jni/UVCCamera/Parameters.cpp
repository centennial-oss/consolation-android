/*
 * UVCCamera
 * library and sample to access to UVC web camera on non-rooted Android device
 *
 * Copyright (c) 2014-2017 saki t_saki@serenegiant.com
 *
 * File name: Parameters.cpp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * All files in the folder are under this Apache License, Version 2.0.
 * Files in the jni/libjpeg, jni/libusb, jin/libuvc, jni/rapidjson folder may have a different license, see the respective files.
*/
#define LOG_TAG "Parameters"

#include "Parameters.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "libuvc/libuvc_internal.h"
#include <android/log.h>
#include <string.h>

/** Full UVC descriptor tree for debugging (same JSON shape as [getDescriptions]). */
#define CONSOL_UVC_NATIVE_TAG "ConsolationUvcNative"

using namespace rapidjson;

static void write(Writer<StringBuffer> &writer, const char *key, const char *value);

static const uint8_t UVC_GUID_YUY2[16] =
	{'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
static const uint8_t UVC_GUID_MJPG[16] =
	{'M', 'J', 'P', 'G', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t UVC_GUID_H264[16] =
	{'H', '2', '6', '4', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
static const uint8_t UVC_GUID_NV12[16] =
	{'N', 'V', '1', '2', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
static const uint8_t UVC_GUID_P010[16] =
	{'P', '0', '1', '0', 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

static int previewModeForGuid(const uint8_t guid[16]) {
	if (!memcmp(guid, UVC_GUID_YUY2, 4))
		return 0;
	if (!memcmp(guid, UVC_GUID_MJPG, 4))
		return 1;
	if (!memcmp(guid, UVC_GUID_H264, 4))
		return 2;
	if (!memcmp(guid, UVC_GUID_NV12, 4))
		return 3;
	if (!memcmp(guid, UVC_GUID_P010, 4))
		return 4;
	return -1;
}

static void writeGuid(Writer<StringBuffer> &writer, const char *key, const uint8_t guid[16]) {
	static const char hex[] = "0123456789abcdef";
	char buf[33];
	for (int i = 0; i < 16; ++i) {
		buf[i * 2] = hex[(guid[i] >> 4) & 0x0f];
		buf[i * 2 + 1] = hex[guid[i] & 0x0f];
	}
	buf[32] = '\0';
	write(writer, key, buf);
}

static void guidToHex(const uint8_t guid[16], char buf[33]) {
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < 16; ++i) {
		buf[i * 2] = hex[(guid[i] >> 4) & 0x0f];
		buf[i * 2 + 1] = hex[guid[i] & 0x0f];
	}
	buf[32] = '\0';
}

/** Emit libusb/UVC frame interval array for one frame descriptor (100 ns units). */
static void writeFrameIntervalArray(Writer<StringBuffer> &writer, uvc_frame_desc_t *frame_desc) {
	writer.StartArray();
	if (frame_desc->intervals) {
		for (uint32_t *p = frame_desc->intervals; *p; ++p) {
			writer.Uint(*p);
		}
	} else if (frame_desc->dwMinFrameInterval > 0) {
		uint32_t mn = frame_desc->dwMinFrameInterval;
		uint32_t mx = frame_desc->dwMaxFrameInterval ? frame_desc->dwMaxFrameInterval : mn;
		uint32_t step = frame_desc->dwFrameIntervalStep;
		if (step > 0 && mx >= mn) {
			int n = 0;
			for (uint64_t v = mn; v <= mx && n < 64; v += step, n++) {
				writer.Uint((uint32_t)v);
			}
		} else {
			/* Continuous range only: emit standard candidate intervals that fall within [mn,mx]. */
			static const int cand_fps[] = {120, 119, 60, 59, 50, 30, 29, 25, 24, 15, 10};
			for (unsigned i = 0; i < sizeof(cand_fps) / sizeof(cand_fps[0]); ++i) {
				uint32_t iv = (uint32_t)(10000000 / cand_fps[i]);
				if (iv >= mn && iv <= mx) {
					writer.Uint(iv);
				}
			}
			writer.Uint(mn);
			if (mx != mn) {
				writer.Uint(mx);
			}
		}
	}
	writer.EndArray();
}

static void write(Writer<StringBuffer> &writer, const char *key, const char *value) {
	writer.String(key);
	writer.String(value);
}

static void write(Writer<StringBuffer> &writer, const char *key, uint16_t value) {
	writer.String(key);
	writer.Uint(value);
}

static void write(Writer<StringBuffer> &writer, const char *key, int32_t value) {
	writer.String(key);
	writer.Int(value);
}

static void write(Writer<StringBuffer> &writer, const char *key, uint32_t value) {
	writer.String(key);
	writer.Uint(value);
}

static void write(Writer<StringBuffer> &writer, const char *key, int64_t value) {
	writer.String(key);
	writer.Int64(value);
}

static void write(Writer<StringBuffer> &writer, const char *key, uint64_t value) {
	writer.String(key);
	writer.Uint64(value);
}

static const char *_uvc_name_for_format_subtype(uint8_t subtype) {
	switch (subtype) {
	case UVC_VS_FORMAT_UNCOMPRESSED:
		return "UncompressedFormat";
	case UVC_VS_FORMAT_MJPEG:
		return "MJPEGFormat";
	default:
		return "Unknown";
	}
}

#define	INDEX						"index"
#define TYPE						"type"
#define SUBTYPE						"subType"
#define WIDTH						"width"
#define HEIGHT						"height"
#define	VALUE						"value"
#define DETAIL						"detail"

#define DESCRIPTION					"description"
#define DESC_SUBTYPE				SUBTYPE
#define	DESC_VENDERID				"venderId"
#define	DESC_PRODUCTID				"productId"
#define	DESC_SERIALNUMBER			"serialNumber"
#define DESC_MANIFUCTURE			"manifuctureName"
#define DESC_PRODUCT				"productName"
#define DESC_UVC					"uvc"
#define DESC_VIDEO_CONTROL			"videoControl"
#define DESC_INTERFACES				"interfaces"

#define INTERFACE_TYPE 				TYPE
#define INTERFACE_TYPE_VIDEOSTREAM	"videoStreaming"
#define INTERFACE_TYPE_AUDIOSTREAM	"audioStreaming"
#define INTERFACE_INDEX				INDEX
#define INTERFACE_ENDPOINT_ADDR		"endpointAddress"

#define	FORMATS						"formats"
#define FORMAT_INDEX				INDEX
#define FORMAT_NAME					"format"
#define FORMAT_DETAIL				DETAIL
#define FORMAT_BITS_PER_PIXEL		"bitsPerPixel"
#define FORMAT_GUID					"GUID"
#define FORMAT_DEFAULT_FRAME_INDEX	"defaultFrameIndex"
#define FORMAT_ASPECTRATIO_X		"aspectRatioX"
#define FORMAT_ASPECTRATIO_Y		"aspectRatioY"
#define FORMAT_INTERLACE_FLAGS		"interlaceFlags"
#define FORMAT_COPY_PROTECT			"copyProtect"
#define FORMAT_FRAMEDESCRIPTORS		"frameDescriptors"

#define FRAME_INDEX					INDEX
#define FRAME_CAPABILITIES			"capabilities"
#define	FRAME_WIDTH					WIDTH
#define	FRAME_HEIGHT				HEIGHT
#define FRAME_BITRATE_MIN			"minBitRate"
#define FRAME_BITRATE_MAX			"maxBitRate"
#define FRAME_FRAMEBUFFERSIZE_MAX	"maxFrameBufferSize"
#define FRAME_INTERVAL_DEFAULT		"defaultFrameInterval"
#define FRAME_FPS_DEFAULT			"defaultFps"
#define FRAME_INTERVALS				"intervals"
#define FRAME_INTERVAL_INDEX		INDEX
#define FRAME_INTERVAL_VALUE		VALUE
#define FRAME_INTERVAL_FPS			"fps"
#define FRAME_INTERVAL_MIN			"minFrameInterval"
#define FRAME_INTERVAL_MAX			"maxFrameInterval"
#define FRAME_INTERVAL_STEP			"frameIntervalStep"

static void writerFormat(Writer<StringBuffer> &writer, uvc_format_desc_t *fmt_desc) {
	uvc_frame_desc_t *frame_desc;
	char work[256];

	writer.String(FORMAT_DETAIL);
	writer.StartObject();
	{
		write(writer, FORMAT_BITS_PER_PIXEL, fmt_desc->bBitsPerPixel);
		sprintf(work,
			"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
				fmt_desc->guidFormat[0], fmt_desc->guidFormat[1],
				fmt_desc->guidFormat[2], fmt_desc->guidFormat[3],
				fmt_desc->guidFormat[4], fmt_desc->guidFormat[5],
				fmt_desc->guidFormat[6], fmt_desc->guidFormat[7],
				fmt_desc->guidFormat[8], fmt_desc->guidFormat[9],
				fmt_desc->guidFormat[10], fmt_desc->guidFormat[11],
				fmt_desc->guidFormat[12], fmt_desc->guidFormat[13],
				fmt_desc->guidFormat[14], fmt_desc->guidFormat[15]);
		write(writer, FORMAT_GUID, work);
		write(writer, FORMAT_DEFAULT_FRAME_INDEX, fmt_desc->bDefaultFrameIndex);
		write(writer, FORMAT_ASPECTRATIO_X, fmt_desc->bAspectRatioX);
		write(writer, FORMAT_ASPECTRATIO_Y, fmt_desc->bAspectRatioY);
		write(writer, FORMAT_INTERLACE_FLAGS, fmt_desc->bmInterlaceFlags);
		write(writer, FORMAT_COPY_PROTECT, fmt_desc->bCopyProtect);

		writer.String(FORMAT_FRAMEDESCRIPTORS);
		writer.StartArray();
		DL_FOREACH(fmt_desc->frame_descs, frame_desc)
		{
			uint32_t *interval_ptr;

			writer.StartObject();
			{
				write(writer, FRAME_INDEX, frame_desc->bFrameIndex);
				write(writer, FRAME_CAPABILITIES, frame_desc->bmCapabilities);
				write(writer, FRAME_WIDTH, frame_desc->wWidth);
				write(writer, FRAME_HEIGHT, frame_desc->wHeight);
				write(writer, FRAME_BITRATE_MIN, frame_desc->dwMinBitRate);
				write(writer, FRAME_BITRATE_MAX, frame_desc->dwMaxBitRate);
				write(writer, FRAME_FRAMEBUFFERSIZE_MAX, frame_desc->dwMaxVideoFrameBufferSize);
				write(writer, FRAME_INTERVAL_DEFAULT, frame_desc->dwDefaultFrameInterval);
				write(writer, FRAME_FPS_DEFAULT, 10000000 / frame_desc->dwDefaultFrameInterval);

				if (frame_desc->intervals) {
					writer.String(FRAME_INTERVALS);
					writer.StartArray();
					for (interval_ptr = frame_desc->intervals; *interval_ptr; ++interval_ptr) {
						writer.StartObject();
						write(writer, FRAME_INTERVAL_INDEX, (int ) (interval_ptr - frame_desc->intervals));
						write(writer, FRAME_INTERVAL_VALUE, *interval_ptr);
						write(writer, FRAME_INTERVAL_FPS, 10000000 / *interval_ptr);
						writer.EndObject();
					}
					writer.EndArray();
				} else {
					// 最小fps
					writer.String(FRAME_INTERVAL_MIN);
					writer.StartObject();
					{
						write(writer, FRAME_INTERVAL_INDEX, frame_desc->dwMinFrameInterval);
						write(writer, FRAME_INTERVAL_VALUE, frame_desc->dwMinFrameInterval);
						write(writer, FRAME_INTERVAL_FPS, 10000000 / frame_desc->dwMinFrameInterval);
					}
					writer.EndObject();
					// 最大fps
					writer.String(FRAME_INTERVAL_MAX);
					writer.StartObject();
					{
						write(writer, FRAME_INTERVAL_INDEX, frame_desc->dwMaxFrameInterval);
						write(writer, FRAME_INTERVAL_VALUE, frame_desc->dwMaxFrameInterval);
						write(writer, FRAME_INTERVAL_FPS, 10000000 / frame_desc->dwMaxFrameInterval);
					}
					writer.EndObject();
					if (frame_desc->dwFrameIntervalStep) {
						// fpsステップ
						writer.String(FRAME_INTERVAL_STEP);
						writer.StartObject();
						{
							write(writer, FRAME_INTERVAL_INDEX, frame_desc->dwFrameIntervalStep);
							write(writer, FRAME_INTERVAL_VALUE, frame_desc->dwFrameIntervalStep);
							write(writer, FRAME_INTERVAL_FPS, 10000000 / frame_desc->dwFrameIntervalStep);
						}
						writer.EndObject();
					}
				}
			}
			writer.EndObject();
		}
		writer.EndArray();	// end of FORMAT_FRAMEDESCRIPTORS
	}
	writer.EndObject();	// end of FORMAT_DETAIL
}

static void writerFormatDescriptions(Writer<StringBuffer> &writer, uvc_streaming_interface_t *stream_if) {

	uvc_format_desc_t *fmt_desc;
	int i;

	writer.String(FORMATS);
	writer.StartArray();
	DL_FOREACH(stream_if->format_descs, fmt_desc)
	{
		writer.StartObject();
		{
			write(writer, FORMAT_INDEX, fmt_desc->bFormatIndex);
			write(writer, DESC_SUBTYPE, fmt_desc->bDescriptorSubtype);
			write(writer, FORMAT_NAME, _uvc_name_for_format_subtype(fmt_desc->bDescriptorSubtype));
			switch (fmt_desc->bDescriptorSubtype) {
			case UVC_VS_FORMAT_UNCOMPRESSED:
			case UVC_VS_FORMAT_MJPEG:
				writerFormat(writer, fmt_desc);
				break;
			default:
				break;
			}
		}
		writer.EndObject();
	}
	writer.EndArray();	// end of FORMATS
}

UVCDiags::UVCDiags() {}
UVCDiags::~UVCDiags() {};

char *UVCDiags::getDescriptions(const uvc_device_handle_t *deviceHandle) {
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	char work[256];

	ENTER();
	writer.StartObject();
	{
		writer.String(DESCRIPTION);
		writer.StartObject();
		{
			uvc_device_descriptor_t *desc;
			uvc_get_device_descriptor(deviceHandle->dev, &desc);
			write(writer, DESC_VENDERID, desc->idVendor);
			write(writer, DESC_PRODUCTID, desc->idProduct);
			write(writer, DESC_SERIALNUMBER, desc->serialNumber ? desc->serialNumber : "[none]");
			write(writer, DESC_MANIFUCTURE, desc->manufacturer ? desc->manufacturer : "[unknown]");
//			write(writer, DESC_PRODUCT, desc->product ? desc->product : "UVC Camera");
			if (desc->product)
				write(writer, DESC_PRODUCT, desc->product);
			else {
				sprintf(work, "UVC Camera (%x:%x)", desc->idVendor, desc->idProduct);
				write(writer, DESC_PRODUCT, work);
			}
			uvc_free_device_descriptor(desc);

			if (deviceHandle->info->ctrl_if.bcdUVC) {
				writer.String(DESC_UVC);
				writer.StartObject();
				{
					write(writer, DESC_VIDEO_CONTROL, deviceHandle->info->ctrl_if.bcdUVC);

					writer.String(DESC_INTERFACES);
					writer.StartArray();
					{
						assert(deviceHandle->info->stream_ifs);
						uvc_streaming_interface_t *stream_if;
						int stream_idx = 0;

						DL_FOREACH(deviceHandle->info->stream_ifs, stream_if)
						{
							++stream_idx;
							writer.StartObject();
							{
								write(writer, INTERFACE_TYPE, INTERFACE_TYPE_VIDEOSTREAM);
								write(writer, INTERFACE_INDEX, stream_idx);
								write(writer, INTERFACE_ENDPOINT_ADDR, stream_if->bEndpointAddress);
								writerFormatDescriptions(writer, stream_if);
							}
							writer.EndObject();
						}
					}
					writer.EndArray();	// end of DESC_INTERFACES
				}
				writer.EndObject();	// end of DESC_UVC
			}
			// XXX other interfaces
		}
		writer.EndObject();	// end of DESCRIPTION
	}
	writer.EndObject();
	RETURN(strdup(buffer.GetString()), char *);
}

char *UVCDiags::getCurrentStream(const uvc_stream_ctrl_t *ctrl) {
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);

	ENTER();
	writer.StartObject();
	{
		write(writer, "hint", ctrl->bmHint);
		write(writer, "formatIndex", ctrl->bFormatIndex);
		write(writer, "frameIndex", ctrl->bFrameIndex);
		write(writer, "frameInterval", ctrl->dwFrameInterval);
		write(writer, "keyFrameRate", ctrl->wKeyFrameRate);
		write(writer, "frameRate", ctrl->wPFrameRate);
		write(writer, "compQuality", ctrl->wCompQuality);
		write(writer, "compWindowSize", ctrl->wCompWindowSize);
		write(writer, "delay", ctrl->wDelay);
		write(writer, "maxVideoFrameSize", ctrl->dwMaxVideoFrameSize);
		write(writer, "maxPayloadTransferSize", ctrl->dwMaxPayloadTransferSize);
		write(writer, "interfaceNumber", ctrl->bInterfaceNumber);
	}
	writer.EndObject();
	RETURN(strdup(buffer.GetString()), char *);
}

char *UVCDiags::getSupportedSize(const uvc_device_handle_t *deviceHandle) {
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	char buf[256];

	ENTER();

	if (deviceHandle->info->stream_ifs) {
		uvc_streaming_interface_t *stream_if;
		int stream_idx = 0;
		DL_FOREACH(deviceHandle->info->stream_ifs, stream_if)
		{
			++stream_idx;
			uvc_format_desc_t *fmt_desc;
			DL_FOREACH(stream_if->format_descs, fmt_desc)
			{
				char guid[33];
				guidToHex(fmt_desc->guidFormat, guid);
#ifndef APP_NATIVE_LOG_SILENT
				__android_log_print(ANDROID_LOG_INFO, CONSOL_UVC_NATIVE_TAG,
					"getSupportedSize: stream_if=%d format subtype=0x%02x formatIndex=%u frameFormat=%d guid=%s",
					stream_idx, fmt_desc->bDescriptorSubtype, fmt_desc->bFormatIndex,
					previewModeForGuid(fmt_desc->guidFormat), guid);
#endif
			}
		}
	}

	writer.StartObject();
	{
		if (deviceHandle->info->stream_ifs) {
			uvc_streaming_interface_t *stream_if;

			writer.String("formats");
			writer.StartArray();
			DL_FOREACH(deviceHandle->info->stream_ifs, stream_if)
			{
				uvc_format_desc_t *fmt_desc;
				DL_FOREACH(stream_if->format_descs, fmt_desc)
				{
					const uint8_t st = fmt_desc->bDescriptorSubtype;
					if (st != UVC_VS_FORMAT_UNCOMPRESSED && st != UVC_VS_FORMAT_MJPEG
							&& st != UVC_VS_FORMAT_FRAME_BASED && st != UVC_VS_FORMAT_STREAM_BASED) {
						continue;
					}
					if (!fmt_desc->frame_descs) {
#ifndef APP_NATIVE_LOG_SILENT
						__android_log_print(ANDROID_LOG_WARN, CONSOL_UVC_NATIVE_TAG,
							"getSupportedSize: skip formatIndex=%u subtype=0x%02x (no frame_descs)",
							fmt_desc->bFormatIndex, st);
#endif
						continue;
					}

					writer.StartObject();
					write(writer, "index", fmt_desc->bFormatIndex);
					write(writer, "type", (uint32_t)st);
					write(writer, "frameFormat", previewModeForGuid(fmt_desc->guidFormat));
					writeGuid(writer, "guid", fmt_desc->guidFormat);
					write(writer, "default", fmt_desc->bDefaultFrameIndex);

					writer.String("size");
					writer.StartArray();
					uvc_frame_desc_t *frame_desc;
					DL_FOREACH(fmt_desc->frame_descs, frame_desc)
					{
						snprintf(buf, sizeof(buf), "%dx%d", frame_desc->wWidth, frame_desc->wHeight);
						buf[sizeof(buf)-1] = '\0';
						writer.String(buf);
					}
					writer.EndArray();

					writer.String("intervals");
					writer.StartArray();
					DL_FOREACH(fmt_desc->frame_descs, frame_desc)
					{
						writeFrameIntervalArray(writer, frame_desc);
					}
					writer.EndArray();

					writer.EndObject();
				}
			}
			writer.EndArray();
		}
	}
	writer.EndObject();
	RETURN(strdup(buffer.GetString()), char *);
}
