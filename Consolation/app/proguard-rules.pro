# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

# Vendored UVC JNI (`org.centennialoss.consolation.uvc`) — JNI field layout must be preserved when minify is enabled.
-keep class org.centennialoss.consolation.uvc.UVCCamera {
    native <methods>;
}

-keepclassmembers class org.centennialoss.consolation.uvc.UVCCamera {
    protected long mNativePtr;

    protected int mScanningModeMin;
    protected int mScanningModeMax;
    protected int mScanningModeDef;

    protected int mExposureModeMin;
    protected int mExposureModeMax;
    protected int mExposureModeDef;

    protected int mExposurePriorityMin;
    protected int mExposurePriorityMax;
    protected int mExposurePriorityDef;

    protected int mExposureMin;
    protected int mExposureMax;
    protected int mExposureDef;

    protected int mAutoFocusMin;
    protected int mAutoFocusMax;
    protected int mAutoFocusDef;

    protected int mFocusMin;
    protected int mFocusMax;
    protected int mFocusDef;

    protected int mFocusRelMin;
    protected int mFocusRelMax;
    protected int mFocusRelDef;

    protected int mFocusSimpleMin;
    protected int mFocusSimpleMax;
    protected int mFocusSimpleDef;

    protected int mIrisMin;
    protected int mIrisMax;
    protected int mIrisDef;

    protected int mIrisRelMin;
    protected int mIrisRelMax;
    protected int mIrisRelDef;

    protected int mPanMin;
    protected int mPanMax;
    protected int mPanDef;

    protected int mTiltMin;
    protected int mTiltMax;
    protected int mTiltDef;

    protected int mRollMin;
    protected int mRollMax;
    protected int mRollDef;

    protected int mPanRelMin;
    protected int mPanRelMax;
    protected int mPanRelDef;

    protected int mTiltRelMin;
    protected int mTiltRelMax;
    protected int mTiltRelDef;

    protected int mRollRelMin;
    protected int mRollRelMax;
    protected int mRollRelDef;

    protected int mPrivacyMin;
    protected int mPrivacyMax;
    protected int mPrivacyDef;

    protected int mAutoWhiteBalanceMin;
    protected int mAutoWhiteBalanceMax;
    protected int mAutoWhiteBalanceDef;

    protected int mAutoWhiteBalanceCompoMin;
    protected int mAutoWhiteBalanceCompoMax;
    protected int mAutoWhiteBalanceCompoDef;

    protected int mWhiteBalanceMin;
    protected int mWhiteBalanceMax;
    protected int mWhiteBalanceDef;

    protected int mWhiteBalanceCompoMin;
    protected int mWhiteBalanceCompoMax;
    protected int mWhiteBalanceCompoDef;

    protected int mWhiteBalanceRelMin;
    protected int mWhiteBalanceRelMax;
    protected int mWhiteBalanceRelDef;

    protected int mBacklightCompMin;
    protected int mBacklightCompMax;
    protected int mBacklightCompDef;

    protected int mBrightnessMin;
    protected int mBrightnessMax;
    protected int mBrightnessDef;

    protected int mContrastMin;
    protected int mContrastMax;
    protected int mContrastDef;

    protected int mSharpnessMin;
    protected int mSharpnessMax;
    protected int mSharpnessDef;

    protected int mGainMin;
    protected int mGainMax;
    protected int mGainDef;

    protected int mGammaMin;
    protected int mGammaMax;
    protected int mGammaDef;

    protected int mSaturationMin;
    protected int mSaturationMax;
    protected int mSaturationDef;

    protected int mHueMin;
    protected int mHueMax;
    protected int mHueDef;

    protected int mZoomMin;
    protected int mZoomMax;
    protected int mZoomDef;

    protected int mZoomRelMin;
    protected int mZoomRelMax;
    protected int mZoomRelDef;

    protected int mPowerlineFrequencyMin;
    protected int mPowerlineFrequencyMax;
    protected int mPowerlineFrequencyDef;

    protected int mMultiplierMin;
    protected int mMultiplierMax;
    protected int mMultiplierDef;

    protected int mMultiplierLimitMin;
    protected int mMultiplierLimitMax;
    protected int mMultiplierLimitDef;

    protected int mAnalogVideoStandardMin;
    protected int mAnalogVideoStandardMax;
    protected int mAnalogVideoStandardDef;

    protected int mAnalogVideoLockStateMin;
    protected int mAnalogVideoLockStateMax;
    protected int mAnalogVideoLockStateDef;
}

-keep interface org.centennialoss.consolation.uvc.IButtonCallback {
    <methods>;
}

-keep interface org.centennialoss.consolation.uvc.IFrameCallback {
    <methods>;
}

-keep interface org.centennialoss.consolation.uvc.IStatusCallback {
    <methods>;
}

# Strip debug/info app logs from release bytecode.
-assumenosideeffects class org.centennialoss.consolation.logging.AppLog {
    public int v(java.lang.String,java.lang.String);
    public int v(java.lang.String,java.lang.String,java.lang.Throwable);
    public int d(java.lang.String,java.lang.String);
    public int d(java.lang.String,java.lang.String,java.lang.Throwable);
    public int i(java.lang.String,java.lang.String);
    public int i(java.lang.String,java.lang.String,java.lang.Throwable);
}