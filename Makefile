.PHONY: build build-release build-release-unsigned bundle-release test lint clean clean-build clear-config-cache generate-android-build-info generate-android-build-info-manual debug-autostart-on debug-autostart-off patch-libusb ci-pull-request ci-merge-main validate-semver-tag validate-android-signing-secrets validate-google-play-secrets validate-android-release-secrets validate-android-release-secret-material install-android-release-keystore install-google-play-service-account prepare-android-release-version prepare-android-preflight-version android-release-artifacts verify-android-release-artifacts upload-google-play android-publish-release android-release-preflight

APP_NAME := Consolation
APP_VERSION ?= $(shell awk -F= '/^consolation\.build\.version=/{print $$2}' build.properties)
RELEASE_APK_PATH := Consolation/app/build/outputs/apk/release
RELEASE_BUNDLE_PATH := Consolation/app/build/outputs/bundle/release
RELEASE_DIST_DIR := dist/release
BUNDLE_ID ?= org.centennialoss.consolation
APPICON_BG := assets/app-icon-background-large.png
APPICON_TRANSPARENT_SRC := assets/app-icon-large-transparent.png
APPICON_LARGE := assets/app-icon-large.png
APPICON_PREVIEW := assets/app-icon.png
BUILD_NUMBER ?= $(if $(GITHUB_RUN_NUMBER),$(GITHUB_RUN_NUMBER),1)
CI_KEYSTORE_PATH ?= $(CURDIR)/build/ci-release.keystore
KEYSTORE_PROPERTIES_PATH ?= $(CURDIR)/keystore.properties
GOOGLE_PLAY_SERVICE_ACCOUNT_FILE ?= $(CURDIR)/build/google-play-service-account.json
PLAY_TRACK ?= internal
PLAY_RELEASE_STATUS ?= DRAFT

COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || echo local)
DATE ?= $(shell date -u +"%Y-%m-%dT%H:%M:%S.000Z")
TAG ?= $(if $(GITHUB_REF),$(patsubst refs/tags/%,%,$(GITHUB_REF)),localdev)
VERSION ?= $(patsubst v%,%,$(TAG))
BUILD_TYPE ?= $(if $(filter localdev,$(TAG)),localdev,Release)
BUILD_INFO_COMMIT ?= $(if $(filter localdev,$(TAG)),localdev,$(COMMIT))
BUILD_INFO_DATE ?= $(if $(filter localdev,$(TAG)),localdev,$(DATE))
ANDROID_BUILD_INFO := Consolation/app/src/main/java/org/centennialoss/consolation/AppBuildInfo.kt
DEBUG_AUTOSTART_FILE := Consolation/app/src/debug/java/org/centennialoss/consolation/DebugAutoStartPlayback.kt

# Prefer the Android Studio bundled JDK locally; keep CI-provided JAVA_HOME on other runners.
ifneq ($(shell test -d "/Applications/Android Studio.app/Contents/jbr/Contents/Home" && echo yes),)
export JAVA_HOME := /Applications/Android Studio.app/Contents/jbr/Contents/Home
endif

LIBUSB_DIR    := Consolation/app/src/main/jni/libusb-1.0.30
PREALLOC_DIR  := patches/libusb-1.0.30-prealloc
PREALLOC_SENTINEL := $(LIBUSB_DIR)/.prealloc-applied

patch-libusb: $(PREALLOC_SENTINEL)

$(PREALLOC_SENTINEL):
	patch --forward --batch -p1 \
		< $(PREALLOC_DIR)/libusb-1.0.30-prealloc.patch
	cp $(PREALLOC_DIR)/libusb_prealloc_ext.c $(LIBUSB_DIR)/libusb/os/
	cp $(PREALLOC_DIR)/libusb_prealloc.h     $(LIBUSB_DIR)/libusb/
	touch $(PREALLOC_SENTINEL)

build: patch-libusb test
	cd Consolation && ./gradlew :app:assembleDebug

build-release: patch-libusb test
	cd Consolation && ./gradlew :app:assembleRelease
	mv "$(RELEASE_APK_PATH)/app-release.apk" "$(RELEASE_APK_PATH)/$(APP_NAME)-$(APP_VERSION)-android.apk"

build-release-unsigned: patch-libusb test
	cd Consolation && ./gradlew :app:assembleRelease
	@if [ -f "$(RELEASE_APK_PATH)/app-release-unsigned.apk" ]; then \
		echo "Unsigned release APK built: $(RELEASE_APK_PATH)/app-release-unsigned.apk"; \
	else \
		echo "error: unsigned release APK not found at $(RELEASE_APK_PATH)/app-release-unsigned.apk" >&2; \
		exit 1; \
	fi

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

