-- CONFIGURE_MISSING
add_rules("mode.debug", "mode.release")
set_languages("c++20")
add_requires("libsdl3")

option("fancy", {default = false, description = "A project option"})

target("hello")
    set_kind("binary")
    add_files("src/*.cpp")
