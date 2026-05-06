# <img src="assets/app-icon.png" alt="Consolation" height="48" /> Consolation™

A 100% free, no-frills, incredibly performant video capture viewer for Android tablets with no analytics or snooping.

Consolation is coming soon to the Play Store, or can be downloaded directly from [this project's releases](https://github.com/centennial-oss/consolation-android/releases).

## About

Consolation is a free app that enables your Android tablet to be used as a display for devices like gaming consoles, Raspberry Pis, and even a Mac mini or other PC, via a standard USB Video Class (UVC) video capture card.

Consolation is _blazingly fast_, with less real-time lag than anything you've ever used before on Android. Seriously, its performance will blow your mind. On capable capture cards (including many available for less than $30 online), it is difficult to notice any latency at all, even at 1440p/60 and higher.

The app is intentionally simple: watch the live video on your tablet. No recording or saving, no streaming to the internet. Just plug and play, privately with no ads or tracking. Consolation will never make an outbound network request or listen for inbound network connections.

## Screenshots

<img src="assets/screenshots/consolation-android-01.png" alt="Android start screen" width="270" /><img src="assets/screenshots/consolation-android-05.png" alt="Android Raspberry PI" width="270" /> <img src="assets/screenshots/consolation-android-06.png" alt="mac mini" width="270" />

<img src="assets/screenshots/consolation-android-02.png" alt="Android gameplay" width="270" /> <img src="assets/screenshots/consolation-android-03.png" alt="Android gameplay" width="270" /> <img src="assets/screenshots/consolation-android-04.png" alt="Android gameplay" width="270" /> 

## Privacy

Consolation does not collect, send, or share your data. Audio and video stay local and transient while you are watching a connected capture device. The app is open source, contains no trackers or analytics, makes no network calls, and does not record, stream, save, or analyze audio or video. Consolation has no idea what content is coming through your capture card's feed, and nothing leaves your device, ever.

Read the full privacy policy at [PRIVACY.md](PRIVACY.md) or <https://centennialoss.org/privacy/>.

## Supported Capture Devices

Any capture device that appears to the Android OS as a USB Video Class (UVC) capture device should work with Consolation.

Consolation has been tested by the developers on a Samsung Galaxy S8 Ultra (SM-X900) with these capture devices:

- Elgato HD60 X - 👌 🚀
- Acer USB 3.0 Video Capture Card (model OCB5B0) - 👍
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

### UVCCamera and libuvc

We vendored <https://github.com/alexey-pelykh/UVCCamera> into Consolation for Android. Alexey's project is a fork of <https://github.com/saki4510t/UVCCamera> which has gone dormant. We thank both projects for helping make Consolation for Android possible.

UVCCamera depends on <https://github.com/libuvc/libuvc>, which we have also vendored.

We have significantly modified our vendored UVCCamera and libuvc libs for stability and performance. These mods are in the codebase at [Consolation/app/src/main/jni/UVCCamera](Consolation/app/src/main/jni/UVCCamera) and [Consolation/app/src/main/jni/libuvc](Consolation/app/src/main/jni/libuvc):
* eliminated nearly all frame copies, resulting in significant lag reduction
* added support for H.264, NV12, and P010 input pixel formats
* fixed defects in yuyv2iyuv and any2yuv frame handlers
* numerous other performance improvements for a true real-time experience on modern Android devices with modern capture cards

### libjpeg-turbo
We use [libjpeg-turbo v3.1.4.1](https://github.com/libjpeg-turbo/libjpeg-turbo) unmodified to decode the MJPEG pixel format.

### libusb

We vendored <https://github.com/libusb/libusb> v1.0.18 and made a number of improvements on the code paths used by Consolation, including:
* improved device auto-detection and hot re-plug recovery
* better heap memory safety on hot and error paths
* fixed `itransfer->lock` deadlocks on error branch
* fixed `libusb_lock_events` deadlock in `do_close`
* protections against potential Java file descriptor leakage on Android

These changes are at [Consolation/app/src/main/jni/libusb](Consolation/app/src/main/jni/libusb).

### Upstreaming

We intend to contribute these improvements back to their respective base projects so everyone can take advantage of their benefits.

## Contributor Disclosure

Humans write this software with AI assistance. All contributions are well-tested and merged only after being reviewed and approved by humans who fully understand and take responsibility for the contribution.

While we welcome pull requests and other contributions from other humans, including AI-generated code, we do not accept contributions from AI bots. A human must review, understand, and sign off on all commits. All contributors must be able to defend their contributions under reasonable technical scrutiny. Please file an issue to discuss any proposed feature before working on it.

## Trademark Notice

Consolation and its logo are trademarks of Centennial OSS Inc.
Use of the name and branding is not permitted for modified versions or forks without permission.
See [TRADEMARKS.md](TRADEMARKS.md) for details.
