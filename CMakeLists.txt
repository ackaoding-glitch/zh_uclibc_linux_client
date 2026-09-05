cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0079)
  cmake_policy(SET CMP0079 NEW)
endif()

project(zh_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ZH_TARGET_PLATFORM "rv1106" CACHE STRING "Target platform: rv1106 or x86")
set_property(CACHE ZH_TARGET_PLATFORM PROPERTY STRINGS rv1106 x86)
string(TOLOWER "${ZH_TARGET_PLATFORM}" ZH_TARGET_PLATFORM)
string(TOUPPER "${ZH_TARGET_PLATFORM}" ZH_TARGET_PLATFORM_UPPER)

set(ZH_IS_RV1106 OFF)
set(ZH_IS_X86 OFF)
if (ZH_TARGET_PLATFORM STREQUAL "rv1106")
  set(ZH_IS_RV1106 ON)
elseif (ZH_TARGET_PLATFORM STREQUAL "x86")
  set(ZH_IS_X86 ON)
else()
  message(FATAL_ERROR "Unsupported ZH_TARGET_PLATFORM=${ZH_TARGET_PLATFORM}. Use rv1106 or x86.")
endif()

set(ZH_APP_VERSION "dev-local" CACHE STRING "Application version")
set(ZH_GIT_SHA "unknown" CACHE STRING "Git short sha")
set(ZH_CARD_NAME "wlan0" CACHE STRING "Network interface name")
set(ZH_AUDIO_DEVICE "hw:0,0" CACHE STRING "ALSA capture device")
set(ZH_TTS_PLAY_DEVICE "plughw:0,0" CACHE STRING "ALSA playback device")
set(ZH_AUDIO_CHANNELS "2" CACHE STRING "Capture channel count")
set(ZH_AUDIO_FRAME_MS "20" CACHE STRING "Capture frame length in milliseconds")
set(ZH_AUDIO_MIC_CHANNEL_INDEX "0" CACHE STRING "0-based capture channel selected for VAD/Opus uplink")
set(ZH_AUDIO_REF_CHANNEL_INDEX "1" CACHE STRING "0-based capture channel used as AEC reference")
set(ZH_AUDIO_REF_CHANNELS "0" CACHE STRING "AEC reference channel count")
set(ZH_AUDIO_AEC_FRAME_SAMPLES "256" CACHE STRING "AEC backend frame length in samples")
set(ZH_ALSA_BUFFER_PERIODS "4" CACHE STRING "ALSA buffer size in periods")
set(ZH_ENABLE_AEC "1" CACHE STRING "Enable AEC/audio preprocess capability")

if (ZH_IS_RV1106)
  set(ZH_AUDIO_BACKEND_ROCKIT_DEFAULT ON)
  set(ZH_ENABLE_FACE_DEFAULT ON)
  set(ZH_BUILD_BLE_GATT_SERVER_DEFAULT ON)
else()
  set(ZH_AUDIO_BACKEND_ROCKIT_DEFAULT OFF)
  set(ZH_ENABLE_FACE_DEFAULT OFF)
  set(ZH_BUILD_BLE_GATT_SERVER_DEFAULT OFF)
endif()
option(ZH_AUDIO_BACKEND_ROCKIT "Use RV1106 Rockit/RK_MPI audio backend" ${ZH_AUDIO_BACKEND_ROCKIT_DEFAULT})
option(ZH_ENABLE_FACE "Enable face recognition integration" ${ZH_ENABLE_FACE_DEFAULT})
option(ZH_ENABLE_VISION_CAPTURE "Enable request-driven vision snapshot upload" OFF)
option(ZH_BUILD_BLE_GATT_SERVER "Build bundled BLE GATT provisioning helper" ${ZH_BUILD_BLE_GATT_SERVER_DEFAULT})
option(ZH_BUILD_FACE_TEST "Build face recognition test target" ${ZH_ENABLE_FACE_DEFAULT})

