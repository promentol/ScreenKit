# R8 keep rules for the ScreenKit APK.
#
# Everything here is code that native code reaches **by name**, which R8 cannot
# see: a renamed or removed class or method is then a `NoSuchMethodError` from
# `GetMethodID`, or a `RegisterNatives` failure, the first time the path runs --
# a silent total failure of the network layer in a minified build.
#
# `sh tools/android/android.sh test` with MINIFY=1 runs every net row against an
# R8-processed APK, which is what checks this file.

# ---- ours ---------------------------------------------------------------------

# runtime/core/src/net/NetServiceAndroid.cpp binds HttpClient's constructor and
# public methods with GetMethodID and its `native*` callbacks with
# RegisterNatives, all by name and signature.
-keep class dev.screenkit.net.HttpClient { *; }

# runtime/core/src/media/MediaPlayerAndroid.cpp binds VideoPlayer's constructor,
# its public methods and static capabilities() with GetMethodID, reads its
# ERR_COUNT with GetStaticFieldID, and registers its `native*` callbacks -- all
# by name and signature. Media3 itself ships consumer rules in its AARs.
-keep class dev.screenkit.media.VideoPlayer { *; }

# Any class with native methods keeps them under their own names: the JNI
# symbol or RegisterNatives entry is the only thing that refers to them.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# ---- the prebuilt AARs that ship no rules of their own -------------------------
#
# SDL3's AAR carries a proguard.txt and OkHttp's jar its own rules; these two do
# not.

# hermes-android: libhermesvm calls its Intl and Unicode helpers
# (com.facebook.hermes.intl.*, com.facebook.hermes.unicode.*) through fbjni by
# name -- `toLocaleUpperCase` and every `Intl` constructor go through them.
# React Native's own rules keep the same package.
-keep class com.facebook.hermes.** { *; }
# ...whose classes carry React Native's @DoNotStrip from
# com.facebook.proguard.annotations, a package this build does not have (it
# ships in react-android). The rule above keeps them whole, so the annotation
# has nothing left to do; R8 only has to be told it is absent on purpose.
-dontwarn com.facebook.proguard.annotations.**

# fbjni: its Java half is found from native code by name, and it marks what it
# needs with its own @DoNotStrip.
-keep class com.facebook.jni.** { *; }
-keep @com.facebook.jni.annotations.DoNotStrip class *
-keepclassmembers class * {
    @com.facebook.jni.annotations.DoNotStrip *;
}
