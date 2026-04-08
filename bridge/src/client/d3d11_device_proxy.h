/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <d3d11.h>

struct __declspec(uuid("1F23F1A3-467B-42CB-8D59-4AA2BC74A6E9")) IBridgeD3D11DeviceProxy : public IUnknown {
  virtual ID3D11Device* STDMETHODCALLTYPE GetUnderlyingDevice() = 0;
};

namespace bridge::client {

  HRESULT createProxyD3D11Device(ID3D11Device** ppDevice);

  inline ID3D11Device* unwrapProxyD3D11Device(ID3D11Device* pDevice) {
    if (pDevice == nullptr) {
      return nullptr;
    }

    IBridgeD3D11DeviceProxy* pProxy = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IBridgeD3D11DeviceProxy), reinterpret_cast<void**>(&pProxy)))) {
      ID3D11Device* pUnderlyingDevice = pProxy->GetUnderlyingDevice();
      pProxy->Release();
      return pUnderlyingDevice;
    }

    return pDevice;
  }

}