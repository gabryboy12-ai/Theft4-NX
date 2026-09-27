project("u8extract")
set(CMAKE_CXX_STANDARD 17)

find_package(ZLIB REQUIRED)

add_executable(u8extract
    "u8extract.cpp"
)

target_link_libraries(u8extract PRIVATE
    ZLIB::ZLIB
)
