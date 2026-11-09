add_rules("mode.debug", "mode.release")
set_languages("c++23")

-- An option of the project's own: the user's `xmake f --fancy=y` (kept in .xmake/…/xmake.conf) is followed too.
option("fancy", {default = false, showmenu = true, description = "Enable the fancy part"})

target("hello")
    set_kind("binary")
    set_toolchains("gcc")
    add_files("src/*.cpp", "src/*.cppm")
    if has_config("fancy") then
        add_defines("FANCY_PART")
    end
