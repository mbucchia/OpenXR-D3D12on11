# OpenXR Direct3D 12 on Direct3D 11 interoperability layer

This software enables OpenXR apps developed for Direct3D 12 to work with OpenXR runtimes that have support for Direct3D 11 and not Direct3D 12.

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

## Setup

Download the latest version from the [Releases page](https://github.com/mbucchia/OpenXR-D3D12on11/releases). Find the installer program under **Assets**, file `OpenXR-D3D12on11.msi`.

For troubleshooting, the log file can be found at `%LocalAppData%\XR_APILAYER_NOVENDOR_d3d12on11_interop.log`.

## Limitations

- This has only been tested with Windows Mixed Reality.
- This has only been tested with NVIDIA.
- This has been tested with the HelloXR sample app from Khronos and Flight Simulator 2020.

## Known issues

- Support for applications submitting depth buffer (eg: Unity) is currently not implemented, and will cause a crash with the following log message.

```
    Origin: dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, textureHandle.put())
    Source: C:\Users\mbucc\Desktop\XR\API-Layers\OpenXR-D3D12on11\XR_APILAYER_NOVENDOR_d3d12on11_interop\layer.cpp:359
```
