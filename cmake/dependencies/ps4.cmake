#=================== ImGui ===================
# Rendering goes through Piglet (OpenGL ES 2.0), SDL2 is only used for input and timing.
find_package(SDL2 REQUIRED CONFIG)
target_link_libraries(ImGui PUBLIC SDL2::SDL2-static)
target_compile_definitions(ImGui PUBLIC IMGUI_IMPL_OPENGL_ES2)
set(USE_OPENGLES ON CACHE BOOL "" FORCE)

# imgui_impl_opengl3.cpp always prepends a "#version" line to its shaders; route its glShaderSource
# calls through Ps4GlShaderSourceNoVersion (Ps4Platform.cpp), which strips it.
set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp PROPERTIES
    COMPILE_DEFINITIONS "glShaderSource=Ps4GlShaderSourceNoVersion")
