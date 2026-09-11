VideoThumb v1.1 CMake/miniz build fix

Replace the CMakeLists.txt in the root of your existing VideoThumb project
with the included CMakeLists.txt.

Fix: AMALGAMATE_SOURCES is now OFF. This prevents miniz 3.1.2's own
create_zip/amalgamation target from being built. VideoThumb only needs the
normal static miniz library, so the amalgamation packaging target was both
unnecessary and the cause of the build failure.

After replacing the file in VS Code:
  1. CMake: Delete Cache and Reconfigure
  2. choose VS 2022 x64 Release preset if asked
  3. CMake: Build
