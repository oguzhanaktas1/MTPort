add_library(mtport-cli-lib STATIC CliCommands.cpp Doctor.cpp)
target_include_directories(mtport-cli-lib PUBLIC .)
target_link_libraries(mtport-cli-lib PUBLIC mtport-common mtport-fs)

add_executable(mtport main.cpp)
target_link_libraries(mtport PRIVATE mtport-cli-lib)
