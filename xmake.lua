-- Real Videos: play real video files (MP4 etc.) on in-game screens in Fallout 4.
-- Built against Dear-Modding CommonLibF4. Primary target: Fallout 4 OG (1.10.163).
includes("lib/commonlibf4")

set_project("RealVideos")
set_version("0.2.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")
set_allowedarchs("windows|x64")
set_allowedmodes("debug", "releasedbg")
set_defaultarchs("windows|x64")
set_defaultmode("releasedbg")

add_rules("mode.debug", "mode.releasedbg")

-- REL::ID / REL::Offset initializer slots are [OG, NG, AE].
add_defines("COMMONLIB_RUNTIMECOUNT=3")

target("RealVideos", function()
    add_rules("commonlibf4.plugin", {
        name = "RealVideos",
        author = "evodube",
        description = "Plays video files on in-game TVs.",
        plugin_template = path.join(os.projectdir(), "res/commonlibf4-plugin.cpp.in"),
    })

    -- don't run `xmake install` after every build (CI has no game folder)
    set_values("commonlib.plugin.install", false)

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

    add_defines(
        "_UNICODE",
        "UNICODE",
        "NOMINMAX",
        "_CRT_SECURE_NO_WARNINGS",
        "_WIN32_WINNT=0x0A00",
        "WINVER=0x0A00"
    )

    add_cxxflags(
        "/Zi",
        "/permissive-",
        "/Zc:preprocessor",
        "/EHsc",
        "/wd4100", -- unreferenced parameter
        "/wd4189", -- local variable initialized but not referenced
        "/wd4244", -- conversion, possible loss of data
        "/wd4267", -- size_t conversion
        "/wd5054"  -- enum bitwise ops
    )

    add_syslinks("mfplat", "mfreadwrite", "mfuuid", "ole32", "xaudio2", "d3d11", "dxgi", "user32", "kernel32")

    set_pcxxheader("src/PCH.h")
    set_runtimes("MD")
    set_symbols("debug")
    set_optimize("fastest")
    set_targetdir("build/out/F4SE/Plugins")
end)
