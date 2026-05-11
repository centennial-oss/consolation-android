.PHONY: build build-release test lint clean clean-build clear-config-cache generate-android-build-info generate-android-build-info-manual debug-autostart-on debug-autostart-off patch-libusb

APP_NAME := Consolation
APP_VERSION ?= $(shell awk -F= '/^consolation\.build\.version=/{print $$2}' build.properties)
RELEASE_APK_PATH := Consolation/app/build/outputs/apk/release
RELEASE_BUNDLE_PATH := Consolation/app/build/outputs/bundle/release
BUNDLE_ID ?= org.centennialoss.consolation
APPICON_BG := assets/app-icon-background-large.png
APPICON_TRANSPARENT_SRC := assets/app-icon-large-transparent.png
APPICON_LARGE := assets/app-icon-large.png
APPICON_PREVIEW := assets/app-icon.png

COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || echo local)
DATE ?= $(shell date -u +"%Y-%m-%dT%H:%M:%S.000Z")
TAG ?= $(if $(GITHUB_REF),$(patsubst refs/tags/%,%,$(GITHUB_REF)),localdev)
VERSION ?= $(patsubst v%,%,$(TAG))
BUILD_TYPE ?= $(if $(filter localdev,$(TAG)),localdev,Release)
BUILD_INFO_COMMIT ?= $(if $(filter localdev,$(TAG)),localdev,$(COMMIT))
BUILD_INFO_DATE ?= $(if $(filter localdev,$(TAG)),localdev,$(DATE))
ANDROID_BUILD_INFO := Consolation/app/src/main/java/org/centennialoss/consolation/AppBuildInfo.kt
DEBUG_AUTOSTART_FILE := Consolation/app/src/debug/java/org/centennialoss/consolation/DebugAutoStartPlayback.kt

# Set JAVA_HOME to the Android Studio bundled JDK
export JAVA_HOME := /Applications/Android Studio.app/Contents/jbr/Contents/Home

LIBUSB_DIR    := Consolation/app/src/main/jni/libusb-1.0.29
PREALLOC_DIR  := patches/libusb-1.0.29-prealloc
PREALLOC_SENTINEL := $(LIBUSB_DIR)/.prealloc-applied

patch-libusb: $(PREALLOC_SENTINEL)

$(PREALLOC_SENTINEL):
	patch --forward --batch -p1 \
		< $(PREALLOC_DIR)/libusb-1.0.29-prealloc.patch
	cp $(PREALLOC_DIR)/libusb_prealloc_ext.c $(LIBUSB_DIR)/libusb/os/
	cp $(PREALLOC_DIR)/libusb_prealloc.h     $(LIBUSB_DIR)/libusb/
	touch $(PREALLOC_SENTINEL)

build: patch-libusb test
	cd Consolation && ./gradlew :app:assembleDebug

build-release: patch-libusb test
	cd Consolation && ./gradlew :app:assembleRelease
	mv "$(RELEASE_APK_PATH)/app-release.apk" "$(RELEASE_APK_PATH)/$(APP_NAME)-$(APP_VERSION)-android.apk"

bundle-release: clean-build build-release
	cd Consolation && ./gradlew :app:bundleRelease
	mv "$(RELEASE_BUNDLE_PATH)/app-release.aab" "$(RELEASE_BUNDLE_PATH)/$(APP_NAME)-$(APP_VERSION)-android.aab"

test: lint
	cd Consolation && ./gradlew :app:testDebugUnitTest

lint:
	cd Consolation && ./gradlew lint

generate-android-build-info:
	mkdir -p Consolation/app/src/main/java/org/centennialoss/consolation
	printf '%s\n' 'package org.centennialoss.consolation' '' 'object AppBuildInfo {' '    const val version: String = "$(VERSION)"' '    const val buildType: String = "$(BUILD_TYPE)"' '    const val buildDate: String = "$(BUILD_INFO_DATE)"' '    const val commit: String = "$(BUILD_INFO_COMMIT)"' '' '    val copyableBlob: String' '        get() = """' '            Version: $$version (Android)' '            Build Type: $$buildType' '            Date: $$buildDate' '            Commit: $$commit' '        """.trimIndent()' '}' > $(ANDROID_BUILD_INFO)

clear-config-cache:
	rm -rf Consolation/.gradle/configuration-cache

clean-build:
	cd Consolation && ./gradlew clean

clean: clean-build clear-config-cache

debug-autostart-on:
	@sed -i '' -E 's/^(internal const val AUTO_START_PLAYBACK = ).*/\11/' "$(DEBUG_AUTOSTART_FILE)"
	@grep -n 'AUTO_START_PLAYBACK' "$(DEBUG_AUTOSTART_FILE)"

debug-autostart-off:
	@sed -i '' -E 's/^(internal const val AUTO_START_PLAYBACK = ).*/\10/' "$(DEBUG_AUTOSTART_FILE)"
	@grep -n 'AUTO_START_PLAYBACK' "$(DEBUG_AUTOSTART_FILE)"

# Requires BUILD_VERSION_MANUAL (e.g. export BUILD_VERSION_MANUAL=1.2.3). Uses git HEAD, UTC RFC3339 date (.000Z), updates repo-root build.properties.
generate-android-build-info-manual:
	@if [ -z "$(BUILD_VERSION_MANUAL)" ]; then \
		echo "error: BUILD_VERSION_MANUAL is not set; export it to the app version string (e.g. export BUILD_VERSION_MANUAL=1.2.3)" >&2; \
		exit 1; \
	fi
	@set -e; \
	V="$(BUILD_VERSION_MANUAL)"; \
	COMMIT=$$(git rev-parse HEAD 2>/dev/null || echo unknown); \
	DATE=$$(date -u +"%Y-%m-%dT%H:%M:%S.000Z"); \
	mkdir -p $$(dirname "$(ANDROID_BUILD_INFO)"); \
	printf '%s\n' \
		'package org.centennialoss.consolation' '' \
		'object AppBuildInfo {' \
		"    const val version: String = \"$$V\"" \
		'    const val buildType: String = "Release"' \
		"    const val buildDate: String = \"$$DATE\"" \
		"    const val commit: String = \"$$COMMIT\"" \
		'' \
		'    val copyableBlob: String' \
		'        get() = """' \
		'            Version: $$version (Android)' \
		'            Build Type: $$buildType' \
		'            Date: $$buildDate' \
		'            Commit: $$commit' \
		'        """.trimIndent()' \
		'}' > "$(ANDROID_BUILD_INFO)"; \
	BP="$(CURDIR)/build.properties"; \
	if [ ! -f "$$BP" ]; then \
		printf 'consolation.build.number=1\nconsolation.build.version=%s\n' "$$V" > "$$BP"; \
	else \
		tmp=$$(mktemp); \
		awk -v ver="$$V" 'BEGIN{FS=OFS="="} /^consolation\.build\.version=/{$$2=ver; found=1} {print} END{if(!found) print "consolation.build.version=" ver}' "$$BP" > "$$tmp" && mv "$$tmp" "$$BP"; \
	fi