ci-pull-request:
	$(MAKE) VERSION=ci BUILD_TYPE=Debug BUILD_INFO_COMMIT="$(COMMIT)" BUILD_INFO_DATE="$(DATE)" generate-android-build-info
	$(MAKE) clean build

ci-merge-main:
	$(MAKE) VERSION=main BUILD_TYPE=Release BUILD_INFO_COMMIT="$(COMMIT)" BUILD_INFO_DATE="$(DATE)" generate-android-build-info
	$(MAKE) clean build-release-unsigned

validate-semver-tag:
	@set -e; \
	tag="$${GITHUB_REF_NAME:-$(TAG)}"; \
	case "$$tag" in \
		v[0-9]*.[0-9]*.[0-9]*) ;; \
		*) echo "error: release tags must be semantic versions like v1.2.3; got '$$tag'" >&2; exit 1 ;; \
	esac; \
	if ! printf '%s\n' "$$tag" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+([+-][0-9A-Za-z.-]+)?$$'; then \
		echo "error: release tags must be semantic versions like v1.2.3; got '$$tag'" >&2; \
		exit 1; \
	fi

validate-android-signing-secrets:
	@set -e; \
	for var in ANDROID_RELEASE_KEYSTORE_BASE64 ANDROID_RELEASE_KEYSTORE_PASSWORD ANDROID_RELEASE_KEY_ALIAS ANDROID_RELEASE_KEY_PASSWORD; do \
		eval "value=\$$$$var"; \
		if [ -z "$$value" ]; then \
			echo "error: missing required Android signing secret/value: $$var" >&2; \
			exit 1; \
		fi; \
	done

validate-google-play-secrets:
	@set -e; \
	if [ -z "$$GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64" ]; then \
		echo "error: missing required Play Console secret/value: GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64" >&2; \
		exit 1; \
	fi

validate-android-release-secrets: validate-android-signing-secrets validate-google-play-secrets

validate-android-release-secret-material: validate-android-release-secrets
	@set -e; \
	if ! python3 -c 'import base64, os; data=base64.b64decode(os.environ["ANDROID_RELEASE_KEYSTORE_BASE64"], validate=True); assert data' >/dev/null 2>/dev/null; then \
		echo "::error::ANDROID_RELEASE_KEYSTORE_BASE64 is not valid base64 or decoded to an empty file" >&2; \
		exit 1; \
	fi; \
	if ! python3 -c 'import base64, os; data=base64.b64decode(os.environ["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64"], validate=True); assert data' >/dev/null 2>/dev/null; then \
		echo "::error::GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64 is not valid base64 or decoded to an empty file" >&2; \
		exit 1; \
	fi; \
	if ! python3 -c 'import base64, json, os; data=json.loads(base64.b64decode(os.environ["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64"], validate=True).decode("utf-8")); required=["type","project_id","private_key","client_email"]; missing=[key for key in required if not data.get(key)]; assert not missing, "missing required field(s): " + ", ".join(missing); assert data["type"] == "service_account", "type must be service_account"; assert "BEGIN PRIVATE KEY" in str(data["private_key"]), "private_key does not contain a PEM private key"; assert str(data["client_email"]).endswith(".gserviceaccount.com"), "client_email does not look like a service account"' >/dev/null 2>/dev/null; then \
		echo "::error::GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64 does not decode to a structurally valid service account JSON file" >&2; \
		exit 1; \
	fi

install-android-release-keystore: validate-android-signing-secrets
	@set -e; \
	mkdir -p "$(dir $(CI_KEYSTORE_PATH))"; \
	python3 -c 'import base64, os, sys; sys.stdout.buffer.write(base64.b64decode(os.environ["ANDROID_RELEASE_KEYSTORE_BASE64"], validate=True))' > "$(CI_KEYSTORE_PATH)"; \
	test -s "$(CI_KEYSTORE_PATH)"; \
	printf '%s\n' \
		"storeFile=$(CI_KEYSTORE_PATH)" \
		"storePassword=$$ANDROID_RELEASE_KEYSTORE_PASSWORD" \
		"keyAlias=$$ANDROID_RELEASE_KEY_ALIAS" \
		"keyPassword=$$ANDROID_RELEASE_KEY_PASSWORD" \
		> "$(KEYSTORE_PROPERTIES_PATH)"

