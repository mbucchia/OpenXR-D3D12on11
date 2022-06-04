# OpenXR Direct3D 12 on Direct3D 11 interoperability layer

This software enables OpenXR apps developed for Direct3D 12 to work with OpenXR runtimes that have support for Direct3D 11 and not Direct3D 12.

DISCLAIMER: This software is distributed as-is, without any warranties or conditions of any kind. Use at your own risks.

## Setup

Download the latest version from the [Releases page](https://github.com/mbucchia/OpenXR-D3D12on11/releases). Find the installer program under **Assets**, file `OpenXR-D3D12on11.msi`.

For troubleshooting, the log file can be found at `%LocalAppData%\XR_APILAYER_NOVENDOR_d3d12on11_interop.log`.

## Limitations

- This has only been tested with Windows Mixed Reality and Varjo.
- This has only been tested with NVIDIA.
- This has been tested with the HelloXR sample app from Khronos and Flight Simulator 2020.

## How does it work?

This API layer sits between any OpenXR application and the OpenXR runtime. It enhances the currently selected OpenXR runtime with the OpenXR extension necessary for Direct3D 12 support (`XR_KHR_d3d12_enable`). It uses the OpenXR runtime's Direct3D 11 support to efficiently bridge the application's Direct3D 12 rendering to Direct3D 11. This processes does not add any overhead: the swapchains (drawing surfaces) requested by the application as Direct3D 12 resources are imported as-is from Direct3D 11, there is no additional copy nor composition phase. Upon submission of the rendered frame, a simple fence synchronization primitive is inserted in the GPU queue shared with the OpenXR runtime, which will not block the application's rendering loop.

## Known issues

- TBD.

If you are having issues, please visit the [Issues page](https://github.com/mbucchia/OpenXR-D3D12on11/issues) to look at existing support requests or to file a new one.
