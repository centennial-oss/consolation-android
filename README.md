# <img src="assets/app-icon.png" alt="Consolation" height="48" /> Consolation™

A 100% free, no-frills, incredibly performant video capture viewer for Android tablets with no analytics or snooping.

Consolation is coming soon to the Play Store, or can be downloaded directly from [this project's releases](https://github.com/centennial-oss/consolation-android/releases).

## About

Consolation is a free app that enables your Android tablet to be used as a screen for devices like gaming consoles, Raspberry Pis, and even a Mac mini or other PC, via a standard USB Video Class (UVC) video capture card.

The app is intentionally simple: watch the live video on your tablet. No recording or saving, no streaming to the internet. Just plug and play, privately with no ads or tracking. Consolation will never make an outbound network request or listen for inbound network connections.

Consolation is _blazingly fast_, with less real-time lag than anything you've ever used before on Android. Seriously, its performance will blow your mind. On capable capture cards (including many available for less than $30 online), it is difficult to notice any latency at all, even at 1440p/60.

## Screenshots

(coming soon)

## Privacy

Consolation does not collect, send, or share your data. Audio and video stay local and transient while you are watching a connected capture device. The app is open source, contains no trackers or analytics, makes no network calls, and does not record, stream, save, or analyze audio or video. Nothing leaves your device - ever.

## Supported Capture Devices

Any capture device that appears to Android OS as a USB Video Class (UVC) capture device should work with Consolation.

Consolation has been tested by the developers on a Samsung Galaxy S8 Ultra (SM-X900) with these capture devices:

- Elgato HD60 X - 👌 🚀
- acer USB 3.0 Video Capture Card (model OCB5B0) - 👍
- WANKEDA 4K Capture Card 1080p 60FPS for Streaming (1da603d4) - 👍
- UGREEN Full HD 1080p Capture Card (model 40189) -  ⚠️ max 30p @ 1920x1080

## Requirements

### Running

- Android device with a USB port
- Android OS 15 or higher
- A UVC-compliant video capture card

### Developer

- Android Studio Panda 4 or higher

## Building

1. Open the `Consolation` directory in Android Studio
2. Build and run.

You can make a debug build with `make build` and a release build with `make build-release`.

## Acknowledgements

We incorporated <https://github.com/alexey-pelykh/UVCCamera> into the Consolation for Android. Alexey's project is a fork of <https://github.com/saki4510t/UVCCamera> which has gone dormant. We thank both projects for helping make Consolation for Android possible.

Our modifications to UVCCamera are embedded into the codebase at [Consolation/app/src/main/jni/UVCCamera](Consolation/app/src/main/jni/UVCCamera) and [Consolation/app/src/main/jni/libuvc](Consolation/app/src/main/jni/libuvc), and make significant improvements for performance and stability:
* eliminates nearly all frame copies, resulting in significant lag reduction
* adds support for h.264, NV12 and P010 input pixel formats
* fixes defects in yuyv2iyuv and any2yuv frame handlers
* improves device auto-detection and hot re-plug recovery
* numerous other performance improvements for a true real-time experience on modern Android devices with modern capture cards

We intend to contribute these improvements back to Alexey's project so everyone can take advantage of their benefits.

## Contributor Disclosure

Humans write this software with AI assistance. All contributions are well-tested and merged only after being reviewed and approved by humans who fully understand and take responsibility for the contribution.

While we welcome pull requests and other contributions from other humans, including AI-generated code, we do not accept contributions from AI bots. A human must review, understand, and sign off on all commits. All contributors must be able to defend their contributions under reasonable technical scrutiny. Please file an issue to discuss any proposed feature before working on it.

## Trademark Notice

Consolation and its logo are trademarks of Centennial OSS Inc.
Use of the name and branding is not permitted for modified versions or forks without permission.
See [TRADEMARKS.md](TRADEMARKS.md) for details.
