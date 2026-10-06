// Copyright (c) ScreenKit contributors. MIT.
//
// What the Android host has to do before the network client works, and the one
// number the suite needs to see that it let go of everything afterwards.
#pragma once

#include <jni.h>

#include <cstddef>
#include <string>

namespace screenkit::net {

/// The JavaVM the client calls into. Called from `JNI_OnLoad`, which is the only
/// place a `JavaVM*` is handed out.
void setJavaVm(JavaVM* vm);

/// Resolve `dev.screenkit.net.HttpClient`, cache its method ids and register the
/// callbacks. **This has to run where the app's class loader is reachable** --
/// `JNI_OnLoad`, on the thread that called `System.loadLibrary` -- because the JS
/// thread and the I/O queue are native threads attached to the VM and their
/// `FindClass` reaches only the system class loader.
///
/// Idempotent, and false with `error` set when the client cannot be used;
/// `networkAvailable()` then stays false and every request fails `unsupported`.
bool prepareAndroidNetwork(JNIEnv* env, std::string& error);

/// JNI references and call registrations this client still holds: one per live
/// `HttpClient` and one per request or socket it has not yet retired. A leak has
/// nothing else to show -- the Java object and its connection simply never go --
/// so the Android-only rows count this.
std::size_t liveJavaRefCount();

}  // namespace screenkit::net
