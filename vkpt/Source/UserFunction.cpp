// Copyright (c) 2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "UserFunction.h"

#include <cassert>

vkpt::UserPrint::UserPrint(PFN_rgPrint _printFunc, void *_pUserData)
    : printFunc(_printFunc), pUserData(_pUserData) {}

void vkpt::UserPrint::Print(const char *pMessage) const
{
    if (printFunc != nullptr)
    {
        printFunc(pMessage, pUserData);
    }
}



vkpt::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(const UserFileLoad *_pUFL, const char *_pFilePath)
    : pData(nullptr), dataSize(0), pUFL(_pUFL), pFileHandle(nullptr)
{
    assert(pUFL != nullptr);
    pUFL->OpenFile(_pFilePath, &pData, &dataSize, &pFileHandle);
}

vkpt::UserFileLoad::UserFileLoadHandle::~UserFileLoadHandle()
{
    assert(pUFL != nullptr);
    pUFL->CloseFile(pFileHandle);
}

vkpt::UserFileLoad::UserFileLoadHandle::UserFileLoadHandle(UserFileLoadHandle &&other) noexcept
{
    this->pData = other.pData;
    this->dataSize = other.dataSize;
    this->pFileHandle = other.pFileHandle;
    this->pUFL = other.pUFL;

    other.pData = nullptr;
    other.dataSize = 0;
    other.pFileHandle = nullptr;
    other.pUFL = nullptr;
}

vkpt::UserFileLoad::UserFileLoadHandle &vkpt::UserFileLoad::UserFileLoadHandle::operator=(UserFileLoadHandle && other) noexcept
{
    this->pData = other.pData;
    this->dataSize = other.dataSize;
    this->pFileHandle = other.pFileHandle;
    this->pUFL = other.pUFL;

    other.pData = nullptr;
    other.dataSize = 0;
    other.pFileHandle = nullptr;
    other.pUFL = nullptr;

    return *this;
}

vkpt::UserFileLoad::UserFileLoadHandle::operator bool() const
{
    return Contains();
}

bool vkpt::UserFileLoad::UserFileLoadHandle::Contains() const
{
    return pData != nullptr && dataSize > 0;
}



vkpt::UserFileLoad::UserFileLoad(PFN_rgOpenFile _openFileFunc, PFN_rgCloseFile _closeFileFunc, void *_pUserData)
    : openFileFunc(_openFileFunc), closeFileFunc(_closeFileFunc), pUserData(_pUserData) {}

bool vkpt::UserFileLoad::Exists() const
{
    return openFileFunc != nullptr && closeFileFunc != nullptr;
}

vkpt::UserFileLoad::UserFileLoadHandle vkpt::UserFileLoad::Open(const char *pFilePath) const
{
    return UserFileLoadHandle(this, pFilePath);
}

void vkpt::UserFileLoad::OpenFile(const char *pFilePath, const void **ppOutData, uint32_t *pOutDataSize, void **ppOutFileUserHandle) const
{
    *ppOutData = nullptr;
    *pOutDataSize = 0;

    if (Exists())
    {
        openFileFunc(pFilePath, pUserData, ppOutData, pOutDataSize, ppOutFileUserHandle);
    }
}

void vkpt::UserFileLoad::CloseFile(void *pFileUserHandle) const
{
    if (Exists())
    {
        closeFileFunc(pFileUserHandle, pUserData);
    }
}
