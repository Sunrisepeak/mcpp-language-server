add_rules("mode.debug", "mode.release")
set_languages("c++23")

target("hello")
    set_kind("binary")
    set_toolchains("gcc")
    add_files("src/*.cpp", "src/*.cppm")
