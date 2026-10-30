#pragma once

// Stand-in for GLFW's glfw3native.h, used only by Dear ImGui's GLFW backend when C++ is compiled by GCC on macOS (see
// src/app/CMakeLists.txt). Outside Objective-C, the real header exposes the Cocoa functions through
// <ApplicationServices/ApplicationServices.h>, whose declarations use Clang blocks ('^') that GCC cannot parse.
// The backend only needs the NSWindow of a window: an Objective-C 'id', declared here as an opaque pointer (same C
// ABI, the backend converts it to 'void*' anyway).

#include <GLFW/glfw3.h>

extern "C" {
GLFWAPI void* glfwGetCocoaWindow(GLFWwindow* window);
}
