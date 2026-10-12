add_library(mtport-webdav STATIC HttpServer.cpp WebDavService.cpp WebDavMount.cpp LoopbackName.cpp)
target_include_directories(mtport-webdav PUBLIC .)
target_link_libraries(mtport-webdav PUBLIC mtport-fs "-framework NetFS" "-framework CoreFoundation")
