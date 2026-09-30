add_rules("mode.debug", "mode.release")

target("hello")
    set_kind("binary")
    set_toolchains("gcc")
    add_files("src/*.cpp", "src/*.cppm")
