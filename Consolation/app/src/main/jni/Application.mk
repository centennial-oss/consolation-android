# Enables correct ELF LOAD segment alignment for 16 KB page-size devices when using a
# recent enough NDK. See MITIGATION_16k.md and https://developer.android.com/guide/practices/page-sizes
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true

# libuvc and UVCCamera are first-party code merged into libUVCCamera.so (see
# UVCCamera/Android.mk). Thin LTO applies to that unit; libusb/jpeg remain
# separate shared libraries with no cross-DSO LTO.
CONSOLATION_FIRST_PARTY_LTO_CFLAGS := -flto=thin
CONSOLATION_FIRST_PARTY_LTO_LDFLAGS := -flto=thin
