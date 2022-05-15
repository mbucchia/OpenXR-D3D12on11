// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

#include "layer.h"
#include "log.h"

namespace {

    using namespace d3d12on11_interop;
    using namespace d3d12on11_interop::log;

    using namespace xr::math;

    class OpenXrLayer : public d3d12on11_interop::OpenXrApi {
      private:
        // State associated with an OpenXR session.
        struct Session {
            XrSession xrSession{XR_NULL_HANDLE};

            // We create a D3D11 device that the runtime will be using.
            ComPtr<ID3D11Device5> d3d11Device;
            ComPtr<ID3D11DeviceContext4> d3d11Context;

            // We store information about the D3D12 device that the app is using.
            ComPtr<ID3D12Device> d3d12Device;
            ComPtr<ID3D12CommandQueue> d3d12Queue;

            // For synchronization between the app and the runtime, we use a fence.
            ComPtr<ID3D11Fence> d3d11Fence;
            ComPtr<ID3D12Fence> d3d12Fence;
            UINT64 fenceValue{0};
        };

        struct Swapchain {
            XrSwapchain xrSwapchain{XR_NULL_HANDLE};
            XrSwapchainCreateInfo createInfo;

            // The parent session.
            XrSession xrSession{XR_NULL_HANDLE};

            // We import the D3D11 textures into our D3D12 device.
            std::vector<ComPtr<ID3D12Resource>> textures;
        };

      public:
        OpenXrLayer() = default;

        ~OpenXrLayer() override {
            while (m_sessions.size()) {
                cleanupSession(m_sessions.begin()->second);
                m_sessions.erase(m_sessions.begin());
            }
        }

        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            const std::string apiName(name);
            XrResult result = XR_SUCCESS;

