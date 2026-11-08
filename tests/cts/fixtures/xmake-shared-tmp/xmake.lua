add_rules("mode.debug", "mode.release")
set_languages("c++23")

target("greet")
    set_kind("static")
    add_files("src/greet.cppm", {public = true})
    add_files("src/greet.cpp")

target("hello")
    set_kind("binary")
    add_deps("greet")
    add_files("src/main.cpp")
