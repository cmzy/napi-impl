# Android cross-compile toolchain — delegates to the NDK's own toolchain file.
#
# Inputs (set by scripts/build.py or the environment):
#   ANDROID_NDK_ROOT / ANDROID_NDK   path to the NDK
#   ANDROID_ABI                      arm64-v8a (default) | x86_64
#   ANDROID_PLATFORM                 android-21 (default; minSdk 21)
#
# NOTE: Hermes is built standalone for the target, but it needs a *host* hermesc
# to compile its baked-in JS. scripts/build.py builds host hermesc first and
# passes -DIMPORT_HERMESC=<host build>/ImportHermesc.cmake to the Hermes
# configure when cross-compiling.

if(NOT ANDROID_NDK)
  if(DEFINED ENV{ANDROID_NDK_ROOT})
    set(ANDROID_NDK $ENV{ANDROID_NDK_ROOT})
  elseif(DEFINED ENV{ANDROID_NDK_HOME})
    set(ANDROID_NDK $ENV{ANDROID_NDK_HOME})
  endif()
endif()
if(NOT ANDROID_NDK)
  message(FATAL_ERROR "ANDROID_NDK not set (export ANDROID_NDK_ROOT)")
endif()

if(NOT ANDROID_ABI)
  set(ANDROID_ABI "arm64-v8a")
endif()
if(NOT ANDROID_PLATFORM)
  set(ANDROID_PLATFORM "android-21")
endif()

include(${ANDROID_NDK}/build/cmake/android.toolchain.cmake)