configure_file(
    ${CMAKE_SOURCE_DIR}/include/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

set(ZH_CLIENT_SOURCES
    src/main.c
    src/app.c
    src/audio_uplink.c
    src/audio_capture_alsa.c
    src/audio_capture_rockit.c
    src/audio_device.c
    src/audio_playback_ref.c
    src/audio_preprocess_none.c
    src/bithion_core_compat.c
    src/ble_provision.c
    src/wifi_bootstrap.c
    src/music_player.c
    src/prompt_tone.c
    src/udp_tts.c
    src/vision_capture.c
    src/config.c
    src/utils.c
    src/sntp.c
    src/audio_playback_rockit.c
    src/audio_playback_alsa.c
)

if (ZH_ENABLE_FACE)
  list(APPEND ZH_CLIENT_SOURCES src/face_recognition.c)
else()
  list(APPEND ZH_CLIENT_SOURCES src/face_recognition_stub.c)
endif()

add_executable(zh_client ${ZH_CLIENT_SOURCES})

set(ZH_CLIENT_WORKER_SOURCES ${ZH_CLIENT_SOURCES})
list(REMOVE_ITEM ZH_CLIENT_WORKER_SOURCES src/main.c)

if(DEFINED ENV{BITHION_CORE_SDK_ROOT})
  set(BITHION_CORE_SDK_ROOT "$ENV{BITHION_CORE_SDK_ROOT}" CACHE PATH "bithion-core SDK root")
else()
  set(BITHION_CORE_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/core-sdk CACHE PATH "bithion-core SDK root")
endif()
set(BITHION_CORE_SDK_LIB "" CACHE PATH "bithion-core SDK lib dir")
if (NOT BITHION_CORE_SDK_LIB)
  set(BITHION_CORE_SDK_LIB ${BITHION_CORE_SDK_ROOT}/lib)
  set(BITHION_CORE_SDK_LIB ${BITHION_CORE_SDK_LIB} CACHE PATH "bithion-core SDK lib dir" FORCE)
endif()
set(BITHION_CORE_INCLUDE_DIR ${BITHION_CORE_SDK_ROOT}/include CACHE PATH "bithion-core SDK include dir")
find_library(BITHION_CORE_LIBRARY
  NAMES bithion-core libbithion-core
  PATHS ${BITHION_CORE_SDK_LIB}
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH
  REQUIRED
)
add_library(bithion-core::bithion-core SHARED IMPORTED)
set_target_properties(bithion-core::bithion-core PROPERTIES
  IMPORTED_LOCATION ${BITHION_CORE_LIBRARY}
  INTERFACE_INCLUDE_DIRECTORIES ${BITHION_CORE_INCLUDE_DIR}
)

set(ZH_PREBUILT_ROOT "" CACHE PATH "Platform prebuilt dependency root")
option(ZH_USE_SYSROOT_ALSA "Use ALSA library from the active CMake sysroot instead of ALSA_PREBUILT_DIR" OFF)
if (NOT ZH_PREBUILT_ROOT)
  if (ZH_IS_RV1106)
    set(ZH_PREBUILT_ROOT ${CMAKE_SOURCE_DIR}/third_party/prebuilt/rv1106-uclibc-armv7)
  endif()
endif()

if (ZH_PREBUILT_ROOT AND EXISTS "${ZH_PREBUILT_ROOT}")
  set(OPUS_PREBUILT_DIR ${ZH_PREBUILT_ROOT}/opus CACHE PATH "Opus prebuilt dir" FORCE)
  set(ALSA_PREBUILT_DIR ${ZH_PREBUILT_ROOT}/alsa CACHE PATH "ALSA prebuilt dir" FORCE)
elseif (ZH_IS_RV1106)
  set(OPUS_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/third_party/opus_prebuilt CACHE PATH "Opus prebuilt dir")
  set(ALSA_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/third_party/alsa_prebuilt_rk CACHE PATH "ALSA prebuilt dir")
elseif (ZH_IS_X86)
  set(OPUS_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/third_party/opus_prebuilt CACHE PATH "Opus prebuilt dir")
  set(ALSA_PREBUILT_DIR "" CACHE PATH "ALSA prebuilt dir")
endif()

set(OPUS_LIBRARY ${OPUS_PREBUILT_DIR}/lib/libopus.a CACHE FILEPATH "Opus library")
set(ALSA_INCLUDE_DIR ${ALSA_PREBUILT_DIR}/include CACHE PATH "ALSA include dir")
if (ZH_USE_SYSROOT_ALSA)
  set(_ZH_ALSA_SYSROOT_SEARCH_PATHS
    ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu
    ${CMAKE_SYSROOT}/lib/aarch64-linux-gnu
    ${CMAKE_SYSROOT}/usr/lib
    ${CMAKE_SYSROOT}/lib
  )
  find_library(ALSA_LIBRARY
    NAMES asound libasound.so libasound.so.2 libasound.so.2.0.0
    PATHS ${_ZH_ALSA_SYSROOT_SEARCH_PATHS}
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
else()
  find_library(ALSA_LIBRARY
    NAMES asound libasound.so
    PATHS ${ALSA_PREBUILT_DIR}/lib
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
endif()
if (NOT ALSA_LIBRARY AND ZH_IS_X86)
  find_library(ALSA_LIBRARY NAMES asound REQUIRED)
endif()
if (NOT ALSA_LIBRARY)
  message(FATAL_ERROR "ALSA library not found. Set ALSA_PREBUILT_DIR or ZH_PREBUILT_ROOT.")
endif()
if (NOT EXISTS "${OPUS_LIBRARY}")
  message(FATAL_ERROR "Opus library not found at ${OPUS_LIBRARY}. Set OPUS_PREBUILT_DIR or ZH_PREBUILT_ROOT.")
endif()

# rkmedia/rockit SDK（必须显式指定，如 -DRK_SDK_ROOT=/rv1106-sdk）
if(DEFINED ENV{RK_SDK_ROOT})
  set(RK_SDK_ROOT "$ENV{RK_SDK_ROOT}" CACHE PATH "Rockchip SDK root")
else()
  set(RK_SDK_ROOT "" CACHE PATH "Rockchip SDK root")
endif()
if (EXISTS "${RK_SDK_ROOT}/media/out/lib")
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/media/out)
elseif (EXISTS "${RK_SDK_ROOT}/output/out/media_out/lib")
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/output/out/media_out)
else()
  set(RK_MEDIA_ROOT ${RK_SDK_ROOT}/media/out)
endif()
set(RK_MEDIA_INCLUDE ${RK_MEDIA_ROOT}/include)
set(RK_ROCKIT_INCLUDE ${RK_SDK_ROOT}/media/rockit/out/include)
set(RK_ROCKIT_MPI_INCLUDE ${RK_SDK_ROOT}/media/rockit/rockit/mpi/sdk/include)
set(RK_MEDIA_LIB ${RK_MEDIA_ROOT}/lib)
set(RK_OEM_LIB ${RK_MEDIA_ROOT}/oem/usr/lib)
set(RK_SDK_INCLUDE_DIRS ${RK_MEDIA_INCLUDE})
if (EXISTS "${RK_ROCKIT_INCLUDE}")
  list(APPEND RK_SDK_INCLUDE_DIRS ${RK_ROCKIT_INCLUDE})
endif()
if (EXISTS "${RK_ROCKIT_MPI_INCLUDE}")
  list(APPEND RK_SDK_INCLUDE_DIRS ${RK_ROCKIT_MPI_INCLUDE})
endif()

if (EXISTS ${RK_MEDIA_LIB}/librga.so)
  set(RK_LIBRGA ${RK_MEDIA_LIB}/librga.so)
elseif (EXISTS ${RK_SDK_ROOT}/media/rga/out/lib/librga.so)
  set(RK_LIBRGA ${RK_SDK_ROOT}/media/rga/out/lib/librga.so)
else()
  set(RK_LIBRGA ${RK_MEDIA_LIB}/librga.so)
endif()

if (ZH_AUDIO_BACKEND_ROCKIT)
  if (NOT ZH_IS_RV1106)
    message(FATAL_ERROR "ZH_AUDIO_BACKEND_ROCKIT is only supported for ZH_TARGET_PLATFORM=rv1106 in this tree.")
  endif()
  if (NOT RK_SDK_ROOT)
    message(FATAL_ERROR "RK_SDK_ROOT is required for rv1106 builds (Rockchip SDK root, e.g. -DRK_SDK_ROOT=/rv1106-sdk).")
  endif()
  target_link_directories(zh_client PRIVATE ${RK_MEDIA_LIB} ${RK_OEM_LIB})
  target_link_libraries(zh_client PRIVATE
    ${RK_MEDIA_LIB}/librockit_full.so
    ${RK_MEDIA_LIB}/librockchip_mpp.so
    ${RK_MEDIA_LIB}/libdrm.so
    ${RK_LIBRGA}
    ${RK_MEDIA_LIB}/libaec_bf_process.so
    ${RK_MEDIA_LIB}/librkaudio_common.so
  )
  target_link_options(zh_client PRIVATE -Wl,-rpath,/oem/usr/lib)
endif()

target_link_options(zh_client PRIVATE
  "-Wl,-rpath-link,${BITHION_CORE_SDK_LIB}"
)

target_link_libraries(zh_client PRIVATE
  bithion-core::bithion-core
  ${OPUS_LIBRARY}
  ${ALSA_LIBRARY}
  m
  pthread
  dl
)

target_compile_definitions(zh_client PRIVATE
  ZH_TARGET_PLATFORM_${ZH_TARGET_PLATFORM_UPPER}=1
  ZH_AUDIO_BACKEND_ROCKIT=$<BOOL:${ZH_AUDIO_BACKEND_ROCKIT}>
  ZH_ENABLE_FACE=$<BOOL:${ZH_ENABLE_FACE}>
  ZH_ENABLE_VISION_CAPTURE=$<BOOL:${ZH_ENABLE_VISION_CAPTURE}>
  ZH_ENABLE_AEC=${ZH_ENABLE_AEC}
  ZH_CARD_NAME="${ZH_CARD_NAME}"
  ZH_AUDIO_DEVICE="${ZH_AUDIO_DEVICE}"
  ZH_TTS_PLAY_DEVICE="${ZH_TTS_PLAY_DEVICE}"
  ZH_AUDIO_CHANNELS=${ZH_AUDIO_CHANNELS}
  ZH_AUDIO_FRAME_MS=${ZH_AUDIO_FRAME_MS}
  ZH_AUDIO_MIC_CHANNEL_INDEX=${ZH_AUDIO_MIC_CHANNEL_INDEX}
  ZH_AUDIO_REF_CHANNEL_INDEX=${ZH_AUDIO_REF_CHANNEL_INDEX}
  ZH_AUDIO_REF_CHANNELS=${ZH_AUDIO_REF_CHANNELS}
  ZH_AUDIO_AEC_FRAME_SAMPLES=${ZH_AUDIO_AEC_FRAME_SAMPLES}
  ZH_ALSA_BUFFER_PERIODS=${ZH_ALSA_BUFFER_PERIODS}
)
if (ZH_IS_RV1106)
  target_compile_definitions(zh_client PRIVATE
    ZH_PLATFORM_RV1106=1
    ZH_ARCH_ARMV7=1
    ZH_LIBC_UCLIBC=1
    ZH_ENABLE_BAIDU_AUDIO_CAPABILITY=1
  )
elseif (ZH_IS_X86)
  target_compile_definitions(zh_client PRIVATE ZH_PLATFORM_X86=1)
endif()

if (ZH_BUILD_BLE_GATT_SERVER)
  add_executable(zh_ble_gatt_server
    ble_sdk/btgatt-server.c
    ble_sdk/uuid-helper.c)
  set(RK_BLE_TEST_ROOT ${RK_SDK_ROOT}/sysdrv/packages/ble_test)
  target_link_libraries(zh_ble_gatt_server PRIVATE
    ${RK_BLE_TEST_ROOT}/libs/libbluetooth-internal.a
    ${RK_BLE_TEST_ROOT}/libs/libshared-mainloop.a
    pthread rt dl m)
  target_include_directories(zh_ble_gatt_server PRIVATE
    include
    ${BITHION_CORE_INCLUDE_DIR}
    ${RK_BLE_TEST_ROOT}/src/include
    ${RK_BLE_TEST_ROOT}/src/include/lib)
endif()

if (ZH_BUILD_FACE_TEST)
  add_executable(zh_face_test
      tests/face_test.c
      src/utils.c
      src/config.c
      src/face_recognition.c
  )
  target_link_options(zh_face_test PRIVATE
    "-Wl,-rpath-link,${BITHION_CORE_SDK_LIB}"
  )
  target_link_libraries(zh_face_test PRIVATE
    bithion-core::bithion-core
    m
    pthread
  )
  target_include_directories(zh_face_test PRIVATE
      ${CMAKE_SOURCE_DIR}
      include
      ${BITHION_CORE_INCLUDE_DIR}
      ${OPUS_PREBUILT_DIR}/include
      ${ALSA_INCLUDE_DIR}
  )
endif()

target_include_directories(zh_client PRIVATE
    ${CMAKE_SOURCE_DIR}
    include
    ${CMAKE_BINARY_DIR}/generated
    ${BITHION_CORE_INCLUDE_DIR}
    third_party/minimp3
    ${OPUS_PREBUILT_DIR}/include
    ${ALSA_INCLUDE_DIR}
    ${RK_SDK_INCLUDE_DIRS}
)