install-google-play-service-account: validate-google-play-secrets
	@set -e; \
	mkdir -p "$(dir $(GOOGLE_PLAY_SERVICE_ACCOUNT_FILE))"; \
	python3 -c 'import base64, os, sys; sys.stdout.buffer.write(base64.b64decode(os.environ["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64"], validate=True))' > "$(GOOGLE_PLAY_SERVICE_ACCOUNT_FILE)"; \
	python3 -c 'import json, sys; data=json.load(open(sys.argv[1])); required=["type","project_id","private_key","client_email"]; missing=[key for key in required if not data.get(key)]; assert not missing, "service account JSON missing required field(s): " + ", ".join(missing); assert data["type"] == "service_account", "service account JSON field type must be service_account"; assert str(data["client_email"]).endswith(".gserviceaccount.com"), "client_email does not look like a service account"; assert "BEGIN PRIVATE KEY" in str(data["private_key"]), "private_key does not contain a PEM private key"; print("Play service account JSON passed structural validation.")' "$(GOOGLE_PLAY_SERVICE_ACCOUNT_FILE)"

prepare-android-release-version: validate-semver-tag
	@set -e; \
	version="$${GITHUB_REF_NAME:-$(TAG)}"; \
	version="$${version#v}"; \
	printf 'consolation.build.number=%s\nconsolation.build.version=%s\n' "$(BUILD_NUMBER)" "$$version" > build.properties; \
	$(MAKE) VERSION="$$version" BUILD_TYPE=Release BUILD_INFO_COMMIT="$(COMMIT)" BUILD_INFO_DATE="$(DATE)" generate-android-build-info

prepare-android-preflight-version:
	@set -e; \
	version="0.0.0-preflight"; \
	printf 'consolation.build.number=%s\nconsolation.build.version=%s\n' "$(BUILD_NUMBER)" "$$version" > build.properties; \
	$(MAKE) VERSION="$$version" BUILD_TYPE=Release BUILD_INFO_COMMIT="$(COMMIT)" BUILD_INFO_DATE="$(DATE)" generate-android-build-info

android-release-artifacts: patch-libusb test
	$(MAKE) clean-build
	cd Consolation && ./gradlew :app:assembleRelease :app:bundleRelease
	@set -e; \
	version="$$(awk -F= '/^consolation\.build\.version=/{print $$2}' build.properties)"; \
	mkdir -p "$(RELEASE_DIST_DIR)"; \
	cp "$(RELEASE_APK_PATH)/app-release.apk" "$(RELEASE_DIST_DIR)/$(APP_NAME)-$$version-android.apk"; \
	cp "$(RELEASE_BUNDLE_PATH)/app-release.aab" "$(RELEASE_DIST_DIR)/$(APP_NAME)-$$version-android.aab"

verify-android-release-artifacts:
	@set -e; \
	version="$$(awk -F= '/^consolation\.build\.version=/{print $$2}' build.properties)"; \
	apk="$(RELEASE_DIST_DIR)/$(APP_NAME)-$$version-android.apk"; \
	aab="$(RELEASE_DIST_DIR)/$(APP_NAME)-$$version-android.aab"; \
	test -s "$$apk"; \
	test -s "$$aab"; \
	unzip -t "$$apk" >/dev/null; \
	unzip -t "$$aab" >/dev/null; \
	apksigner="$$(find "$${ANDROID_HOME:-}" -path '*/build-tools/*/apksigner' -type f 2>/dev/null | sort | tail -n 1 || true)"; \
	if [ -n "$$apksigner" ]; then \
		"$$apksigner" verify --verbose "$$apk"; \
	else \
		echo "warning: apksigner not found; skipped APK signature verification"; \
	fi; \
	echo "Verified release artifacts: $$apk $$aab"

upload-google-play:
	@set -e; \
	version="$$(awk -F= '/^consolation\.build\.version=/{print $$2}' build.properties)"; \
	cd Consolation && ./gradlew :app:publishReleaseBundle \
		-Pconsolation.play.serviceAccountCredentials="$(GOOGLE_PLAY_SERVICE_ACCOUNT_FILE)" \
		-Pconsolation.play.track="$(PLAY_TRACK)" \
		-Pconsolation.play.releaseStatus="$(PLAY_RELEASE_STATUS)" \
		-Pconsolation.play.releaseName="$(APP_NAME) $$version"

android-publish-release: validate-android-release-secret-material install-android-release-keystore install-google-play-service-account prepare-android-release-version android-release-artifacts verify-android-release-artifacts upload-google-play

android-release-preflight: validate-android-release-secret-material install-android-release-keystore install-google-play-service-account prepare-android-preflight-version android-release-artifacts verify-android-release-artifacts

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