            if (apiName == "xrGetD3D12GraphicsRequirementsKHR") {
                *function = reinterpret_cast<PFN_xrVoidFunction>(wrapper_xrGetD3D12GraphicsRequirementsKHR);
            } else {
                result = OpenXrApi::xrGetInstanceProcAddr(instance, name, function);
            }
            return result;
        }

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // TODO: This should be auto-generated in the call above, but today our generator only looks at core spec.
            // We allow this call to fail, in case the app creates a bootstrap instance without requesting D3D11
            // support.
            xrGetInstanceProcAddr(GetXrInstance(),
                                  "xrGetD3D11GraphicsRequirementsKHR",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&xrGetD3D11GraphicsRequirementsKHR));

            // Dump the application name and OpenXR runtime information to help debugging customer issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            Log("Application: %s\n", GetApplicationName().c_str());
            Log("Using OpenXR runtime: %s\n", runtimeName.c_str());

            return XR_SUCCESS;
        }

        XrResult xrGetD3D12GraphicsRequirementsKHR(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrGraphicsRequirementsD3D12KHR* graphicsRequirements) {
            XrGraphicsRequirementsD3D11KHR runtimeRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            const XrResult result = xrGetD3D11GraphicsRequirementsKHR(instance, systemId, &runtimeRequirements);
            if (XR_SUCCEEDED(result)) {
                graphicsRequirements->adapterLuid = runtimeRequirements.adapterLuid;
                // We need at least feature level 11 for D3D12.
                graphicsRequirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_1;
            }

            return result;
        }

        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY &&
                xrGetD3D11GraphicsRequirementsKHR) {
                XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                Log("Using OpenXR system: %s\n", systemProperties.systemName);

                // Remember the XrSystemId to use.
                m_systemId = *systemId;
            }

            return result;
        }

        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            XrGraphicsBindingD3D11KHR d3d11Bindings{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            Session newSession;
            bool handled = false;
            const XrBaseInStructure* const* alteredPrev = nullptr;
            const XrBaseInStructure* restoreNext = nullptr;

            if (isSystemHandled(createInfo->systemId)) {
                const XrBaseInStructure* const* pprev =
                    reinterpret_cast<const XrBaseInStructure* const*>(&createInfo->next);
                const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                while (entry) {
                    if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        const XrGraphicsBindingD3D12KHR* d3d12Bindings =
                            reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);

                        newSession.d3d12Device = d3d12Bindings->device;
                        newSession.d3d12Queue = d3d12Bindings->queue;

                        // Create interop resources.
                        {
                            ComPtr<IDXGIFactory1> dxgiFactory;
                            CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));

                            const auto adapterLuid = newSession.d3d12Device->GetAdapterLuid();
                            ComPtr<IDXGIAdapter1> dxgiAdapter;
                            for (UINT adapterIndex = 0;; adapterIndex++) {
                                // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to
                                // enumerate.
                                CHECK_HRCMD(
                                    dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

                                DXGI_ADAPTER_DESC1 adapterDesc;
                                CHECK_HRCMD(dxgiAdapter->GetDesc1(&adapterDesc));
                                if (!memcmp(&adapterDesc.AdapterLuid, &adapterLuid, sizeof(LUID))) {
                                    const std::wstring wadapterDescription(adapterDesc.Description);
                                    std::string adapterDescription;
                                    std::transform(wadapterDescription.begin(),
                                                   wadapterDescription.end(),
                                                   std::back_inserter(adapterDescription),
                                                   [](wchar_t c) { return (char)c; });

                                    // Log the adapter name to help debugging customer issues.
                                    Log("Using Direct3D 12 on adapter: %s\n", adapterDescription.c_str());
                                    break;
                                }
                            }

                            // Create the interop device that the runtime will be using.
                            ComPtr<ID3D11Device> device;
                            ComPtr<ID3D11DeviceContext> deviceContext;
                            D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
                            UINT flags = 0;
#ifdef _DEBUG
                            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
                            CHECK_HRCMD(D3D11CreateDevice(dxgiAdapter.Get(),
                                                          D3D_DRIVER_TYPE_UNKNOWN,
                                                          0,
                                                          flags,
                                                          &featureLevel,
                                                          1,
                                                          D3D11_SDK_VERSION,
                                                          device.ReleaseAndGetAddressOf(),
                                                          nullptr,
                                                          deviceContext.ReleaseAndGetAddressOf()));

                            // Query the necessary flavors of device & device context, which will let us use fences.
                            CHECK_HRCMD(device->QueryInterface(newSession.d3d11Device.ReleaseAndGetAddressOf()));
                            CHECK_HRCMD(
                                deviceContext->QueryInterface(newSession.d3d11Context.ReleaseAndGetAddressOf()));

                            // We will use a shared fence to synchronize between the D3D12 queue and the D3D11
                            // context.
                            CHECK_HRCMD(newSession.d3d12Device->CreateFence(
                                0,
                                D3D12_FENCE_FLAG_SHARED,
                                IID_PPV_ARGS(newSession.d3d12Fence.ReleaseAndGetAddressOf())));
                            wil::unique_handle fenceHandle = nullptr;
                            CHECK_HRCMD(newSession.d3d12Device->CreateSharedHandle(
                                newSession.d3d12Fence.Get(), nullptr, GENERIC_ALL, nullptr, fenceHandle.put()));
                            newSession.d3d11Device->OpenSharedFence(
                                fenceHandle.get(), IID_PPV_ARGS(newSession.d3d11Fence.ReleaseAndGetAddressOf()));
                        }

                        // Fill out the struct that we are passing to the OpenXR runtime.
                        // TODO: Do not write to the const struct!
                        alteredPrev = pprev;
                        restoreNext = *pprev;
                        *const_cast<XrBaseInStructure**>(pprev) = reinterpret_cast<XrBaseInStructure*>(&d3d11Bindings);
                        d3d11Bindings.next = entry->next;
                        d3d11Bindings.device = newSession.d3d11Device.Get();

                        handled = true;

                        break;
                    }

                    entry = entry->next;
                }

                if (!handled) {
                    Log("Direct3D 12 is not requested for the session\n");
                }
            }

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (handled) {
                // Restore the original struct. This is needed for downstream API layers, like the OpenXR Toolkit.
                *const_cast<const XrBaseInStructure**>(alteredPrev) = restoreNext;

                if (XR_SUCCEEDED(result)) {
                    // On success, record the state.
                    newSession.xrSession = *session;
                    m_sessions.insert_or_assign(*session, newSession);
                }
            }
            return result;
        }

        XrResult xrDestroySession(XrSession session) override {
            const XrResult result = OpenXrApi::xrDestroySession(session);
            if (XR_SUCCEEDED(result) && isSessionHandled(session)) {
                auto& sessionState = m_sessions[session];

                cleanupSession(sessionState);
                m_sessions.erase(session);
            }

            return result;
        }

        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            Swapchain newSwapchain;
            bool handled = false;

            if (isSessionHandled(session)) {
                Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
                    "usage=0x%x\n",
                    createInfo->width,
                    createInfo->height,
                    createInfo->arraySize,
                    createInfo->mipCount,
                    createInfo->sampleCount,
                    createInfo->format,
                    createInfo->usageFlags);

                newSwapchain.xrSession = session;
                newSwapchain.createInfo = *createInfo;

                // The rest will be filled in by xrEnumerateSwapchainImages().

                handled = true;
            }

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
            if (XR_SUCCEEDED(result) && handled) {
                // On success, record the state.
                newSwapchain.xrSwapchain = *swapchain;
                m_swapchains.insert_or_assign(*swapchain, newSwapchain);
            }

            return result;
        }

        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                m_swapchains.erase(swapchain);
            }

            return result;
        }

        XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                            uint32_t imageCapacityInput,
                                            uint32_t* imageCountOutput,
                                            XrSwapchainImageBaseHeader* images) override {
            if (!isSwapchainHandled(swapchain) || imageCapacityInput == 0) {
                return OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
            }

            // Enumerate the D3D11 swapchain images.
            std::vector<XrSwapchainImageD3D11KHR> d3d11Images(imageCapacityInput, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            const XrResult result = OpenXrApi::xrEnumerateSwapchainImages(
                swapchain,
                imageCapacityInput,
                imageCountOutput,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d11Images.data()));
            if (XR_SUCCEEDED(result)) {
                auto& swapchainState = m_swapchains[swapchain];
                auto& sessionState = m_sessions[swapchainState.xrSession];

                // Export each D3D11 texture to D3D12.
                XrSwapchainImageD3D12KHR* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
                for (uint32_t i = 0; i < *imageCountOutput; i++) {
                    // Dump the runtime texture descriptor.
                    if (i == 0) {
                        D3D11_TEXTURE2D_DESC desc;
                        d3d11Images[0].texture->GetDesc(&desc);
                        Log("Swapchain image descriptor:\n");
                        Log("  w=%u h=%u arraySize=%u format=%u\n",
                            desc.Width,
                            desc.Height,
                            desc.ArraySize,
                            desc.Format);
                        Log("  mipCount=%u sampleCount=%u\n", desc.MipLevels, desc.SampleDesc.Count);
                        Log("  usage=0x%x bindFlags=0x%x cpuFlags=0x%x misc=0x%x\n",
                            desc.Usage,
                            desc.BindFlags,
                            desc.CPUAccessFlags,
                            desc.MiscFlags);
                    }

                    // TODO: Depth textures do not appear to be shareable. Need to implement texture copy for them.
                    wil::unique_handle textureHandle;
                    ComPtr<IDXGIResource1> dxgiResource;
                    CHECK_HRCMD(
                        d3d11Images[i].texture->QueryInterface(IID_PPV_ARGS(dxgiResource.ReleaseAndGetAddressOf())));
                    CHECK_HRCMD(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, textureHandle.put()));
                    ComPtr<ID3D12Resource> d3d12Resource;
                    CHECK_HRCMD(sessionState.d3d12Device->OpenSharedHandle(
                        textureHandle.get(), IID_PPV_ARGS(d3d12Resource.ReleaseAndGetAddressOf())));

                    swapchainState.textures.push_back(d3d12Resource);
                    d3d12Images[i].texture = d3d12Resource.Get();

                    // TODO: Do we need explicit barriers upon xrAcquireSwapchainImage()/xrReleaseSwapchainImage()?
                }
            }

            return result;
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (isSessionHandled(session)) {
                auto& sessionState = m_sessions[session];

                // Serializes the app work between D3D12 and D3D11.
                CHECK_HRCMD(sessionState.d3d12Queue->Signal(sessionState.d3d12Fence.Get(), ++sessionState.fenceValue));
                CHECK_HRCMD(sessionState.d3d11Context->Wait(sessionState.d3d11Fence.Get(), sessionState.fenceValue));
            }

            return OpenXrApi::xrEndFrame(session, frameEndInfo);
        }

      private:
        void cleanupSession(Session& sessionState) {
            // Wait for all the queued work to complete.
            wil::unique_handle eventHandle;
            sessionState.d3d12Queue->Signal(sessionState.d3d12Fence.Get(), ++sessionState.fenceValue);
            *eventHandle.put() = CreateEventEx(nullptr, L"Flush Fence", 0, EVENT_ALL_ACCESS);
            CHECK_HRCMD(sessionState.d3d12Fence->SetEventOnCompletion(sessionState.fenceValue, eventHandle.get()));
            WaitForSingleObject(eventHandle.get(), INFINITE);
            ResetEvent(eventHandle.get());
            sessionState.d3d11Context->Flush1(D3D11_CONTEXT_TYPE_ALL, eventHandle.get());
            WaitForSingleObject(eventHandle.get(), INFINITE);

            for (auto it = m_swapchains.begin(); it != m_swapchains.end();) {
                auto& swapchainState = it->second;
                if (swapchainState.xrSession == sessionState.xrSession) {
                    it = m_swapchains.erase(it);
                } else {
                    it++;
                }
            }
        }

        bool isSystemHandled(XrSystemId systemId) const {
            return systemId == m_systemId;
        }

        bool isSessionHandled(XrSession session) const {
            return m_sessions.find(session) != m_sessions.cend();
        }

        bool isSwapchainHandled(XrSwapchain swapchain) const {
            return m_swapchains.find(swapchain) != m_swapchains.cend();
        }

        // TODO: This should be auto-generated in the dispatch layer.
        static XrResult wrapper_xrGetD3D12GraphicsRequirementsKHR(
            XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsD3D12KHR* graphicsRequirements) {
            DebugLog("--> xrGetD3D12GraphicsRequirementsKHR\n");

            XrResult result;
            try {
                result = dynamic_cast<OpenXrLayer*>(GetInstance())
                             ->xrGetD3D12GraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
            } catch (std::exception& exc) {
                Log("%s\n", exc.what());
                result = XR_ERROR_RUNTIME_FAILURE;
            }

            DebugLog("<-- xrGetD3D12GraphicsRequirementsKHR %d\n", result);
            return result;
        }

        XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

        // TODO: This should be auto-generated and accessible via OpenXrApi.
        PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR{nullptr};

        std::map<XrSession, Session> m_sessions;
        std::map<XrSwapchain, Swapchain> m_swapchains;
    };

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

} // namespace

namespace d3d12on11_interop {
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance() {
        g_instance.reset();
    }

} // namespace d3d12on11_interop
