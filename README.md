# VideoThumb v1.4.3

Fast 64-bit video thumbnail plugin for Total Commander.

VideoThumb was created primarily to solve a Windows / Total Commander edge case where
video thumbnails fail when files are accessed through a local directory symbolic link
whose target is an SMB/UNC network share.

Conceptually:

Local directory symlink
        ↓
SMB / UNC network share

Windows Explorer and standard Total Commander thumbnail providers may fail to generate
video thumbnails through this path even though the same files work correctly when the
UNC path is opened directly.

VideoThumb bypasses the Windows Shell thumbnail path and decodes thumbnails directly
with FFmpeg libraries.


## Features

- Native 64-bit Total Commander WLX plugin
- Direct FFmpeg/libavformat/libavcodec decoding
- No ffmpeg.exe process is started for every thumbnail
- Works with tested local-directory-symlink → UNC/SMB paths
- In-memory LRU thumbnail cache
- Configurable parallel decoding
- Network I/O timeout protection
- ZIP image-sequence support
  - A file may have an .mp4 extension but actually contain a ZIP archive of images
  - VideoThumb can use the naturally first decodable image as its thumbnail
- Smart thumbnail selection
  - rejects black frames
  - rejects white or nearly uniform frames
  - rejects single-color backgrounds
  - rejects title-card-like frames
  - rejects low-detail fade / transition frames
  - tries later positions automatically and selects a better candidate
- Total Commander can additionally store the generated thumbnails in its own persistent
  thumbnail database


## Supported video formats

VideoThumb uses FFmpeg, so common formats such as the following are supported:

- MP4
- MKV
- AVI
- MOV
- WebM
- WMV
- M4V
- MPEG / MPG
- TS / M2TS

Actual codec/container support depends on the FFmpeg build used with the plugin.


## Installation

Copy the release contents to a directory such as:

<Total Commander directory>\Plugins\wlx\VideoThumb\

The directory should contain:

VideoThumb.wlx64
VideoThumb.ini

avcodec-*.dll
avformat-*.dll
avutil-*.dll
swscale-*.dll
swresample-*.dll

Then in Total Commander:

Configuration
→ Options
→ Plugins
→ Lister plugins (.WLX)
→ Add

Select:

VideoThumb.wlx64

Then open:

Configuration
→ Options
→ Thumbnails

Make sure the required video extensions are enabled for Lister-plugin thumbnail generation.


## Configuration

VideoThumb.ini controls performance and thumbnail selection.

Example:

[Performance]
CacheMB=96
CacheEntries=256
MaxParallel=2
DecoderThreads=2
TimeoutMs=12000
PacketLimit=5000

[Thumbnail]
SeekMs=1000

BlackFrameDetection=1
BlackThreshold=20
BlackPixelPercent=95

TitleCardDetection=1
TitleCardDarkPercent=70
TitleCardMidtoneMaxPercent=20
TitleCardMeanLumaMax=70

UniformFrameDetection=1
MaxLumaStdDev=18
MaxColorStdDev=16
DominantColorPercent=82

TransitionDetection=1
MinEdgePercent=2
EdgeThreshold=20
MaxMeanGradient=7

FallbackSeekMs=3000,5000,10000
FallbackPercent=10

[ZipSequence]
Enabled=1
MaxEntryMB=64
MaxCandidates=16
MaxSourceMP=100

[Compatibility]
ResolveSymlinkFallback=0

The default settings are intentionally conservative and are suitable for older CPUs and
SMB/network storage.


## Configuration reference

### [Performance]

CacheMB
Maximum size of VideoThumb's in-memory LRU thumbnail cache, in megabytes.

CacheEntries
Maximum number of cached thumbnail entries.

MaxParallel
Maximum number of thumbnails that may be decoded concurrently.

DecoderThreads
Maximum number of decoder threads used per FFmpeg decoder context.

TimeoutMs
Maximum time allowed for libav/FFmpeg I/O before the operation is aborted.

PacketLimit
Safety limit on the number of media packets processed while looking for a usable frame.


### [Thumbnail]

SeekMs
Initial seek position used for selecting a representative video frame.
1000 means 1 second.

BlackFrameDetection
Enables detection of nearly black frames.

BlackThreshold
Luma threshold below which a sampled pixel is considered black.

BlackPixelPercent
Percentage of black pixels required before a frame is classified as a black frame.

TitleCardDetection
Enables detection of title-card-like frames.

TitleCardDarkPercent
Percentage of dark pixels used by title-card detection.

TitleCardMidtoneMaxPercent
Maximum percentage of midtone pixels allowed for title-card classification.

TitleCardMeanLumaMax
Maximum average luma for title-card classification.

UniformFrameDetection
Enables detection of nearly uniform frames of any color.

MaxLumaStdDev
Maximum luma standard deviation for a frame to be considered too uniform.

MaxColorStdDev
Maximum color-channel standard deviation for a frame to be considered too uniform.

DominantColorPercent
Percentage threshold for determining whether one color dominates the frame.

TransitionDetection
Enables detection of low-detail fade and transition frames.

MinEdgePercent
Minimum percentage of sampled pixels that must contain meaningful edges.

EdgeThreshold
Threshold used when detecting image edges.

MaxMeanGradient
Maximum mean image gradient for a frame to be considered a low-detail transition.

FallbackSeekMs
Comma-separated list of additional seek positions in milliseconds.

FallbackPercent
Additional seek position expressed as a percentage of total video duration.


### [ZipSequence]

Enabled
Enables support for image sequences stored inside ZIP archives, even when the file uses
a video-like extension such as .mp4.

MaxEntryMB
Maximum uncompressed size of a candidate image entry inside the ZIP archive.

MaxCandidates
Maximum number of candidate images inspected inside the archive.

MaxSourceMP
Maximum source image size in megapixels accepted for WIC image decoding.


### [Compatibility]

ResolveSymlinkFallback
If enabled, VideoThumb may retry a failed decode after resolving the final target path
through the Windows API.

The default value is 0 because direct libav access already works with the tested
local-directory-symlink → UNC/SMB configuration, and the Windows path-resolution fallback
may block longer on an unavailable network target.


## Building

Requirements:

- Windows x64
- Visual Studio 2022 C++ toolchain
- CMake 3.25 or newer
- PowerShell
- FFmpeg shared development build

Download the FFmpeg SDK:

Set-ExecutionPolicy -Scope Process Bypass
.\scripts\get_ffmpeg_sdk.ps1

Configure:

cmake --preset vs2022-x64-release

Build the Release version:

cmake --build .\build\vs2022-x64 --config Release --clean-first

The plugin will be created under:

build\vs2022-x64\dist\Release\

To install the locally built version:

.\scripts\install_to_totalcmd.ps1

Close Total Commander before replacing the plugin DLL.


## Dependencies

VideoThumb uses:

- FFmpeg / libavformat / libavcodec / libavutil / libswscale
- miniz
- Windows Imaging Component (WIC)

FFmpeg is dynamically linked and is distributed under its own license terms.

miniz is compiled statically into the plugin and remains subject to its own license.


## Security notes

VideoThumb decodes media directly inside the Total Commander process.

The plugin includes:

- decoding time limits
- packet limits
- archive entry size limits
- image dimension/resource limits
- controlled parallelism
- exception handling at the WLX boundary

As with any software using media parsers and codecs, keeping the FFmpeg libraries up to
date is recommended.


## Platform

Tested on:

- Windows 10 x64
- Total Commander x64
- FFmpeg 9.x shared libraries

The plugin itself is x64-only.


## Version

Current version:

1.4.3
