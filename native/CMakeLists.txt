# ---- mtport-core ------------------------------------------------------------------
set(CORE_SOURCES
  core/src/RuntimePaths.cpp
  core/src/DeviceLock.cpp
  core/src/Sha256.cpp)
if(AF_HAVE_LIBMTP)
  list(APPEND CORE_SOURCES
    core/src/MtpSessionGuard.cpp
    core/src/DeviceEnumerator.cpp
    core/src/LibmtpDevice.cpp
    core/src/LibmtpDeviceFactory.cpp)
endif()
add_library(mtport-core STATIC ${CORE_SOURCES})
target_include_directories(mtport-core PUBLIC core/include)
target_include_directories(mtport-core PRIVATE core/src)
if(AF_HAVE_LIBMTP)
  target_link_libraries(mtport-core PUBLIC PkgConfig::LIBMTP)
  target_compile_definitions(mtport-core PUBLIC MTPORT_HAVE_LIBMTP=1)
endif()

# ---- mtport-fs --------------------------------------------------------------------
add_library(mtport-fs STATIC
  filesystem/src/PosixErrorMapper.cpp
  filesystem/src/VirtualPathMapper.cpp
  filesystem/src/MetadataCache.cpp
  filesystem/src/FileHandleTable.cpp
  filesystem/src/WriteStaging.cpp
  filesystem/src/ReadStageCache.cpp
  filesystem/src/MtpFilesystem.cpp)
target_include_directories(mtport-fs PUBLIC filesystem/include)
target_link_libraries(mtport-fs PUBLIC mtport-core)
target_link_libraries(mtport-fs PRIVATE "-framework CoreFoundation")

# ---- common helpers (JSON, process spawning, mount state files) ---------------------------
add_library(mtport-common STATIC
  common/Json.cpp
  common/Process.cpp
  common/MountState.cpp
  common/Platform.cpp
  common/UsbPresence.cpp
  common/Backend.cpp)
target_include_directories(mtport-common PUBLIC common)
target_link_libraries(mtport-common PUBLIC mtport-core "-framework IOKit" "-framework CoreFoundation")
if(TARGET mtport-fake)
  target_link_libraries(mtport-common PUBLIC mtport-fake)
  target_compile_definitions(mtport-common PUBLIC MTPORT_HAVE_MOCK=1)
endif()

add_subdirectory(cli)
add_subdirectory(webdav)
add_subdirectory(fuse)
add_subdirectory(mount)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/agent/CMakeLists.txt)
  add_subdirectory(agent)
endif()
