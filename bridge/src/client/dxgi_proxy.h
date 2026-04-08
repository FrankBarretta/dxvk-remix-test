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

#include <dxgi.h>
#include <dxgi1_2.h>

struct __declspec(uuid("6C7C8BCE-1BF5-4D3B-9C42-4CB6EF2DE111")) IBridgeDxgiAdapterProxy : public IUnknown {
  virtual IDXGIAdapter1* STDMETHODCALLTYPE GetUnderlyingAdapter() = 0;
  virtual UINT STDMETHODCALLTYPE GetAdapterIndex() = 0;
};

struct __declspec(uuid("4B8E6B0F-8F16-4B67-9E4C-2AF84B9D5AF2")) IBridgeDxgiOutputProxy : public IUnknown {
  virtual IDXGIOutput* STDMETHODCALLTYPE GetUnderlyingOutput() = 0;
};

namespace bridge::client {

  constexpr UINT kDefaultProxyAdapterIndex = UINT_MAX;

  HRESULT createProxyDxgiFactory(REFIID riid, void** ppFactory, IDXGIFactory1* pUnderlyingFactory);

  inline bool tryGetProxyAdapterIndex(IDXGIAdapter* pAdapter, UINT& adapterIndex) {
    adapterIndex = kDefaultProxyAdapterIndex;

    if (pAdapter == nullptr) {
      return true;
    }

    IBridgeDxgiAdapterProxy* pProxy = nullptr;
    if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IBridgeDxgiAdapterProxy), reinterpret_cast<void**>(&pProxy)))) {
      adapterIndex = pProxy->GetAdapterIndex();
      pProxy->Release();
      return true;
    }

    return false;
  }

  inline IDXGIAdapter* unwrapProxyAdapter(IDXGIAdapter* pAdapter) {
    if (pAdapter == nullptr) {
      return nullptr;
    }

    IBridgeDxgiAdapterProxy* pProxy = nullptr;
    if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IBridgeDxgiAdapterProxy), reinterpret_cast<void**>(&pProxy)))) {
      IDXGIAdapter1* pUnderlyingAdapter = pProxy->GetUnderlyingAdapter();
      pProxy->Release();
      return pUnderlyingAdapter;
    }

    return pAdapter;
  }

  inline IDXGIOutput* unwrapProxyOutput(IDXGIOutput* pOutput) {
    if (pOutput == nullptr) {
      return nullptr;
    }

    IBridgeDxgiOutputProxy* pProxy = nullptr;
    if (SUCCEEDED(pOutput->QueryInterface(__uuidof(IBridgeDxgiOutputProxy), reinterpret_cast<void**>(&pProxy)))) {
      IDXGIOutput* pUnderlyingOutput = pProxy->GetUnderlyingOutput();
      pProxy->Release();
      return pUnderlyingOutput;
    }

    return pOutput;
  }

}