# The mount worker always has the WebDAV transport (no macFUSE needed). The FSKit transport is added when
# macFUSE's libfuse is installed; without it only the object library below compile-checks the FUSE code.
add_executable(mtport-mount main.cpp)
target_link_libraries(mtport-mount PRIVATE mtport-common mtport-webdav "-framework CoreFoundation" "-framework IOKit")
if(AF_HAVE_FUSE)
  target_link_libraries(mtport-mount PRIVATE mtport-fuse-adapter "${AF_FUSE_LIB}")
  target_compile_definitions(mtport-mount PRIVATE MTPORT_FUSE_TRANSPORT=1)
elseif(AF_FUSE_COMPILE_ONLY)
  add_library(mtport-mount-compilecheck OBJECT main.cpp)
  target_compile_definitions(mtport-mount-compilecheck PRIVATE MTPORT_FUSE_TRANSPORT=1)
  target_link_libraries(mtport-mount-compilecheck PRIVATE mtport-fuse-adapter mtport-common mtport-webdav)
endif()
