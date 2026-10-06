// Copyright (c) ScreenKit contributors. MIT.
//
// What an Android process has to do before its media player works, and the one
// number that shows it let go of everything afterwards.
#pragma once

#include <jni.h>

#include <cstddef>
#include <string>

namespace screenkit::media {

/// Resolve `dev.screenkit.media.VideoPlayer`, cache its method ids and register
/// its `native*` callbacks. **This has to run where the app's class loader is
/// reachable** -- `JNI_OnLoad`, on the thread that called `System.loadLibrary`
/// -- for the reason runtime/android/jni/HostMain.cpp spells out: the JS thread
/// is a native thread attached to the VM, and its `FindClass` reaches only the
/// system class loader.
///
/// Idempotent. False with `error` set when the player cannot be used;
/// `mediaAvailable()` then stays false and every load fails `Unavailable`.
bool prepareAndroidMedia(JavaVM* vm, JNIEnv* env, std::string& error);

/// Global references the media layer still holds: one per live Java player.
std::size_t liveMediaJavaRefCount();

}  // namespace screenkit::media
