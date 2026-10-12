# FUSE adapter: needs the macFUSE (libfuse 2.9 API) headers. With MTPORT_FUSE_INCLUDE_DIR
# set and macFUSE absent, the adapter is built and unit-tested *without* linking libfuse
# (callbacks are plain functions), but no mount worker can exist.
if(AF_HAVE_FUSE OR AF_FUSE_COMPILE_ONLY)
  if(AF_HAVE_FUSE)
    set(AF_FUSE_HDR_DIR "${AF_FUSE_INCLUDE}")
  else()
    set(AF_FUSE_HDR_DIR "${MTPORT_FUSE_INCLUDE_DIR}")
  endif()
  add_library(mtport-fuse-adapter STATIC MtpFuseAdapter.cpp)
  target_include_directories(mtport-fuse-adapter SYSTEM PUBLIC "${AF_FUSE_HDR_DIR}")
  target_include_directories(mtport-fuse-adapter PUBLIC .)
  target_link_libraries(mtport-fuse-adapter PUBLIC mtport-fs)
  target_compile_definitions(mtport-fuse-adapter PUBLIC FUSE_USE_VERSION=29 _FILE_OFFSET_BITS=64)
  set(AF_FUSE_ADAPTER_AVAILABLE ON PARENT_SCOPE)
endif()
