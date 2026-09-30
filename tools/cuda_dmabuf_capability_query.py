#!/usr/bin/env python3
"""Query CUDA's documented DMA-BUF export capability flags; allocate no memory."""

import ctypes
import sys


# CUDA Driver API 13.4 cuda.h enum values. Keep these numeric constants visible
# because this host does not install the CUDA toolkit headers.
DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED = 124
DEVICE_ATTRIBUTE_HOST_ALLOC_DMA_BUF_SUPPORTED = 146
DEVICE_ATTRIBUTE_DMA_BUF_MMAP_SUPPORTED = 152

CUresult = ctypes.c_int
CUdevice = ctypes.c_int

try:
    cuda = ctypes.CDLL("libcuda.so.1")
except OSError as error:
    print(f"cannot load libcuda.so.1: {error}", file=sys.stderr)
    raise SystemExit(1)

cuda.cuInit.argtypes = [ctypes.c_uint]
cuda.cuInit.restype = CUresult
cuda.cuDeviceGetCount.argtypes = [ctypes.POINTER(ctypes.c_int)]
cuda.cuDeviceGetCount.restype = CUresult
cuda.cuDeviceGet.argtypes = [ctypes.POINTER(CUdevice), ctypes.c_int]
cuda.cuDeviceGet.restype = CUresult
cuda.cuDeviceGetName.argtypes = [ctypes.POINTER(ctypes.c_char), ctypes.c_int, CUdevice]
cuda.cuDeviceGetName.restype = CUresult
cuda.cuDriverGetVersion.argtypes = [ctypes.POINTER(ctypes.c_int)]
cuda.cuDriverGetVersion.restype = CUresult
cuda.cuDeviceGetAttribute.argtypes = [
    ctypes.POINTER(ctypes.c_int), ctypes.c_int, CUdevice
]
cuda.cuDeviceGetAttribute.restype = CUresult


def check(result: int, operation: str) -> None:
    if result != 0:
        raise RuntimeError(f"{operation} failed with CUresult {result}")


check(cuda.cuInit(0), "cuInit")
device_count = ctypes.c_int()
check(cuda.cuDeviceGetCount(ctypes.byref(device_count)), "cuDeviceGetCount")
driver_api_version = ctypes.c_int()
check(cuda.cuDriverGetVersion(ctypes.byref(driver_api_version)), "cuDriverGetVersion")
print(f"driver_api_version={driver_api_version.value} device_count={device_count.value}")

attributes = (
    (DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, "DMA_BUF_SUPPORTED"),
    (DEVICE_ATTRIBUTE_HOST_ALLOC_DMA_BUF_SUPPORTED, "HOST_ALLOC_DMA_BUF_SUPPORTED"),
    (DEVICE_ATTRIBUTE_DMA_BUF_MMAP_SUPPORTED, "DMA_BUF_MMAP_SUPPORTED"),
)

for index in range(device_count.value):
    device = CUdevice()
    check(cuda.cuDeviceGet(ctypes.byref(device), index), "cuDeviceGet")
    name = ctypes.create_string_buffer(256)
    check(cuda.cuDeviceGetName(name, len(name), device), "cuDeviceGetName")
    print(f"device[{index}]={name.value.decode(errors='replace')}")
    for attribute, label in attributes:
        value = ctypes.c_int(-1)
        result = cuda.cuDeviceGetAttribute(ctypes.byref(value), attribute, device)
        print(f"{label} attribute={attribute} result={result} value={value.value}")
