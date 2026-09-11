# VideoThumb v1.4.1 warning cleanup

Build-only cleanup; runtime behavior is unchanged from v1.4.

Changes:
- Removes `MINIZ_NO_DEFLATE_APIS` from our compile definitions. miniz already disables ZIP writing; defining `MINIZ_NO_DEFLATE_APIS` caused `miniz.h` to define `MINIZ_NO_ARCHIVE_WRITING_APIS` a second time, producing MSVC C4005 warnings.
- Marks the FFmpeg include directory as `SYSTEM` and uses MSVC `/external:W0`, so warnings originating inside FFmpeg headers (the C4244 conversions in `libavutil/common.h`) do not pollute the build.
- VideoThumb's own source remains at `/W4`; warnings in our code are NOT globally disabled.

Install: overwrite the project-root `CMakeLists.txt`, then run:

```powershell
cmake --build .\build\vs2022-x64 --config Release --clean-first
```

If CMake notices the project version change and reconfigures automatically, that's fine. If not, run `CMake: Delete Cache and Reconfigure` once, then build again.
