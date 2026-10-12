add_library(mtport-fake STATIC FakeMtpDevice.cpp HostDirMtpDevice.cpp)
target_include_directories(mtport-fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mtport-fake PUBLIC mtport-core)
