# Repository Secrets Guide

The Android release workflows expect these repository secrets in GitHub.

## Android App Signing

`ANDROID_RELEASE_KEYSTORE_BASE64`

Base64-encoded contents of the release keystore file (`.jks` or `.keystore`).

```sh
base64 -i release.keystore | pbcopy
```

On Linux:

```sh
base64 -w 0 release.keystore
```

`ANDROID_RELEASE_KEYSTORE_PASSWORD`

The keystore password.

`ANDROID_RELEASE_KEY_ALIAS`

The key alias inside the keystore.

`ANDROID_RELEASE_KEY_PASSWORD`

The password for the release key.

## Google Play Publishing

`GOOGLE_PLAY_SERVICE_ACCOUNT_JSON_BASE64`

Base64-encoded contents of a Google Cloud service account JSON key that has access to this app in Play Console.

```sh
base64 -i play-service-account.json | pbcopy
```

On Linux:

```sh
base64 -w 0 play-service-account.json
```

The service account should be linked in Play Console under **Setup > API access** and granted permission to create releases for `org.centennialoss.consolation`.

## Workflow Behavior

`pull-request.yml` runs the debug build, lint, and tests with no release secrets.

`merge-main.yml` runs lint, tests, and an unsigned release build with no release secrets.

`release-preflight.yml` must be manually triggered from `main`. It validates all release secrets, creates a synthetic `0.0.0-preflight` version, builds signed APK/AAB artifacts, verifies them, and uploads them as workflow artifacts. It does not upload anything to Google Play and does not create a GitHub release.

`publish-release.yml` runs when a semantic version tag like `v1.2.3` is pushed. It builds signed APK/AAB artifacts, uploads the AAB to Google Play, and creates a draft GitHub release with the APK and AAB attached.

By default, release uploads target the Play `production` track with `DRAFT` status. Change `PLAY_TRACK` or `PLAY_RELEASE_STATUS` in `.github/workflows/publish-release.yml` if the release process should target another Play track or status.
