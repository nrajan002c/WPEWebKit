/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebGPUTextureDescriptor.h"

#if ENABLE(WEBGPU)

namespace WebCore {

Ref<WebGPUTextureDescriptor> WebGPUTextureDescriptor::create(unsigned pixelFormat, unsigned width, unsigned height, bool mipmapped)
{
    return adoptRef(*new WebGPUTextureDescriptor(pixelFormat, width, height, mipmapped));
}

WebGPUTextureDescriptor::WebGPUTextureDescriptor(unsigned pixelFormat, unsigned width, unsigned height, bool mipmapped)
    : m_descriptor { pixelFormat, width, height, mipmapped }
{
}

unsigned WebGPUTextureDescriptor::width() const
{
    return m_descriptor.width();
}

void WebGPUTextureDescriptor::setWidth(unsigned width)
{
    m_descriptor.setWidth(width);
}

unsigned WebGPUTextureDescriptor::height() const
{
    return m_descriptor.height();
}

void WebGPUTextureDescriptor::setHeight(unsigned height)
{
    m_descriptor.setHeight(height);
}

unsigned WebGPUTextureDescriptor::sampleCount() const
{
    return m_descriptor.sampleCount();
}

void WebGPUTextureDescriptor::setSampleCount(unsigned sampleCount)
{
    m_descriptor.setSampleCount(sampleCount);
}

unsigned WebGPUTextureDescriptor::textureType() const
{
    return m_descriptor.textureType();
}

void WebGPUTextureDescriptor::setTextureType(unsigned textureType)
{
    m_descriptor.setTextureType(textureType);
}

unsigned WebGPUTextureDescriptor::storageMode() const
{
    return m_descriptor.storageMode();
}

void WebGPUTextureDescriptor::setStorageMode(unsigned storageMode)
{
    m_descriptor.setStorageMode(storageMode);
}

unsigned WebGPUTextureDescriptor::usage() const
{
    return m_descriptor.usage();
}

void WebGPUTextureDescriptor::setUsage(unsigned usage)
{
    m_descriptor.setUsage(usage);
}
    
} // namespace WebCore

#endif
