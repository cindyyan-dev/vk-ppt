# Build the layer for Android arm64.
#   build.ps1     -> out\android\libVkLayer_ppt.so
# Vulkan headers come from the Vulkan-Headers submodule (git submodule update --init).
# Override with -VulkanHeaders for another checkout.
param(
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$NdkVersion = '25.1.8937393',
    [string]$VulkanHeaders = (Join-Path $PSScriptRoot 'external\Vulkan-Headers\include')
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (-not (Test-Path (Join-Path $VulkanHeaders 'vulkan\vk_layer.h'))) {
    throw "no vulkan\vk_layer.h under $VulkanHeaders (run: git submodule update --init)"
}
$cmake = Join-Path $Sdk 'cmake\3.22.1\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }
$ninja = Join-Path $Sdk 'cmake\3.22.1\bin\ninja.exe'
$build = Join-Path $root 'out\android'
$argsList = @('-S', $root, '-B', $build, "-DVULKAN_HEADERS=$VulkanHeaders",
    '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$Sdk\ndk\$NdkVersion\build\cmake\android.toolchain.cmake",
    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release')
& $cmake @argsList
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Get-Item (Join-Path $build 'libVkLayer_ppt.so') | Select-Object FullName, Length, LastWriteTime
