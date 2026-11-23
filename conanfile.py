from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class LwsConan(ConanFile):
    name = "lws"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # fmt is the one runtime requirement of lws_core (formatting in the core, never std::format).
        self.requires("fmt/12.2.0")

    def build_requirements(self):
        # Test-only dependency: linked into the test executables, never into the library.
        # Further packages arrive in later phases together with the code that needs them.
        self.test_requires("gtest/1.15.0")

    def generate(self):
        tc = CMakeToolchain(self)
        # Keep Conan from writing CMakeUserPresets.json into the repository root.
        tc.user_presets_path = False
        tc.generate()
        CMakeDeps(self).generate()
