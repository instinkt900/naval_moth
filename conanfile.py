from conan import ConanFile
from conan.tools.cmake import cmake_layout

class NavalMoth(ConanFile):
    name = "naval_moth"
    version = "0.1"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps", "MSBuildToolchain", "MSBuildDeps"

    def requirements(self):
        self.requires("moth_graphics/[>=1.1.0]")
        self.requires("box2d/2.4.1")
        self.requires("entt/3.13.2")

    def build_requirements(self):
        self.tool_requires("cmake/[>=3.27.0]")

    def layout(self):
        cmake_layout(self)
